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
#define VERSION 1.1
#define ALL 1

static int sl = 0;
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
 						   struct pingpong_dest *my_dest,
						   const char *servername) {

	int i;

	params->machine   = servername ? CLIENT : SERVER;
	params->side      = LOCAL;

	for (i=0; i < params->num_of_qps; i++) {
		ctx_print_pingpong_data(&my_dest[i],params);
	}

	if (servername) 
		params->sockfd = ctx_client_connect(servername,params->port);
	else 
		params->sockfd = ctx_server_connect(params->port);

	if(params->sockfd < 0) {
		fprintf(stderr,"Unable to open file descriptor for socket connection");
		return 1;
	}
	return 0;
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,unsigned size,
											struct perftest_parameters *user_parm)
{
	struct pingpong_context *ctx;
	int counter;

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;
	ctx->qp = malloc(sizeof (struct ibv_qp*) * user_parm->num_of_qps );
	ctx->size     = size;
	ctx->tx_depth = user_parm->tx_depth;
	ctx->scnt = malloc(user_parm->num_of_qps * sizeof (int));
	if (!ctx->scnt) {
		perror("malloc");
		return NULL;
	}
	ctx->ccnt = malloc(user_parm->num_of_qps * sizeof (int));
	if (!ctx->ccnt) {
		perror("malloc");
		return NULL;
	}
	memset(ctx->scnt, 0, user_parm->num_of_qps * sizeof (int));
	memset(ctx->ccnt, 0, user_parm->num_of_qps * sizeof (int));
	
	ctx->buf = memalign(page_size, size * 2 * user_parm->num_of_qps  );
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0, size * 2 * user_parm->num_of_qps);

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

	/* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
	 * The Consumer is not allowed to assign Remote Write or Remote Atomic to
	 * a Memory Region that has not been assigned Local Write. */
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size * 2 * user_parm->num_of_qps,
			     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return NULL;
	}

	ctx->cq = ibv_create_cq(ctx->context,user_parm->tx_depth*user_parm->num_of_qps,NULL,NULL,0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}
	for (counter =0 ; counter < user_parm->num_of_qps ; counter++)
	{
		struct ibv_qp_init_attr initattr;
		struct ibv_qp_attr attr;
		memset(&initattr, 0, sizeof(struct ibv_qp_init_attr));
		initattr.send_cq = ctx->cq;
		initattr.recv_cq = ctx->cq;
		initattr.cap.max_send_wr  = user_parm->tx_depth;
		/* Work around:  driver doesnt support
		 * recv_wr = 0 */
		initattr.cap.max_recv_wr  = 1;
		initattr.cap.max_send_sge = 1;
		initattr.cap.max_recv_sge = 1;
		initattr.cap.max_inline_data = user_parm->inline_size;

		if (user_parm->connection_type == 1) {
			initattr.qp_type = IBV_QPT_UC;
		} else {
			initattr.qp_type = IBV_QPT_RC;
		}
		ctx->qp[counter] = ibv_create_qp(ctx->pd, &initattr);
		if (!ctx->qp[counter])  {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}
	
		attr.qp_state        = IBV_QPS_INIT;
		attr.pkey_index      = 0;
		attr.port_num        = user_parm->ib_port;
		attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;

		if (ibv_modify_qp(ctx->qp[counter], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_ACCESS_FLAGS)) {
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
		attr.ah_attr.sl         = sl;
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

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>         listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>        use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>      use port <port> of IB device (default 1)\n");
	printf("  -c, --connection=<RC/UC>  connection type RC/UC (default RC)\n");
	printf("  -m, --mtu=<mtu>           mtu size (256 - 4096. default for hermon is 2048)\n");
	printf("  -g, --post=<num of posts> number of posts for each qp in the chain (default tx_depth)\n");
	printf("  -q, --qp=<num of qp's>    Num of qp's(default 1)\n");
	printf("  -s, --size=<size>         size of message to exchange (default 65536)\n");
	printf("  -a, --all                 Run sizes from 2 till 2^23\n");
	printf("  -t, --tx-depth=<dep>      size of tx queue (default 100)\n");
	printf("  -n, --iters=<iters>       number of exchanges (at least 2, default 5000)\n");
	printf("  -I, --inline_size=<size>  max size of message to be sent in inline mode (default 400)\n");
	printf("  -u, --qp-timeout=<timeout> QP timeout, timeout value is 4 usec * 2 ^(timeout), default 14\n");
	printf("  -S, --sl=<sl>             SL (default 0)\n");
	printf("  -x, --gid-index=<index>   test uses GID with GID index taken from command line (for RDMAoE index should be 0)\n");
	printf("  -b, --bidirectional       measure bidirectional bandwidth (default unidirectional)\n");
	printf("  -V, --version             display version number\n");
	printf("  -F, --CPU-freq            do not fail even if cpufreq_ondemand module is loaded\n");
}

static void print_report(unsigned size, int duplex,
			 cycles_t *tposted, cycles_t *tcompleted, struct perftest_parameters *user_param, int no_cpu_freq_fail)
{
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

	cycles_to_units = get_cpu_mhz(no_cpu_freq_fail) * 1000000;

	tsize = duplex ? 2 : 1;
	tsize = tsize * size;
	printf("%7d        %d            %7.2f               %7.2f\n",
	       size,user_param->iters,tsize * cycles_to_units / opt_delta / 0x100000,
	       tsize * user_param->iters * user_param->num_of_qps * cycles_to_units /(tcompleted[(user_param->iters* user_param->num_of_qps) - 1] - tposted[0]) / 0x100000);
}
int run_iter(struct pingpong_context *ctx, struct perftest_parameters *user_param,
	     struct pingpong_dest *rem_dest, int size,int maxpostsofqpiniteration)
{
    struct ibv_qp           *qp;
    int                      totscnt, totccnt ;
    int                      index , qpindex;
    int                      numpostperqp ;
    struct ibv_send_wr  *wrlist;
    struct ibv_send_wr *bad_wr;
    struct ibv_wc wc;

    wrlist = malloc(user_param->num_of_qps * sizeof (struct ibv_send_wr) * user_param->tx_depth);
    if (!wrlist) {
        perror("malloc");
        return -1;
    } 
    ctx->list.addr = (uintptr_t) ctx->buf;
	ctx->list.length = size;
	ctx->list.lkey = ctx->mr->lkey;
	
	/* prepare the wqe */
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_RDMA_WRITE;
	if (size > user_param->inline_size) {/* complaince to perf_main */
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
	  for (index =0 ; index <  maxpostsofqpiniteration ; index++) {
	    wrlist[qpindex*maxpostsofqpiniteration+index]=ctx->wr;
	    wrlist[qpindex*maxpostsofqpiniteration+ index].wr_id      = qpindex ;
	    if(index < maxpostsofqpiniteration -1) {
	      wrlist[qpindex*maxpostsofqpiniteration+index].next=&wrlist[qpindex*maxpostsofqpiniteration+index+1];
	    } else {
	      wrlist[qpindex*maxpostsofqpiniteration+index].next=NULL;
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
            numpostperqp = maxpostsofqpiniteration - (ctx->scnt[qpindex] - ctx->ccnt[qpindex]);
            if (numpostperqp > 40 || ((user_param->iters - ctx->scnt[qpindex]) <= 40 && numpostperqp > 0) ){
                wrlist[qpindex*maxpostsofqpiniteration+numpostperqp-1].next=NULL;
                tposted[totscnt] = get_cycles();
                if (ibv_post_send(qp, &wrlist[qpindex*maxpostsofqpiniteration], &bad_wr)) {
                    fprintf(stderr, "Couldn't post %d send: qp index = %d qp scnt=%d total scnt %d qp scnt=%d total ccnt=%d\n",
                            numpostperqp,qpindex,ctx->scnt[qpindex],totscnt,ctx->ccnt[qpindex],totccnt);
                    return 1;
                }
                ctx->scnt[qpindex]= ctx->scnt[qpindex]+numpostperqp;
                totscnt=totscnt + numpostperqp;
                wrlist[qpindex*maxpostsofqpiniteration+numpostperqp-1].next=&wrlist[qpindex*maxpostsofqpiniteration+numpostperqp];
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

int main(int argc, char *argv[])
{
	struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest    *my_dest,*rem_dest;
	struct perftest_parameters user_param;
	struct ibv_device_attr device_attribute;
	char                    *ib_devname = NULL;
	long long                size = 65536;
	int                      duplex = 0;
	int                      i = 0;
	int                      inline_given_in_cmd = 0;
	struct ibv_context       *context;
	int                      no_cpu_freq_fail = 0;

	int all = 0;	
	const char *servername = NULL;
	int maxpostsofqpiniteration = 100;

	/* init default values to user's parameters */
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	user_param.mtu = 0;
	user_param.iters = 5000;
	user_param.tx_depth = 100;
	user_param.port = 18515;
	user_param.ib_port = 1;
	user_param.num_of_qps = 1;
	user_param.inline_size = MAX_INLINE;
	user_param.qp_timeout = 14;
	user_param.gid_index = -1; /*gid will not be used*/
	/* Parameter parsing. */
	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",           .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",         .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",        .has_arg = 1, .val = 'i' },
			{ .name = "mtu",            .has_arg = 1, .val = 'm' },
			{ .name = "qp",             .has_arg = 1, .val = 'q' },
			{ .name = "post",           .has_arg = 1, .val = 'g' },
			{ .name = "connection",     .has_arg = 1, .val = 'c' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "inline_size",    .has_arg = 1, .val = 'I' },
			{ .name = "qp-timeout",     .has_arg = 1, .val = 'u' },
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
			{ .name = "gid-index",      .has_arg = 1, .val = 'x' },
			{ .name = "all",            .has_arg = 0, .val = 'a' },
			{ .name = "bidirectional",  .has_arg = 0, .val = 'b' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "CPU-freq",       .has_arg = 0, .val = 'F' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:m:q:g:c:s:n:t:I:u:S:x:baVF", long_options, NULL);
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

		case 'd':
			ib_devname = strdupa(optarg);
			break;
		case 'c':
			if (strcmp("UC",optarg)==0)
				user_param.connection_type=UC;
			break;

		case 'm':
			user_param.mtu = strtol(optarg, NULL, 0);
			break;
		case 'q':
			user_param.num_of_qps = strtol(optarg, NULL, 0);
			break;
		case 'g':
			maxpostsofqpiniteration = strtol(optarg, NULL, 0);
			break;
		case 'a':
			all = ALL;
			break;
		case 'V':
			printf("rdma_bw version : %.2f\n",VERSION);
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

		case 'n':
			user_param.iters = strtol(optarg, NULL, 0);
			if (user_param.iters < 2) {
				usage(argv[0]);
				return 1;
			}

			break;

		case 'b':
			duplex = 1;
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

		case 'x':
			user_param.gid_index = strtol(optarg, NULL, 0);
			if (user_param.gid_index > 63) {
				usage(argv[0]);
				return 1;
			}
			break;

		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind == argc - 1)
		servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}
	
	printf("------------------------------------------------------------------\n");
	if (duplex == 1) {
	  printf("                    RDMA_Write Bidirectional Post List BW Test\n");
	} else {
	  printf("                    RDMA_Write Post List BW Test\n");
	}
	
	printf("Number of qp's running %d\n",user_param.num_of_qps);
	if (user_param.connection_type==RC) {
		printf("Connection type : RC\n");
	} else {
		printf("Connection type : UC\n");
	}
	if (maxpostsofqpiniteration > user_param.tx_depth ) {
	  printf("Can not post more than tx_depth , adjusting number of post to tx_depth\n");
	  maxpostsofqpiniteration = user_param.tx_depth;
	} else {
	  printf("Each Qp will post %d messages each time\n",maxpostsofqpiniteration);
	}
		if (user_param.gid_index > -1) {
		printf("Using GID to support RDMAoE configuration. Refer to port type as Ethernet, default MTU 1024B\n");
	}
	/* Done with parameter parsing. Perform setup. */
	if (all == ALL) {
		/*since we run all sizes */
		size = 8388608; /*2^23 */
	}
	srand48(getpid() * time(NULL));

	page_size = sysconf(_SC_PAGESIZE);

	dev_list = ibv_get_device_list(NULL);

	if (!ib_devname) {
		ib_dev = dev_list[0];
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return 1;
		}
	} else {
		for (; (ib_dev = *dev_list); ++dev_list)
			if (!strcmp(ibv_get_device_name(ib_dev), ib_devname))
				break;
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}

	context = ibv_open_device(ib_dev);
	if (ibv_query_device(context, &device_attribute)) {
		fprintf(stderr, "Failed to query device props");
		return 1;
	}
	if ((device_attribute.vendor_part_id == 25408 ||
		device_attribute.vendor_part_id == 25418 ||
		device_attribute.vendor_part_id == 26408 ||
		device_attribute.vendor_part_id == 26418 ||
		device_attribute.vendor_part_id == 26428) && (!inline_given_in_cmd)) {
		user_param.inline_size = 1;
        }
	printf("Inline data is used up to %d bytes message\n", user_param.inline_size);

	ctx = pp_init_ctx(ib_dev,size,&user_param);
	if (!ctx)
		return 1;

	my_dest  = malloc(sizeof(struct pingpong_dest)*user_param.num_of_qps);
	rem_dest = malloc(sizeof(struct pingpong_dest)*user_param.num_of_qps);
    if (!my_dest || !rem_dest) {
		perror("malloc my_dest or rem_dest");
		return 1;
	}

	// Set up the Connection.
	if (set_up_connection(ctx,&user_param,my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}	

	// Init the connection and print the local data.
	if (init_connection(&user_param,my_dest,servername)) {
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

	printf("------------------------------------------------------------------\n");
	printf(" #bytes #iterations    BW peak[MB/sec]    BW average[MB/sec]  \n");

	// For half duplex tests, server just waits for client to exit 
	if (!servername && !duplex) {
		if (ctx_close_connection(&user_param,&my_dest[0],&rem_dest[0])) {
			fprintf(stderr,"Failed to close connection between server and client\n");
			return 1;
		}
		return 0;
	}

	tposted = malloc(user_param.iters * user_param.num_of_qps * sizeof *tposted);

	if (!tposted) {
		perror("malloc");
		return 1;
	}

	tcompleted = malloc(user_param.iters * user_param.num_of_qps * sizeof *tcompleted);

	if (!tcompleted) {
		perror("malloc");
		return 1;
	}

	if (all == ALL) {
		for (i = 1; i < 24 ; ++i) {
			size = 1 << i;
			if(run_iter(ctx, &user_param, rem_dest, size,maxpostsofqpiniteration))
				return 17;
			print_report(size, duplex, tposted, tcompleted,&user_param, no_cpu_freq_fail);
			}
	} else {
		if(run_iter(ctx, &user_param, rem_dest, size,maxpostsofqpiniteration))
			return 18;
		print_report(size, duplex, tposted, tcompleted,&user_param, no_cpu_freq_fail);
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
