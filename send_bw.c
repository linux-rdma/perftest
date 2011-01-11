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

#define VERSION 2.0
#define SIGNAL 1

static int sl;
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
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,unsigned size,
											struct perftest_parameters *user_parm) {

	int i,m_size;
	int duplex_ind;
	struct pingpong_context *ctx;

	ALLOCATE(ctx,struct pingpong_context,1);
	ALLOCATE(ctx->buf,void*,user_parm->num_of_qps);
	ALLOCATE(ctx->mr,struct ibv_mr*,user_parm->num_of_qps);

	ctx->ah       = NULL;
	ctx->channel  = NULL;
	ctx->size     = size;

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

		m_size = (BUFF_SIZE(size) + IF_UD_ADD(user_parm->connection_type))*duplex_ind;
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
		attr.ah_attr.sl         = sl;
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
static int set_recv_wqes(struct pingpong_context *ctx,int size,
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

		ctx->sge_list[i].length = SIZE(user_param->connection_type,size);
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

			if (size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(&ctx->sge_list[i],size,j,ctx->my_addr[i],user_param->connection_type);
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

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>           Listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>          Use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>        Use port <port> of IB device (default 1)\n");
	printf("  -c, --connection=<RC/UC/UD> Connection type RC/UC/UD (default RC)\n");
	printf("  -m, --mtu=<mtu>             Mtu size (256 - 4096. default for hermon is 2048)\n");
	printf("  -s, --size=<size>           Size of message to exchange (default 65536)\n");
	printf("  -a, --all                   Run sizes from 2 till 2^23\n");
	printf("  -t, --tx-depth=<dep>        Size of tx queue (default 300)\n");
	printf("  -r, --rx-depth=<dep>        Make rx queue bigger than tx (default 600)\n");
	printf("  -n, --iters=<iters>         Number of exchanges (at least 2, default 1000)\n");
	printf("  -I, --inline_size=<size>    Max size of message to be sent in inline mode (default 0)\n");
	printf("  -u, --qp-timeout=<timeout>  QP timeout, timeout value is 4 usec * 2 ^(timeout), default 14\n");
	printf("  -S, --sl=<sl>               SL (default 0)\n");
	printf("  -x, --gid-index=<index>     Test uses GID with GID index taken from command line (for RDMAoE index should be 0)\n");
	printf("  -b, --bidirectional         Measure bidirectional bandwidth (default unidirectional)\n");
	printf("  -V, --version               Display version number\n");
	printf("  -e, --events                Sleep on CQ events (default poll)\n");
	printf("  -N, --no peak-bw            Cancel peak-bw calculation (default with peak-bw)\n");
	printf("  -F, --CPU-freq              Do not fail even if cpufreq_ondemand module is loaded\n");
	printf("  -g, --mcg=<num_of_qps>      Send messages to multicast group with <num_of_qps> qps attached to it.\n");
	printf("  -M, --MGID=<multicast_gid>  In case of multicast, uses <multicast_gid> as the group MGID.\n");
	printf("                              The format must be '255:1:X:X:X:X:X:X:X:X:X:X:X:X:X:X', where X is a vlaue within [0,255].\n");
}

/****************************************************************************** 
 *
 ******************************************************************************/
static void print_report(unsigned int iters, unsigned size, int duplex,
			 cycles_t *tposted, cycles_t *tcompleted, int noPeak, int no_cpu_freq_fail)
{
	double cycles_to_units;
	unsigned long tsize;	/* Transferred size, in megabytes */
	int i, j;
	int opt_posted = 0, opt_completed = 0;
	cycles_t opt_delta;
	cycles_t t;


	opt_delta = tcompleted[opt_posted] - tposted[opt_completed];

	if (!noPeak) {
		/* Find the peak bandwidth, unless asked not to in command line */
		for (i = 0; i < iters; ++i)
			for (j = i; j < iters; ++j) {
				t = (tcompleted[j] - tposted[i]) / (j - i + 1);
				if (t < opt_delta) {
					opt_delta  = t;
					opt_posted = i;
					opt_completed = j;
				}
			}
	}

	cycles_to_units = get_cpu_mhz(no_cpu_freq_fail) * 1000000;

	tsize = duplex ? 2 : 1;
	tsize = tsize * size;
	printf(REPORT_FMT,size,iters,!(noPeak) * tsize * cycles_to_units / opt_delta / 0x100000,
	       tsize * iters * cycles_to_units /(tcompleted[iters - 1] - tposted[0]) / 0x100000);
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
				struct perftest_parameters *user_param,int size)  {

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
	ctx->list.length = size;
	ctx->list.addr   = (uintptr_t)ctx->buf[0];
	
	if (size <= user_param->inline_size) 
		ctx->wr.send_flags |= IBV_SEND_INLINE; 

	while (ccnt < user_param->iters || rcnt < user_param->iters) {
                
		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth / 2) {

			tposted[scnt] = get_cycles();
			if (ibv_post_send(ctx->qp[0],&ctx->wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}

			if (size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(&ctx->list,size,scnt,(uintptr_t)ctx->buf[0],0);

			++scnt;
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
						tcompleted[ccnt++] = get_cycles();
					}

					else {

						rcnt_for_qp[wc[i].wr_id]++;
						rcnt++;
						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id , rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						if (size <= (CYCLE_BUFFER / 2))
							increase_loc_addr(&ctx->sge_list[wc[i].wr_id],
							  size,rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth - 1,
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
	
	if (size <= user_param->inline_size) 
		ctx->wr.send_flags &= ~IBV_SEND_INLINE;
	
	free(rcnt_for_qp);
	free(wc);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter_uni_server(struct pingpong_context *ctx, 
						struct perftest_parameters *user_param,int size) {

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

					if (size <= (CYCLE_BUFFER / 2))
						increase_loc_addr(&ctx->sge_list[wc[i].wr_id],size,
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
						struct perftest_parameters *user_param,int size) {

	int 		       ne;
	int 			   i    = 0;
	int                scnt = 0;
	int                ccnt = 0;
	struct ibv_wc      *wc     = NULL;
	struct ibv_send_wr *bad_wr = NULL;

	ALLOCATE(wc,struct ibv_wc,DEF_WC_SIZE);

	// Set the lenght of the scatter in case of ALL option.
	ctx->list.length = size;
	ctx->list.addr   = (uintptr_t)ctx->buf[0];

	if (size <= user_param->inline_size) 
		ctx->wr.send_flags |= IBV_SEND_INLINE; 
	

	while (scnt < user_param->iters || ccnt < user_param->iters) {
		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth ) {

			tposted[scnt] = get_cycles();
			if (ibv_post_send(ctx->qp[0], &ctx->wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}

			if (size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(&ctx->list,size,scnt,(uintptr_t)ctx->buf[0],0);

			scnt++;
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

						tcompleted[ccnt++] = get_cycles();
					}	
				}
                         
					
			} while (ne > 0);

			if (ne < 0) {
				fprintf(stderr, "poll CQ failed\n");
				return 1;
			}
		}
	}

	if (size <= user_param->inline_size) 
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
	struct ibv_device_attr 		device_attribute;
	char                    	*ib_devname = NULL;
	long long                	size = 65536;
	int                      	i = 0;
	int                      	size_max_pow = 24;
	int                      	noPeak = 0;
	int                      	inline_given_in_cmd = 0;
	struct ibv_context       	*context;
	int                      	no_cpu_freq_fail = 0;
	int 						all = 0;
	int							size_of_arr;
	const char 					*servername = NULL;

	// Pointer to The relevent function of run_iter according to machine type.
	int (*ptr_to_run_iter_uni)(struct pingpong_context*,struct perftest_parameters*,int);

	/* init default values to user's parameters */
	memset(&user_param, 0 , sizeof(struct perftest_parameters));
	memset(&mcg_params, 0 , sizeof(struct mcast_parameters));
	memset(&my_dest   , 0 , sizeof(struct pingpong_dest));
	memset(&rem_dest   , 0 , sizeof(struct pingpong_dest));

	user_param.ib_port = 1;
	user_param.port = 18515;
	user_param.iters = 1000;
	user_param.tx_depth = 300;
	user_param.rx_depth = 600;
	user_param.qp_timeout = 14;
	user_param.gid_index = -1; 
	user_param.verb = SEND;
	user_param.num_of_qps = 1;

	/* Parameter parsing. */
	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",           .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",         .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",        .has_arg = 1, .val = 'i' },
			{ .name = "mtu",            .has_arg = 1, .val = 'm' },
			{ .name = "connection",     .has_arg = 1, .val = 'c' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "inline_size",    .has_arg = 1, .val = 'I' },
			{ .name = "rx-depth",       .has_arg = 1, .val = 'r' },
			{ .name = "qp-timeout",     .has_arg = 1, .val = 'u' },
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
			{ .name = "gid-index",      .has_arg = 1, .val = 'x' },
			{ .name = "MGID",           .has_arg = 1, .val = 'M' },
			{ .name = "all",            .has_arg = 0, .val = 'a' },
			{ .name = "bidirectional",  .has_arg = 0, .val = 'b' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "events",         .has_arg = 0, .val = 'e' },
			{ .name = "noPeak",         .has_arg = 0, .val = 'N' },
			{ .name = "CPU-freq",       .has_arg = 0, .val = 'F' },
			{ .name = "mcg",            .has_arg = 1, .val = 'g' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:m:c:s:n:t:I:r:u:S:x:g:q:M:ebaVNF", long_options, NULL);
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
			user_param.use_event++;
			break;
        case 'g':
			user_param.use_mcg++;
			user_param.num_of_qps = strtol(optarg, NULL, 0);
            if (user_param.num_of_qps < 1 || user_param.num_of_qps > 56) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'M' :
			mcg_params.is_user_mgid = 1;
			mcg_params.user_mgid = strdupa(optarg);
			break;
		case 'd':
			ib_devname = strdupa(optarg);
			break;
		case 'c':
			if (strcmp("UC",optarg)==0)
				user_param.connection_type=UC;
			if (strcmp("UD",optarg)==0)
				user_param.connection_type=UD;
			break;
		case 'm':
			user_param.mtu = strtol(optarg, NULL, 0);
			break;
		case 'a':
			all = ALL;
			break;
		case 'V':
			printf("send_bw version : %.2f\n",VERSION);
			return 0;
			break;
		case 'i':
			user_param.ib_port = strtol(optarg, NULL, 0);
			if (user_param.ib_port < 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 's':
			size = strtoll(optarg, NULL, 0);
			if (size < 1 || size > UINT_MAX / 2) {
				usage(argv[0]);
				return 1;
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
			if (user_param.tx_depth < 1) { usage(argv[0]); return 1; }
			break;

		case 'I':
			user_param.inline_size = strtol(optarg, NULL, 0);
			inline_given_in_cmd =1;
			if (user_param.inline_size > MAX_INLINE) {
				usage(argv[0]);
				return 7;
			}
			break;

		case 'r':
			errno = 0;
			user_param.rx_depth = strtol(optarg, NULL, 0);
			if (errno) { usage(argv[0]); return 1; }
			break;

		case 'n':
			user_param.iters = strtol(optarg, NULL, 0);
			if (user_param.iters < 2) {
				usage(argv[0]);
				return 1;
			}

			break;

		case 'b':
			user_param.duplex = 1;
			break;

		case 'N':
			noPeak = 1;
			break;

		case 'F':
			no_cpu_freq_fail = 1;
			break;

		case 'u':
			user_param.qp_timeout = strtol(optarg, NULL, 0);
			break;

		case 'S':
			sl = strtol(optarg, NULL, 0);
			if (sl > 15) { usage(argv[0]); return 1; }
			break;

		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind == argc - 1) {
		servername = strdupa(argv[optind]);
	}
	
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	printf(RESULT_LINE);
	user_param.rx_depth = (user_param.iters < user_param.rx_depth) ? user_param.iters : user_param.rx_depth ;

	user_param.machine = servername ? CLIENT : SERVER;
    if (user_param.use_mcg) {

		user_param.connection_type = UD;
		if (user_param.duplex) {
			user_param.num_of_qps++;
			printf("                    Send Bidirectional BW  -  Multicast Test\n");
		}
		else {
			printf("                    Send BW  -  Multicast Test\n");
			if (user_param.machine == CLIENT)
				user_param.num_of_qps = 1;
		}
    }

	else if (user_param.duplex) {
		    printf("                    Send Bidirectional BW Test\n");
	} else 
		    printf("                    Send BW Test\n");

	if (user_param.use_event) 
		printf(" Test with events.\n");

	if (user_param.connection_type == RC)
		printf(" Connection type : RC\n");
	else if (user_param.connection_type == UC)
		printf(" Connection type : UC\n");
	else{
		printf(" Connection type : UD\n");
	}
	
	// Done with parameter parsing. Perform setup.
	if (all == ALL) {
		// since we run all sizes 
		size = 8388608;
	}

	srand48(getpid() * time(NULL));

	page_size = sysconf(_SC_PAGESIZE);

	ib_dev = ctx_find_dev(ib_devname);
	if (!ib_dev)
		return 7;

	if (user_param.use_mcg) 
		mcg_params.ib_devname = ibv_get_device_name(ib_dev);

	// Should be a function over here that computes the inline.
	context = ibv_open_device(ib_dev);
	
	if (ibv_query_device(context, &device_attribute)) {
		fprintf(stderr, "Failed to query device props");
		return 1;
	}
	if ((device_attribute.vendor_part_id == 25408  ||
		 device_attribute.vendor_part_id == 25418  ||
		 device_attribute.vendor_part_id == 26408  ||
         device_attribute.vendor_part_id == 26468  ||  // Mountain Top.
		 device_attribute.vendor_part_id == 26418  ||
		 device_attribute.vendor_part_id == 26428) &&  (!inline_given_in_cmd)) {

            user_param.inline_size = 0;
	}
	printf(" Inline data is used up to %d bytes message\n", user_param.inline_size);

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
		return 1;

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
		noPeak = 1;
	}

	if (user_param.machine == CLIENT || user_param.duplex) {
		set_send_wqe(ctx,rem_dest.qpn,&user_param);
	}

	if (all == ALL) {
		if (user_param.connection_type == UD) {
			if (user_param.gid_index < 0 || user_param.use_mcg) {
				size_max_pow = 12;
			} else {
				size_max_pow = 11;
			}
		}

		for (i = 1; i < size_max_pow ; ++i) {
			size = 1 << i;

			if (user_param.machine == SERVER || user_param.duplex) {
				if (set_recv_wqes(ctx,size,&user_param)) {
					fprintf(stderr," Failed to post receive recv_wqes\n");
					return 1;
				}
			}

			if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
				fprintf(stderr,"Failed to exchange date between server and clients\n");
				return 1;
			}

			if (user_param.duplex) {
				if(run_iter_bi(ctx,&user_param,size))
					return 17;
			} else {
				if((*ptr_to_run_iter_uni)(ctx,&user_param,size))
					return 17;
			}
			print_report(user_param.iters, size, user_param.duplex, tposted, tcompleted, noPeak, no_cpu_freq_fail);

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
			if (set_recv_wqes(ctx,size,&user_param)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				return 1;
			}
		}

		if (ctx_hand_shake(&user_param,&my_dest,&rem_dest)) {
			fprintf(stderr,"Failed to exchange date between server and clients\n");
			return 1;
		}

		if (user_param.duplex) {
			if(run_iter_bi(ctx,&user_param,size))
				return 18;

		} else {
			if((*ptr_to_run_iter_uni)(ctx,&user_param,size))
				return 18;
		}

		print_report(user_param.iters, size, user_param.duplex, tposted, tcompleted, noPeak, no_cpu_freq_fail);	
	}
		
	if (ctx_close_connection(&user_param,&my_dest,&rem_dest)) {
		fprintf(stderr," Failed to close connection between server and client\n");
		return 1;
	}

	printf(RESULT_LINE);
	return destroy_ctx_resources(ctx,&user_param,&my_dest,&rem_dest,&mcg_params);
}
