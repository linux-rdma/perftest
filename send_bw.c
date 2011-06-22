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
#include <limits.h>
#include <malloc.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <infiniband/verbs.h>

#include "get_clock.h"
#include "multicast_resources.h"
#include "perftest_resources.h"

#define VERSION 2.1

static int page_size;
cycles_t	*tposted;
cycles_t	*tcompleted;

struct pingpong_context {
	struct ibv_context 		*context;
	struct ibv_comp_channel *channel;
	struct ibv_pd      		*pd;
	struct ibv_mr     		**mr;
	struct ibv_cq      		*cq;
	struct ibv_qp      		**qp;
	struct ibv_sge      	list;
	struct ibv_send_wr  	wr;
	struct ibv_sge 			*sge_list;
	struct ibv_recv_wr  	*rwr;
	struct ibv_ah			*ah;
	void               		**buf;
	unsigned            	size;
	uint64_t				*my_addr;
};

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
static int set_up_connection(struct pingpong_context *ctx,
							 struct perftest_parameters *user_parm,
							 struct pingpong_dest *my_dest,
							 struct mcast_parameters *mcg_params) {

	int i = (user_parm->duplex) ? 1 : 0;

	if (user_parm->use_mcg && (user_parm->duplex || user_parm->machine == SERVER)) {

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
static int destroy_ctx_resources(struct pingpong_context    *ctx, 
								 struct perftest_parameters *user_parm,
								 struct pingpong_dest		*my_dest,
								 struct pingpong_dest		*rem_dest,
								 struct mcast_parameters    *mcg_params)  {

	int test_result = 0;
	int i = (user_parm->duplex) ? 1 : 0;

	if (user_parm->use_mcg) {

		if (user_parm->machine == SERVER || user_parm->duplex) {
			
			while (i < user_parm->num_of_qps) {
				if (ibv_detach_mcast(ctx->qp[i],&my_dest->gid,my_dest->lid)) {
					fprintf(stderr, "Couldn't deattach QP from MultiCast group\n");
					return 1;
				}
				i++;
			}
			mcg_params->mgid = my_dest->gid;
			if (!strcmp(link_layer_str(user_parm->link_type),"IB")) {
				if (join_multicast_group(SUBN_ADM_METHOD_DELETE,mcg_params)) {
					fprintf(stderr,"Couldn't Unregister the Mcast group on the SM\n");
					return 1;
				}
			}
		}

		if (user_parm->machine == CLIENT || user_parm->duplex) {

			mcg_params->mgid = rem_dest->gid;
			if (!strcmp(link_layer_str(user_parm->link_type),"IB")) {
				if (join_multicast_group(SUBN_ADM_METHOD_DELETE,mcg_params)) {
					fprintf(stderr,"Couldn't Unregister the Mcast group on the SM\n");
					return 1;
				}
			}

		}
	}	

	if (ctx->ah) {
		if (ibv_destroy_ah(ctx->ah)) {
			fprintf(stderr, "failed to destroy AH\n");
			test_result = 1;
		}
	}

	for(i = 0; i < user_parm->num_of_qps; i++) {
		if (ibv_destroy_qp(ctx->qp[i])) {
			test_result = 1;
		}
	}
	free(ctx->qp);

	if (ibv_destroy_cq(ctx->cq)) {
		test_result = 1;
	}

	for(i = 0; i < user_parm->num_of_qps; i++) {

		if (ibv_dereg_mr(ctx->mr[i])) {
			test_result = 1;
		}
		free(ctx->buf[i]);
	}
	
	if (ibv_dealloc_pd(ctx->pd)) {
		test_result = 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			test_result = 1;
		}
	}
	
	if (ibv_close_device(ctx->context)) {
		test_result = 1;
	}

	if (user_parm->machine == SERVER || user_parm->duplex) {
		free(ctx->rwr);
		free(ctx->sge_list);
		free(ctx->my_addr);
	}

	free(ctx->mr);
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

	int i,m_size;
	int duplex_ind;
	struct pingpong_context *ctx;

	ALLOCATE(ctx,struct pingpong_context,1);
	ALLOCATE(ctx->buf,void*,user_parm->num_of_qps);
	ALLOCATE(ctx->mr,struct ibv_mr*,user_parm->num_of_qps);

	ctx->ah       = NULL;
	ctx->channel  = NULL;

	duplex_ind = (user_parm->duplex && !user_parm->use_mcg) ? 2 : 1;

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
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

	if (user_parm->connection_type == UD && user_parm->size > MTU_SIZE(user_parm->curr_mtu)) {	 
		printf(" Max msg size in UD is MTU - %d . changing to MTU\n",MTU_SIZE(user_parm->curr_mtu));
		user_parm->size = MTU_SIZE(user_parm->curr_mtu);
	}

	if (is_dev_hermon(ctx->context) != HERMON && user_parm->inline_size != 0)
		user_parm->inline_size = 0;

	printf(" Inline data is used up to %d bytes message\n", user_parm->inline_size);

	ctx->size = user_parm->size;
	
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

	for (i = 0; i < user_parm->num_of_qps; i++) {

		m_size = (BUFF_SIZE(user_parm->size) + IF_UD_ADD(user_parm->connection_type))*duplex_ind;
		ctx->buf[i] = memalign(page_size,m_size);
		if (!ctx->buf[i]) {
			fprintf(stderr, "Couldn't allocate work buf.\n");
			return NULL;
		}
		memset(ctx->buf[i],0,m_size);

		// We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says :
		// The Consumer is not allowed to assign Remote Write or Remote Atomic to
		// a Memory Region that has not been assigned Local Write. 
		ctx->mr[i] = ibv_reg_mr(ctx->pd,
								ctx->buf[i],
								m_size,
								IBV_ACCESS_REMOTE_WRITE | 
								IBV_ACCESS_LOCAL_WRITE);

		if (!ctx->mr[i]) {
			fprintf(stderr, "Couldn't allocate MR\n");
			return NULL;
		}
	}

	// Create the CQ according to Client/Server or Duplex setting.
	ctx->cq = ctx_cq_create(ctx->context,ctx->channel,user_parm);
	if (ctx->cq == NULL) {
		fprintf(stderr, "Couldn't create CQ \n");
		return NULL;
	}

	ALLOCATE(ctx->qp,struct ibv_qp*,user_parm->num_of_qps);
	
	for(i=0; i < user_parm->num_of_qps; i++) {
		ctx->qp[i] = ctx_qp_create(ctx->pd,ctx->cq,ctx->cq,user_parm);
		if (ctx->qp[i] == NULL) {
			return NULL;
		}

		if(ctx_modify_qp_to_init(ctx->qp[i],user_parm)) {
			return NULL;
		}
	}

	return ctx;
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
						 struct perftest_parameters *user_param) {
						
	int					i,j,buff_size;
	int 				duplex_ind;
	struct ibv_recv_wr  *bad_wr_recv;

	i = (user_param->duplex && user_param->use_mcg) ? 1 : 0;
	duplex_ind = (user_param->duplex && !user_param->use_mcg) ? 1 : 0;

	buff_size = BUFF_SIZE(ctx->size) + IF_UD_ADD(user_param->connection_type);

	while (i < user_param->num_of_qps) {

		ctx->sge_list[i].addr   = (uintptr_t)ctx->buf[i] + duplex_ind*buff_size;

		if (user_param->connection_type == UD) 
			ctx->sge_list[i].addr += (CACHE_LINE_SIZE - UD_ADDITION);

		ctx->sge_list[i].length = SIZE(user_param->connection_type,user_param->size);
		ctx->sge_list[i].lkey   = ctx->mr[i]->lkey;
		ctx->rwr[i].sg_list     = &ctx->sge_list[i];
		ctx->rwr[i].wr_id       = i;
		ctx->rwr[i].next        = NULL;
		ctx->rwr[i].num_sge	    = MAX_RECV_SGE;
		ctx->my_addr[i]		    = (uintptr_t)ctx->buf[i] + duplex_ind*buff_size;
		
		for (j = 0; j < user_param->rx_depth; ++j) {

			if (ibv_post_recv(ctx->qp[i],&ctx->rwr[i],&bad_wr_recv)) {
				fprintf(stderr, "Couldn't post recv Qp = %d: counter=%d\n",i,j);
				return 1;
			}

			if (user_param->size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(&ctx->sge_list[i],user_param->size,j,ctx->my_addr[i],user_param->connection_type);
		}
		i++;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static void set_send_wqe(struct pingpong_context *ctx,int rem_qpn,
						 struct perftest_parameters *user_param) {

	ctx->list.addr     = (uintptr_t)ctx->buf[0];
	ctx->list.lkey 	   = ctx->mr[0]->lkey;

	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_SEND;
	ctx->wr.next       = NULL;
	ctx->wr.wr_id      = PINGPONG_SEND_WRID;
	ctx->wr.send_flags = IBV_SEND_SIGNALED;

	if (user_param->connection_type == UD) {
		ctx->wr.wr.ud.ah          = ctx->ah;
		ctx->wr.wr.ud.remote_qkey = DEF_QKEY;
		ctx->wr.wr.ud.remote_qpn  = rem_qpn;
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

		while (ibv_poll_cq(ctx->cq,1,&wc));
   
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
				struct perftest_parameters *user_param)  {

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
	ctx->list.length = user_param->size;
	ctx->list.addr   = (uintptr_t)ctx->buf[0];
	ctx->wr.send_flags = IBV_SEND_SIGNALED;
	
	if (user_param->size <= user_param->inline_size) 
		ctx->wr.send_flags |= IBV_SEND_INLINE; 

	while (ccnt < user_param->iters || rcnt < user_param->iters) {
                
		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth / 2) {

			if (scnt %  user_param->cq_mod == 0 && user_param->cq_mod > 1)
				ctx->wr.send_flags &= ~IBV_SEND_SIGNALED;

			tposted[scnt] = get_cycles();
			if (ibv_post_send(ctx->qp[0],&ctx->wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}

			if (user_param->size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(&ctx->list,user_param->size,scnt,(uintptr_t)ctx->buf[0],0);

			++scnt;

			if ((scnt % user_param->cq_mod) == (user_param->cq_mod - 1) || scnt == (user_param->iters - 1)) 
				ctx->wr.send_flags |= IBV_SEND_SIGNALED;
		}

		if (user_param->use_event) {

			if (ctx_notify_events(ctx->cq,ctx->channel)) {
				fprintf(stderr,"Failed to notify events to CQ");
				return 1;
			}
		}

		do {
			ne = ibv_poll_cq(ctx->cq,DEF_WC_SIZE,wc);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					
					if (wc[i].status != IBV_WC_SUCCESS)
						 NOTIFY_COMP_ERROR_SEND(wc[i],scnt,ccnt);

					if ((int) wc[i].wr_id == PINGPONG_SEND_WRID) {
						ccnt += user_param->cq_mod;
						if (ccnt >= user_param->iters - 1) 
							tcompleted[user_param->iters - 1] = get_cycles();

						else 
							tcompleted[ccnt - 1] = get_cycles();
					}

					else {

						rcnt_for_qp[wc[i].wr_id]++;
						rcnt++;
						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id , rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						if (user_param->size <= (CYCLE_BUFFER / 2))
							increase_loc_addr(&ctx->sge_list[wc[i].wr_id],
							  user_param->size,rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth - 1,
							  ctx->my_addr[wc[i].wr_id],user_param->connection_type);	
					}
				}
			}
		} while (ne > 0);

		if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return 1;
		}
	}
	
	if (user_param->size <= user_param->inline_size) 
		ctx->wr.send_flags &= ~IBV_SEND_INLINE;
	
	free(rcnt_for_qp);
	free(wc);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter_uni_server(struct pingpong_context *ctx, 
						struct perftest_parameters *user_param) {

	int 				rcnt = 0;
	int 				ne,i;
	int                 *rcnt_for_qp = NULL;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr  *bad_wr_recv = NULL;

	ALLOCATE(wc,struct ibv_wc,DEF_WC_SIZE);
	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);

	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	while (rcnt < user_param->iters) {

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->cq,ctx->channel)) {
				fprintf(stderr ," Failed to notify events to CQ");
				return 1;
			}
		}
		
		do {
			ne = ibv_poll_cq(ctx->cq,DEF_WC_SIZE,wc);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					
					if (wc[i].status != IBV_WC_SUCCESS) 
						NOTIFY_COMP_ERROR_RECV(wc[i],rcnt_for_qp[wc[i].wr_id]);
						
					rcnt_for_qp[wc[i].wr_id]++;
					tcompleted[rcnt++] = get_cycles();

				   	if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
						return 15;
					}

					if (user_param->size <= (CYCLE_BUFFER / 2))
						increase_loc_addr(&ctx->sge_list[wc[i].wr_id],user_param->size,
										  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth,
										  ctx->my_addr[wc[i].wr_id],user_param->connection_type);						
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
						struct perftest_parameters *user_param) {

	int 		       ne;
	int 			   i    = 0;
	int                scnt = 0;
	int                ccnt = 0;
	struct ibv_wc      *wc     = NULL;
	struct ibv_send_wr *bad_wr = NULL;

	ALLOCATE(wc,struct ibv_wc,DEF_WC_SIZE);

	// Set the lenght of the scatter in case of ALL option.
	ctx->list.length = user_param->size;
	ctx->list.addr   = (uintptr_t)ctx->buf[0];
	ctx->wr.send_flags = IBV_SEND_SIGNALED; 

	if (user_param->size <= user_param->inline_size) 
		ctx->wr.send_flags |= IBV_SEND_INLINE; 
	

	while (scnt < user_param->iters || ccnt < user_param->iters) {
		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth ) {

			if (scnt %  user_param->cq_mod == 0 && user_param->cq_mod > 1)
				ctx->wr.send_flags &= ~IBV_SEND_SIGNALED;

			tposted[scnt] = get_cycles();
			if (ibv_post_send(ctx->qp[0], &ctx->wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}

			if (user_param->size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(&ctx->list,user_param->size,scnt,(uintptr_t)ctx->buf[0],0);

			scnt++;

			if ((scnt % user_param->cq_mod) == (user_param->cq_mod - 1) || scnt == (user_param->iters - 1)) 
				ctx->wr.send_flags |= IBV_SEND_SIGNALED;
		}

		if (ccnt < user_param->iters) {	
			
			if (user_param->use_event) {
				if (ctx_notify_events(ctx->cq,ctx->channel)) {
					fprintf(stderr , " Failed to notify events to CQ");
					return 1;
				}
			} 
			do {
				ne = ibv_poll_cq(ctx->cq,DEF_WC_SIZE,wc);
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
		ctx->wr.send_flags &= ~IBV_SEND_INLINE;

	free(wc);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[])
{
	struct ibv_device		 	*ib_dev = NULL;
	struct pingpong_context  	*ctx;
	struct pingpong_dest	 	my_dest,rem_dest;
	struct perftest_parameters  user_param;
	struct mcast_parameters     mcg_params;
	int                      	i = 0;
	int                      	size_max_pow = 24;
	int							size_of_arr;

	// Pointer to The relevent function of run_iter according to machine type.
	int (*ptr_to_run_iter_uni)(struct pingpong_context*,struct perftest_parameters*);

	/* init default values to user's parameters */
	memset(&user_param, 0 , sizeof(struct perftest_parameters));
	memset(&mcg_params, 0 , sizeof(struct mcast_parameters));
	memset(&my_dest   , 0 , sizeof(struct pingpong_dest));
	memset(&rem_dest   , 0 , sizeof(struct pingpong_dest));
 
	user_param.verb    = SEND;
	user_param.tst     = BW;
	user_param.version = VERSION;

	if (parser(&user_param,argv,argc)) 
		return 1;

	// Print basic test information.
	ctx_print_test_info(&user_param);

	// Done with parameter parsing. Perform setup.
	if (user_param.all == ON) {
		// since we run all sizes 
		user_param.size = MAX_SIZE;
	}

	srand48(getpid() * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);

	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev)
		return 7;

	mcg_params.ib_devname = ibv_get_device_name(ib_dev);

	ctx = pp_init_ctx(ib_dev,&user_param);
	if (!ctx)
		return 1;

	// Set up the Connection.
	if (set_up_connection(ctx,&user_param,&my_dest,&mcg_params)) {
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
	// For printing only MGID in the remote side.
	user_param.side = REMOTE;
	ctx_print_pingpong_data(&rem_dest,&user_param);

	// Joining the Send side port the Mcast gid
	if (user_param.use_mcg && (user_param.machine == CLIENT || user_param.duplex)) {
		memcpy(mcg_params.mgid.raw, rem_dest.gid.raw, 16);
		if (set_mcast_group(ctx,&user_param,&mcg_params)) {
			fprintf(stderr," Unable to Join Sender to Mcast gid\n");
			return 1;
		}
	}

	// Prepare IB resources for rtr/rts.
	if (pp_connect_ctx(ctx,my_dest.psn,&rem_dest,&user_param)) {
		fprintf(stderr," Unable to Connect the HCA's through the link\n");
		return 1;
	}
	
	// shaking hands and gather the other side info.
    if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
        
    }

	if (user_param.use_event) {
		if (ibv_req_notify_cq(ctx->cq, 0)) {
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
		ALLOCATE(ctx->rwr,struct ibv_recv_wr,user_param.num_of_qps);
		ALLOCATE(ctx->sge_list,struct ibv_sge,user_param.num_of_qps);
		ALLOCATE(ctx->my_addr ,uint64_t ,user_param.num_of_qps);
	}

	ptr_to_run_iter_uni = (user_param.machine == CLIENT) ?	&run_iter_uni_client : &run_iter_uni_server;
	
	if (user_param.machine == SERVER && !user_param.duplex) {
		user_param.noPeak = ON;
	}

	if (user_param.machine == CLIENT || user_param.duplex) {
		set_send_wqe(ctx,rem_dest.qpn,&user_param);
	}

	if (user_param.all == ON) {

		if (user_param.connection_type == UD) 
		   size_max_pow =  (int)UD_MSG_2_EXP(MTU_SIZE(user_param.curr_mtu)) + 1;

		for (i = 1; i < size_max_pow ; ++i) {
			user_param.size = 1 << i;

			if (user_param.machine == SERVER || user_param.duplex) {
				if (set_recv_wqes(ctx,&user_param)) {
					fprintf(stderr," Failed to post receive recv_wqes\n");
					return 1;
				}
			}

			if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
				fprintf(stderr,"Failed to exchange date between server and clients\n");
				return 1;
			}

			if (user_param.duplex) {
				if(run_iter_bi(ctx,&user_param))
					return 17;
			} else {
				if((*ptr_to_run_iter_uni)(ctx,&user_param))
					return 17;
			}
			print_report(&user_param);

			if (pp_drain_qp(ctx,&user_param,my_dest.psn,&rem_dest,&mcg_params)) {
				fprintf(stderr,"Failed to drain Recv queue (performance optimization)\n");
				return 1;
			}

			if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
				fprintf(stderr,"Failed to exchange date between server and clients\n");
				return 1;
			}
        
		}

	} else {

		if (user_param.machine == SERVER || user_param.duplex) {
			if (set_recv_wqes(ctx,&user_param)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				return 1;
			}
		}

		if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
			fprintf(stderr,"Failed to exchange date between server and clients\n");
			return 1;
		}

		if (user_param.duplex) {
			if(run_iter_bi(ctx,&user_param))
				return 18;

		} else {
			if((*ptr_to_run_iter_uni)(ctx,&user_param))
				return 18;
		}

		print_report(&user_param);	
	}
		
	if (ctx_close_connection(&user_param,&my_dest,&rem_dest)) {
		fprintf(stderr," Failed to close connection between server and client\n");
		return 1;
	}

	printf(RESULT_LINE);
	return destroy_ctx_resources(ctx,&user_param,&my_dest,&rem_dest,&mcg_params);
}
