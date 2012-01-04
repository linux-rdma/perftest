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
#include <malloc.h>

#include "get_clock.h"
#include "perftest_resources.h"
#include "perftest_parameters.h"
#include "perftest_communication.h"

#define VERSION 2.4

cycles_t	*tposted;
cycles_t	*tcompleted;

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
	if (ibv_modify_qp(ctx->qp[0], &attr,
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
	if (ibv_modify_qp(ctx->qp[0], &attr,
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
	struct ibv_sge     	list;
	struct ibv_send_wr 	wr;
	struct ibv_wc       *wc     = NULL;
	struct ibv_send_wr  *bad_wr = NULL;

	ALLOCATE(wc , struct ibv_wc , DEF_WC_SIZE);

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
	
	while (scnt < user_param->iters || ccnt < user_param->iters) {

		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth ) {

			if (scnt%user_param->cq_mod == 0 && user_param->cq_mod > 1)
			    wr.send_flags  &= ~IBV_SEND_SIGNALED;

			tposted[scnt] = get_cycles();
			if (ibv_post_send(ctx->qp[0], &wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}

			if (user_param->size <= (CYCLE_BUFFER / 2)) { 
				increase_rem_addr(&wr,user_param->size,scnt,rem_addr,READ);
				increase_loc_addr(&list,user_param->size,scnt,my_addr,0);
			}
			++scnt;

			if (scnt%user_param->cq_mod == user_param->cq_mod - 1 || scnt == user_param->iters - 1)
				wr.send_flags |= IBV_SEND_SIGNALED;
		}

		if (ccnt < user_param->iters) {

			if (user_param->use_event) {
				if (ctx_notify_events(ctx->channel)) {
					fprintf(stderr, "Couldn't request CQ notification\n");
					return 1;
				}
			}

			do {
				ne = ibv_poll_cq(ctx->send_cq,DEF_WC_SIZE,wc);
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
			} while (ne > 0);

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
	struct pingpong_context    ctx;
	struct pingpong_dest       my_dest,rem_dest;
	struct perftest_parameters user_param;
	struct perftest_comm	   user_comm;
	
	/* init default values to user's parameters */
	memset(&ctx,0,sizeof(struct pingpong_context));
	memset(&user_param , 0 , sizeof(struct perftest_parameters));
	memset(&user_comm,0,sizeof(struct perftest_comm));
	memset(&my_dest , 0 ,  sizeof(struct pingpong_dest));
	memset(&rem_dest , 0 ,  sizeof(struct pingpong_dest));
	
	user_param.verb    = READ;
	user_param.tst     = BW;
	user_param.version = VERSION;

	if (parser(&user_param,argv,argc)) 
		return 1;

	ib_dev =ctx_find_dev(user_param.ib_devname);
	if (!ib_dev)
		return 7;

	// Getting the relevant context from the device
	ctx.context = ibv_open_device(ib_dev);
	if (!ctx.context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		return 1;
	}

	// See if MTU and link type are valid and supported.
	if (check_link_and_mtu(ctx.context,&user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		return FAILURE;
	}

	// Print basic test information.
	ctx_print_test_info(&user_param);

	// copy the rellevant user parameters to the comm struct + creating rdma_cm resources.
	if (create_comm_struct(&user_comm,&user_param)) { 
		fprintf(stderr," Unable to create RDMA_CM resources\n");
		return 1;
	}

	// Create (if nessacery) the rdma_cm ids and channel.
	if (user_param.work_rdma_cm == ON) {

	    if (create_rdma_resources(&ctx,&user_param)) {
			fprintf(stderr," Unable to create the rdma_resources\n");
			return FAILURE;
	    }
		
  	    if (user_param.machine == CLIENT) {

			if (rdma_client_connect(&ctx,&user_param)) {
				fprintf(stderr,"Unable to perform rdma_client function\n");
				return FAILURE;
			}
		
		} else {

			if (rdma_server_connect(&ctx,&user_param)) {
				fprintf(stderr,"Unable to perform rdma_client function\n");
				return FAILURE;
			}
		}

	} else {

		// create all the basic IB resources.
	    if (ctx_init(&ctx,&user_param)) {
			fprintf(stderr, " Couldn't create IB resources\n");
			return FAILURE;
	    }
	}

	// Set up the Connection.
	if (set_up_connection(&ctx,&user_param,&my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return FAILURE;
	}

	ctx_print_pingpong_data(&my_dest,&user_comm);

	// Init the connection and print the local data.
	if (establish_connection(&user_comm)) {
		fprintf(stderr," Unable to init the socket connection\n");
		return 1;
	}

	// shaking hands and gather the other side info.
	if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to exchange date between server and clients\n");
		return 1;
	}

	user_comm.rdma_params->side = REMOTE;
	ctx_print_pingpong_data(&rem_dest,&user_comm);

	if (user_param.work_rdma_cm == OFF) {

		if (pp_connect_ctx(&ctx,my_dest.psn,my_dest.out_reads,&rem_dest,&user_param)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			return 1;
		}
	}

	// An additional handshake is required after moving qp to RTR.
	if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;    
    }
     
	// For half duplex tests, server just waits for client to exit 
	if (user_param.machine == SERVER && !user_param.duplex) {

		if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
			fprintf(stderr,"Failed to close connection between server and client\n");
			return 1;
		}

		printf(RESULT_LINE);
		return destroy_ctx(&ctx,&user_param);

	} 

	if (user_param.use_event) {
		if (ibv_req_notify_cq(ctx.send_cq, 0)) {
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
			if(run_iter(&ctx,&user_param,&rem_dest))
				return 17;
			print_report(&user_param);
		}

	} 

	else {

		if(run_iter(&ctx,&user_param,&rem_dest))
			return 17;
		
		print_report(&user_param);
	}

	if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}
	
	printf(RESULT_LINE);

	return destroy_ctx(&ctx,&user_param);	
}
