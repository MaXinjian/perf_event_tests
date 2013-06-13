#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include <fcntl.h>

#include "shm.h"
#include "syscall.h"
#include "../include/perf_event.h"
#include "../include/perf_helpers.h"

static int error=0;

#define FD_REMAP_SIZE 2048

static int fd_remap[FD_REMAP_SIZE];
static char *mmap_remap[FD_REMAP_SIZE];

static void mmap_event(char *line) {

	int fd,size;
	char *data;

	sscanf(line,"%*c %d %d",&size,&fd);

	data=mmap(NULL, size,
		PROT_READ|PROT_WRITE, MAP_SHARED,
		fd_remap[fd], 0);

	if (data==MAP_FAILED) {
		fprintf(stderr,"Error with mmap of size %d of %d/%d\n",
			size,fd,fd_remap[fd]);
		error=1;
		return;
	}
	mmap_remap[fd_remap[fd]]=data;

//		fcntl(event_data[i].fd, F_SETFL, O_RDWR|O_NONBLOCK|O_ASYNC);
//		fcntl(event_data[i].fd, F_SETSIG, SIGRTMIN+2);
//		fcntl(event_data[i].fd, F_SETOWN,getpid());

}


static void munmap_event(char *line) {

	int fd,size,result;

	sscanf(line,"%*c %d %d",&fd,&size);

	result=munmap(mmap_remap[fd_remap[fd]], size);
	if (result<0) {
		fprintf(stderr,"Error with munmap of %p size %d of %d/%d\n",
			mmap_remap[fd_remap[fd]],size,fd,fd_remap[fd]);
		error=1;
		return;
	}

}


static void open_event(char *line) {


	struct perf_event_attr pe;
	int fd,orig_fd,remapped_group_fd;
	pid_t pid;
	int cpu,group_fd;
	long int flags;

	/* I hate bitfields */
	int disabled,inherit,pinned,exclusive;
	int exclude_user,exclude_kernel,exclude_hv,exclude_idle;
	int mmap,comm,freq,inherit_stat;
	int enable_on_exec,task,watermark,precise_ip;
	int mmap_data,sample_id_all,exclude_host,exclude_guest;

	sscanf(line,
		"%*c %d %d %d %d %lx "
		"%x %x "
		"%llx %llx %llx %llx "
		"%d %d %d %d "
		"%d %d %d %d "
		"%d %d %d %d "
		"%d %d %d %d "
		"%d %d %d %d "
		"%d %d "
		"%llx %llx %lld ",
		&orig_fd,&pid,&cpu,&group_fd,&flags,
		&pe.type,&pe.size,
		&pe.config,&pe.sample_period,&pe.sample_type,&pe.read_format,
		&disabled,&inherit,&pinned,&exclusive,
		&exclude_user,&exclude_kernel,&exclude_hv,&exclude_idle,
		&mmap,&comm,&freq,&inherit_stat,
		&enable_on_exec,&task,&watermark,&precise_ip,
		&mmap_data,&sample_id_all,&exclude_host,&exclude_guest,
		&pe.wakeup_events,&pe.bp_type,
		&pe.config1,&pe.config2,&pe.branch_sample_type);

	/* re-populate bitfields */
	/* can't sscanf into them */
	pe.disabled=disabled;
	pe.inherit=inherit;
	pe.pinned=pinned;
	pe.exclusive=exclusive;
	pe.exclude_user=exclude_user;
	pe.exclude_kernel=exclude_kernel;
	pe.exclude_hv=exclude_hv;
	pe.exclude_idle=exclude_idle;
	pe.mmap=mmap;
	pe.comm=comm;
	pe.freq=freq;
	pe.inherit_stat=inherit_stat;
	pe.enable_on_exec=enable_on_exec;
	pe.task=task;
	pe.watermark=watermark;
	pe.precise_ip=precise_ip;
	pe.mmap_data=mmap_data;
	pe.sample_id_all=sample_id_all;
	pe.exclude_host=exclude_host;
	pe.exclude_guest=exclude_guest;


	if (group_fd==-1) {
		remapped_group_fd=-1;
	} else {
		remapped_group_fd=fd_remap[group_fd];
	}

	fd=perf_event_open(&pe,pid,cpu,remapped_group_fd,flags);
	if (fd<0) {
		fprintf(stderr,"Error opening %s",line);
		error=1;
		return;
	}

	if (fd>FD_REMAP_SIZE) {
		fprintf(stderr,"fd out of range\n");
		error=1;
		return;
	}
	fd_remap[orig_fd]=fd;



}

static void close_event(char *line) {

	int fd;
	int result;

	sscanf(line,"%*c %d",&fd);

	result=close(fd_remap[fd]);
	if (result<0) {
		fprintf(stderr,"Error closing %d/%d\n",
			fd,fd_remap[fd]);

	}

}


static void ioctl_event(char *line) {

	int fd,arg,arg2,result;

	sscanf(line,"%*c %d %d %d",&fd,&arg,&arg2);
	result=ioctl(fd_remap[fd],arg,arg2);
	if (result<0) {
		fprintf(stderr,"Error with ioctl %d %d on %d/%d\n",
			arg,arg2,fd,fd_remap[fd]);
		error=1;
		return;
	}

}


#define MAX_READ_SIZE 65536

static long long data[MAX_READ_SIZE];


static void read_event(char *line) {

	int result,read_size,fd;

	sscanf(line,"%*c %d %d",&fd,&read_size);

	if (read_size>MAX_READ_SIZE*sizeof(long long)) {
		fprintf(stderr,"Read size of %d exceeds max of %ld\n",
			read_size,MAX_READ_SIZE*sizeof(long long));
		error=1;
		return;
	}

	result=read(fd_remap[fd],data,read_size);

	if (result<0) {
		fprintf(stderr,"Error reading %d bytes from %d/%d\n",
			read_size,fd,fd_remap[fd]);
		error=1;
		return;
	}
}

static void prctl_event(char *line) {

        int enable=0;

        sscanf(line,"%*c %d",&enable);

        if (enable) {
                prctl(PR_TASK_PERF_EVENTS_ENABLE);
        }
        else {
                prctl(PR_TASK_PERF_EVENTS_DISABLE);
        }
}

static int forked_pid=0;

static void fork_event(char *line) {

        int enable=0;

        sscanf(line,"%*c %d",&enable);

        if (enable) {
                forked_pid=fork();
                if (forked_pid==0) while(1);
        }
        else {
                kill(forked_pid,SIGKILL);
        }

}


#define REPLAY_OPEN	0x001
#define REPLAY_CLOSE	0x002
#define REPLAY_IOCTL	0x004
#define REPLAY_READ	0x008
#define REPLAY_MMAP	0x010
#define REPLAY_MUNMAP	0x020
#define REPLAY_PRCTL	0x040
#define REPLAY_FORK	0x080
#define REPLAY_ALL	0xfff



int main(int argc, char **argv) {

	FILE *logfile;
	char line[BUFSIZ];
	char *result;
	long long total_syscalls=0,replay_syscalls=0;

	int i;

	int replay_which=REPLAY_ALL;

	if (argc<2) {
		fprintf(stderr,"\nUsage: %s logfile <OCIRMUPF>\n\n",
			argv[0]);
		exit(1);
	}

	logfile=fopen(argv[1],"r");
	if (logfile==NULL) {
		fprintf(stderr,"Error opening %s\n",argv[1]);
		exit(1);
	}

	if (argc==3) {
		replay_which=0;
		for(i=0;i<strlen(argv[2]);i++) {
			switch(argv[2][i]) {
			case 'O':	replay_which|=REPLAY_OPEN;
					break;
			case 'C':	replay_which|=REPLAY_CLOSE;
					break;
			case 'I':	replay_which|=REPLAY_IOCTL;
					break;
			case 'R':	replay_which|=REPLAY_READ;
					break;
			case 'M':	replay_which|=REPLAY_MMAP;
					break;
			case 'U':	replay_which|=REPLAY_MUNMAP;
					break;
			case 'P':	replay_which|=REPLAY_PRCTL;
					break;
			case 'F':	replay_which|=REPLAY_FORK;
					break;
			default:	fprintf(stderr,"Unknown replay %c\n",
						argv[2][i]);

			}
		}
	}

	while(1) {
		result=fgets(line,BUFSIZ,logfile);
		if (result==NULL) break;

		switch(line[0]) {
			case 'O':
				if (replay_which & REPLAY_OPEN) {
					open_event(line);
					replay_syscalls++;
				}
				break;
			case 'C':
				if (replay_which & REPLAY_CLOSE) {
					close_event(line);
					replay_syscalls++;
				}
				break;
			case 'I':
				if (replay_which & REPLAY_IOCTL) {
					ioctl_event(line);
					replay_syscalls++;
				}
				break;
			case 'R':
				if (replay_which & REPLAY_READ) {
					read_event(line);
					replay_syscalls++;
				}
				break;
			case 'M':
				if (replay_which & REPLAY_MMAP) {
					mmap_event(line);
					replay_syscalls++;
				}
				break;
			case 'U':
				if (replay_which & REPLAY_MUNMAP) {
					munmap_event(line);
					replay_syscalls++;
				}
				break;
			case 'P':
				if (replay_which & REPLAY_PRCTL) {
					prctl_event(line);
					replay_syscalls++;
				}
				break;
			case 'F':
				if (replay_which & REPLAY_FORK) {
					fork_event(line);
					replay_syscalls++;
				}
				break;
			default:
				fprintf(stderr,"Unknown log type \'%c\'\n",
					line[0]);
				break;
		}
		if (error) break;
		total_syscalls++;
	}

	printf("Replayed %lld of %lld syscalls\n",
		replay_syscalls,total_syscalls);

	return 0;

}