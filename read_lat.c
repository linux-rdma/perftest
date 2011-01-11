/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2005 Hewlett Packard, Inc (Grant Grundler)
 * Copyright (c) 2009 HNR Consulting.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id$
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <getopt.h>
#include <time.h>
#include <infiniband/verbs.h>

#include "get_clock.h"
#include "perftest_resources.h"

#define VERSION 2.0

static int sl = 0;
static int page_size;

cycles_t *tstamp;

struct report_options {
	int unsorted;
	int histogram;
	int cycles;   /* report delta's in cycles, not microsec's */
};

struct pingpong_context {
	struct ibv_context *context;
	struct ibv_comp_channel *channel;
	struct ibv_pd *pd;
	struct ibv_mr *mr;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	void *buf;
	int size;
	int tx_depth;
};

/****************************************************************************** 
 *
 ******************************************************************************/
static int set_up_connection(struct pingpong_context *ctx,
							 struct perftest_parameters *user_parm,
							 struct pingpong_dest *my_dest) {

	int use_i = user_parm->gid_index;
	int port  = user_parm->ib_port;

	if (use_i != -1) {
		if (ibv_query_gid(ctx->context,port,use_i,&my_dest->gid)) {
			return -1;
		}
	}
	my_dest->lid       = ctx_get_local_lid(ctx->context,user_parm->ib_port);
	my_dest->out_reads = ctx_set_out_reads(ctx->context,user_parm->out_reads);
	my_dest->qpn       = ctx->qp->qp_num;
	my_dest->psn       = lrand48() & 0xffffff;
	my_dest->rkey      = ctx->mr->rkey;
	my_dest->vaddr     = (uintptr_t)ctx->buf;

	// We do not fail test upon lid in RDMAoE/Eth conf.
	if (use_i < 0) {
		if (!my_dest->lid) {
			fprintf(stderr," Local lid 0x0 detected. Is an SM running? \n");
			fprintf(stderr," If you're running RMDAoE you must use GIDs\n");
			return -1;
		}
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int init_connection(struct perftest_parameters *params,
						   struct pingpong_dest *my_dest,
						   const char *servername) {

	params->side      = LOCAL;
	ctx_print_pingpong_data(my_dest,params);

	if (servername) 
		params->sockfd = ctx_client_connect(servername,params->port);
	else 
		params->sockfd = ctx_server_connect(params->port);

	if(params->sockfd < 0) {
		fprintf(stderr,"Unable to open file descriptor for socket connection");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int destroy_ctx_resources(struct pingpong_context *ctx)  {

	int test_result = 0;

	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "failed to destroy QP\n");
		test_result = 1;
	}
	
	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "failed to destroy CQ\n");
		test_result = 1;
	}
	
	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "failed to deregister MR\n");
		test_result = 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "failed to destroy channel \n");
			test_result = 1;
		}
	}
	
	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "failed to deallocate PD\n");
		test_result = 1;
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "failed to close device context\n");
		test_result = 1;
	}

	free(ctx->buf);
	free(ctx);
	free(tstamp);

	return test_result;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,int size,
										    struct perftest_parameters *user_parm) {

	struct pingpong_context *ctx;

	ALLOCATE(ctx,struct pingpong_context,1);
	
	ctx->size     = size;
	ctx->tx_depth = user_parm->tx_depth;

	ctx->buf = memalign(page_size, BUFF_SIZE(size));
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0, BUFF_SIZE(size));

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",ibv_get_device_name(ib_dev));
		return NULL;
	}

	// Finds the link type and configure the HCA accordingly.
	if (ctx_set_link_layer(ctx->context,user_parm)) {
		fprintf(stderr, " Couldn't set the link layer\n");
		return NULL;
	}

	// Configure the Link MTU acoording to the user or the active mtu.
	if (ctx_set_mtu(ctx->context,user_parm)) {
		fprintf(stderr, "Couldn't set the link layer\n");
		return NULL;
	}

	if (user_parm->use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			return NULL;
		}
	} else
		ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return NULL;
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf,BUFF_SIZE(size),IBV_ACCESS_REMOTE_WRITE | 
														   IBV_ACCESS_LOCAL_WRITE  | 
														   IBV_ACCESS_REMOTE_READ);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return NULL;
	}

	// Creates the CQ according to ctx_cq_create in perfetst_resources.
	ctx->cq = ctx_cq_create(ctx->context,ctx->channel,user_parm);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	ctx->qp = ctx_qp_create(ctx->pd,ctx->cq,ctx->cq,user_parm);
	if (!ctx->qp)  {
		fprintf(stderr, "Couldn't create QP\n");
		return NULL;
	}

	if (ctx_modify_qp_to_init(ctx->qp,user_parm)) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return NULL;
	}
	return ctx;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int pp_connect_ctx(struct pingpong_context *ctx,int my_psn,
						  struct pingpong_dest *dest,int my_reads,
						  struct perftest_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(struct ibv_qp_attr));

	attr.qp_state               = IBV_QPS_RTR;
	attr.path_mtu				= user_parm->curr_mtu;
	attr.dest_qp_num            = dest->qpn;
	attr.rq_psn                 = dest->psn;
	attr.ah_attr.dlid           = dest->lid;
	attr.max_dest_rd_atomic     = my_reads;
	attr.min_rnr_timer          = 12;
	if (user_parm->gid_index < 0) {
		attr.ah_attr.is_global      = 0;
		attr.ah_attr.sl             = sl;
	} else {
		attr.ah_attr.is_global      = 1;
		attr.ah_attr.grh.dgid       = dest->gid;
		attr.ah_attr.grh.sgid_index = user_parm->gid_index;
		attr.ah_attr.grh.hop_limit  = 1;
		attr.ah_attr.sl             = 0;
	}
	attr.ah_attr.src_path_bits  = 0;
	attr.ah_attr.port_num       = user_parm->ib_port;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MIN_RNR_TIMER      |
			  IBV_QP_MAX_DEST_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify RC QP to RTR\n");
		return 1;
	}
	attr.timeout            = user_parm->qp_timeout;
	attr.retry_cnt          = 7;
	attr.rnr_retry          = 7;
	attr.qp_state           = IBV_QPS_RTS;
	attr.max_rd_atomic      = dest->out_reads;
	attr.sq_psn             = my_psn;

	if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_SQ_PSN             |
				  IBV_QP_TIMEOUT            |
				  IBV_QP_RETRY_CNT          |
				  IBV_QP_RNR_RETRY          |
				  IBV_QP_MAX_QP_RD_ATOMIC)) {
			fprintf(stderr, "Failed to modify RC QP to RTS\n");
			return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>            listen on/connect to port <port> (default 18515)\n");
	printf("  -m, --mtu=<mtu>              mtu size (256 - 4096. default for hermon is 2048)\n");
	printf("  -d, --ib-dev=<dev>           use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>         use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>            size of message to exchange (default 1)\n");
	printf("  -t, --tx-depth=<dep>         size of tx queue (default 50)\n");
	printf("  -n, --iters=<iters>          number of exchanges (at least 2, default 1000)\n");
	printf("  -o, --outs=<num>             num of outstanding read/atom(default 4)\n");
	printf("  -u, --qp-timeout=<timeout> QP timeout, timeout value is 4 usec * 2 ^(timeout), default 14\n");
	printf("  -S, --sl=<sl>                SL (default 0)\n");
	printf("  -x, --gid-index=<index>      test uses GID with GID index taken from command line (for RDMAoE index should be 0)\n");
	printf("  -a, --all                    Run sizes from 2 till 2^23\n");
	printf("  -C, --report-cycles          report times in cpu cycle units (default microseconds)\n");
	printf("  -H, --report-histogram       print out all results (default print summary only)\n");
	printf("  -U, --report-unsorted        (implies -H) print out unsorted results (default sorted)\n");
	printf("  -V, --version                display version number\n");
	printf("  -e, --events                 sleep on CQ events (default poll)\n");
	printf("  -F, --CPU-freq         do not fail test on different cpu frequencies\n");
}

/*
 * When there is an
 *	odd number of samples, the median is the middle number.
 * 	even number of samples, the median is the mean of the
 *		two middle numbers.
 *
 */
static inline cycles_t get_median(int n, cycles_t delta[])
{
	if ((n - 1) % 2)
		return (delta[n / 2] + delta[n / 2 - 1]) / 2;
	else
		return delta[n / 2];
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int cycles_compare(const void * aptr, const void * bptr)
{
	const cycles_t *a = aptr;
	const cycles_t *b = bptr;
	if (*a < *b) return -1;
	if (*a > *b) return 1;
	return 0;

}

/****************************************************************************** 
 *
 ******************************************************************************/
static void print_report(struct report_options * options,
			 unsigned int iters, cycles_t *tstamp,int size, int no_cpu_freq_fail)
{
	double cycles_to_units;
	cycles_t median;
	unsigned int i;
	const char* units;
	cycles_t *delta = malloc((iters - 1) * sizeof *delta);

	if (!delta) {
		perror("malloc");
		return;
	}

	for (i = 0; i < iters - 1; ++i)
		delta[i] = tstamp[i + 1] - tstamp[i];


	if (options->cycles) {
		cycles_to_units = 1;
		units = "cycles";
	} else {
		cycles_to_units = get_cpu_mhz(no_cpu_freq_fail);
		units = "usec";
	}

	if (options->unsorted) {
		printf("#, %s\n", units);
		for (i = 0; i < iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units );
	}

	qsort(delta, iters - 1, sizeof *delta, cycles_compare);

	if (options->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units );
	}

	median = get_median(iters - 1, delta);
	printf(REPORT_FMT_LAT,size,iters,delta[0] / cycles_to_units ,
	       delta[iters - 2] / cycles_to_units ,median / cycles_to_units );

	free(delta);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest, int size) {

	int scnt = 0;
	int ne;
	struct ibv_sge 		list;
	struct ibv_send_wr 	wr;
	struct ibv_send_wr  *bad_wr;
	struct ibv_wc       wc;
	uint64_t            my_addr,rem_addr;


	list.addr   = (uintptr_t)ctx->buf;
	list.length = size;
	list.lkey   = ctx->mr->lkey;

	wr.sg_list             = &list;
	wr.wr.rdma.remote_addr = rem_dest->vaddr;
	wr.wr.rdma.rkey        = rem_dest->rkey;
	wr.wr_id      		   = PINGPONG_READ_WRID;
	wr.num_sge             = MAX_RECV_SGE;
	wr.opcode              = IBV_WR_RDMA_READ;
	wr.send_flags          = IBV_SEND_SIGNALED;
	wr.next                = NULL;

	my_addr  = list.addr;
	rem_addr = wr.wr.rdma.remote_addr;
	
	while (scnt < user_param->iters ) {
	
		tstamp[scnt] = get_cycles();
		if (ibv_post_send(ctx->qp,&wr,&bad_wr)) {
			fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
			return 11;
		}

		if (size <= (CYCLE_BUFFER / 2)) { 
			increase_rem_addr(&wr,size,scnt,rem_addr);
			increase_loc_addr(&list,size,scnt,my_addr,0);
		}
		scnt++;

	
		if (user_param->use_event) {
			if (ctx_notify_events(ctx->cq,ctx->channel)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return 1;
			}
		}

		do {
			ne = ibv_poll_cq(ctx->cq, 1, &wc);
			if(ne > 0) { 
				if (wc.status != IBV_WC_SUCCESS) 
					NOTIFY_COMP_ERROR_SEND(wc,scnt,scnt);
			}
		} while (!user_param->use_event && ne == 0);

		if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return 12;
		}
		
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	const char                  *ib_devname = NULL;
	int                         size = 2;
	int                         i = 0;
	struct report_options       report = {};
	struct pingpong_context     *ctx;
	struct ibv_device           *ib_dev;
	struct perftest_parameters  user_param;
	int                         no_cpu_freq_fail = 0;
	struct pingpong_dest	    my_dest,rem_dest;

	int all = 0;
	const char *servername = NULL;
	
	/* init default values to user's parameters */
	memset(&user_param,0,sizeof(struct perftest_parameters));
	memset(&my_dest,0,sizeof(struct pingpong_dest));
	memset(&rem_dest,0,sizeof(struct pingpong_dest));

	user_param.mtu        = 0;
	user_param.ib_port    = 1;
	user_param.port       = 18515;
	user_param.iters      = 1000;
	user_param.tx_depth   = 50;
	user_param.rx_depth  = 1;
	user_param.use_event  = 0;
	user_param.qp_timeout = 14;
	user_param.gid_index  = -1; 
	user_param.num_of_qps = 1;
	user_param.verb       = READ;

	/* Parameter parsing. */
	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",           .has_arg = 1, .val = 'p' },
			{ .name = "mtu",            .has_arg = 1, .val = 'm' },
			{ .name = "ib-dev",         .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",        .has_arg = 1, .val = 'i' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "outs",           .has_arg = 1, .val = 'o' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "qp-timeout",     .has_arg = 1, .val = 'u' },
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
			{ .name = "gid-index",      .has_arg = 1, .val = 'x' },
			{ .name = "all",            .has_arg = 0, .val = 'a' },
			{ .name = "report-cycles",  .has_arg = 0, .val = 'C' },
			{ .name = "report-histogram",.has_arg = 0, .val = 'H' },
			{ .name = "report-unsorted",.has_arg = 0, .val = 'U' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "events",         .has_arg = 0, .val = 'e' },
			{ .name = "CPU-freq",       .has_arg = 0, .val = 'F' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:c:m:d:i:s:o:n:t:u:S:x:aeHUVF", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			user_param.port = strtol(optarg, NULL, 0);
			if (user_param.port < 0 || user_param.port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'e':
			++user_param.use_event;
			break;

		case 'm':
			user_param.mtu = strtol(optarg, NULL, 0);
			break;
		case 'o':
			user_param.out_reads = strtol(optarg, NULL, 0);
			break;
		case 'a':
			all = ALL;
			break;
		case 'V':
			printf("perftest version : %.2f\n",VERSION);
			return 0;
			break;
		case 'd':
			ib_devname = strdupa(optarg);
			break;

		case 'i':
			user_param.ib_port = strtol(optarg, NULL, 0);
			if (user_param.ib_port < 0) {
				usage(argv[0]);
				return 2;
			}
			break;

		case 's':
			size = strtol(optarg, NULL, 0);
			if (size < 1) {
				usage(argv[0]); return 3;
			}
			break;

		case 't':
			user_param.tx_depth = strtol(optarg, NULL, 0);
			if (user_param.tx_depth < 1) {
				usage(argv[0]); return 4;
			}
			break;

		case 'n':
			user_param.iters = strtol(optarg, NULL, 0);
			if (user_param.iters < 2) {
				usage(argv[0]);
				return 5;
			}

			break;

		case 'C':
			report.cycles = 1;
			break;

		case 'H':
			report.histogram = 1;
			break;

		case 'U':
			report.unsorted = 1;
			break;

		case 'F':
			no_cpu_freq_fail = 1;
			break;

		case 'u':
			user_param.qp_timeout = strtol(optarg, NULL, 0);
			break;

		case 'S':
			sl = strtol(optarg, NULL, 0);
			if (sl > 15) { usage(argv[0]); return 5; }
			break;

		case 'x':
			user_param.gid_index = strtol(optarg, NULL, 0);
			if (user_param.gid_index > 63) {
				usage(argv[0]);
				return 1;
			}
			break;

		default:
			usage(argv[0]);
			return 6;
		}
	}

	if (optind == argc - 1)
		servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 6;
	}

	ALLOCATE(tstamp,cycles_t,user_param.iters);
	user_param.machine = servername ? CLIENT : SERVER;
	
	printf(RESULT_LINE);
	printf("                    RDMA_Read Latency Test\n");

	if (user_param.use_event) 
		printf(" Test with events.\n");

	printf(" Connection type : RC\n");
	
	if (all == ALL) {
		/*since we run all sizes */
		size = 8388608; /*2^23 */
	} 

	srand48(getpid() * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);

	ib_dev = ctx_find_dev(ib_devname);
	if (!ib_dev)
		return 7;

	ctx = pp_init_ctx(ib_dev,size,&user_param);
	if (!ctx)
		return 8;

	// Set up the Connection.
	if (set_up_connection(ctx,&user_param,&my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}	

	// Init the connection and print the local data.
	if (init_connection(&user_param,&my_dest,servername)) {
		fprintf(stderr," Unable to init the socket connection\n");
		return 1;
	}

	// shaking hands and gather the other side info.
    if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
        
    }
	user_param.side = REMOTE;
	ctx_print_pingpong_data(&rem_dest,&user_param);

	if (pp_connect_ctx(ctx,my_dest.psn,&rem_dest,my_dest.out_reads,&user_param)) {
		fprintf(stderr," Unable to Connect the HCA's through the link\n");
		return 1;
	}

	// shaking hands and gather the other side info.
    if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
        
    }

	// Only Client post read request. 
	if (user_param.machine == SERVER) {
		if (ctx_close_connection(&user_param,&my_dest,&rem_dest)) {
			fprintf(stderr,"Failed to close connection between server and client\n");
			return 1;
		}
		printf(RESULT_LINE);
		return destroy_ctx_resources(ctx);

	} 

	if (user_param.use_event) {
		if (ibv_req_notify_cq(ctx->cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		} 
	}

	printf(RESULT_LINE);
	printf(RESULT_FMT_LAT);

	if (all == ALL) {
		for (i = 1; i < 24 ; ++i) {
			size = 1 << i;
			if(run_iter(ctx, &user_param, &rem_dest, size))
				return 17;
	    	
			print_report(&report,user_param.iters, tstamp, size, no_cpu_freq_fail);
		}
	} else {
		if(run_iter(ctx, &user_param, &rem_dest, size))
			return 18;
		
		print_report(&report, user_param.iters, tstamp, size, no_cpu_freq_fail);
	}

	if (ctx_close_connection(&user_param,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}

	printf(RESULT_LINE);

	return destroy_ctx_resources(ctx);;
}
