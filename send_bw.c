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
#include "multicast_resources.h"
#include "perftest_communication.h"

#define VERSION 2.4

cycles_t	*tposted;
cycles_t	*tcompleted;

/****************************************************************************** 
 *
 ******************************************************************************/
static int set_mcast_group(struct pingpong_context *ctx,
						   struct perftest_parameters *user_parm,
						   struct mcast_parameters *mcg_params) {

	struct ibv_port_attr port_attr;

	if (ibv_query_gid(ctx->context,user_parm->ib_port,user_parm->gid_index,&mcg_params->port_gid)) {
			return 1;
	}
		
	if (ibv_query_pkey(ctx->context,user_parm->ib_port,DEF_PKEY_IDX,&mcg_params->pkey)) {
		return 1;
	}

	if (ibv_query_port(ctx->context,user_parm->ib_port,&port_attr)) {
		return 1;
	}
	mcg_params->sm_lid  = port_attr.sm_lid;
	mcg_params->sm_sl   = port_attr.sm_sl;
	mcg_params->ib_port = user_parm->ib_port;
	
	if (!strcmp(link_layer_str(user_parm->link_type),"IB")) {
		// Request for Mcast group create registery in SM.
		if (join_multicast_group(SUBN_ADM_METHOD_SET,mcg_params)) {
			fprintf(stderr,"Couldn't Register the Mcast group on the SM\n");
			return 1;
		}
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int send_set_up_connection(struct pingpong_context *ctx,
								  struct perftest_parameters *user_parm,
								  struct pingpong_dest *my_dest,
								  struct mcast_parameters *mcg_params,
								  struct perftest_comm *comm) {

	int i = (user_parm->duplex) ? 1 : 0;

	if (user_parm->use_mcg && (user_parm->duplex || user_parm->machine == SERVER)) {

		mcg_params->user_mgid = user_parm->user_mgid;
		set_multicast_gid(mcg_params,ctx->qp[0]->qp_num,(int)user_parm->machine);
		if (set_mcast_group(ctx,user_parm,mcg_params)) {
			return 1;
		}
		
		while (i < user_parm->num_of_qps) {
			if (ibv_attach_mcast(ctx->qp[i],&mcg_params->mgid,mcg_params->mlid)) {
				fprintf(stderr, "Couldn't attach QP to MultiCast group");
				return 1;
			}
			i++;
		}

		mcg_params->mcast_state |= MCAST_IS_ATTACHED;
		my_dest->gid = mcg_params->mgid;
		my_dest->lid = mcg_params->mlid;
		my_dest->qpn = QPNUM_MCAST;

	} else {
		if (user_parm->gid_index != -1) {
			if (ibv_query_gid(ctx->context,user_parm->ib_port,user_parm->gid_index,&my_dest->gid)) {
				return -1;
			}
		}
		my_dest->lid = ctx_get_local_lid(ctx->context,user_parm->ib_port);
		my_dest->qpn = ctx->qp[0]->qp_num;
	}
	my_dest->psn  = lrand48() & 0xffffff;

	// We do not fail test upon lid above RoCE.

	if (user_parm->gid_index < 0) {
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
static int pp_connect_ctx(struct pingpong_context *ctx,int my_psn,
			              struct pingpong_dest *dest, 
						  struct perftest_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof attr);
	int i;

	attr.qp_state 		= IBV_QPS_RTR;
	attr.path_mtu       = user_parm->curr_mtu;
    attr.dest_qp_num    = dest->qpn;
	attr.rq_psn         = dest->psn;
	attr.ah_attr.dlid   = dest->lid;
	if (user_parm->connection_type == RC) {
		attr.max_dest_rd_atomic     = 1;
		attr.min_rnr_timer          = 12;
	}
	if (user_parm->gid_index < 0) {
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
	} else if (user_parm->connection_type == UC) {
		if (ibv_modify_qp(ctx->qp[0], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_AV                 |
				  IBV_QP_PATH_MTU           |
				  IBV_QP_DEST_QPN           |
				  IBV_QP_RQ_PSN)) {
			fprintf(stderr, "Failed to modify UC QP to RTR\n");
			return 1;
		}
	} 
	 
	else {
		for (i = 0; i < user_parm->num_of_qps; i++) {
			if (ibv_modify_qp(ctx->qp[i],&attr,IBV_QP_STATE )) {
				fprintf(stderr, "Failed to modify UD QP to RTR\n");
				return 1;
			}
		}
		if (user_parm->machine == CLIENT || user_parm->duplex) {
			ctx->ah = ibv_create_ah(ctx->pd,&attr.ah_attr);
			if (!ctx->ah) {
				fprintf(stderr, "Failed to create AH for UD\n");
				return 1;
			}
		}
	}

	if (user_parm->machine == CLIENT || user_parm->duplex) {

		attr.qp_state 	    = IBV_QPS_RTS;
		attr.sq_psn 	    = my_psn;
		if (user_parm->connection_type == RC) {
			attr.max_rd_atomic  = 1;
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

		} else {
			if(ibv_modify_qp(ctx->qp[0],&attr,IBV_QP_STATE |IBV_QP_SQ_PSN)) {
				fprintf(stderr, "Failed to modify UC QP to RTS\n");
				return 1;
			}
		}
	}

	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int set_recv_wqes(struct pingpong_context *ctx,
						 struct perftest_parameters *user_param,
						 struct ibv_recv_wr *rwr,
						 struct ibv_sge *sge_list,
						 uint64_t *my_addr) {
						
	int					i,j;
	int 				duplex_ind;
	struct ibv_recv_wr  *bad_wr_recv;

	i = (user_param->duplex && user_param->use_mcg) ? 1 : 0;
	duplex_ind = (user_param->duplex && !user_param->use_mcg) ? 1 : 0;

	while (i < user_param->num_of_qps) {

		sge_list[i].addr = (uintptr_t)ctx->buf + (user_param->num_of_qps + i)*BUFF_SIZE(ctx->size);

		//if (user_param->connection_type == UD) 
		//	sge_list[i].addr += (CACHE_LINE_SIZE - UD_ADDITION);

		sge_list[i].length = SIZE(user_param->connection_type,
								  user_param->size,
								  (user_param->machine == SERVER || user_param->duplex));

		sge_list[i].lkey   = ctx->mr->lkey;
		rwr[i].sg_list     = &sge_list[i];
		rwr[i].wr_id       = i;
		rwr[i].next        = NULL;
		rwr[i].num_sge	   = MAX_RECV_SGE;
		my_addr[i]		   = sge_list[i].addr;
		
		for (j = 0; j < user_param->rx_depth; ++j) {

			if (ibv_post_recv(ctx->qp[i],&rwr[i],&bad_wr_recv)) {
				fprintf(stderr, "Couldn't post recv Qp = %d: counter=%d\n",i,j);
				return 1;
			}

			if (SIZE(user_param->connection_type,
				     user_param->size,
					(user_param->machine == SERVER || 
					 user_param->duplex)) <= (CYCLE_BUFFER / 2))

				increase_loc_addr(&sge_list[i],
								  SIZE(user_param->connection_type,
									   user_param->size,
									   (user_param->machine == SERVER || 
									   user_param->duplex)),
								  j,
								  my_addr[i],
								  user_param->connection_type);
		}
		i++;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static void set_send_wqe(struct pingpong_context *ctx,int rem_qpn,
						 struct perftest_parameters *user_param,
						 struct ibv_sge *list,
						 struct ibv_send_wr *wr) {

	list->addr     = (uintptr_t)ctx->buf;
	list->lkey 	   = ctx->mr->lkey;

	wr->sg_list    = list;
	wr->num_sge    = 1;
	wr->opcode     = IBV_WR_SEND;
	wr->next       = NULL;
	wr->wr_id      = PINGPONG_SEND_WRID;
	wr->send_flags = IBV_SEND_SIGNALED;

	if (user_param->connection_type == UD) {
		wr->wr.ud.ah          = ctx->ah;
		wr->wr.ud.remote_qkey = DEF_QKEY;
		wr->wr.ud.remote_qpn  = rem_qpn;
	}
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int pp_drain_qp(struct pingpong_context *ctx,
						struct perftest_parameters *user_param,
						int psn,struct pingpong_dest *dest,
						struct mcast_parameters *mcg_params) {

	struct ibv_qp_attr attr;
	struct ibv_wc      wc;
	int                i;

	memset(&attr, 0, sizeof attr);
	attr.qp_state = IBV_QPS_ERR;

	for (i = 0; i <  user_param->num_of_qps; i++) {

		if (ibv_modify_qp(ctx->qp[i],&attr,IBV_QP_STATE)) {
			fprintf(stderr, "Failed to modify RC QP to ERR\n");
			return 1;
		}

		while (ibv_poll_cq(ctx->recv_cq,1,&wc));
   
		attr.qp_state = IBV_QPS_RESET;

		if (ibv_modify_qp(ctx->qp[i],&attr,IBV_QP_STATE)) {
			fprintf(stderr, "Failed to modify RC QP to RESET\n");
			return 1;
		}

		if(ctx_modify_qp_to_init(ctx->qp[i],user_param)) {
			return 1;
		}

		if (user_param->use_mcg) {

			if ((!user_param->duplex && user_param->machine == SERVER) || (user_param->duplex && i > 0)) {
				if (ibv_attach_mcast(ctx->qp[i],&mcg_params->mgid,mcg_params->mlid)) {
					fprintf(stderr, "Couldn't attach QP to MultiCast group");
					return 1;
				}
			}
		}
	}

	if (pp_connect_ctx(ctx,psn,dest,user_param)) {
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
		/* Find the peak bandwidth, unless asked not to in command line */
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
	printf(REPORT_FMT, (unsigned long) user_param->size,
		user_param->iters,
		(user_param->noPeak == OFF) * tsize * cycles_to_units / opt_delta / 0x100000,
		tsize * user_param->iters * cycles_to_units / (tcompleted[user_param->iters - 1] - tposted[0]) / 0x100000);
}

/****************************************************************************** 
 * Important note :															  
 * In case of UD/UC this is NOT the way to measureBW since we are running with 
 * loop on the send side , while we should run on the recieve side or enable 
 * retry in SW , Since the sender may be faster than the reciver.
 * Although	we had posted recieve it is not enough and might end this will
 * result in deadlock of test since both sides are stuck on poll cq.
 * In this test i do not solve this for the general test ,need to write
 * seperate test for UC/UD but in case the tx_depth is ~1/3 from the
 * number of iterations this should be ok .
 * Also note that the sender is limited in the number of send, ans
 * i try to make the reciver full .
 ******************************************************************************/
int run_iter_bi(struct pingpong_context *ctx, 
				struct perftest_parameters *user_param,
				struct ibv_recv_wr *rwr,
				struct ibv_send_wr *wr,
				uint64_t *my_addr)  {

	int                     scnt    = 0;
	int 					ccnt    = 0;
	int 					rcnt    = 0;
	int 					i       = 0;
	int 					num_of_qps = user_param->num_of_qps;
	int 					ne;
	struct ibv_wc 			*wc          = NULL;
	int 					*rcnt_for_qp = NULL;
	struct ibv_recv_wr      *bad_wr_recv = NULL;
	struct ibv_send_wr 		*bad_wr      = NULL;

	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);
	ALLOCATE(wc,struct ibv_wc,DEF_WC_SIZE);
	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	if (user_param->use_mcg)
		num_of_qps--; 
	
	// Set the length of the scatter in case of ALL option.
	wr->sg_list->length = user_param->size;
	wr->sg_list->addr   = (uintptr_t)ctx->buf;
	wr->send_flags = IBV_SEND_SIGNALED;
	
	if (user_param->size <= user_param->inline_size) 
		wr->send_flags |= IBV_SEND_INLINE; 

	while (ccnt < user_param->iters || rcnt < user_param->iters) {
                
		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth / 2) {

			if (scnt %  user_param->cq_mod == 0 && user_param->cq_mod > 1)
				wr->send_flags &= ~IBV_SEND_SIGNALED;

			tposted[scnt] = get_cycles();
			if (ibv_post_send(ctx->qp[0],wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}

			if (user_param->size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(wr->sg_list,user_param->size,scnt,(uintptr_t)ctx->buf,0);

			++scnt;

			if ((scnt % user_param->cq_mod) == (user_param->cq_mod - 1) || scnt == (user_param->iters - 1)) 
				wr->send_flags |= IBV_SEND_SIGNALED;
		}

		if (user_param->use_event) {

			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr,"Failed to notify events to CQ");
				return 1;
			}
		}

		do {
			ne = ibv_poll_cq(ctx->recv_cq,DEF_WC_SIZE,wc);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					
					if (wc[i].status != IBV_WC_SUCCESS)
						 NOTIFY_COMP_ERROR_SEND(wc[i],scnt,ccnt);

					rcnt_for_qp[wc[i].wr_id]++;
					rcnt++;

					if (rcnt + user_param->rx_depth*user_param->num_of_qps <= user_param->iters*user_param->num_of_qps) {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id , rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						if (user_param->size <= (CYCLE_BUFFER / 2) && user_param->connection_type != UD)
							increase_loc_addr(rwr[wc[i].wr_id].sg_list,
											  user_param->size,
											  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth - 1,
											  my_addr[wc[i].wr_id],user_param->connection_type);	
					}
				}
			}

		} while (ne > 0);

		if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return 1;
		}

		do {
			ne = ibv_poll_cq(ctx->send_cq,DEF_WC_SIZE,wc);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {

					if (wc[i].status != IBV_WC_SUCCESS)
						 NOTIFY_COMP_ERROR_SEND(wc[i],scnt,ccnt);

					ccnt += user_param->cq_mod;
					if (ccnt >= user_param->iters - 1) 
						tcompleted[user_param->iters - 1] = get_cycles();

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
	
	if (user_param->size <= user_param->inline_size) 
		wr->send_flags &= ~IBV_SEND_INLINE;
	
	free(rcnt_for_qp);
	free(wc);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter_uni_server(struct pingpong_context *ctx, 
						struct perftest_parameters *user_param,
						struct ibv_recv_wr *rwr,
						uint64_t *my_addr) {

	int 				rcnt = 0;
	int 				ne,i;
	int                 *rcnt_for_qp = NULL;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr  *bad_wr_recv = NULL;

	ALLOCATE(wc,struct ibv_wc,DEF_WC_SIZE);
	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);

	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	while (rcnt < user_param->iters*user_param->num_of_qps) {

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr ," Failed to notify events to CQ");
				return 1;
			}
		}
		
		do {
			ne = ibv_poll_cq(ctx->recv_cq,DEF_WC_SIZE,wc);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					
					if (wc[i].status != IBV_WC_SUCCESS) {
				
						NOTIFY_COMP_ERROR_RECV(wc[i],rcnt_for_qp[0]);
					}
						
					rcnt_for_qp[wc[i].wr_id]++;
					tcompleted[rcnt++] = get_cycles();

					if (rcnt + user_param->rx_depth*user_param->num_of_qps <= user_param->iters*user_param->num_of_qps) {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (CYCLE_BUFFER / 2)) {
							increase_loc_addr(rwr[wc[i].wr_id].sg_list,
											  SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine),
											  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth,
											  my_addr[wc[i].wr_id],user_param->connection_type);						
						}
					}
				}
			}
		} while (ne > 0);

		if (ne < 0) {
			fprintf(stderr, "Poll Recieve CQ failed %d\n", ne);
			return 1;
		}
	}

	tposted[0] = tcompleted[0];
	free(wc);
	free(rcnt_for_qp);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter_uni_client(struct pingpong_context *ctx, 
						struct perftest_parameters *user_param,
						struct ibv_send_wr *wr) {

	int 		       ne;
	int 			   i    = 0;
	int                scnt = 0;
	int                ccnt = 0;
	struct ibv_wc      *wc     = NULL;
	struct ibv_send_wr *bad_wr = NULL;

	ALLOCATE(wc,struct ibv_wc,DEF_WC_SIZE);

	// Set the lenght of the scatter in case of ALL option.
	wr->sg_list->length = user_param->size;
	wr->sg_list->addr   = (uintptr_t)ctx->buf;
	wr->send_flags = IBV_SEND_SIGNALED; 

	if (user_param->size <= user_param->inline_size) 
		wr->send_flags |= IBV_SEND_INLINE; 
	
	while (scnt < user_param->iters || ccnt < user_param->iters) {
		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth ) {

			if (scnt %  user_param->cq_mod == 0 && user_param->cq_mod > 1)
				wr->send_flags &= ~IBV_SEND_SIGNALED;

			tposted[scnt] = get_cycles();
			if (ibv_post_send(ctx->qp[0], wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}

			if (user_param->size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(wr->sg_list,user_param->size,scnt,(uintptr_t)ctx->buf,0);

			scnt++;

			if ((scnt % user_param->cq_mod) == (user_param->cq_mod - 1) || scnt == (user_param->iters - 1)) 
				wr->send_flags |= IBV_SEND_SIGNALED;
		}

		if (ccnt < user_param->iters) {
			
			if (user_param->use_event) {
				if (ctx_notify_events(ctx->channel)) {
					fprintf(stderr , " Failed to notify events to CQ");
					return 1;
				}
			} 
			do {
				ne = ibv_poll_cq(ctx->send_cq,DEF_WC_SIZE,wc);
				if (ne > 0) {
					for (i = 0; i < DEF_WC_SIZE; i++) {

						if (wc[i].status != IBV_WC_SUCCESS) 
							NOTIFY_COMP_ERROR_SEND(wc[i],scnt,ccnt);
			
						ccnt += user_param->cq_mod;
						if (ccnt >= user_param->iters - 1) 
							tcompleted[user_param->iters - 1] = get_cycles();

						else 
							tcompleted[ccnt - 1] = get_cycles();
					}
				}
                         
					
			} while (ne > 0);

			if (ne < 0) {
				fprintf(stderr, "poll CQ failed\n");
				return 1;
			}
		}
	}

	if (user_param->size <= user_param->inline_size) 
		wr->send_flags &= ~IBV_SEND_INLINE;

	free(wc);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	struct ibv_device		 	*ib_dev = NULL;
	struct pingpong_context  	ctx;
	struct pingpong_dest	 	my_dest,rem_dest;
	struct perftest_parameters  user_param;
	struct perftest_comm		user_comm;
	struct mcast_parameters     mcg_params;
    struct ibv_sge          	list;
	struct ibv_send_wr      	wr;
    struct ibv_sge              *sge_list = NULL;
	struct ibv_recv_wr      	*rwr = NULL;
	int                      	i = 0;
	int                      	size_max_pow = 24;
	int							size_of_arr;
    uint64_t                    *my_addr = NULL;

	/* init default values to user's parameters */
	memset(&ctx, 0,sizeof(struct pingpong_context));
	memset(&user_param, 0 , sizeof(struct perftest_parameters));
	memset(&mcg_params, 0 , sizeof(struct mcast_parameters));
	memset(&user_comm, 0,sizeof(struct perftest_comm));
	memset(&my_dest, 0 , sizeof(struct pingpong_dest));
	memset(&rem_dest, 0 , sizeof(struct pingpong_dest));
 
	user_param.verb    = SEND;
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

	mcg_params.ib_devname = ibv_get_device_name(ib_dev);

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

		 // create all the basic IB resources (data buffer, PD, MR, CQ and events channel)
	    if (ctx_init(&ctx,&user_param)) {
			fprintf(stderr, " Couldn't create IB resources\n");
			return FAILURE;
	    }
	}

	// Set up the Connection.
	if (send_set_up_connection(&ctx,&user_param,&my_dest,&mcg_params,&user_comm)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
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

	// Joining the Send side port the Mcast gid
	if (user_param.use_mcg && (user_param.machine == CLIENT || user_param.duplex)) {
		memcpy(mcg_params.mgid.raw, rem_dest.gid.raw, 16);
		if (set_mcast_group(&ctx,&user_param,&mcg_params)) {
			fprintf(stderr," Unable to Join Sender to Mcast gid\n");
			return 1;
		}
	}

	if (user_param.work_rdma_cm == OFF) {

		// Prepare IB resources for rtr/rts.
		if (pp_connect_ctx(&ctx,my_dest.psn,&rem_dest,&user_param)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			return 1;
		}
	}

	// shaking hands and gather the other side info.
	if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to exchange date between server and clients\n");
		return 1;    
	}

    if (user_param.use_event) {

		if (ibv_req_notify_cq(ctx.send_cq, 0)) {
			fprintf(stderr, " Couldn't request CQ notification\n");
            return 1;
        }

		if (ibv_req_notify_cq(ctx.recv_cq, 0)) {
			fprintf(stderr, " Couldn't request CQ notification\n");
            return 1;
        }
    }

	printf(RESULT_LINE);
	printf(RESULT_FMT);

	size_of_arr = (user_param.duplex) ? 1 : user_param.num_of_qps;

	ALLOCATE(tposted,cycles_t,user_param.iters*size_of_arr);
	ALLOCATE(tcompleted,cycles_t,user_param.iters*size_of_arr);

	if (user_param.machine == SERVER || user_param.duplex) {
		ALLOCATE(rwr,struct ibv_recv_wr,user_param.num_of_qps);
		ALLOCATE(sge_list,struct ibv_sge,user_param.num_of_qps);
		ALLOCATE(my_addr ,uint64_t ,user_param.num_of_qps);
	}
	
	if (user_param.machine == SERVER && !user_param.duplex) {
		user_param.noPeak = ON;
	}

	if (user_param.machine == CLIENT || user_param.duplex) {
		set_send_wqe(&ctx,rem_dest.qpn,&user_param,&list,&wr);
	}

	if (user_param.all == ON) {

		if (user_param.connection_type == UD) 
		   size_max_pow =  (int)UD_MSG_2_EXP(MTU_SIZE(user_param.curr_mtu)) + 1;

		for (i = 1; i < size_max_pow ; ++i) {
			user_param.size = 1 << i;

			if (user_param.machine == SERVER || user_param.duplex) {
				if (set_recv_wqes(&ctx,&user_param,rwr,sge_list,my_addr)) {
					fprintf(stderr," Failed to post receive recv_wqes\n");
					return 1;
				}
			}

			if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
				fprintf(stderr,"Failed to exchange date between server and clients\n");
				return 1;
			}

			if (user_param.duplex) {

				if(run_iter_bi(&ctx,&user_param,rwr,&wr,my_addr))
					return 17;

			} else if (user_param.machine == CLIENT) {

				if(run_iter_uni_client(&ctx,&user_param,&wr)) {
					return 17;
				}

			} else	{		  				

				if(run_iter_uni_server(&ctx,&user_param,rwr,my_addr)) {
					return 17;
				}
			}

			print_report(&user_param);

			if (user_param.work_rdma_cm == OFF) {

				if (pp_drain_qp(&ctx,&user_param,my_dest.psn,&rem_dest,&mcg_params)) {
					fprintf(stderr,"Failed to drain Recv queue (performance optimization)\n");
					return 1;
				}
			}

			if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
				fprintf(stderr,"Failed to exchange date between server and clients\n");
				return 1;
			}
        
		}

	} else {

		if (user_param.machine == SERVER || user_param.duplex) {
			if (set_recv_wqes(&ctx,&user_param,rwr,sge_list,my_addr)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				return 1;
			}
		}

		if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
			fprintf(stderr,"Failed to exchange date between server and clients\n");
			return 1;
		}

		if (user_param.duplex) {
				if(run_iter_bi(&ctx,&user_param,rwr,&wr,my_addr))
					return 17;
			} else if (user_param.machine == CLIENT) {

				if(run_iter_uni_client(&ctx,&user_param,&wr)) {
					return 17;
				}

			} else	{		  				

				if(run_iter_uni_server(&ctx,&user_param,rwr,my_addr)) {
					return 17;
				}
			}

		print_report(&user_param);	
	}
		
	if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
		fprintf(stderr," Failed to close connection between server and client\n");
		return 1;
	}

	printf(RESULT_LINE);
	return 0; 
}

