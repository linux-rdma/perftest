/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
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

#include <sysfs/libsysfs.h>

#include <infiniband/verbs.h>

#include "get_clock.h"

static int double_compar(const void * aptr, const void * bptr)
{
	const double *a = aptr;
	const double *b = bptr;

	if (*a < *b) return -1;
	if (*a > *b) return 1;
	else return 0;
}


enum {
	PINGPONG_RDMA_WRID = 3,
};

static int page_size;

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
	int                 rx_depth;
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

/*
 * pp_get_local_lid() uses a pretty bogus method for finding the LID
 * of a local port.  Please don't copy this into your app (or if you
 * do, please rip it out soon).
 */
static uint16_t pp_get_local_lid(struct ibv_device *dev, int port)
{
	char path[256];
	char val[16];
	char *name;

	if (sysfs_get_mnt_path(path, sizeof path)) {
		fprintf(stderr, "Couldn't find sysfs mount.\n");
		return 0;
	}

	asprintf(&name, "%s/class/infiniband/%s/ports/%d/lid", path,
		 ibv_get_device_name(dev), port);

	if (sysfs_read_attribute_value(name, val, sizeof val)) {
		fprintf(stderr, "Couldn't read LID at %s\n", name);
		return 0;
	}

	return strtol(val, NULL, 0);
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

struct pingpong_dest * pp_client_exch_dest(int sockfd,
					   const struct pingpong_dest *my_dest)
{
	struct pingpong_dest *rem_dest = NULL;
	char msg[sizeof "0000:000000:000000:00000000:0000000000000000"];
	int parsed;

	sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx", my_dest->lid, my_dest->qpn,
			my_dest->psn,my_dest->rkey,my_dest->vaddr);
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client write");
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client read");
		fprintf(stderr, "Couldn't read remote address\n");
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", &rem_dest->lid, &rem_dest->qpn,
			&rem_dest->psn,&rem_dest->rkey,&rem_dest->vaddr);

	if (parsed != 5) {
		fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg,
				msg);
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}
out:
	return rem_dest;
}

int pp_server_connect(int port)
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

static struct pingpong_dest *pp_server_exch_dest(int connfd, const struct pingpong_dest *my_dest)
{
	char msg[sizeof "0000:000000:000000:00000000:0000000000000000"];
	struct pingpong_dest *rem_dest = NULL;
	int parsed;
	int n;

	n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", &rem_dest->lid, &rem_dest->qpn,
			&rem_dest->psn, &rem_dest->rkey, &rem_dest->vaddr);
	if (parsed != 5) {
		fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg,
				msg);
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx", my_dest->lid, my_dest->qpn,
			my_dest->psn, my_dest->rkey, my_dest->vaddr);
	if (write(connfd, msg, sizeof msg) != sizeof msg) {
		perror("server write");
		fprintf(stderr, "Couldn't send local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}
out:
	return rem_dest;
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
					    int tx_depth, int rx_depth, int port)
{
	struct pingpong_context *ctx;

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = size;
	ctx->rx_depth = rx_depth;
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

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size * 2,
			     IBV_ACCESS_REMOTE_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return NULL;
	}

	ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL);
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
				.max_recv_wr  = rx_depth,
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
		struct ibv_qp_attr attr;

		attr.qp_state        = IBV_QPS_INIT;
		attr.pkey_index      = 0;
		attr.port_num        = port;
		attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;

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
	struct ibv_qp_attr attr;

	attr.qp_state 		= IBV_QPS_RTR;
	attr.path_mtu 		= IBV_MTU_256;
	attr.dest_qp_num 	= dest->qpn;
	attr.rq_psn 		= dest->psn;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer 	= 12;
	attr.ah_attr.is_global  = 0;
	attr.ah_attr.dlid       = dest->lid;
	attr.ah_attr.sl         = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num   = port;
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
	printf("  -s, --size=<size>      size of message to exchange (default 4096)\n");
	printf("  -t, --tx-depth=<dep>   size of tx queue (default 50)\n");
	printf("  -n, --iters=<iters>    number of exchanges (at least 2, default 1000)\n");
	printf("  -C, --report-cycles    report times in cpu cycle units (default microseconds)\n");
	printf("  -A, --report-all       print out all results (default print summary only)\n");
	printf("  -U, --report-unsorted  (implies -A) print out unsorted results (default sorted)\n");
}

int main(int argc, char *argv[])
{
	struct dlist 	  	*dev_list;
	struct ibv_device 	*ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest     my_dest;
	struct pingpong_dest    *rem_dest;
	char                    *ib_devname = NULL;
	char                    *servername = NULL;
	int                      port = 18515;
	int                      ib_port = 1;
	int                      size = 1;
	int                      rx_depth = 1;
	int                      tx_depth = 50;
	int                      iters = 1000;
	int                      report_all = 0;
	int                      report_unsorted = 0;
	int                      report_cpu_cycles = 0;
	int                      scnt, rcnt, ccnt;
	int			 client_first_post;
	int			 sockfd;
	struct ibv_qp		*qp;
	struct ibv_send_wr 	*wr;
	volatile char		*poll_buf;
	volatile char		*post_buf;

	cycles_t	*tstamp;
	double median;
	double *delta;
	int i;

	double mhz;
	const char* units;

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
			{ .name = "report-all",     .has_arg = 0, .val = 'A' },
			{ .name = "report-unsorted",.has_arg = 0, .val = 'U' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:s:n:t:CAU", long_options, NULL);
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
				return 1;
			}
			break;

		case 's':
			size = strtol(optarg, NULL, 0);
			break;

		case 't':
			tx_depth = strtol(optarg, NULL, 0);
			break;

		case 'n':
			iters = strtol(optarg, NULL, 0);
			if (iters < 2) {
				usage(argv[0]);
				return 1;
			}

			break;

		case 'A':
			report_all = 1;
			break;

		case 'C':
			report_cpu_cycles = 1;
			break;

		case 'U':
			report_unsorted = 1;
			report_all = 1;
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


	/* Done with parameter parsing. Perform setup. */

	srand48(getpid() * time(NULL));

	if (report_cpu_cycles) {
		mhz = 1;
		units = "usec";
	} else {
		mhz = get_cpu_mhz();
		units = "clocks";
	}

	page_size = sysconf(_SC_PAGESIZE);

	dev_list = ibv_get_devices();

	dlist_start(dev_list);
	if (!ib_devname) {
		ib_dev = dlist_next(dev_list);
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return 1;
		}
	} else {
		dlist_for_each_data(dev_list, ib_dev, struct ibv_device)
			if (!strcmp(ibv_get_device_name(ib_dev), ib_devname))
				break;
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}

	ctx = pp_init_ctx(ib_dev, size, tx_depth, rx_depth, ib_port);
	if (!ctx)
		return 1;

	/* Create connection between client and server.
	 * We do it by exchanging data over a TCP socket connection. */

	my_dest.lid = pp_get_local_lid(ib_dev, ib_port);
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;
	if (!my_dest.lid) {
		fprintf(stderr, "Local lid 0x0 detected. Is an SM running?\n");
		return 1;
	}
	my_dest.rkey = ctx->mr->rkey;
	my_dest.vaddr = (uintptr_t)ctx->buf + ctx->size;

	printf("  local address:  LID %#04x, QPN %#06x, PSN %#06x "
			"RKey %#08x VAddr %#016Lx\n",
			my_dest.lid, my_dest.qpn, my_dest.psn,
			my_dest.rkey, my_dest.vaddr);
	

	if (servername) {
		sockfd = pp_client_connect(servername, port);
	} else {
		sockfd = pp_server_connect(port);
	}
	if (sockfd < 0)
		return 1;

	if (servername) {
		rem_dest = pp_client_exch_dest(sockfd, &my_dest);
	} else {
		rem_dest = pp_server_exch_dest(sockfd, &my_dest);
	}

	if (!rem_dest)
		return 1;

	printf("  remote address: LID %#04x, QPN %#06x, PSN %#06x, "
			"RKey %#08x VAddr %#016Lx\n",
			rem_dest->lid, rem_dest->qpn, rem_dest->psn,
			rem_dest->rkey, rem_dest->vaddr);

	if (pp_connect_ctx(ctx, ib_port, my_dest.psn, rem_dest))
		return 1;

	/* An additional handshake is required *after* moving qp to RTR.
           Arbitrarily reuse exch_dest for this purpose. */
	if (servername) {
		rem_dest = pp_client_exch_dest(sockfd, &my_dest);
	} else {
		rem_dest = pp_server_exch_dest(sockfd, &my_dest);
	}

	write(sockfd, "done", sizeof "done");
	close(sockfd);

	wr = &ctx->wr;
	ctx->list.addr = (uintptr_t) ctx->buf;
	ctx->list.length = ctx->size;
	ctx->list.lkey = ctx->mr->lkey;
	wr->wr.rdma.remote_addr = rem_dest->vaddr;
	wr->wr.rdma.rkey = rem_dest->rkey;

	scnt = 0;
	rcnt = 0;
	ccnt = 0;
	if (servername)
		client_first_post = 1;
	else
		client_first_post = 0;

	poll_buf = ctx->poll_buf;
	post_buf = ctx->post_buf;
	qp = ctx->qp;

	tstamp = malloc(iters * sizeof *tstamp);

	if (!tstamp) {
		perror("malloc");
		return 1;
	}

	/* Done with setup. Start the test. */

	while (scnt < iters || ccnt < iters || rcnt < iters) {

		/* Wait till buffer changes. */
		if (rcnt < iters && ! client_first_post) {
			++rcnt;
			while (*poll_buf != (char)rcnt) {
			}
			/* Here the data is already in the physical memory.
			   If we wanted to actually use it, we may need
			   a read memory barrier here. */
		} else
			client_first_post = 0;

		if (scnt < iters) {
			struct ibv_send_wr *bad_wr;

			*post_buf = (char)++scnt;
			if (ibv_post_send(qp, wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",
					scnt);
				return 1;
			}
			tstamp[scnt - 1] = get_cycles();
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
				return 1;
			}
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Completion wth error at %s:\n",
					servername?"client":"server");
				fprintf(stderr, "Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				fprintf(stderr, "scnt=%d, rcnt=%d, ccnt=%d\n",
					scnt, rcnt, ccnt);
				return 1;
			}
		}
	}

	/* Done with the test. Report results. */

	delta = malloc(iters * sizeof *delta);
 	if (!delta) {
		perror("malloc");
		return 1;
	}

	for (i = 0; i < iters - 1; ++i) {
		delta[i]=(tstamp[i + 1] - tstamp[i]) / mhz;
	}

	if (report_unsorted) {
		printf("#, %s\n", units);
		for(i = 0; i < iters - 1; ++i) {
			printf("%d, %f\n", i, delta[i] / 2);
		}

		printf("\n\n");
	}

	qsort(delta, iters - 1, sizeof *delta, double_compar);

	if (report_all && ! report_unsorted) {
		printf("#, %s\n", units);
		for(i = 0; i < iters - 1; ++i) {
			printf("%d, %f\n", i, delta[i] / 2);
		}

		printf("\n\n");
	}

	/* When there is an odd number of numbers, the median is simply
	 * the middle number.
	 * When there is an even number of numbers, the median is the mean
	 * of the two middle numbers.
	 *
	 * Reminder: iters is the number of exchanges, not number of samples.
	 */

	if ((iters - 1) % 2)
		median = delta[iters / 2];
	else
		median = (delta[iters / 2] + delta[iters / 2 + 1]) / 2;

	printf("Latency            minimum: %f %s\n", delta[0] / 2, units);
	printf("Latency statistical median: %f %s\n", median / 2, units);
	printf("Latency            maximum: %f %s\n", delta[iters - 2] / 2, units);

	return 0;
}
