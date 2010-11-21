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

#define VERSION 2.0
#define SIGNAL 1

static int page_size;
cycles_t  *tstamp;

struct report_options {
	int unsorted;
	int histogram;
	int cycles;   /* report delta's in cycles, not microsec's */
};

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
 						   struct pingpong_dest *my_dest,
						   const char *servername) {

	params->side = LOCAL;

	ctx_print_pingpong_data(my_dest,params);
	
	if (params->machine == CLIENT) 
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
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
											struct perftest_parameters *user_parm) {

	struct pingpong_context *ctx;
	int i,buff_size;

	ALLOCATE(ctx,struct pingpong_context,1);

	ctx->ah 	  = NULL;
	ctx->size     = size;

	buff_size = BUFF_SIZE(SIZE(user_parm->connection_type,size))*(1 + user_parm->num_of_qps);

	// Allocating the Buff size according to connection type and size.
	ctx->buf = memalign(page_size,buff_size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}
	memset(ctx->buf, 0,buff_size);

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
static int set_recv_wqes(struct pingpong_context *ctx,int size,
						 struct perftest_parameters *user_param) {
						
	int					i,j,buff_size;
	struct ibv_recv_wr  *bad_wr_recv;

	buff_size = BUFF_SIZE(SIZE(user_param->connection_type,ctx->size));

	for (i = 0; i < user_param->num_of_qps; i++) {

		ctx->sge_list[i].addr   = (uintptr_t)ctx->buf + (i + 1)*buff_size;
		ctx->sge_list[i].length = SIZE(user_param->connection_type,size);
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
	printf("  -p, --port=<port>            Listen on/connect to port <port> (default 18515)\n");
	printf("  -c, --connection=<RC/UC/UD>  Connection type RC/UC/UD (default RC)\n");
	printf("  -m, --mtu=<mtu>              Mtu size (256 - 4096. default for hermon is 2048)\n");
	printf("  -d, --ib-dev=<dev>           Use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>         Use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>            Size of message to exchange (default 1)\n");
	printf("  -t, --tx-depth=<dep>         Size of tx queue (default 50)\n");
	printf("  -l, --signal                 Signal completion on each msg\n");
	printf("  -a, --all                    Run sizes from 2 till 2^23\n");
	printf("  -n, --iters=<iters>          Number of exchanges (at least 2, default 1000)\n");
	printf("  -I, --inline_size=<size>     Max size of message to be sent in inline mode (default 400)\n");
	printf("  -u, --qp-timeout=<timeout>   QP timeout, timeout value is 4 usec * 2 ^(timeout), default 14\n");
	printf("  -S, --sl=<sl>                SL (default 0)\n");
	printf("  -x, --gid-index=<index>      Test uses GID with GID index taken from command line (for RDMAoE index should be 0)\n");
	printf("  -C, --report-cycles          Report times in cpu cycle units (default microseconds)\n");
	printf("  -H, --report-histogram       Print out all results (default print summary only)\n");
	printf("  -U, --report-unsorted        (implies -H) print out unsorted results (default sorted)\n");
	printf("  -V, --version                Display version number\n");
	printf("  -e, --events                 Sleep on CQ events (default poll)\n");
	printf("  -g, --mcg=<num_of_qps>       Send messages to multicast group with <num_of_qps> qps attached to it.\n");
	printf("  -M, --MGID=<multicast_gid>   In case of multicast, uses <multicast_gid> as the group MGID.\n");
	printf("                               The format must be '255:1:X:X:X:X:X:X:X:X:X:X:X:X:X:X', where X is a vlaue within [0,255].\n");
	printf("                               You must specify a different MGID on both sides (to avoid loopback).\n");
	printf("  -F, --CPU-freq               Do not fail even if cpufreq_ondemand module is loaded\n");
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
static void print_report(struct report_options *options,
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
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	qsort(delta, iters - 1, sizeof *delta, cycles_compare);

	if (options->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	median = get_median(iters - 1, delta);
	printf(REPORT_FMT_LAT,size,iters,delta[0] / cycles_to_units / 2,
	       delta[iters - 2] / cycles_to_units / 2,median / cycles_to_units / 2);
	free(delta);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest, int size)
{
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
	if (set_recv_wqes(ctx,size,user_param)) {
		fprintf(stderr," Failed to post receive recv_wqes\n");
		return 1;
	}
	
	ctx->list.length = size;

	if (size <= user_param->inline_size) 
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

			if (scnt % CQ_MODERATION == 0 || scnt == user_param->iters) {
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

	if (size <= user_param->inline_size) 
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
	const char                 *ib_devname = NULL;
	int                        size = 2;
	int                        i = 0;
	int                        size_max_pow = 24;
	struct report_options      report = {};
	struct pingpong_context    *ctx = NULL;
	struct pingpong_dest	   my_dest,rem_dest;
	struct mcast_parameters	   mcg_params;
	struct ibv_device          *ib_dev = NULL;
	struct perftest_parameters user_param;
	int                        no_cpu_freq_fail = 0;
	struct ibv_context         *context;

	int all = 0;
	const char *servername = NULL;

	/* init default values to user's parameters */
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	memset(&mcg_params, 0, sizeof(struct mcast_parameters));
	memset(&my_dest   , 0, sizeof(struct pingpong_dest));
	memset(&rem_dest  , 0, sizeof(struct pingpong_dest));

	user_param.iters = 1000;
	user_param.port = 18515;
	user_param.ib_port = 1;
	user_param.tx_depth = 50;
	user_param.rx_depth = 50;
	user_param.inline_size = MAX_INLINE;
	user_param.verb = SEND;
	user_param.qp_timeout = 14;
	user_param.gid_index = -1; /*gid will not be used*/
	user_param.num_of_qps = 1;

	/* Parameter parsing. */
	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",           .has_arg = 1, .val = 'p' },
			{ .name = "connection",     .has_arg = 1, .val = 'c' },
			{ .name = "mtu",            .has_arg = 1, .val = 'm' },
			{ .name = "ib-dev",         .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",        .has_arg = 1, .val = 'i' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "inline_size",    .has_arg = 1, .val = 'I' },
			{ .name = "qp-timeout",     .has_arg = 1, .val = 'u' },
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
			{ .name = "gid-index",      .has_arg = 1, .val = 'x' },
			{ .name = "signal",         .has_arg = 0, .val = 'l' },
			{ .name = "all",            .has_arg = 0, .val = 'a' },
			{ .name = "report-cycles",  .has_arg = 0, .val = 'C' },
			{ .name = "report-histogram",.has_arg = 0, .val = 'H'},
			{ .name = "report-unsorted",.has_arg = 0, .val = 'U' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "events",         .has_arg = 0, .val = 'e' },
			{ .name = "mcg",            .has_arg = 1, .val = 'g' },
			{ .name = "MGID",           .has_arg = 1, .val = 'M' },
			{ .name = "CPU-freq",       .has_arg = 0, .val = 'F' },
			{ 0 }
		};
		c = getopt_long(argc, argv, "p:c:m:d:i:s:n:t:I:u:S:x:g:M:laeCHUVF", long_options, NULL);
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
		case 'c':
			if (strcmp("UC",optarg)==0)
				user_param.connection_type=UC;
			if (strcmp("UD",optarg)==0)
				user_param.connection_type=UD;
			/* default is 0 for any other option RC*/
			break;
        case 'e':
			++user_param.use_event;
			break;
		case 'g':
			++user_param.use_mcg;
			user_param.num_of_qps = strtol(optarg, NULL, 0);
			if (user_param.num_of_qps < 1 || user_param.num_of_qps > 57) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'M' :
			mcg_params.is_user_mgid = 1;
			mcg_params.user_mgid = strdupa(optarg);
			break;
		case 'm':
			user_param.mtu = strtol(optarg, NULL, 0);
			break;
		case 'l':
			user_param.signal_comp = SIGNAL;
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

		case 'x':
			user_param.gid_index = strtol(optarg, NULL, 0);
			if (user_param.gid_index > 63) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 't':
			user_param.tx_depth = strtol(optarg, NULL, 0);
			user_param.rx_depth = user_param.tx_depth;
			if (user_param.tx_depth < 1) {
				usage(argv[0]); return 4;
			}
			break;

		case 'I':
			user_param.inline_size = strtol(optarg, NULL, 0);
			if (user_param.inline_size > MAX_INLINE) {
				usage(argv[0]);
				return 19;
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
			user_param.sl = strtol(optarg, NULL, 0);
			if (user_param.sl > 15) { usage(argv[0]); return 6; }
			break;

		default:
			usage(argv[0]);
			return 7;
		}
	}

	if (optind == argc - 1)
		servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 6;
	}

	user_param.machine = servername ? CLIENT : SERVER;

	printf(RESULT_LINE);

	if (user_param.use_mcg) { 
		user_param.connection_type = UD;
		printf("                    Send Latency Multicast Test\n");
	} else {
		printf("                    Send Latency Test\n");
	}

	 if (user_param.use_event) 
        printf(" Test with events.\n");

	if (user_param.connection_type==RC) {
		printf(" Connection type : RC\n");
	} else if (user_param.connection_type==UC) { 
		printf(" Connection type : UC\n");
	} else {
		printf(" Connection type : UD\n");
	}

	printf(" Inline data is used up to %d bytes message\n", user_param.inline_size);

	if (all == ALL) {
		/*since we run all sizes lets allocate big enough buffer */
		size = 8388608; /*2^23 */
	}

	srand48(getpid() * time(NULL));
	page_size 			  = sysconf(_SC_PAGESIZE);
	
	ib_dev = ctx_find_dev(ib_devname);
	if (!ib_dev)
		return 7;

	mcg_params.ib_devname = ibv_get_device_name(ib_dev);
	context               = ibv_open_device(ib_dev);

	// Configure the Link MTU acoording to the user or the active mtu.
	if (ctx_set_mtu(context,&user_param)) {
		fprintf(stderr, "Couldn't set the link layer\n");
		return 1;
	}

	if (user_param.connection_type == UD && size > MTU_SIZE(user_param.curr_mtu)) {	 
		printf(" Max msg size in UD is MTU - %d . changing to MTU\n",MTU_SIZE(user_param.curr_mtu));
		size = MTU_SIZE(user_param.curr_mtu);
	}

	ctx = pp_init_ctx(ib_dev,size,&user_param);
	if (!ctx)
		return 8;

	// Set up the Connection.
	if (set_up_connection(ctx,&user_param,&my_dest,&mcg_params)) {
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
    
	if (all == ALL) {

		if (user_param.connection_type==UD) {
			if (user_param.gid_index < 0 || user_param.use_mcg) {
				size_max_pow = 12;
			} else {
				size_max_pow = 11;
			}
		}

		for (i = 1; i < size_max_pow ; ++i) {
			size = 1 << i;
			if(run_iter(ctx, &user_param, &rem_dest, size))
				return 17;

			print_report(&report,user_param.iters,tstamp,size,no_cpu_freq_fail);

			if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
				fprintf(stderr,"Failed to exchange date between server and clients\n");
				return 1;
			}
		}
	} else {
		if(run_iter(ctx,&user_param,&rem_dest,size))
			return 18;	
		print_report(&report, user_param.iters, tstamp, size, no_cpu_freq_fail);
	}

	if (ctx_close_connection(&user_param,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}
	
	printf(RESULT_LINE);

	return destroy_ctx_resources(ctx,&user_param,&mcg_params);
}
