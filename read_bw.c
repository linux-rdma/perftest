/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2006 Mellanox Technologies Ltd.  All rights reserved.
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
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
// #include <limits.h>
#include <malloc.h>
// #include <getopt.h>
#include <time.h>
#include <infiniband/verbs.h>

#include "get_clock.h"
#include "perftest_resources.h"

#define VERSION 2.1

static int page_size;
cycles_t	*tposted;
cycles_t	*tcompleted;

struct pingpong_context {
	struct ibv_context *context;
	struct ibv_comp_channel *channel;
	struct ibv_pd      *pd;
	struct ibv_mr      *mr;
	struct ibv_cq      *cq;
	struct ibv_qp      *qp;
	void               *buf;
	unsigned            size;
	int                 tx_depth;
	struct ibv_sge      list;
	struct ibv_send_wr  wr;
};

/****************************************************************************** 
 *
 ******************************************************************************/
static int set_up_connection(struct pingpong_context *ctx,
							 struct perftest_parameters *user_parm,
							 struct pingpong_dest *my_dest) {

	if (user_parm->gid_index != -1) {
		if (ibv_query_gid(ctx->context,user_parm->ib_port,user_parm->gid_index,&my_dest->gid)) {
			return -1;
		}
	}
	
	my_dest->lid   	   = ctx_get_local_lid(ctx->context,user_parm->ib_port);
	my_dest->out_reads = ctx_set_out_reads(ctx->context,user_parm->out_reads);
	my_dest->qpn   	   = ctx->qp->qp_num;
	my_dest->psn       = lrand48() & 0xffffff;
	my_dest->rkey      = ctx->mr->rkey;
	my_dest->vaddr     = (uintptr_t)ctx->buf + BUFF_SIZE(user_parm->size);

	// We do not fail test upon lid above RoCE.
	if (user_parm->gid_index == -1) {
		if (!my_dest->lid) {
			fprintf(stderr," Local lid 0x0 detected,without any use of gid. Is SM running?\n");
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

	params->side = LOCAL;

	ctx_print_pingpong_data(my_dest,params);
	
	if (params->machine == CLIENT) 
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
	free(tposted);
    free(tcompleted);

	return test_result;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
											struct perftest_parameters *user_parm) {	

	struct pingpong_context *ctx;

	ALLOCATE(ctx,struct pingpong_context,1);

	ctx->buf = memalign(page_size, BUFF_SIZE(user_parm->size) * 2);
	if (!ctx->buf) {
		fprintf(stderr, " Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0, BUFF_SIZE(user_parm->size) * 2);

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

	// We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
	// The Consumer is not allowed to assign Remote Write or Remote Atomic to
	// a Memory Region that has not been assigned Local Write.
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, BUFF_SIZE(user_parm->size) * 2, 
						 IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
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
static int pp_connect_ctx(struct pingpong_context *ctx,int my_psn,int my_out_reads,
						  struct pingpong_dest *dest,struct perftest_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof attr);

	attr.qp_state 		= IBV_QPS_RTR;
	attr.path_mtu       = user_parm->curr_mtu;
	attr.dest_qp_num 	= dest->qpn;
	attr.rq_psn 		= dest->psn;
	attr.ah_attr.dlid       = dest->lid;	
	attr.max_dest_rd_atomic     = my_out_reads;
	attr.min_rnr_timer          = 12;
	if (user_parm->gid_index<0) {
		attr.ah_attr.is_global  = 0;
		attr.ah_attr.sl         = user_parm->sl;
	} else {
		attr.ah_attr.is_global  = 1;
		attr.ah_attr.grh.dgid   = dest->gid;
		attr.ah_attr.grh.sgid_index = user_parm->gid_index;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.sl         = 0;
	}
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num   = user_parm->ib_port;
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
	attr.qp_state 	    = IBV_QPS_RTS;
	attr.sq_psn 	    = my_psn;
	attr.max_rd_atomic  = dest->out_reads;
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
static void print_report(struct perftest_parameters *user_param) {

	double cycles_to_units;
	unsigned long tsize;	/* Transferred size, in megabytes */
	int i, j;
	int opt_posted = 0, opt_completed = 0;
	cycles_t opt_delta;
	cycles_t t;


	opt_delta = tcompleted[opt_posted] - tposted[opt_completed];
	
	if (user_param->noPeak == OFF) {
		/* Find the peak bandwidth */
		for (i = 0; i < user_param->iters; ++i)
			for (j = i; j < user_param->iters; ++j) {
				t = (tcompleted[j] - tposted[i]) / (j - i + 1);
				if (t < opt_delta) {
					opt_delta  = t;
					opt_posted = i;
					opt_completed = j;
				}
			}
	}

	cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f) * 1000000;
	tsize = user_param->duplex ? 2 : 1;
	tsize = tsize * user_param->size;
	
	printf(REPORT_FMT,(unsigned long)user_param->size,user_param->iters,!(user_param->noPeak) * tsize * cycles_to_units / opt_delta / 0x100000,
	       tsize * user_param->iters * cycles_to_units /(tcompleted[user_param->iters - 1] - tposted[0]) / 0x100000);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest)
{
	
	int scnt = 0;
	int ccnt = 0;
	int i,ne;
	uint64_t rem_addr, my_addr;
	struct ibv_wc      *wc     = NULL;
	struct ibv_send_wr *bad_wr = NULL;

	ALLOCATE(wc , struct ibv_wc , DEF_WC_SIZE);

	ctx->list.addr   = (uintptr_t)ctx->buf;
	ctx->list.length = user_param->size;
	ctx->list.lkey   = ctx->mr->lkey;

	ctx->wr.sg_list             = &ctx->list;
	ctx->wr.wr.rdma.remote_addr = rem_dest->vaddr;
	ctx->wr.wr.rdma.rkey        = rem_dest->rkey;
	ctx->wr.wr_id      		    = PINGPONG_READ_WRID;
	ctx->wr.num_sge             = MAX_RECV_SGE;
	ctx->wr.opcode              = IBV_WR_RDMA_READ;
	ctx->wr.send_flags          = IBV_SEND_SIGNALED;
	ctx->wr.next                = NULL;

	my_addr  = ctx->list.addr;
	rem_addr = ctx->wr.wr.rdma.remote_addr;
	
	while (scnt < user_param->iters || ccnt < user_param->iters) {

		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth ) {

			if (scnt%user_param->cq_mod == 0 && user_param->cq_mod > 1)
			    ctx->wr.send_flags  &= ~IBV_SEND_SIGNALED;

			tposted[scnt] = get_cycles();
			if (ibv_post_send(ctx->qp, &ctx->wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}

			if (user_param->size <= (CYCLE_BUFFER / 2)) { 
				increase_rem_addr(&ctx->wr,user_param->size,scnt,rem_addr);
				increase_loc_addr(&ctx->list,user_param->size,scnt,my_addr,0);
			}
			++scnt;

			if (scnt%user_param->cq_mod == user_param->cq_mod - 1 || scnt == user_param->iters - 1)
				ctx->wr.send_flags |= IBV_SEND_SIGNALED;
		}

		if (ccnt < user_param->iters) {

			if (user_param->use_event) {
				if (ctx_notify_events(ctx->cq,ctx->channel)) {
					fprintf(stderr, "Couldn't request CQ notification\n");
					return 1;
				}
			}

			do {
				ne = ibv_poll_cq(ctx->cq,DEF_WC_SIZE,wc);
				if (ne > 0) {
					for (i = 0; i < ne; i++) {

						if (wc[i].status != IBV_WC_SUCCESS) 
							NOTIFY_COMP_ERROR_SEND(wc[i],scnt,ccnt);

						ccnt+=user_param->cq_mod;

						if (ccnt >= user_param->iters - 1)
						    tcompleted[user_param->iters - 1] =  get_cycles();

						else 
						    tcompleted[ccnt - 1] = get_cycles();
					}
				}
			} while (ne > 0 );

			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}
	}
	free(wc);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	int                        i = 0;
	struct ibv_device		   *ib_dev = NULL;
	struct pingpong_context    *ctx;
	struct pingpong_dest       my_dest,rem_dest;
	struct perftest_parameters user_param;
	

	/* init default values to user's parameters */
	memset(&user_param , 0 , sizeof(struct perftest_parameters));
	memset(&my_dest , 0 ,  sizeof(struct pingpong_dest));
	memset(&rem_dest , 0 ,  sizeof(struct pingpong_dest));

	
	user_param.verb    = READ;
	user_param.tst     = BW;
	user_param.version = VERSION;

	if (parser(&user_param,argv,argc)) 
		return 1;
	
	// Print basic test information.
	ctx_print_test_info(&user_param);


	// Done with parameter parsing. Perform setup. 
	if (user_param.all == ON)
		user_param.size = MAX_SIZE;

	srand48(getpid() * time(NULL));

	page_size = sysconf(_SC_PAGESIZE);

	ib_dev =ctx_find_dev(user_param.ib_devname);
	if (!ib_dev)
		return 7;

	ctx = pp_init_ctx(ib_dev,&user_param);
	if (!ctx)
		return 1;

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

	if (pp_connect_ctx(ctx,my_dest.psn,my_dest.out_reads,&rem_dest,&user_param)) {
		fprintf(stderr," Unable to Connect the HCA's through the link\n");
		return 1;
	}

	// An additional handshake is required after moving qp to RTR.
	if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
        
    }
     
	// For half duplex tests, server just waits for client to exit 
	if (user_param.machine == SERVER && !user_param.duplex) {
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
	printf(RESULT_FMT);

	ALLOCATE(tposted , cycles_t , user_param.iters);
	ALLOCATE(tcompleted , cycles_t , user_param.iters);

	if (user_param.all == ON) {

		for (i = 1; i < 24 ; ++i) {
			user_param.size = 1 << i;
			if(run_iter(ctx,&user_param,&rem_dest))
				return 17;
			print_report(&user_param);
		}

	} 

	else {

		if(run_iter(ctx,&user_param,&rem_dest))
			return 17;
		
		print_report(&user_param);
	}

	if (ctx_close_connection(&user_param,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}
	
	printf(RESULT_LINE);

	return destroy_ctx_resources(ctx);
	
}
