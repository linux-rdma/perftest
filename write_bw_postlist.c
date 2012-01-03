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

#define VERSION 2.4
#define ALL 1

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

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param,
			 struct pingpong_dest *rem_dest)
{
    struct ibv_qp            *qp;
    int                      totscnt, totccnt ;
    int                      index , qpindex;
    int                      numpostperqp ;
	struct ibv_send_wr		 wr;
    struct ibv_send_wr       *wrlist;
    struct ibv_send_wr       *bad_wr;
	struct ibv_sge			 list;
    struct ibv_wc            wc;

    wrlist = malloc(user_param->num_of_qps * sizeof (struct ibv_send_wr) * user_param->tx_depth);
    if (!wrlist) {
        perror("malloc");
        return -1;
    } 
    list.addr = (uintptr_t) ctx->buf;
	list.length = user_param->size;
	list.lkey = ctx->mr->lkey;
	
	/* prepare the wqe */
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_RDMA_WRITE;
	if (user_param->size > user_param->inline_size) {/* complaince to perf_main */
		wr.send_flags = IBV_SEND_SIGNALED;
	} else {
		wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
	}
	wr.next       = NULL;
	/*These should be the i'th place ... */
	wr.wr.rdma.remote_addr = rem_dest[0].vaddr;
	wr.wr.rdma.rkey = rem_dest[0].rkey;
    /* lets make the list with the right id's*/
	for (qpindex=0 ; qpindex < user_param->num_of_qps ; qpindex++) {
	  for (index =0 ; index <  user_param->tx_depth ; index++) {
	    wrlist[qpindex*user_param->tx_depth+index]=wr;
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
              ne = ibv_poll_cq(ctx->send_cq, 1, &wc);
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

/****************************************************************************** 
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	struct ibv_device	       *ib_dev;
	struct pingpong_context    ctx;
	struct pingpong_dest       *my_dest,*rem_dest;
	struct perftest_parameters user_param;
	struct perftest_comm	   user_comm;
	int                        i = 0;

	memset(&ctx,0,sizeof(struct pingpong_context));
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	memset(&user_comm,0,sizeof(struct perftest_comm));

	user_param.verb    = WRITE;
	user_param.tst     = BW;
	user_param.spec    = PL;
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
		return 0;
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
	return 0;
}

