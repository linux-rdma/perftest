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
#include "multicast_resources.h"

#define VERSION 2.1

static int page_size;
cycles_t  *tstamp;

struct pingpong_context {
	struct ibv_sge          *sge_list;
	struct ibv_recv_wr      *rwr;
	struct ibv_context      *context;
	struct ibv_comp_channel *channel;
	struct ibv_pd           *pd;
	struct ibv_mr           *mr;
	struct ibv_cq           *rcq;
	struct ibv_cq           *scq;
	struct ibv_qp           **qp;
	struct ibv_ah	        *ah;
	void                    *buf;
	int                     size;
	struct ibv_sge      	list;
	struct ibv_send_wr  	wr;
};

/****************************************************************************** 
 *
 ******************************************************************************/
static int set_mcast_group(struct pingpong_context *ctx,
						   struct perftest_parameters *user_parm,
						   struct mcast_parameters *mcg_params) {

	int i;
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
	set_multicast_gid(mcg_params,ctx->qp[0]->qp_num,(int)user_parm->machine);

	if (!strcmp(link_layer_str(user_parm->link_type),"IB")) {
		// Request for Mcast group create registery in SM.
		if (join_multicast_group(SUBN_ADM_METHOD_SET,mcg_params)) {
			fprintf(stderr," Failed to Join Mcast request\n");
			return 1;
		}
	}

	for (i=0; i < user_parm->num_of_qps; i++) {

		if (ibv_attach_mcast(ctx->qp[i],&mcg_params->mgid,mcg_params->mlid)) {
			fprintf(stderr, "Couldn't attach QP to MultiCast group");
			return 1;
		}
	}
	mcg_params->mcast_state |= MCAST_IS_ATTACHED;

	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int destroy_mcast_group(struct pingpong_context *ctx,
							   struct perftest_parameters *user_parm,
						       struct mcast_parameters *mcg_params) {
	int i;

	for (i = 0; i < user_parm->num_of_qps; i++) {
		if (ibv_detach_mcast(ctx->qp[i],&mcg_params->mgid,mcg_params->mlid)) {
			fprintf(stderr, "Couldn't deattach QP from MultiCast group\n");
			return 1;
		}
	}
	if (!strcmp(link_layer_str(user_parm->link_type),"IB")) {
		// Removal Request for Mcast group in SM.
		if (join_multicast_group(SUBN_ADM_METHOD_DELETE,mcg_params)) {
			fprintf(stderr,"Couldn't Unregister the Mcast group on the SM\n");
			return 1;
		}
	}

	mcg_params->mcast_state &= ~MCAST_IS_ATTACHED;
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int set_up_connection(struct pingpong_context *ctx,
							 struct perftest_parameters *user_parm,
							 struct pingpong_dest *my_dest,
							 struct mcast_parameters *mcg_params) {

	if (user_parm->use_mcg) {

		if (set_mcast_group(ctx,user_parm,mcg_params)) {
			return 1;
		}

		my_dest->gid = mcg_params->mgid;
		my_dest->lid = mcg_params->mlid;
		my_dest->qpn = QPNUM_MCAST;
	}
	else {
		if (user_parm->gid_index != -1) {
			if (ibv_query_gid(ctx->context,user_parm->ib_port,user_parm->gid_index,&my_dest->gid)) {
				return -1;
			}
		}
		my_dest->lid   	   = ctx_get_local_lid(ctx->context,user_parm->ib_port);
		my_dest->qpn   	   = ctx->qp[0]->qp_num;
	}

	my_dest->psn       = lrand48() & 0xffffff;
	
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
								 struct mcast_parameters    *mcg_params)  {

	int i,test_result = 0;

	if (user_parm->use_mcg) {
		if (destroy_mcast_group(ctx,user_parm,mcg_params)) {
			fprintf(stderr, "failed to destroy MultiCast resources\n");
			test_result = 1;
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
			fprintf(stderr, "failed to destroy QP\n");
			test_result = 1;
		}
	}
	free(ctx->qp);

	if (ibv_destroy_cq(ctx->scq)) {
		fprintf(stderr, "failed to destroy CQ\n");
		test_result = 1;
	}

	if (ibv_destroy_cq(ctx->rcq)) {
		fprintf(stderr, "failed to destroy CQ\n");
		test_result = 1;
	}
	
	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "failed to deregister MR\n");
		test_result = 1;
	}
	
	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "failed to deallocate PD\n");
		test_result = 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "failed to destroy channel \n");
			test_result = 1;
		}
	}
	
	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "failed to close device context\n");
		test_result = 1;
	}
	free(ctx->rwr);
	free(ctx->sge_list);
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
	int i,buff_size;

	ALLOCATE(ctx,struct pingpong_context,1);

	ctx->ah 	  = NULL;

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
		fprintf(stderr, "Couldn't set the Mtu\n");
		return NULL;
	}

	if (user_parm->connection_type == UD && user_parm->size > MTU_SIZE(user_parm->curr_mtu)) {	 
		printf(" Max msg size in UD is MTU - %d . changing to MTU\n",MTU_SIZE(user_parm->curr_mtu));
		user_parm->size = MTU_SIZE(user_parm->curr_mtu);
	}

	printf(" Inline data is used up to %d bytes message\n", user_parm->inline_size);

	ctx->size = user_parm->size;
	buff_size = BUFF_SIZE(SIZE(user_parm->connection_type,ctx->size))*(1 + user_parm->num_of_qps);

	// Allocating the Buff size according to connection type and size.
	ctx->buf = memalign(page_size,buff_size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}
	memset(ctx->buf, 0,buff_size);

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

	ctx->mr = ibv_reg_mr(ctx->pd,ctx->buf,buff_size,IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return NULL;
	}
	
	ctx->scq = ibv_create_cq(ctx->context,user_parm->tx_depth,NULL,ctx->channel,0);
	if (!ctx->scq) {
	    fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	ctx->rcq = ibv_create_cq(ctx->context,user_parm->rx_depth*user_parm->num_of_qps,NULL,ctx->channel,0);
	if (!ctx->rcq) {
	    fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	ALLOCATE(ctx->qp,struct ibv_qp*,user_parm->num_of_qps);

	for (i = 0; i < user_parm->num_of_qps; i++) {
		
		ctx->qp[i] = ctx_qp_create(ctx->pd,ctx->scq,ctx->rcq,user_parm);
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
	int i;

	memset(&attr, 0, sizeof(struct ibv_qp_attr));

	attr.qp_state         = IBV_QPS_RTR;
	attr.path_mtu         = user_parm->curr_mtu;
	attr.dest_qp_num      = dest->qpn;
	attr.rq_psn           = dest->psn;
	attr.ah_attr.dlid     = dest->lid;
	if (user_parm->connection_type == RC) {
		attr.max_dest_rd_atomic     = 1;
		attr.min_rnr_timer          = 12;
	}

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

	if (user_parm->connection_type==RC) {
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
	} else if (user_parm->connection_type==UC) {
		if (ibv_modify_qp(ctx->qp[0], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_AV                 |
				  IBV_QP_PATH_MTU           |
				  IBV_QP_DEST_QPN           |
				  IBV_QP_RQ_PSN)) {
				fprintf(stderr, "Failed to modify UC QP to RTR\n");
				return 1;
		}

	} else {
		for (i = 0; i < user_parm->num_of_qps; i++) {
			if (ibv_modify_qp(ctx->qp[i],&attr,IBV_QP_STATE )) {
				fprintf(stderr, "Failed to modify UC QP to RTR\n");
				return 1;
			}
		}

		ctx->ah = ibv_create_ah(ctx->pd,&attr.ah_attr);
		if (!ctx->ah) {
			fprintf(stderr, "Failed to create AH for UD\n");
            return 1;
		}
	}

	attr.qp_state             = IBV_QPS_RTS;
	attr.sq_psn      		  = my_psn;
	if (user_parm->connection_type==RC) {
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

	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int set_recv_wqes(struct pingpong_context *ctx,
						 struct perftest_parameters *user_param) {
						
	int					i,j,buff_size;
	struct ibv_recv_wr  *bad_wr_recv;

	buff_size = BUFF_SIZE(SIZE(user_param->connection_type,ctx->size));

	for (i = 0; i < user_param->num_of_qps; i++) {

		ctx->sge_list[i].addr   = (uintptr_t)ctx->buf + (i + 1)*buff_size;
		ctx->sge_list[i].length = SIZE(user_param->connection_type,user_param->size);
		ctx->sge_list[i].lkey   = ctx->mr->lkey;

		ctx->rwr[i].sg_list     = &ctx->sge_list[i];
		ctx->rwr[i].wr_id       = i;
		ctx->rwr[i].next        = NULL;
		ctx->rwr[i].num_sge	    = MAX_RECV_SGE;
			
		for (j = 0; j < user_param->rx_depth; ++j) {

			if (ibv_post_recv(ctx->qp[i],&ctx->rwr[i],&bad_wr_recv)) {
				fprintf(stderr, "Couldn't post recv Qp = %d: counter=%d\n",i,j);
				return 1;
			}		
		}
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static void set_send_wqe(struct pingpong_context *ctx,int rem_qpn,
						 struct perftest_parameters *user_param) {

	ctx->list.addr     = (uintptr_t)ctx->buf;
	ctx->list.lkey 	   = ctx->mr->lkey;

	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_SEND;
	ctx->wr.next       = NULL;
	ctx->wr.wr_id      = PINGPONG_SEND_WRID;
	ctx->wr.send_flags = 0;

	if (user_param->connection_type == UD) {
		ctx->wr.wr.ud.ah          = ctx->ah;
		ctx->wr.wr.ud.remote_qkey = DEF_QKEY;
		ctx->wr.wr.ud.remote_qpn  = rem_qpn;
	}
}

/*
 * When there is an
 *	odd number of samples, the median is the middle number.
 *	even number of samples, the median is the mean of the
 *		two middle numbers.
 *
 */
static inline cycles_t get_median(int n, cycles_t delta[])
{
	if ((n - 1) % 2)
		return(delta[n / 2] + delta[n / 2 - 1]) / 2;
	else
		return delta[n / 2];
}


/****************************************************************************** 
 *
 ******************************************************************************/
static int cycles_compare(const void *aptr, const void *bptr)
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
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	qsort(delta, user_param->iters - 1, sizeof *delta, cycles_compare);

	if (user_param->r_flag->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < user_param->iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	median = get_median(user_param->iters - 1, delta);
	printf(REPORT_FMT_LAT,(unsigned long)user_param->size,user_param->iters,delta[0] / cycles_to_units / 2,
	       delta[user_param->iters - 2] / cycles_to_units / 2,median / cycles_to_units / 2);
	free(delta);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest) {

	int                     i    = 0;
	int						scnt = 0;
	int						rcnt = 0;
	int						poll = 0;
	int						ne;
	int 					qp_counter = 0;
	struct ibv_wc 			*wc;
	int 					*rcnt_for_qp = NULL;
	struct ibv_recv_wr      *bad_wr_recv;
	struct ibv_send_wr 		*bad_wr;


	ALLOCATE(wc,struct ibv_wc,DEF_WC_SIZE);
	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	// Post recevie recv_wqe's.
	if (set_recv_wqes(ctx,user_param)) {
		fprintf(stderr," Failed to post receive recv_wqes\n");
		return 1;
	}
	
	ctx->list.length = user_param->size;

	if (user_param->size <= user_param->inline_size) 
		ctx->wr.send_flags = IBV_SEND_INLINE; 

	while (scnt < user_param->iters || rcnt < user_param->iters) {
		if (rcnt < user_param->iters && !(scnt < 1 && user_param->machine == CLIENT)) {
		  
			// Server is polling on recieve first .
		    if (user_param->use_event) {
				if (ctx_notify_events(ctx->rcq,ctx->channel)) {
					fprintf(stderr , " Failed to notify events to CQ");
					return 1;
				}
		    }

			do {
				ne = ibv_poll_cq(ctx->rcq,DEF_WC_SIZE,wc);
				if (ne > 0) {
					for (i = 0; i < ne; i++) {

						if (wc[i].status != IBV_WC_SUCCESS) 
							NOTIFY_COMP_ERROR_RECV(wc[i],rcnt);
							
						rcnt_for_qp[wc[i].wr_id]++;
						qp_counter++;
						
						if (rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth  <= user_param->iters) {
							if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id], &bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv: rcnt=%d\n",rcnt);
								return 15;
							}
						}
					}
				}
			} while (!user_param->use_event && qp_counter < user_param->num_of_qps);
			rcnt++;
			qp_counter  = 0;
		}

		// client post first. 
		if (scnt < user_param->iters) {

			tstamp[scnt++] = get_cycles();

			if (scnt % user_param->cq_mod == 0 || scnt == user_param->iters) {
				poll = 1;
				ctx->wr.send_flags |= IBV_SEND_SIGNALED;
			}
			
			if (ibv_post_send(ctx->qp[0],&ctx->wr,&bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 11;
			}
		}

		if (poll == 1) {

		    struct ibv_wc s_wc;
		    int s_ne;

		    if (user_param->use_event) {
				if (ctx_notify_events(ctx->scq,ctx->channel)) {
					fprintf(stderr , " Failed to notify events to CQ");
					return 1;
				}
		    }

		    do {
				s_ne = ibv_poll_cq(ctx->scq, 1, &s_wc);
		    } while (!user_param->use_event && s_ne == 0);

		    if (s_ne < 0) {
				fprintf(stderr, "poll SCQ failed %d\n", s_ne);
				return 12;
		    }

			if (s_wc.status != IBV_WC_SUCCESS) 
				NOTIFY_COMP_ERROR_SEND(wc[i],scnt,scnt)
				
			poll = 0;
			ctx->wr.send_flags &= ~IBV_SEND_SIGNALED;
		}
	}

	if (user_param->size <= user_param->inline_size) 
		ctx->wr.send_flags &= ~IBV_SEND_INLINE;

	free(wc);
	free(rcnt_for_qp);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[])
{

	int                        i = 0;
	int                        size_max_pow = 24;
	struct report_options      report = {};
	struct pingpong_context    *ctx = NULL;
	struct pingpong_dest	   my_dest,rem_dest;
	struct mcast_parameters	   mcg_params;
	struct ibv_device          *ib_dev = NULL;
	struct perftest_parameters user_param;

	/* init default values to user's parameters */
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	memset(&mcg_params, 0, sizeof(struct mcast_parameters));
	memset(&my_dest   , 0, sizeof(struct pingpong_dest));
	memset(&rem_dest  , 0, sizeof(struct pingpong_dest));

	user_param.verb    = SEND;
	user_param.tst     = LAT;
	user_param.version = VERSION;
	user_param.r_flag  = &report;

	if (parser(&user_param,argv,argc)) 
		return 1;

	// Print basic test information.
	ctx_print_test_info(&user_param);

	if (user_param.all == ON) {
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
		return 8;

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

	// Connects......
	if (pp_connect_ctx(ctx,my_dest.psn,&rem_dest,&user_param)) {
		fprintf(stderr," Unable to Connect the HCA's through the link\n");
		return 1;
	}

	// An additional handshake is required after moving qp to RTR.
	if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
    }

    if (user_param.use_event) {
        if (ibv_req_notify_cq(ctx->rcq, 0)) {
			fprintf(stderr, "Couldn't request RCQ notification\n");
			return 1;
		} 
        if (ibv_req_notify_cq(ctx->scq, 0)) {
			fprintf(stderr, "Couldn't request SCQ notification\n");
			return 1;
		}

    }

	printf(RESULT_LINE);
	printf(RESULT_FMT_LAT);

	ALLOCATE(tstamp,cycles_t,user_param.iters);
	ALLOCATE(ctx->rwr,struct ibv_recv_wr,user_param.num_of_qps);
	ALLOCATE(ctx->sge_list,struct ibv_sge,user_param.num_of_qps);
	set_send_wqe(ctx,rem_dest.qpn,&user_param);
    
	if (user_param.all == ON) {

		if (user_param.connection_type == UD)  
			size_max_pow =  (int)UD_MSG_2_EXP(MTU_SIZE(user_param.curr_mtu)) + 1;

		for (i = 1; i < size_max_pow ; ++i) {
			user_param.size = 1 << i;
			if(run_iter(ctx, &user_param, &rem_dest))
				return 17;

			print_report(&user_param);

			if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
				fprintf(stderr,"Failed to exchange date between server and clients\n");
				return 1;
			}
		}
	} else {
		if(run_iter(ctx, &user_param, &rem_dest))
			return 18;	
		print_report(&user_param);
	}

	if (ctx_close_connection(&user_param,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}
	
	printf(RESULT_LINE);

	return destroy_ctx_resources(ctx,&user_param,&mcg_params);
}
