/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
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
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "perftest_communication.h"

#define VERSION 2.6

cycles_t	*tposted;
cycles_t	*tcompleted;

/****************************************************************************** 
 *
 ******************************************************************************/
static int pp_connect_ctx(struct pingpong_context *ctx,int my_psn,
						  struct pingpong_dest *dest, 
						  struct perftest_parameters *user_parm, int qpindex)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof attr);

	attr.qp_state 		= IBV_QPS_RTR;
	attr.path_mtu       = user_parm->curr_mtu;
	attr.dest_qp_num 	= dest->qpn;
	attr.rq_psn 		= dest->psn;
	attr.ah_attr.dlid   = dest->lid;
	if (user_parm->connection_type==RC) {
		attr.max_dest_rd_atomic     = 1;
		attr.min_rnr_timer          = 12;
	}
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
	if (user_parm->connection_type == RC) {
		if (ibv_modify_qp(ctx->qp[qpindex], &attr,
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
	} else {
		if (ibv_modify_qp(ctx->qp[qpindex], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_AV                 |
				  IBV_QP_PATH_MTU           |
				  IBV_QP_DEST_QPN           |
				  IBV_QP_RQ_PSN)) {
			fprintf(stderr, "Failed to modify UC QP to RTR\n");
			return 1;
		}

	}
	attr.qp_state 	    = IBV_QPS_RTS;
	attr.sq_psn 	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (user_parm->connection_type == 0) {
		attr.max_rd_atomic  = 1;
		if (ibv_modify_qp(ctx->qp[qpindex], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_SQ_PSN             |
				  IBV_QP_TIMEOUT            |
				  IBV_QP_RETRY_CNT          |
				  IBV_QP_RNR_RETRY          |
				  IBV_QP_MAX_QP_RD_ATOMIC)) {
			fprintf(stderr, "Failed to modify RC QP to RTS\n");
			return 1;
		}
	} else {
		if (ibv_modify_qp(ctx->qp[qpindex], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_SQ_PSN)) {
			fprintf(stderr, "Failed to modify UC QP to RTS\n");
			return 1;
		}

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
	int iters = user_param->iters;


	opt_delta = tcompleted[opt_posted] - tposted[opt_completed];

	if (user_param->noPeak == OFF) {
		/* Find the peak bandwidth unless asked not to in command line*/
		for (i = 0; i < iters * user_param->num_of_qps; ++i)
		  for (j = i; j < iters * user_param->num_of_qps; ++j) {
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
	printf(REPORT_FMT,(unsigned long)user_param->size, iters,!(user_param->noPeak) * tsize * cycles_to_units / opt_delta / 0x100000,
	       tsize*iters*user_param->num_of_qps*cycles_to_units/(tcompleted[(iters*user_param->num_of_qps) - 1] - tposted[0]) / 0x100000);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest)	{

    int                totscnt = 0;
	int 			   totccnt = 0;
	int                i       = 0;
    int                index,ne;
	int				   warmindex;
    struct ibv_send_wr *bad_wr;
    struct ibv_wc 	   *wc       = NULL;
	struct ibv_sge     *sge_list = NULL;
    struct ibv_send_wr *wr       = NULL;
	uint64_t		   *my_addr  = NULL;
	uint64_t		   *rem_addr = NULL;

	ALLOCATE(wr ,struct ibv_send_wr , user_param->num_of_qps);
	ALLOCATE(sge_list ,struct ibv_sge , user_param->num_of_qps);
	ALLOCATE(my_addr ,uint64_t ,user_param->num_of_qps);
	ALLOCATE(rem_addr ,uint64_t ,user_param->num_of_qps);
	ALLOCATE(wc ,struct ibv_wc , DEF_WC_SIZE);


	// Each QP has its own wr and sge , that holds the qp addresses and attr.
	// We write in cycles on the buffer to exploid the "Nahalem" system.
	for (index = 0 ; index < user_param->num_of_qps ; index++) {

		sge_list[index].addr   = (uintptr_t)ctx->buf + (index*BUFF_SIZE(ctx->size));
		sge_list[index].length = user_param->size;
		sge_list[index].lkey   = ctx->mr->lkey;

		wr[index].sg_list             = &sge_list[index]; 
		wr[index].num_sge             = MAX_SEND_SGE;
		wr[index].opcode	          = IBV_WR_RDMA_WRITE;
		wr[index].next                = NULL;
		wr[index].wr.rdma.remote_addr = rem_dest[index].vaddr;
		wr[index].wr.rdma.rkey        = rem_dest[index].rkey;
		wr[index].wr_id               = index;
		wr[index].send_flags          = IBV_SEND_SIGNALED;

		if (user_param->size <= user_param->inline_size) 
			wr[index].send_flags |= IBV_SEND_INLINE;

		ctx->scnt[index] = 0;
		ctx->ccnt[index] = 0;
		my_addr[index]	 = sge_list[index].addr;
		rem_addr[index]  = wr[index].wr.rdma.remote_addr;

	}
	
	for (warmindex = 0 ;warmindex < user_param->tx_depth ;warmindex ++ ) {
	  for (index =0 ; index < user_param->num_of_qps ; index++) {

			if (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)
				wr[index].send_flags &= ~IBV_SEND_SIGNALED;

			tposted[totscnt] = get_cycles();
            if (ibv_post_send(ctx->qp[index],&wr[index],&bad_wr)) {
                fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
                return 1;
            }
			// If we can increase the remote address , so the next write will be to other address ,
			// We do it.
			if (user_param->size <= (CYCLE_BUFFER / 2)) { 
				increase_rem_addr(&wr[index],user_param->size,ctx->scnt[index],rem_addr[index],WRITE);
				increase_loc_addr(wr[index].sg_list,user_param->size,ctx->scnt[index],my_addr[index],0);
			}


			ctx->scnt[index]++;
            totscnt++;

			if (ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 || ctx->scnt[index] == user_param->iters - 1)
				wr[index].send_flags |= IBV_SEND_SIGNALED;

      }
	}  

	// main loop for posting 
	while (totscnt < (user_param->iters * user_param->num_of_qps)  || totccnt < (user_param->iters * user_param->num_of_qps) ) {

		// main loop to run over all the qps and post each time n messages 
		for (index =0 ; index < user_param->num_of_qps ; index++) {
          
			while (ctx->scnt[index] < user_param->iters && (ctx->scnt[index] - ctx->ccnt[index]) < user_param->tx_depth) {

				if (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)
					wr[index].send_flags &= ~IBV_SEND_SIGNALED;

				tposted[totscnt] = get_cycles();
				if (ibv_post_send(ctx->qp[index],&wr[index],&bad_wr)) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}     
				
				if (user_param->size <= (CYCLE_BUFFER / 2)) { 
					increase_rem_addr(&wr[index],user_param->size,ctx->scnt[index],rem_addr[index],WRITE);
					increase_loc_addr(wr[index].sg_list,user_param->size,ctx->scnt[index],my_addr[index],0);
				}

				ctx->scnt[index]++;
				totscnt++;

				if (ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 || ctx->scnt[index] == user_param->iters - 1)
					wr[index].send_flags |= IBV_SEND_SIGNALED;
			}
		}

		// finished posting now polling 
		if (totccnt < (user_param->iters * user_param->num_of_qps) ) {
	    
			do {
				ne = ibv_poll_cq(ctx->send_cq, DEF_WC_SIZE, wc);
				if (ne > 0) {
					for (i = 0; i < ne; i++) {

						if (wc[i].status != IBV_WC_SUCCESS) 
							NOTIFY_COMP_ERROR_SEND(wc[i],totscnt,totccnt);

						ctx->ccnt[(int)wc[i].wr_id] += user_param->cq_mod;
						totccnt += user_param->cq_mod;

						if (totccnt >= user_param->iters*user_param->num_of_qps - 1)
							tcompleted[user_param->iters*user_param->num_of_qps - 1] = get_cycles();

						else 
							tcompleted[totccnt-1] = get_cycles();
					}
				}
			} while (ne > 0);

			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}
	}

	free(wr);
	free(sge_list);
	free(my_addr);
	free(rem_addr);
	free(wc);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	int                         i = 0;
	struct ibv_device		    *ib_dev = NULL;
	struct pingpong_context     ctx;
	struct pingpong_dest        *my_dest,*rem_dest;
	struct perftest_parameters  user_param;
	struct perftest_comm		user_comm;

	/* init default values to user's parameters */
	memset(&user_param,0,sizeof(struct perftest_parameters));
	memset(&user_comm,0,sizeof(struct perftest_comm));
	memset(&ctx,0,sizeof(struct pingpong_context));
	
	user_param.verb    = WRITE;
	user_param.tst     = BW;
	user_param.version = VERSION;

	// Configure the parameters values according to user arguments or defalut values.
	if (parser(&user_param,argv,argc)) {
		fprintf(stderr," Parser function exited with Error\n");
		return 1;
	}

	// Finding the IB device selected (or defalut if no selected).
	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE deivce\n");
		return 1;
	}

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

	ALLOCATE(my_dest , struct pingpong_dest , user_param.num_of_qps);
	memset(my_dest, 0, sizeof(struct pingpong_dest)*user_param.num_of_qps);
	ALLOCATE(rem_dest , struct pingpong_dest , user_param.num_of_qps);
	memset(rem_dest, 0, sizeof(struct pingpong_dest)*user_param.num_of_qps);

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
    
	    // create all the basic IB resources (data buffer, PD, MR, CQ and events channel)
	    if (ctx_init(&ctx,&user_param)) {
			fprintf(stderr, " Couldn't create IB resources\n");
			return FAILURE;
	    }
	}

	// Set up the Connection.
	if (set_up_connection(&ctx,&user_param,my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return FAILURE;
	}

	// Print this machine QP information
	for (i=0; i < user_param.num_of_qps; i++) 
		ctx_print_pingpong_data(&my_dest[i],&user_comm);

	// Init the connection and print the local data.
	if (establish_connection(&user_comm)) {
		fprintf(stderr," Unable to init the socket connection\n");
		return FAILURE;
	}

	// shaking hands and gather the other side info.
	for (i=0; i < user_param.num_of_qps; i++) {

			if (ctx_hand_shake(&user_comm,&my_dest[i],&rem_dest[i])) {
				fprintf(stderr," Failed to exchange date between server and clients\n");
				return 1;   
			}

			// Print remote machine QP information
			user_comm.rdma_params->side = REMOTE;
			ctx_print_pingpong_data(&rem_dest[i],&user_comm);

			if (user_param.work_rdma_cm == OFF) {

				if (pp_connect_ctx(&ctx,my_dest[i].psn,&rem_dest[i],&user_param,i)) {
					fprintf(stderr," Unable to Connect the HCA's through the link\n");
					return FAILURE;
				}
			}

			// An additional handshake is required after moving qp to RTR.
			if (ctx_hand_shake(&user_comm,&my_dest[i],&rem_dest[i])) {
				fprintf(stderr," Failed to exchange date between server and clients\n");
				return FAILURE; 
			}
	}	

	printf(RESULT_LINE);
	printf(RESULT_FMT);

	// For half duplex tests, server just waits for client to exit 
	if (user_param.machine == SERVER && !user_param.duplex) {
		
		if (ctx_close_connection(&user_comm,&my_dest[0],&rem_dest[0])) {
			fprintf(stderr,"Failed to close connection between server and client\n");
			return 1;
		}
		printf(RESULT_LINE);
		return destroy_ctx(&ctx,&user_param);
	}

	ALLOCATE(tposted,cycles_t,user_param.iters*user_param.num_of_qps);
	ALLOCATE(tcompleted,cycles_t,user_param.iters*user_param.num_of_qps);

	if (user_param.all == ON) {

		for (i = 1; i < 24 ; ++i) {
			user_param.size = 1 << i;
			if(run_iter(&ctx,&user_param,rem_dest))
				return 17;
			print_report(&user_param);
		}

	} else {

		if(run_iter(&ctx,&user_param,rem_dest))
			return 18;
		print_report(&user_param);
	}

	free(tposted);
	free(tcompleted);

	// Closing connection.
	if (ctx_close_connection(&user_comm,&my_dest[0],&rem_dest[0])) {
	 	fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}

	free(my_dest);
	free(rem_dest);
	printf(RESULT_LINE);
	return destroy_ctx(&ctx,&user_param);
}
