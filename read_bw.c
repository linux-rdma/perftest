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
#include <limits.h>
#include <malloc.h>
#include <getopt.h>
#include <time.h>
#include <infiniband/verbs.h>

#include "get_clock.h"
#include "perftest_resources.h"

#define MAX_OUT_READ_HERMON 4
#define MAX_OUT_READ 4
#define PINGPONG_READ_WRID	1
#define VERSION 1.3
#define ALL 1
#define RC 0

struct user_parameters {
	const char  *servername;
	int connection_type;
	int mtu;
	int all; /* run all msg size */
	int iters;
	int tx_depth;
	int max_out_read;
	int use_out_read;
	int use_event;
	int qp_timeout;
	int gid_index; /* if value not negative, we use gid AND gid_index=value */
	int ib_port;
	int port;
};
static int sl = 0;
static int page_size;
cycles_t	*tposted;
cycles_t	*tcompleted;
struct pingpong_context {
	struct ibv_context *context;
	struct ibv_comp_channel *channel;
	struct ibv_pd      *pd;
	struct ibv_mr      *mr;
	struct ibv_cq      *cq;
	struct ibv_qp      *qp;
	void               *buf;
	unsigned            size;
	int                 tx_depth;
	struct ibv_sge      list;
	struct ibv_send_wr  wr;
	union ibv_gid       dgid;
};

/*
 *
 */
static int set_max_out_read(struct user_parameters *param,struct ibv_device_attr *attr) {

	int is_hermon = 0;
	int max_out_reads;

	// Checks the devide type for setting the max outstanding reads.
	if (attr->vendor_part_id == 25408  || attr->vendor_part_id == 25418  ||
		attr->vendor_part_id == 25448  || attr->vendor_part_id == 26418  || 
		attr->vendor_part_id == 26428  || attr->vendor_part_id == 26438  ||
		attr->vendor_part_id == 26448  || attr->vendor_part_id == 26458  ||
		attr->vendor_part_id == 26468  || attr->vendor_part_id == 26478) {
			is_hermon = 1;		
	}

	max_out_reads = (is_hermon == 1) ? MAX_OUT_READ_HERMON : MAX_OUT_READ;

	if (param->use_out_read) {
		if (param->max_out_read <= 0 || param->max_out_read > max_out_reads) {
			fprintf(stderr,"Max outstanding reads for this device is %d\n",max_out_reads);
			return 1;
		}
	}
	else {
		param->max_out_read = max_out_reads;         
	}
	return 0;
}

/*
 * 
 */
static int set_up_connection(struct pingpong_context *ctx,
							 struct user_parameters *user_parm,
							 struct pingpong_dest *my_dest) {

	int use_i = user_parm->gid_index;
	int port  = user_parm->ib_port;

	if (use_i != -1) {
		if (ibv_query_gid(ctx->context,port,use_i,&my_dest->dgid)) {
			return -1;
		}
	}
	my_dest->lid   = ctx_get_local_lid(ctx->context,user_parm->ib_port);
	my_dest->qpn   = ctx->qp->qp_num;
	my_dest->psn   = lrand48() & 0xffffff;
	my_dest->rkey  = ctx->mr->rkey;
	my_dest->vaddr = (uintptr_t)ctx->buf + ctx->size;

	// We do not fail test upon lid in RDMAoE/Eth conf.
	if (use_i < 0) {
		if (!my_dest->lid) {
			fprintf(stderr,"Local lid 0x0 detected. Is an SM running? \n");
			fprintf(stderr,"If you're running RMDAoE you must use GIDs\n");
			return -1;
		}
	}
	return 0;
}

/*
 * 
 */
static int init_connection(struct pingpong_params *params,
						   struct user_parameters *user_parm,
						   struct pingpong_dest *my_dest) {

	params->conn_type = user_parm->connection_type;
	params->use_index = user_parm->gid_index;
	params->use_mcg	  = 0;
	params->type      = user_parm->servername ? CLIENT : SERVER;
	params->side      = LOCAL;
	ctx_print_pingpong_data(my_dest,params);

	if (user_parm->servername) 
		params->sockfd = ctx_client_connect(user_parm->servername,user_parm->port);
	else 
		params->sockfd = ctx_server_connect(user_parm->port);

	if(params->sockfd < 0) {
		fprintf(stderr,"Unable to open file descriptor for socket connection");
		return 1;
	}
	return 0;
}

/*
 *
 */
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
										    unsigned size,
											struct user_parameters *user_parm)
{
	struct pingpong_context *ctx;
	struct ibv_device_attr device_attr;
	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = size;
	ctx->tx_depth = user_parm->tx_depth;
	ctx->buf = memalign(page_size, size * 2);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0, size * 2);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		return NULL;
	}

	if (ibv_query_device(ctx->context, &device_attr)) {
		fprintf(stderr, "Failed to query device props");
		return NULL;
	}

	if (set_max_out_read(user_parm,&device_attr)) {
		fprintf(stderr, "Failed to set the number of outstanding reads\n");
		return NULL;
	}
	
	if (user_parm->mtu == 0) {/*user did not ask for specific mtu */	
		if (device_attr.vendor_part_id == 23108 || user_parm->gid_index > -1)
			user_parm->mtu = 1024;
		else
			user_parm->mtu = 2048;
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

	/* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
	 * The Consumer is not allowed to assign Remote Write or Remote Atomic to
	 * a Memory Region that has not been assigned Local Write. */
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size * 2,
			     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return NULL;
	}

	ctx->cq = ibv_create_cq(ctx->context,ctx->tx_depth, NULL, ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	{
		struct ibv_qp_init_attr attr;
		memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
		attr.send_cq = ctx->cq;
		attr.recv_cq = ctx->cq;
		attr.cap.max_send_wr  = ctx->tx_depth;
		/* Work around:  driver doesnt support
		 * recv_wr = 0 */
		attr.cap.max_recv_wr  = 1;
		attr.cap.max_send_sge = 1;
		attr.cap.max_recv_sge = 1;
		attr.qp_type = IBV_QPT_RC;
		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}
	}

	{
		struct ibv_qp_attr attr;

		attr.qp_state        = IBV_QPS_INIT;
		attr.pkey_index      = 0;
		attr.port_num        = user_parm->ib_port;
		attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;

		if (ibv_modify_qp(ctx->qp, &attr,
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
						  struct pingpong_dest *dest,struct user_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof attr);

	attr.qp_state 		= IBV_QPS_RTR;
	switch (user_parm->mtu) {
	case 256 : 
		attr.path_mtu               = IBV_MTU_256;
		break;
	case 512 :
		attr.path_mtu               = IBV_MTU_512;
		break;
	case 1024 :
		attr.path_mtu               = IBV_MTU_1024;
		break;
	case 2048 :
		attr.path_mtu               = IBV_MTU_2048;
		break;
	case 4096 :
		attr.path_mtu               = IBV_MTU_4096;
		break;
	}
	printf("Mtu : %d\n", user_parm->mtu);
	attr.dest_qp_num 	= dest->qpn;
	attr.rq_psn 		= dest->psn;
	attr.max_dest_rd_atomic     = user_parm->max_out_read;
	attr.min_rnr_timer          = 12;
	if (user_parm->gid_index<0) {
		attr.ah_attr.is_global  = 0;
		attr.ah_attr.dlid       = dest->lid;
		attr.ah_attr.sl         = sl;
	} else {
		attr.ah_attr.is_global  = 1;
		attr.ah_attr.grh.dgid   = dest->dgid;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.sl         = 0;
	}
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num   = user_parm->ib_port;
	if (ibv_modify_qp(ctx->qp, &attr,
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
	attr.max_rd_atomic  = user_parm->max_out_read;
	if (ibv_modify_qp(ctx->qp, &attr,
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

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
	printf("  -m, --mtu=<mtu>        mtu size (256 - 4096. default for hermon is 2048)\n");
	printf("  -o, --outs=<num>       num of outstanding read/atom(default for hermon 16 (others 4)\n");
	printf("  -s, --size=<size>      size of message to exchange (default 65536)\n");
	printf("  -a, --all              Run sizes from 2 till 2^23\n");
	printf("  -t, --tx-depth=<dep>   size of tx queue (default 100)\n");
	printf("  -n, --iters=<iters>    number of exchanges (at least 2, default 1000)\n");
	printf("  -u, --qp-timeout=<timeout> QP timeout, timeout value is 4 usec * 2 ^(timeout), default 14\n");
	printf("  -S, --sl=<sl>          SL (default 0)\n");
	printf("  -x, --gid-index=<index>   test uses GID with GID index taken from command line (for RDMAoE index should be 0)\n");
	printf("  -b, --bidirectional    measure bidirectional bandwidth (default unidirectional)\n");
	printf("  -V, --version          display version number\n");
	printf("  -e, --events           sleep on CQ events (default poll)\n");
	printf("  -F, --CPU-freq         do not fail even if cpufreq_ondemand module is loaded\n");
}

static void print_report(unsigned int iters, unsigned size, int duplex,
			 cycles_t *tposted, cycles_t *tcompleted, int no_cpu_freq_fail)
{
	double cycles_to_units;
	unsigned long tsize;	/* Transferred size, in megabytes */
	int i, j;
	int opt_posted = 0, opt_completed = 0;
	cycles_t opt_delta;
	cycles_t t;


	opt_delta = tcompleted[opt_posted] - tposted[opt_completed];

	/* Find the peak bandwidth */
	for (i = 0; i < iters; ++i)
		for (j = i; j < iters; ++j) {
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
	       size,iters,tsize * cycles_to_units / opt_delta / 0x100000,
	       tsize * iters * cycles_to_units /(tcompleted[iters - 1] - tposted[0]) / 0x100000);
}
int run_iter(struct pingpong_context *ctx, struct user_parameters *user_param,
	     struct pingpong_dest *rem_dest, int size)
{
	struct ibv_qp           *qp;
	int                      scnt, ccnt ;

	ctx->list.addr = (uintptr_t) ctx->buf;
	ctx->list.length = size;
	ctx->list.lkey = ctx->mr->lkey;
	ctx->wr.wr.rdma.remote_addr = rem_dest->vaddr;
	ctx->wr.wr.rdma.rkey = rem_dest->rkey;
	ctx->wr.wr_id      = PINGPONG_READ_WRID;
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_RDMA_READ;
	ctx->wr.send_flags = IBV_SEND_SIGNALED;
	ctx->wr.next       = NULL;

	scnt = 0;
	ccnt = 0;

	qp = ctx->qp;

	/* Done with setup. Start the test. */
	while (scnt < user_param->iters || ccnt < user_param->iters) {
		while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth ) {
			struct ibv_send_wr *bad_wr;
			tposted[scnt] = get_cycles();
			if (ibv_post_send(qp, &ctx->wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",
					scnt);
				return 1;
			}
			++scnt;
		}
		if (ccnt < user_param->iters) {
			struct ibv_wc wc;
			int ne;
			if (user_param->use_event) {
				struct ibv_cq *ev_cq;
				void          *ev_ctx;
				if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
					fprintf(stderr, "Failed to get cq_event\n");
					return 1;
				}                
				if (ev_cq != ctx->cq) {
					fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
					return 1;
				}
				if (ibv_req_notify_cq(ctx->cq, 0)) {
					fprintf(stderr, "Couldn't request CQ notification\n");
					return 1;
				}
			}
			do {
				ne = ibv_poll_cq(ctx->cq, 1, &wc);
				if (ne) {
					tcompleted[ccnt] = get_cycles();
					if (wc.status != IBV_WC_SUCCESS) {
						fprintf(stderr, "Completion wth error at %s:\n",
							user_param->servername ? "client" : "server");
						fprintf(stderr, "Failed status %d: wr_id %d syndrom 0x%x\n",
							wc.status, (int) wc.wr_id, wc.vendor_err);
						fprintf(stderr, "scnt=%d, ccnt=%d\n",
							scnt, ccnt);
						return 1;
					}
					ccnt = ccnt + ne;
				}
			} while (ne > 0 );

			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest    my_dest,rem_dest;
	struct pingpong_params  png_params;
	struct user_parameters  user_param;
	char                    *ib_devname = NULL;
	long long                size = 65536;
	int                      duplex = 0;
	int                      i = 0;
	int                      no_cpu_freq_fail = 0;

	/* init default values to user's parameters */
	memset(&user_param, 0, sizeof(struct user_parameters));
	user_param.mtu = 0;
	user_param.ib_port = 1;
	user_param.port = 18515;
	user_param.use_out_read = 0;
	user_param.iters = 1000;
	user_param.tx_depth = 100;
	user_param.servername = NULL;
	user_param.use_event = 0;
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
			{ .name = "outs",           .has_arg = 1, .val = 'o' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "qp-timeout",     .has_arg = 1, .val = 'u' },
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
			{ .name = "gid-index",      .has_arg = 1, .val = 'x' },
			{ .name = "all",            .has_arg = 0, .val = 'a' },
			{ .name = "bidirectional",  .has_arg = 0, .val = 'b' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "events",         .has_arg = 0, .val = 'e' },
			{ .name = "CPU-freq",       .has_arg = 0, .val = 'F' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:m:o:s:n:t:u:S:x:abVeF", long_options, NULL);
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
		case 'e':
			++user_param.use_event;
			break;
		case 'm':
			user_param.mtu = strtol(optarg, NULL, 0);
			break;
		case 'o':
			user_param.max_out_read = strtol(optarg, NULL, 0);
			user_param.use_out_read = 1;
			break;
		case 'a':
			user_param.all = ALL;
			break;
		case 'V':
			printf("read_bw version : %.2f\n",VERSION);
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
		user_param.servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}
	printf("------------------------------------------------------------------\n");
	if (duplex == 1)
		printf("                    RDMA_Read Bidirectional BW Test\n");
	else
		printf("                    RDMA_Read BW Test\n");

	printf("Connection type : RC\n");
	if (user_param.gid_index > -1) {
		printf("Using GID to support RDMAoE configuration. Refer to port type as Ethernet, default MTU 1024B\n");
	}

	/* Done with parameter parsing. Perform setup. */
	if (user_param.all == ALL)
		/*since we run all sizes */
		size = 8388608; /*2^23 */

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

	ctx = pp_init_ctx(ib_dev,size,&user_param);
	if (!ctx)
		return 1;

	// Set up the Connection.
	if (set_up_connection(ctx,&user_param,&my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}	

	// Init the connection and print the local data.
	if (init_connection(&png_params,&user_param,&my_dest)) {
		fprintf(stderr," Unable to init the socket connection\n");
		return 1;
	}

	// shaking hands and gather the other side info.
    if (ctx_hand_shake(&png_params,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
        
    }
	png_params.side = REMOTE;
	ctx_print_pingpong_data(&rem_dest,&png_params);

	if (pp_connect_ctx(ctx,my_dest.psn,&rem_dest,&user_param)) {
		fprintf(stderr," Unable to Connect the HCA's through the link\n");
		return 1;
	}

	// An additional handshake is required after moving qp to RTR.
	if (ctx_hand_shake(&png_params,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
        
    }
     
	// For half duplex tests, server just waits for client to exit 
	if (!user_param.servername && !duplex) {
		if (ctx_close_connection(&png_params,&my_dest,&rem_dest)) {
			fprintf(stderr,"Failed to close connection between server and client\n");
			return 1;
		}
		printf("------------------------------------------------------------------\n");
		return 0;

	} else if (user_param.use_event) {
		printf("Test with events.\n");
		if (ibv_req_notify_cq(ctx->cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		} 
	}
    
	printf("------------------------------------------------------------------\n");
	printf(" #bytes #iterations    BW peak[MB/sec]    BW average[MB/sec]  \n");

	tposted = malloc(user_param.iters * sizeof *tposted);

	if (!tposted) {
		perror("malloc");
		return 1;
	}

	tcompleted = malloc(user_param.iters * sizeof *tcompleted);

	if (!tcompleted) {
		perror("malloc");
		return 1;
	}

	if (user_param.all == ALL) {
		for (i = 1; i < 24 ; ++i) {
			size = 1 << i;
			if(run_iter(ctx,&user_param,&rem_dest,size))
				return 17;
			print_report(user_param.iters, size, duplex, tposted, tcompleted, no_cpu_freq_fail);
		}
	} else {
		if(run_iter(ctx,&user_param,&rem_dest,size))
			return 18;
		print_report(user_param.iters,size,duplex,tposted,tcompleted,no_cpu_freq_fail);
	}

	if (ctx_close_connection(&png_params,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}

	free(tposted);
	free(tcompleted);

	printf("------------------------------------------------------------------\n");
	return 0;
}
