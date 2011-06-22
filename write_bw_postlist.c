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
#include <infiniband/verbs.h>

#include "get_clock.h"
#include "perftest_resources.h"

#define PINGPONG_RDMA_WRID	3
#define VERSION 1.3
#define ALL 1

static int page_size;

cycles_t	*tposted;
cycles_t	*tcompleted;
struct pingpong_context {
	struct ibv_context *context;
	struct ibv_pd      *pd;
	struct ibv_mr      *mr;
	struct ibv_cq      *cq;
	struct ibv_qp      **qp;
	void               *buf;
	unsigned            size;
	int                 tx_depth;
	struct ibv_sge      list;
    struct ibv_send_wr  wr;
    int                 *scnt;
    int                 *ccnt ;
};

/*
 * 
 */
static int set_up_connection(struct pingpong_context *ctx,
							 struct perftest_parameters *user_parm,
							 struct pingpong_dest *my_dest) {

	int i;
	int use_i = user_parm->gid_index;
	int port  = user_parm->ib_port;

	if (use_i != -1) {
		if (ibv_query_gid(ctx->context,port,use_i,&my_dest->gid)) {
			return -1;
		}
	}
	
	for (i=0; i < user_parm->num_of_qps; i++) {
		my_dest[i].lid   = ctx_get_local_lid(ctx->context,user_parm->ib_port);
		my_dest[i].qpn   = ctx->qp[i]->qp_num;
		my_dest[i].psn   = lrand48() & 0xffffff;
		my_dest[i].rkey  = ctx->mr->rkey;
		my_dest[i].vaddr = (uintptr_t)ctx->buf + ctx->size;

		// We do not fail test upon lid in RDMAoE/Eth conf.
		if (use_i < 0) {
			if (!my_dest[i].lid) {
				fprintf(stderr," Local lid 0x0 detected. Is an SM running? \n");
				fprintf(stderr," If you're running RMDAoE you must use GIDs\n");
				return -1;
			}
		}
	}
	return 0;
}

/*
 * 
 */
static int init_connection(struct perftest_parameters *params,
 						   struct pingpong_dest *my_dest) {

	int i;

	params->side      = LOCAL;

	for (i=0; i < params->num_of_qps; i++) {
		ctx_print_pingpong_data(&my_dest[i],params);
	}

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

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
											struct perftest_parameters *user_parm) 
{
	struct pingpong_context *ctx;
	int counter;

	ALLOCATE(ctx,struct pingpong_context,1);
	ALLOCATE(ctx->qp,struct ibv_qp*,user_parm->num_of_qps);
	ALLOCATE(ctx->scnt,int,user_parm->num_of_qps);
	ALLOCATE(ctx->ccnt,int,user_parm->num_of_qps);

	memset(ctx->scnt, 0, user_parm->num_of_qps * sizeof (int));
	memset(ctx->ccnt, 0, user_parm->num_of_qps * sizeof (int));

	ctx->size = user_parm->size;
	
	ctx->buf = memalign(page_size, user_parm->size * 2 * user_parm->num_of_qps);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0, user_parm->size * 2 * user_parm->num_of_qps);

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

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return NULL;
	}

	if (is_dev_hermon(ctx->context) != HERMON && user_parm->inline_size != 0)
		user_parm->inline_size = 0;

	printf(" Inline data is used up to %d bytes message\n", user_parm->inline_size);

	/* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
	 * The Consumer is not allowed to assign Remote Write or Remote Atomic to
	 * a Memory Region that has not been assigned Local Write. */
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, user_parm->size * 2 * user_parm->num_of_qps,
			     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return NULL;
	}

	// Creates the CQ according to ctx_cq_create in perfetst_resources.
	ctx->cq = ctx_cq_create(ctx->context,NULL,user_parm);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}


	for (counter = 0 ; counter < user_parm->num_of_qps ; counter++) {

		ctx->qp[counter] = ctx_qp_create(ctx->pd,ctx->cq,ctx->cq,user_parm);
		if (!ctx->qp[counter])  {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}

		if (ctx_modify_qp_to_init(ctx->qp[counter],user_parm)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			return NULL;
		}
	}
	return ctx;
}

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
	if (user_parm->connection_type==RC) {
		attr.max_dest_rd_atomic     = 1;
		attr.min_rnr_timer          = 12;
	}
	if (user_parm->gid_index < 0) {
		attr.ah_attr.is_global  = 0;
		attr.ah_attr.dlid       = dest->lid;
		attr.ah_attr.sl         = user_parm->sl;
	} else {
		attr.ah_attr.is_global  = 1;
		attr.ah_attr.grh.dgid   = dest->gid;
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

static void print_report(struct perftest_parameters *user_param) {

	double cycles_to_units;
	unsigned long tsize;	/* Transferred size, in megabytes */
	int i, j;
	int opt_posted = 0, opt_completed = 0;
	cycles_t opt_delta;
	cycles_t t;


	opt_delta = tcompleted[opt_posted] - tposted[opt_completed];

	/* Find the peak bandwidth */
	for (i = 0; i < user_param->iters * user_param->num_of_qps; ++i)
		for (j = i; j < user_param->iters * user_param->num_of_qps; ++j) {
			t = (tcompleted[j] - tposted[i]) / (j - i + 1);
			if (t < opt_delta) {
				opt_delta  = t;
				opt_posted = i;
				opt_completed = j;
            }
	}

	cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f) * 1000000;

	tsize = user_param->duplex ? 2 : 1;
	tsize = tsize * user_param->size;
	printf(REPORT_FMT,
	       (unsigned long)user_param->size,user_param->iters,tsize * cycles_to_units / opt_delta / 0x100000,
	       tsize * user_param->iters * user_param->num_of_qps * cycles_to_units /(tcompleted[(user_param->iters* user_param->num_of_qps) - 1] - tposted[0]) / 0x100000);
}
int run_iter(struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest)
{
    struct ibv_qp            *qp;
    int                      totscnt, totccnt ;
    int                      index , qpindex;
    int                      numpostperqp ;
    struct ibv_send_wr       *wrlist;
    struct ibv_send_wr       *bad_wr;
    struct ibv_wc             wc;

    wrlist = malloc(user_param->num_of_qps * sizeof (struct ibv_send_wr) * user_param->tx_depth);
    if (!wrlist) {
        perror("malloc");
        return -1;
    } 
    ctx->list.addr = (uintptr_t) ctx->buf;
	ctx->list.length = user_param->size;
	ctx->list.lkey = ctx->mr->lkey;
	
	/* prepare the wqe */
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_RDMA_WRITE;
	if (user_param->size > user_param->inline_size) {/* complaince to perf_main */
		ctx->wr.send_flags = IBV_SEND_SIGNALED;
	} else {
		ctx->wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
	}
	ctx->wr.next       = NULL;
	/*These should be the i'th place ... */
	ctx->wr.wr.rdma.remote_addr = rem_dest[0].vaddr;
	ctx->wr.wr.rdma.rkey = rem_dest[0].rkey;
    /* lets make the list with the right id's*/
	for (qpindex=0 ; qpindex < user_param->num_of_qps ; qpindex++) {
	  for (index =0 ; index <  user_param->tx_depth ; index++) {
	    wrlist[qpindex*user_param->tx_depth+index]=ctx->wr;
	    wrlist[qpindex*user_param->tx_depth+ index].wr_id = qpindex ;
	    if(index < user_param->tx_depth -1) {
	      wrlist[qpindex*user_param->tx_depth+index].next=&wrlist[qpindex*user_param->tx_depth+index+1];
	    } else {
	      wrlist[qpindex*user_param->tx_depth+index].next=NULL;
	    }
	  }
	}
	totscnt = 0;
	totccnt = 0;
	/*clear the scnt ccnt counters for each iteration*/
	for (index =0 ; index < user_param->num_of_qps ; index++) {
	  ctx->scnt[index] = 0;
	  ctx->ccnt[index] = 0;
	}
	index = 0;
	
	/* Done with setup. Start the test. */

	while (totscnt < (user_param->iters * user_param->num_of_qps)  || totccnt < (user_param->iters * user_param->num_of_qps) ) {
	  /* main loop to run over all the qps and post for each accumulated 40 wq's  */
	  for (qpindex =0 ; qpindex < user_param->num_of_qps ; qpindex++) {
	    qp = ctx->qp[qpindex];
	    if (user_param->iters > ctx->scnt[qpindex] ) {
            numpostperqp = user_param->tx_depth - (ctx->scnt[qpindex] - ctx->ccnt[qpindex]);
            if (numpostperqp > 40 || ((user_param->iters - ctx->scnt[qpindex]) <= 40 && numpostperqp > 0) ){
                wrlist[qpindex*user_param->tx_depth+numpostperqp-1].next=NULL;
                tposted[totscnt] = get_cycles();
                if (ibv_post_send(qp, &wrlist[qpindex*user_param->tx_depth], &bad_wr)) {
                    fprintf(stderr, "Couldn't post %d send: qp index = %d qp scnt=%d total scnt %d qp scnt=%d total ccnt=%d\n",
                            numpostperqp,qpindex,ctx->scnt[qpindex],totscnt,ctx->ccnt[qpindex],totccnt);
                    return 1;
                }
                ctx->scnt[qpindex]= ctx->scnt[qpindex]+numpostperqp;
                totscnt=totscnt + numpostperqp;
                wrlist[qpindex*user_param->tx_depth+numpostperqp-1].next=&wrlist[qpindex*user_param->tx_depth+numpostperqp];
            }
	    }
	    /*FINISHED POSTING  */
      }
      if (totccnt < (user_param->iters * user_param->num_of_qps) ) {
          int ne;
          do {
              ne = ibv_poll_cq(ctx->cq, 1, &wc);
          } while (ne == 0);
          tcompleted[totccnt] = get_cycles();
          if (ne < 0) {
              fprintf(stderr, "poll CQ failed %d\n", ne);
              return 1;
          }
          if (wc.status != IBV_WC_SUCCESS) {
              fprintf(stderr, "Completion wth error at %s:\n",user_param->machine == CLIENT ? "client" : "server");
              fprintf(stderr, "Failed status %d: wr_id %d\n",wc.status, (int) wc.wr_id);
              fprintf(stderr, "qp index %d ,qp scnt=%d, qp ccnt=%d total scnt %d total ccnt %d\n",
                      (int)wc.wr_id, ctx->scnt[(int)wc.wr_id], ctx->ccnt[(int)wc.wr_id], totscnt, totccnt);
              return 1;
          }
          /*here the id is the index to the qp num */
          ctx->ccnt[(int)wc.wr_id] = ctx->ccnt[(int)wc.wr_id]+1;
          totccnt += 1;
      }
	}
	free(wrlist);
	return(0);
}

int main(int argc, char *argv[]) {

	struct ibv_device	       *ib_dev;
	struct pingpong_context    *ctx;
	struct pingpong_dest       *my_dest,*rem_dest;
	struct perftest_parameters user_param;
	int                        i = 0;

	memset(&user_param, 0, sizeof(struct perftest_parameters));

	user_param.verb    = WRITE;
	user_param.tst     = BW;
	user_param.spec    = PL;
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
		return 1;

	ALLOCATE(my_dest , struct pingpong_dest , user_param.num_of_qps);
	memset(my_dest, 0, sizeof(struct pingpong_dest)*user_param.num_of_qps);
	ALLOCATE(rem_dest , struct pingpong_dest , user_param.num_of_qps);
	memset(rem_dest, 0, sizeof(struct pingpong_dest)*user_param.num_of_qps);

	// Set up the Connection.
	if (set_up_connection(ctx,&user_param,my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}	

	// Init the connection and print the local data.
	if (init_connection(&user_param,my_dest)) {
		fprintf(stderr," Unable to init the socket connection\n");
		return 1;
	}

	// Shaking hands and gather the other side info.
	user_param.side = REMOTE;
	for (i=0; i < user_param.num_of_qps; i++) {
		if (ctx_hand_shake(&user_param,&my_dest[i],&rem_dest[i])) {
			fprintf(stderr,"Failed to exchange date between server and clients\n");
			return 1;   
		}
		ctx_print_pingpong_data(&rem_dest[i],&user_param);

		if (pp_connect_ctx(ctx,my_dest[i].psn,&rem_dest[i],&user_param,i)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			return 1;
		}

		// An additional handshake is required after moving qp to RTR.
		if (ctx_hand_shake(&user_param,&my_dest[i],&rem_dest[i])) {
			fprintf(stderr,"Failed to exchange date between server and clients\n");
			return 1; 
		}
	}

	printf(RESULT_LINE);
	printf(RESULT_FMT);

	// For half duplex tests, server just waits for client to exit 
	if (user_param.machine == SERVER && !user_param.duplex) {
		if (ctx_close_connection(&user_param,&my_dest[0],&rem_dest[0])) {
			fprintf(stderr,"Failed to close connection between server and client\n");
			return 1;
		}
		printf(RESULT_LINE);
		return 0;
	}

	ALLOCATE(tposted,cycles_t,user_param.iters*user_param.num_of_qps);
	ALLOCATE(tcompleted,cycles_t,user_param.iters*user_param.num_of_qps);

	if (user_param.all == ON) {
		for (i = 1; i < 24 ; ++i) {
			user_param.size = 1 << i;
			if(run_iter(ctx,&user_param,rem_dest))
				return 17;
			print_report(&user_param);
			}
	} else {
		if(run_iter(ctx, &user_param, rem_dest))
			return 18;
		print_report(&user_param);
	}
	

	// Closing connection.
	if (ctx_close_connection(&user_param,&my_dest[0],&rem_dest[0])) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}

	free(tposted);
	free(tcompleted);
	free(my_dest);
	free(rem_dest);

	printf("------------------------------------------------------------------\n");
	return 0;
}
