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

#define VERSION 2.1

static int page_size;

cycles_t *tstamp;

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
						   struct pingpong_dest *my_dest) {

	params->side      = LOCAL;
	ctx_print_pingpong_data(my_dest,params);

	if (params->servername) 
		params->sockfd = ctx_client_connect(params->servername,params->port);
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
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
										    struct perftest_parameters *user_parm) {

	struct pingpong_context *ctx;

	ALLOCATE(ctx,struct pingpong_context,1);

	ctx->buf = memalign(page_size, BUFF_SIZE(user_parm->size));
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0, BUFF_SIZE(user_parm->size));

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

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf,BUFF_SIZE(user_parm->size),
						 IBV_ACCESS_REMOTE_WRITE | 
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
		attr.ah_attr.sl             = user_parm->sl;
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
static void print_report(struct perftest_parameters *user_param) {

	double cycles_to_units;
	cycles_t median;
	unsigned int i;
	const char* units;
	cycles_t *delta = malloc((user_param->iters - 1) * sizeof *delta);

	if (!delta) {
		perror("malloc");
		return;
	}

	for (i = 0; i < user_param->iters - 1; ++i)
		delta[i] = tstamp[i + 1] - tstamp[i];


	if (user_param->r_flag->cycles) {
		cycles_to_units = 1;
		units = "cycles";
	} else {
		cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);
		units = "usec";
	}

	if (user_param->r_flag->unsorted) {
		printf("#, %s\n", units);
		for (i = 0; i < user_param->iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units );
	}

	qsort(delta, user_param->iters - 1, sizeof *delta, cycles_compare);

	if (user_param->r_flag->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < user_param->iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units );
	}

	median = get_median(user_param->iters - 1, delta);
	printf(REPORT_FMT_LAT,(unsigned long)user_param->size,user_param->iters,delta[0] / cycles_to_units ,
	       delta[user_param->iters - 2] / cycles_to_units ,median / cycles_to_units );

	free(delta);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest) {

	int scnt = 0;
	int ne;
	struct ibv_sge 		list;
	struct ibv_send_wr 	wr;
	struct ibv_send_wr  *bad_wr;
	struct ibv_wc       wc;
	uint64_t            my_addr,rem_addr;


	list.addr   = (uintptr_t)ctx->buf;
	list.length = user_param->size;
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

		if (user_param->size <= (CYCLE_BUFFER / 2)) { 
			increase_rem_addr(&wr,user_param->size,scnt,rem_addr);
			increase_loc_addr(&list,user_param->size,scnt,my_addr,0);
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

	int                         i = 0;
	struct report_options       report = {};
	struct pingpong_context     *ctx;
	struct ibv_device           *ib_dev;
	struct perftest_parameters  user_param;
	struct pingpong_dest	    my_dest,rem_dest;
	
	/* init default values to user's parameters */
	memset(&user_param,0,sizeof(struct perftest_parameters));
	memset(&my_dest,0,sizeof(struct pingpong_dest));
	memset(&rem_dest,0,sizeof(struct pingpong_dest));

	user_param.verb    = READ;
	user_param.tst     = LAT;
	user_param.r_flag  = &report;
	user_param.version = VERSION;

	if (parser(&user_param,argv,argc)) 
		return 1;

	// Print basic test information.
	ctx_print_test_info(&user_param);
	
	if (user_param.all == ON) 
		user_param.size = MAX_SIZE; 

	srand48(getpid() * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);

	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev)
		return 7;

	ctx = pp_init_ctx(ib_dev,&user_param);
	if (!ctx)
		return 8;

	// Set up the Connection.
	if (set_up_connection(ctx,&user_param,&my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}	

	// Init the connection and print the local data.
	if (init_connection(&user_param,&my_dest)) {
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

	ALLOCATE(tstamp,cycles_t,user_param.iters);

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

	if (user_param.all == ON) {
		for (i = 1; i < 24 ; ++i) {
			user_param.size = 1 << i;
			if(run_iter(ctx,&user_param,&rem_dest))
				return 17;
	    	
			print_report(&user_param);
		}
	} else {
		if(run_iter(ctx,&user_param,&rem_dest))
			return 18;
		
		print_report(&user_param);
	}

	if (ctx_close_connection(&user_param,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}

	printf(RESULT_LINE);

	return destroy_ctx_resources(ctx);;
}
