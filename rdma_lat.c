/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2005 Hewlett Packard, Inc (Grant Grundler)
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
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <byteswap.h>
#include <time.h>

#include <infiniband/verbs.h>

#include "get_clock.h"

#define PINGPONG_RDMA_WRID	3

static int page_size;

struct report_options {
	int unsorted;
	int histogram;
	int cycles;   /* report delta's in cycles, not microsec's */
};


struct pingpong_context {
	struct ibv_context *context;
	struct ibv_pd      *pd;
	struct ibv_mr      *mr;
	struct ibv_cq      *cq;
	struct ibv_qp      *qp;
	void               *buf;
	volatile char      *post_buf;
	volatile char      *poll_buf;
	int                 size;
	int                 tx_depth;
	struct ibv_sge list;
	struct ibv_send_wr wr;
};

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	unsigned rkey;
	unsigned long long vaddr;
};


static uint16_t pp_get_local_lid(struct pingpong_context *ctx, int port)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(ctx->context, port, &attr))
		return 0;

	return attr.lid;
}

static struct ibv_device *pp_find_dev(const char *ib_devname)
{
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev = NULL;

	dev_list = ibv_get_device_list(NULL);

	if (!ib_devname) {
		ib_dev = dev_list[0];
		if (!ib_dev)
			fprintf(stderr, "No IB devices found\n");
	} else {
		for (; (ib_dev = *dev_list); ++dev_list) {
			if (!strcmp(ibv_get_device_name(ib_dev), ib_devname))
				break;
		}
		if (!ib_dev)
			fprintf(stderr, "IB device %s not found\n", ib_devname);
	}
	return ib_dev;
}

#define KEY_MSG_SIZE (sizeof "0000:000000:000000:00000000:0000000000000000")
#define KEY_PRINT_FMT "%04x:%06x:%06x:%08x:%016Lx"

static int pp_write_keys(int sockfd, const struct pingpong_dest *my_dest)
{
	char msg[KEY_MSG_SIZE];

	sprintf(msg, KEY_PRINT_FMT, my_dest->lid, my_dest->qpn,
			my_dest->psn, my_dest->rkey, my_dest->vaddr);

	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client write");
		fprintf(stderr, "Couldn't send local address\n");
		return -1;
	}

	return 0;
}

static int pp_read_keys(int sockfd, const struct pingpong_dest *my_dest,
		       	struct pingpong_dest *rem_dest)
{
	int parsed;
	char msg[KEY_MSG_SIZE];

	if (read(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("pp_read_keys");
		fprintf(stderr, "Couldn't read remote address\n");
		return -1;
	}

	parsed = sscanf(msg, KEY_PRINT_FMT, &rem_dest->lid, &rem_dest->qpn,
			&rem_dest->psn, &rem_dest->rkey, &rem_dest->vaddr);

	if (parsed != 5) {
		fprintf(stderr, "Couldn't parse line <%.*s>\n",
				(int)sizeof msg, msg);
		return -1;
	}

	return 0;
}

static int pp_client_connect(const char *servername, int port)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int n;
	int sockfd = -1;

	asprintf(&service, "%d", port);
	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		return n;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return sockfd;
	}
	return sockfd;
}

static int pp_client_exch_dest(int sockfd, const struct pingpong_dest *my_dest,
			       struct pingpong_dest *rem_dest)
{
	if (pp_write_keys(sockfd, my_dest))
		return -1;

	return pp_read_keys(sockfd, my_dest, rem_dest);
}

static int pp_server_connect(int port)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int sockfd = -1, connfd;
	int n;

	asprintf(&service, "%d", port);
	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		return n;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return sockfd;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);
	if (connfd < 0) {
		perror("server accept");
		fprintf(stderr, "accept() failed\n");
		close(sockfd);
		return connfd;
	}

	close(sockfd);
	return connfd;
}

static int pp_server_exch_dest(int sockfd, const struct pingpong_dest *my_dest,
			       struct pingpong_dest* rem_dest)
{

	if (pp_read_keys(sockfd, my_dest, rem_dest))
		return -1;

	return pp_write_keys(sockfd, my_dest);
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
					    int tx_depth, int port)
{
	struct pingpong_context *ctx;

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = size;
	ctx->tx_depth = tx_depth;

	ctx->buf = memalign(page_size, size * 2);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0, size * 2);

	ctx->post_buf = (char*)ctx->buf + (size - 1);
	ctx->poll_buf = (char*)ctx->buf + (2 * size - 1);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
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
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size * 2,
			     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return NULL;
	}

	ctx->cq = ibv_create_cq(ctx->context, tx_depth, NULL, NULL, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	{
		struct ibv_qp_init_attr attr = {
			.send_cq = ctx->cq,
			.recv_cq = ctx->cq,
			.cap     = {
				.max_send_wr  = tx_depth,
				/* Work around:  driver doesnt support
				 * recv_wr = 0 */
				.max_recv_wr  = 1,
				.max_send_sge = 1,
				.max_recv_sge = 1,
				.max_inline_data = size
			},
			.qp_type = IBV_QPT_RC
		};

		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}
	}

	{
		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qp_access_flags = IBV_ACCESS_REMOTE_WRITE
		};

		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			return NULL;
		}
	}

	ctx->wr.wr_id      = PINGPONG_RDMA_WRID;
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_RDMA_WRITE;
	ctx->wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
	ctx->wr.next       = NULL;

	return ctx;
}

static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  struct pingpong_dest *dest)
{
	struct ibv_qp_attr attr = {
		.qp_state 		= IBV_QPS_RTR,
		.path_mtu 		= IBV_MTU_256,
		.dest_qp_num 	        = dest->qpn,
		.rq_psn 		= dest->psn,
		.max_dest_rd_atomic     = 1,
		.min_rnr_timer 	        = 12,
		.ah_attr.is_global      = 0,
		.ah_attr.dlid           = dest->lid,
		.ah_attr.sl             = 0,
		.ah_attr.src_path_bits  = 0,
		.ah_attr.port_num       = port,
	};

	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state 	    = IBV_QPS_RTS;
	attr.timeout 	    = 14;
	attr.retry_cnt 	    = 7;
	attr.rnr_retry 	    = 7;
	attr.sq_psn 	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

static int pp_open_port(struct pingpong_context *ctx, const char * servername,
		       	int ib_port, int port, struct pingpong_dest *rem_dest)
{
	char addr_fmt[] = "%8s address: LID %#04x QPN %#06x PSN %#06x RKey %#08x VAddr %#016Lx\n";
	struct pingpong_dest	my_dest;
	int			sockfd;
	int			rc;


	/* Create connection between client and server.
	 * We do it by exchanging data over a TCP socket connection. */

	my_dest.lid = pp_get_local_lid(ctx, ib_port);
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;
	if (!my_dest.lid) {
		fprintf(stderr, "Local lid 0x0 detected. Is an SM running?\n");
		return -1;
	}
	my_dest.rkey = ctx->mr->rkey;
	my_dest.vaddr = (uintptr_t)ctx->buf + ctx->size;

	printf(addr_fmt, "local", my_dest.lid, my_dest.qpn, my_dest.psn,
			my_dest.rkey, my_dest.vaddr);
	
	sockfd = servername ? pp_client_connect(servername, port) :
		pp_server_connect(port);

	if (sockfd < 0) {
		printf("pp_connect_sock(%s,%d) failed (%d)!\n",
					servername, port, sockfd);
		return sockfd;
	}

	rc = servername ? pp_client_exch_dest(sockfd, &my_dest, rem_dest) :
		pp_server_exch_dest(sockfd, &my_dest, rem_dest);
	if (rc)
		return rc;

	printf(addr_fmt, "remote", rem_dest->lid, rem_dest->qpn, rem_dest->psn,
			rem_dest->rkey, rem_dest->vaddr);

	if ((rc = pp_connect_ctx(ctx, ib_port, my_dest.psn, rem_dest)))
		return rc;

	/* An additional handshake is required *after* moving qp to RTR.
	 * Arbitrarily reuse exch_dest for this purpose.
	 */

	rc = servername ? pp_client_exch_dest(sockfd, &my_dest, rem_dest) :
		pp_server_exch_dest(sockfd, &my_dest, rem_dest);

	if (rc)
		return rc;

	write(sockfd, "done", sizeof "done");
	close(sockfd);
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
	printf("  -s, --size=<size>      size of message to exchange (default 1)\n");
	printf("  -t, --tx-depth=<dep>   size of tx queue (default 50)\n");
	printf("  -n, --iters=<iters>    number of exchanges (at least 2, default 1000)\n");
	printf("  -C, --report-cycles    report times in cpu cycle units (default microseconds)\n");
	printf("  -H, --report-histogram print out all results (default print summary only)\n");
	printf("  -U, --report-unsorted  (implies -H) print out unsorted results (default sorted)\n");
}

/*
 * When there is an
 *	odd number of samples, the median is the middle number.
 * 	even number of samples, the median is the mean of the
 *		two middle numbers.
 *
 */
static inline cycles_t get_median(int n, cycles_t delta[])
{
	if (n % 2)
		return (delta[n / 2] + delta[n / 2 - 1]) / 2;
	else
		return delta[n / 2];
}

static int cycles_compare(const void * aptr, const void * bptr)
{
	const cycles_t *a = aptr;
	const cycles_t *b = bptr;
	if (*a < *b) return -1;
	if (*a > *b) return 1;
	return 0;
}

static void print_report(struct report_options * options,
			 unsigned int iters, cycles_t *tstamp)
{
	double cycles_to_units;
	cycles_t median;
	unsigned int i;
	const char* units;
	cycles_t *delta = malloc(iters * sizeof *delta);

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
		cycles_to_units = get_cpu_mhz();
		units = "usec";
	}

	if (options->unsorted) {
		printf("#, %s\n", units);
		for(i = 0; i < iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	qsort(delta, iters - 1, sizeof *delta, cycles_compare);

	if (options->histogram) {
		printf("#, %s\n", units);
		for(i = 0; i < iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	median = get_median(iters - 1, delta);

	printf("Latency typical: %g %s\n", median / cycles_to_units / 2, units);
	printf("Latency best   : %g %s\n", delta[0] / cycles_to_units / 2, units);
	printf("Latency worst  : %g %s\n", delta[iters - 2] / cycles_to_units / 2, units);

	free(delta);
}

int main(int argc, char *argv[])
{
	const char              *ib_devname = NULL;
	const char              *servername = NULL;
	int                      port = 18515;
	int                      ib_port = 1;
	int                      size = 1;
	int                      iters = 1000;
	int                      tx_depth = 50;
	struct report_options    report = {};

	struct pingpong_context *ctx;
	struct pingpong_dest     rem_dest;
	struct ibv_device       *ib_dev;

	struct ibv_qp           *qp;
	struct ibv_send_wr      *wr;
	volatile char           *poll_buf;
	volatile char           *post_buf;

	int                      scnt, rcnt, ccnt;

	cycles_t                *tstamp;

	/* Parameter parsing. */
	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",           .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",         .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",        .has_arg = 1, .val = 'i' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "report-cycles",  .has_arg = 0, .val = 'C' },
			{ .name = "report-histogram",.has_arg = 0, .val = 'H' },
			{ .name = "report-unsorted",.has_arg = 0, .val = 'U' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:s:n:t:CHU", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			port = strtol(optarg, NULL, 0);
			if (port < 0 || port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			ib_devname = strdupa(optarg);
			break;

		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 0) {
				usage(argv[0]);
				return 2;
			}
			break;

		case 's':
			size = strtol(optarg, NULL, 0);
			if (size < 1) { usage(argv[0]); return 3; }
			break;

		case 't':
			tx_depth = strtol(optarg, NULL, 0);
			if (tx_depth < 1) { usage(argv[0]); return 4; }
			break;

		case 'n':
			iters = strtol(optarg, NULL, 0);
			if (iters < 2) {
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

		default:
			usage(argv[0]);
			return 5;
		}
	}

	if (optind == argc - 1)
		servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 6;
	}

	/*
	 *  Done with parameter parsing. Perform setup.
	 */

	srand48(getpid() * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);

	ib_dev = pp_find_dev(ib_devname);
	if (!ib_dev)
		return 7;

	ctx = pp_init_ctx(ib_dev, size, tx_depth, ib_port);
	if (!ctx)
		return 8;

	if (pp_open_port(ctx, servername, ib_port, port, &rem_dest))
		return 9;

	wr = &ctx->wr;
	ctx->list.addr = (uintptr_t) ctx->buf;
	ctx->list.length = ctx->size;
	ctx->list.lkey = ctx->mr->lkey;
	wr->wr.rdma.remote_addr = rem_dest.vaddr;
	wr->wr.rdma.rkey = rem_dest.rkey;

	scnt = 0;
	rcnt = 0;
	ccnt = 0;
	poll_buf = ctx->poll_buf;
	post_buf = ctx->post_buf;
	qp = ctx->qp;

	tstamp = malloc(iters * sizeof *tstamp);
	if (!tstamp) {
		perror("malloc");
		return 10;
	}

	/* Done with setup. Start the test. */

	while (scnt < iters || ccnt < iters || rcnt < iters) {

		/* Wait till buffer changes. */
		if (rcnt < iters && !(scnt < 1 && servername)) {
			++rcnt;
			while (*poll_buf != (char)rcnt)
				;
			/* Here the data is already in the physical memory.
			   If we wanted to actually use it, we may need
			   a read memory barrier here. */
		}

		if (scnt < iters) {
			struct ibv_send_wr *bad_wr;
			tstamp[scnt] = get_cycles();

			*post_buf = (char)++scnt;
			if (ibv_post_send(qp, wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",
					scnt);
				return 11;
			}
		}

		if (ccnt < iters) {
			struct ibv_wc wc;
			int ne;
			++ccnt;
			do {
				ne = ibv_poll_cq(ctx->cq, 1, &wc);
			} while (ne == 0);

			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 12;
			}
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Completion wth error at %s:\n",
					servername ? "client" : "server");
				fprintf(stderr, "Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				fprintf(stderr, "scnt=%d, rcnt=%d, ccnt=%d\n",
					scnt, rcnt, ccnt);
				return 13;
			}
		}
	}

	print_report(&report, iters, tstamp);
	return 0;
}
