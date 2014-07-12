/* parse_record.c  */
/* by Vince Weaver   vincent.weaver _at_ maine.edu */

/* This just tests perf_event sampling */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>

#include <errno.h>

#include <signal.h>

#include <sys/mman.h>

#include <sys/ioctl.h>
#include <asm/unistd.h>
#include <sys/prctl.h>

#include "perf_event.h"
#include "perf_helpers.h"

#include "parse_record.h"

#include <asm/perf_regs.h>

/* Urgh who designed this interface */
static int handle_struct_read_format(unsigned char *sample,
				     int read_format,
				     struct validate_values *validation,
				     int quiet) {

	int offset=0,i;

	if (read_format & PERF_FORMAT_GROUP) {
		long long nr,time_enabled,time_running;

		memcpy(&nr,&sample[offset],sizeof(long long));
		if (!quiet) printf("\t\tNumber: %lld ",nr);
		offset+=8;

		if (validation) {
			if (validation->events!=nr) {
				fprintf(stderr,"Error!  Wrong number "
						"of events %d != %lld\n",
						validation->events,nr);
			}
		}

		if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED) {
			memcpy(&time_enabled,&sample[offset],sizeof(long long));
			if (!quiet) printf("enabled: %lld ",time_enabled);
			offset+=8;
		}
		if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING) {
			memcpy(&time_running,&sample[offset],sizeof(long long));
			if (!quiet) printf("running: %lld ",time_running);
			offset+=8;
		}

		if (!quiet) printf("\n");

		for(i=0;i<nr;i++) {
			long long value, id;

			memcpy(&value,&sample[offset],sizeof(long long));
			if (!quiet) printf("\t\t\tValue: %lld ",value);
			offset+=8;

			if (read_format & PERF_FORMAT_ID) {
				memcpy(&id,&sample[offset],sizeof(long long));
				if (!quiet) printf("id: %lld ",id);
				offset+=8;
			}

			if (!quiet) printf("\n");
		}
	}
	else {

		long long value,time_enabled,time_running,id;

		memcpy(&value,&sample[offset],sizeof(long long));
		if (!quiet) printf("\t\tValue: %lld ",value);
		offset+=8;

		if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED) {
			memcpy(&time_enabled,&sample[offset],sizeof(long long));
			if (!quiet) printf("enabled: %lld ",time_enabled);
			offset+=8;
		}
		if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING) {
			memcpy(&time_running,&sample[offset],sizeof(long long));
			if (!quiet) printf("running: %lld ",time_running);
			offset+=8;
		}
		if (read_format & PERF_FORMAT_ID) {
			memcpy(&id,&sample[offset],sizeof(long long));
			if (!quiet) printf("id: %lld ",id);
			offset+=8;
		}
		if (!quiet) printf("\n");
	}

	return offset;
}

#if defined(__x86_64__)

#define NUM_REGS	PERF_REG_X86_64_MAX
static char reg_names[NUM_REGS][8]=
			{"RAX","RBX","RCX","RDX","RSI","RDI","RBP","RSP",
			 "RIP","RFLAGS","CS","SS","DS","ES","FS","GS",
			 "R8","R9","R10","R11","R12","R13","R14","R15"};


#elif defined(__i386__)

#define NUM_REGS	PERF_REG_X86_32_MAX
static char reg_names[PERF_REG_X86_32_MAX][8]=
			{"EAX","EBX","ECX","EDX","ESI","EDI","EBP","ESP",
			 "EIP","EFLAGS","CS","SS","DS","ES","FS","GS"};

#else

#define NUM_REGS 0

static char reg_names[1][8]={"NONE!"};

#endif



static int print_regs(int quiet,long long abi,long long reg_mask,
		unsigned char *data) {

	int return_offset=0;
	int num_regs=NUM_REGS;
	int i;
	unsigned long long reg_value;

	if (!quiet) printf("\t\tReg mask %llx\n",reg_mask);
	for(i=0;i<64;i++) {
		if (reg_mask&1ULL<<i) {
			if (!quiet) {
				memcpy(&reg_value,&data[return_offset],8);
				if (i<num_regs) {
					printf("\t\t%s : ",reg_names[i]);
				}
				else {
					printf("\t\t??? : ");
				}

				printf("%llx\n",reg_value);
			}
			return_offset+=8;
		}
	}

	return return_offset;
}







long long perf_mmap_read( void *our_mmap, int mmap_size,
                    long long prev_head,
		    int sample_type, int read_format, long long reg_mask,
		    struct validate_values *validate,
		    int quiet ) {

	struct perf_event_mmap_page *control_page = our_mmap;
	long long head,offset;
	int i,size;
	long long bytesize,prev_head_wrap;

	unsigned char *data;

	void *data_mmap=our_mmap+getpagesize();

	if (mmap_size==0) return 0;

	if (control_page==NULL) {
		fprintf(stderr,"ERROR mmap page NULL\n");
		return -1;
	}

	head=control_page->data_head;
	rmb(); /* Must always follow read of data_head */

	size=head-prev_head;

	//printf("Head: %lld Prev_head=%lld\n",head,prev_head);
	//printf("%d new bytes\n",size);

	bytesize=mmap_size*getpagesize();

	if (size>bytesize) {
		printf("error!  we overflowed the mmap buffer %d>%lld bytes\n",
			size,bytesize);
	}

	data=malloc(bytesize);
	if (data==NULL) {
		return -1;
	}

	prev_head_wrap=prev_head%bytesize;

	//   printf("Copying %d bytes from %d to %d\n",
	//	  bytesize-prev_head_wrap,prev_head_wrap,0);
	memcpy(data,(unsigned char*)data_mmap + prev_head_wrap,
		bytesize-prev_head_wrap);

	//printf("Copying %d bytes from %d to %d\n",
	//	  prev_head_wrap,0,bytesize-prev_head_wrap);

	memcpy(data+(bytesize-prev_head_wrap),(unsigned char *)data_mmap,
		prev_head_wrap);

	struct perf_event_header *event;


	offset=0;

	while(offset<size) {

		//printf("Offset %d Size %d\n",offset,size);
		event = ( struct perf_event_header * ) & data[offset];

		/********************/
		/* Print event Type */
		/********************/

		if (!quiet) {
			switch(event->type) {
				case PERF_RECORD_MMAP:
					printf("PERF_RECORD_MMAP"); break;
				case PERF_RECORD_LOST:
					printf("PERF_RECORD_LOST"); break;
				case PERF_RECORD_COMM:
					printf("PERF_RECORD_COMM"); break;
				case PERF_RECORD_EXIT:
					printf("PERF_RECORD_EXIT"); break;
				case PERF_RECORD_THROTTLE:
					printf("PERF_RECORD_THROTTLE"); break;
				case PERF_RECORD_UNTHROTTLE:
					printf("PERF_RECORD_UNTHROTTLE"); break;
				case PERF_RECORD_FORK:
					printf("PERF_RECORD_FORK"); break;
				case PERF_RECORD_READ:
					printf("PERF_RECORD_READ"); break;
				case PERF_RECORD_SAMPLE:
					printf("PERF_RECORD_SAMPLE [%x]",sample_type); break;
				case PERF_RECORD_MMAP2:
					printf("PERF_RECORD_MMAP2"); break;
				default: printf("UNKNOWN %d",event->type); break;
			}

			printf(", MISC=%d (",event->misc);
			switch(event->misc & PERF_RECORD_MISC_CPUMODE_MASK) {
				case PERF_RECORD_MISC_CPUMODE_UNKNOWN:
					printf("PERF_RECORD_MISC_CPUMODE_UNKNOWN"); break; 
				case PERF_RECORD_MISC_KERNEL:
					printf("PERF_RECORD_MISC_KERNEL"); break;
				case PERF_RECORD_MISC_USER:
					printf("PERF_RECORD_MISC_USER"); break;
				case PERF_RECORD_MISC_HYPERVISOR:
					printf("PERF_RECORD_MISC_HYPERVISOR"); break;
				case PERF_RECORD_MISC_GUEST_KERNEL:
					printf("PERF_RECORD_MISC_GUEST_KERNEL"); break;
				case PERF_RECORD_MISC_GUEST_USER:
					printf("PERF_RECORD_MISC_GUEST_USER"); break;
				default:
					printf("Unknown %d!\n",event->misc); break;
			}

			if (event->misc & PERF_RECORD_MISC_EXACT_IP) {
				printf(",PERF_RECORD_MISC_EXACT_IP ");
			}

			if (event->misc & PERF_RECORD_MISC_EXT_RESERVED) {
				printf(",PERF_RECORD_MISC_EXT_RESERVED ");
			}
			printf("), Size=%d\n",event->size);
		}

		offset+=8; /* skip header */

		/***********************/
		/* Print event Details */
		/***********************/

		switch(event->type) {

		/* Lost */
		case PERF_RECORD_LOST: {
			long long id,lost;
			memcpy(&id,&data[offset],sizeof(long long));
			if (!quiet) printf("\tID: %lld\n",id);
			offset+=8;
			memcpy(&lost,&data[offset],sizeof(long long));
			if (!quiet) printf("\tLOST: %lld\n",lost);
			offset+=8;
			}
			break;

		/* Sample */
		case PERF_RECORD_SAMPLE:
			if (sample_type & PERF_SAMPLE_IP) {
				long long ip;
				memcpy(&ip,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_IP, IP: %llx\n",ip);
				offset+=8;
			}
			if (sample_type & PERF_SAMPLE_TID) {
				int pid, tid;
				memcpy(&pid,&data[offset],sizeof(int));
				memcpy(&tid,&data[offset+4],sizeof(int));

				if (validate) {
					if (validate->pid!=pid) {
						fprintf(stderr,"Error, pid %d != %d\n",
							validate->pid,pid);
					}
				}

				if (!quiet) {
					printf("\tPERF_SAMPLE_TID, pid: %d  tid %d\n",pid,tid);
				}
				offset+=8;
			}

			if (sample_type & PERF_SAMPLE_TIME) {
				long long time;
				memcpy(&time,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_TIME, time: %lld\n",time);
				offset+=8;
			}
			if (sample_type & PERF_SAMPLE_ADDR) {
				long long addr;
				memcpy(&addr,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_ADDR, addr: %llx\n",addr);
				offset+=8;
			}
			if (sample_type & PERF_SAMPLE_ID) {
				long long sample_id;
				memcpy(&sample_id,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_ID, sample_id: %lld\n",sample_id);
				offset+=8;
			}
			if (sample_type & PERF_SAMPLE_STREAM_ID) {
				long long sample_stream_id;
				memcpy(&sample_stream_id,&data[offset],sizeof(long long));
				if (!quiet) {
					printf("\tPERF_SAMPLE_STREAM_ID, sample_stream_id: %lld\n",sample_stream_id);
				}
				offset+=8;
			}
			if (sample_type & PERF_SAMPLE_CPU) {
				int cpu, res;
				memcpy(&cpu,&data[offset],sizeof(int));
				memcpy(&res,&data[offset+4],sizeof(int));
				if (!quiet) printf("\tPERF_SAMPLE_CPU, cpu: %d  res %d\n",cpu,res);
				offset+=8;
			}
			if (sample_type & PERF_SAMPLE_PERIOD) {
				long long period;
				memcpy(&period,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_PERIOD, period: %lld\n",period);
				offset+=8;
			}
			if (sample_type & PERF_SAMPLE_READ) {
				int length;

				if (!quiet) printf("\tPERF_SAMPLE_READ, read_format\n");
				length=handle_struct_read_format(&data[offset],
						read_format,
						validate,quiet);
				if (length>=0) offset+=length;
			}
			if (sample_type & PERF_SAMPLE_CALLCHAIN) {
				long long nr,ip;
				memcpy(&nr,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_CALLCHAIN, callchain length: %lld\n",nr);
				offset+=8;

				for(i=0;i<nr;i++) {
					memcpy(&ip,&data[offset],sizeof(long long));
					if (!quiet) printf("\t\t ip[%d]: %llx\n",i,ip);
					offset+=8;
				}
			}
			if (sample_type & PERF_SAMPLE_RAW) {
				int size;

				memcpy(&size,&data[offset],sizeof(int));
				if (!quiet) printf("\tPERF_SAMPLE_RAW, Raw length: %d\n",size);
				offset+=4;

				if (!quiet) printf("\t\t");
				for(i=0;i<size;i++) {
					if (!quiet) printf("%d ",data[offset]);
					offset+=1;
				}
				if (!quiet) printf("\n");
			}

			if (sample_type & PERF_SAMPLE_BRANCH_STACK) {
				long long bnr;
				memcpy(&bnr,&data[offset],sizeof(long long));
				if (!quiet) {
					printf("\tPERF_SAMPLE_BRANCH_STACK, branch_stack entries: %lld\n",bnr);
				}
				offset+=8;

				for(i=0;i<bnr;i++) {
					long long from,to,flags;

					/* From value */
					memcpy(&from,&data[offset],sizeof(long long));
					offset+=8;

					/* Could be more complete here */
					if (validate) {
						if (from < validate->branch_low) {
							fprintf(stderr,"Error! branch out of bounds!\n");
						}
					}


					/* To Value */
					memcpy(&to,&data[offset],sizeof(long long));
					offset+=8;
					if (!quiet) {
						printf("\t\t lbr[%d]: %llx %llx ",
							i,from,to);
		 			}

					/* Flags */
					memcpy(&flags,&data[offset],sizeof(long long));
					offset+=8;

					if (!quiet) {
						if (flags==0) printf("0");

						if (flags&1) {
							printf("MISPREDICTED ");
							flags&=~2;
						}

						if (flags&2) {
							printf("PREDICTED ");
							flags&=~2;
						}

						if (flags&4) {
							printf("IN_TRANSACTION ");
							flags&=~4;
						}

						if (flags&8) {
							printf("TRANSACTION_ABORT ");
							flags&=~8;
						}
						printf("\n");
					}
	      			}
	   		}

			if (sample_type & PERF_SAMPLE_REGS_USER) {
				long long abi;

				memcpy(&abi,&data[offset],sizeof(long long));
				if (!quiet) {
					printf("\tPERF_SAMPLE_REGS_USER, ABI: ");
					if (abi==PERF_SAMPLE_REGS_ABI_NONE) printf ("PERF_SAMPLE_REGS_ABI_NONE");
					if (abi==PERF_SAMPLE_REGS_ABI_32) printf("PERF_SAMPLE_REGS_ABI_32");
					if (abi==PERF_SAMPLE_REGS_ABI_64) printf("PERF_SAMPLE_REGS_ABI_64");
					printf("\n");
				}
				offset+=8;

				offset+=print_regs(quiet,abi,reg_mask,
						&data[offset]);

				if (!quiet) printf("\n");
			}

			if (sample_type & PERF_SAMPLE_STACK_USER) {
				long long abi;

				memcpy(&abi,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_STACK_USER, Raw length: %lld\n",abi);
				offset+=8;

				if (!quiet) printf("\n");
			}

			if (sample_type & PERF_SAMPLE_WEIGHT) {
				long long abi;

				memcpy(&abi,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_WEIGHT, Raw length: %lld\n",abi);
				offset+=8;

				if (!quiet) printf("\n");
			}

			if (sample_type & PERF_SAMPLE_DATA_SRC) {
				long long abi;

				memcpy(&abi,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_DATA_SRC, Raw length: %lld\n",abi);
				offset+=8;

				if (!quiet) printf("\n");
			}

			if (sample_type & PERF_SAMPLE_IDENTIFIER) {
				long long abi;

				memcpy(&abi,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_IDENTIFIER, Raw length: %lld\n",abi);
				offset+=8;

				if (!quiet) printf("\n");
			}

			if (sample_type & PERF_SAMPLE_TRANSACTION) {
				long long abi;

				memcpy(&abi,&data[offset],sizeof(long long));
				if (!quiet) printf("\tPERF_SAMPLE_TRANSACTION, Raw length: %lld\n",abi);
				offset+=8;

				if (!quiet) printf("\n");
			}
			break;

			default: if (!quiet) printf("\tUnknown type %d\n",event->type);
		}
	}

	control_page->data_tail=head;

	free(data);

	return head;

}