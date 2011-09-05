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
#include <rdma/rdma_cma.h>

#include "get_clock.h"

#define PINGPONG_RDMA_WRID	3

static int sl = 0;
static int tos = -1;
static int page_size;
static pid_t pid;

struct pingpong_context {
	struct ibv_context *context;
	struct ibv_pd      *pd;
	struct ibv_mr      *mr;
	struct ibv_cq      *rcq;
	struct ibv_cq      *scq;
	struct ibv_qp      *qp;
	struct ibv_comp_channel *ch;
	void               *buf;
	unsigned            size;
	int                 tx_depth;
	struct ibv_sge      list;
	struct ibv_send_wr  wr;
};

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	unsigned rkey;
	unsigned long long vaddr;
};

struct pp_data {
	int				port;
	int				ib_port;
	unsigned            		size;
	int                 		tx_depth;
	int				use_cma;
	int 		    		sockfd;
	char				*servername;
	struct pingpong_dest		my_dest;
	struct pingpong_dest 		*rem_dest;
	struct ibv_device		*ib_dev;
	struct rdma_event_channel 	*cm_channel;
	struct rdma_cm_id 		*cm_id;

};

static void pp_post_recv(struct pingpong_context *);
static void pp_wait_for_done(struct pingpong_context *);
static void pp_send_done(struct pingpong_context *);
static void pp_wait_for_start(struct pingpong_context *);
static void pp_send_start(struct pingpong_context *);
static void pp_close_cma(struct pp_data );
static struct pingpong_context *pp_init_ctx(void *, struct pp_data *);

static uint16_t pp_get_local_lid(struct pingpong_context *ctx, int port)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(ctx->context, port, &attr))
		return 0;

	return attr.lid;
}

static struct pingpong_context *pp_client_connect(struct pp_data *data)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int n;
	int sockfd = -1;
	int n_retries = 10;
	struct rdma_cm_event *event;
	struct sockaddr_in sin;
	struct pingpong_context *ctx = NULL;
	struct rdma_conn_param conn_param;

	if (asprintf(&service, "%d", data->port) < 0)
		goto err4;

	n = getaddrinfo(data->servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%d:%s: %s for %s:%d\n", 
				pid, __func__, gai_strerror(n),
				data->servername, data->port);
		goto err4;
	}

	if (data->use_cma) {
		sin.sin_addr.s_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
		sin.sin_family = AF_INET;
		sin.sin_port = htons(data->port);
retry_addr:
		if (rdma_resolve_addr(data->cm_id, NULL,
					 (struct sockaddr *)&sin, 2000)) {
			fprintf(stderr, "%d:%s: rdma_resolve_addr failed\n",
					 pid, __func__ );
			goto err2;
		}
	
		if (rdma_get_cm_event(data->cm_channel, &event)) 
			goto err2;

		if (event->event == RDMA_CM_EVENT_ADDR_ERROR
		 && n_retries-- > 0) {
			rdma_ack_cm_event (event);
			goto retry_addr;
		}

		if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
			fprintf(stderr, "%d:%s: unexpected CM event %d\n", 
				pid, __func__, event->event);
			goto err1;
		}
		rdma_ack_cm_event(event);

        if (tos >= 0) {
			uint8_t _tos = tos;
			if (rdma_set_option(data->cm_id, RDMA_OPTION_ID,RDMA_OPTION_ID_TOS, &_tos, sizeof _tos)) {
				fprintf(stderr, "%d:%s: set TOS option failed: %d\n",pid, __func__, event->event);
				goto err1;
			}
		}
	
retry_route:
		if (rdma_resolve_route(data->cm_id, 2000)) {
			fprintf(stderr, "%d:%s: rdma_resolve_route failed\n", 
						pid, __func__);
			goto err2;
		}
	
		if (rdma_get_cm_event(data->cm_channel, &event))
			goto err2;

		if (event->event == RDMA_CM_EVENT_ROUTE_ERROR
		 && n_retries-- > 0) {
			rdma_ack_cm_event(event);
			goto retry_route;
		}

		if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
			fprintf(stderr, "%d:%s: unexpected CM event %d\n", 
					pid, __func__, event->event);
			rdma_ack_cm_event(event);
			goto err1;
		}
		rdma_ack_cm_event(event);
		ctx = pp_init_ctx(data->cm_id, data);
		if (!ctx) {
			fprintf(stderr, "%d:%s: pp_init_ctx failed\n", pid, __func__);
			goto err2;
		}
		data->my_dest.psn = lrand48() & 0xffffff;
		data->my_dest.qpn = 0;
		data->my_dest.rkey = ctx->mr->rkey;
		data->my_dest.vaddr = (uintptr_t)ctx->buf + ctx->size;
	
		memset(&conn_param, 0, sizeof conn_param);
		conn_param.responder_resources = 1;
		conn_param.initiator_depth = 1;
		conn_param.retry_count = 5;
		conn_param.private_data = &data->my_dest;
		conn_param.private_data_len = sizeof(data->my_dest);

		if (rdma_connect(data->cm_id, &conn_param)) {
			fprintf(stderr, "%d:%s: rdma_connect failure\n", pid, __func__);
			goto err2;
		}
	
		if (rdma_get_cm_event(data->cm_channel, &event))
			goto err2;
	
		if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
			fprintf(stderr, "%d:%s: unexpected CM event %d\n", 
 					pid, __func__, event->event);
			goto err1;
		}
		if (!event->param.conn.private_data || 
		    (event->param.conn.private_data_len < sizeof(*data->rem_dest))) {
			fprintf(stderr, "%d:%s: bad private data ptr %p len %d\n",  
				pid, __func__, event->param.conn.private_data, 
				event->param.conn.private_data_len);
			goto err1;
		}
		data->rem_dest = malloc(sizeof *data->rem_dest);
		if (!data->rem_dest)
			goto err1;
		
		memcpy(data->rem_dest, event->param.conn.private_data, sizeof(*data->rem_dest));
		rdma_ack_cm_event(event);

	} else {
		for (t = res; t; t = t->ai_next) {
			sockfd = socket(t->ai_family, t->ai_socktype,
						 t->ai_protocol);
			if (sockfd >= 0) {
				if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
					break;
				close(sockfd);
				sockfd = -1;
			}
		}
		if (sockfd < 0) {
			fprintf(stderr, "%d:%s: Couldn't connect to %s:%d\n", 
				 pid, __func__, data->servername, data->port);
			goto err3;
		}
		ctx = pp_init_ctx(data->ib_dev, data);
		if (!ctx)
			goto err3;
		data->sockfd = sockfd;
	}

	freeaddrinfo(res);
	return ctx;

err1:
	rdma_ack_cm_event(event);
err2:
	rdma_destroy_id(data->cm_id);
	rdma_destroy_event_channel(data->cm_channel);
err3: 
	freeaddrinfo(res);
err4: 
	return NULL;

}

static int pp_client_exch_dest(struct pp_data *data)
{
	char msg[sizeof "0000:000000:000000:00000000:0000000000000000"];
	int parsed;
	
	if (!data->use_cma) {
		sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx", data->my_dest.lid, 
				data->my_dest.qpn, data->my_dest.psn,
				data->my_dest.rkey, data->my_dest.vaddr);
		if (write(data->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "%d:%s: Couldn't send local address\n", 
					pid, __func__);
			goto err;
		}
	
		if (read(data->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client read");
			fprintf(stderr, "%d:%s: Couldn't read remote address\n", 
					pid, __func__);
			goto err;
		}
	
 		if (data->rem_dest != NULL)
			free(data->rem_dest);
		data->rem_dest = malloc(sizeof *data->rem_dest);
		if (!data->rem_dest)
			goto err;
	
		parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", &data->rem_dest->lid,
				&data->rem_dest->qpn, &data->rem_dest->psn,
				&data->rem_dest->rkey, &data->rem_dest->vaddr);
	
		if (parsed != 5) {
			fprintf(stderr, "%d:%s: Couldn't parse line <%.*s>\n",
					pid, __func__, (int)sizeof msg, msg);
			free(data->rem_dest);
			goto err;
		}
	}
	return 0;
err:
	return 1;
}

static struct pingpong_context *pp_server_connect(struct pp_data *data)
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
	struct rdma_cm_event *event;
	struct sockaddr_in sin;
	struct pingpong_context *ctx = NULL;
	struct rdma_cm_id *child_cm_id;
	struct rdma_conn_param conn_param;

	if (asprintf(&service, "%d", data->port) < 0)
		goto err5;

	if ( (n = getaddrinfo(NULL, service, &hints, &res)) < 0 ) {
		fprintf(stderr, "%d:%s: %s for port %d\n", pid, __func__, 
					gai_strerror(n), data->port);
		goto err5;
	}

	if (data->use_cma) {
		sin.sin_addr.s_addr = 0;
		sin.sin_family = AF_INET;
		sin.sin_port = htons(data->port);
		if (rdma_bind_addr(data->cm_id, (struct sockaddr *)&sin)) {
			fprintf(stderr, "%d:%s: rdma_bind_addr failed\n", pid, __func__);
			goto err3;
		}
	
		if (rdma_listen(data->cm_id, 0)) {
			fprintf(stderr, "%d:%s: rdma_listen failed\n", pid, __func__);
			goto err3;
		}

		printf("asaa2\n");
	
		if (rdma_get_cm_event(data->cm_channel, &event)) 
			goto err3;

		printf("asaa1\n");

		if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
			fprintf(stderr, "%d:%s: bad event waiting for connect request %d\n", 
				pid, __func__, event->event);
			goto err2;
		}
	
		if (!event->param.conn.private_data ||
		    (event->param.conn.private_data_len < sizeof(*data->rem_dest))) {
			fprintf(stderr, "%d:%s: bad private data len %d\n", pid,
				__func__, event->param.conn.private_data_len);
			goto err2;
		}
		
		data->rem_dest = malloc(sizeof *data->rem_dest);
		if (!data->rem_dest)
			goto err2;

		memcpy(data->rem_dest, event->param.conn.private_data, sizeof(*data->rem_dest));

		child_cm_id = (struct rdma_cm_id *)event->id;
		ctx = pp_init_ctx(child_cm_id, data);
		if (!ctx) {
			free(data->rem_dest);
			goto err1;
		}
		data->my_dest.psn = lrand48() & 0xffffff;
		data->my_dest.qpn = 0;
		data->my_dest.rkey = ctx->mr->rkey;
		data->my_dest.vaddr = (uintptr_t)ctx->buf + ctx->size;

		memset(&conn_param, 0, sizeof conn_param);
		conn_param.responder_resources = 1;
		conn_param.initiator_depth = 1;
		conn_param.private_data = &data->my_dest;
		conn_param.private_data_len = sizeof(data->my_dest);

		printf("asaa3\n");

		if (rdma_accept(child_cm_id, &conn_param)) {
			fprintf(stderr, "%d:%s: rdma_accept failed\n", pid, __func__);
			goto err1;
		}	
		rdma_ack_cm_event(event);
		if (rdma_get_cm_event(data->cm_channel, &event)) {
			fprintf(stderr, "%d:%s: rdma_get_cm_event error\n", pid, __func__);
			rdma_destroy_id(child_cm_id);
			goto err3;
		}
		if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
			fprintf(stderr, "%d:%s: bad event waiting for established %d\n", 
				pid, __func__, event->event);
			goto err1;
		}
		rdma_ack_cm_event(event);	
	} else {
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
	
		if (sockfd < 0) {
			fprintf(stderr, "%d:%s: Couldn't listen to port %d\n", pid,
						__func__, data->port);
			goto err4;
		}
	
		listen(sockfd, 1);
		connfd = accept(sockfd, NULL, 0);
		if (connfd < 0) {
			perror("server accept");
			fprintf(stderr, "%d:%s: accept() failed\n", pid, __func__);
			close(sockfd);
			goto err4;
		}
	
		close(sockfd);

		ctx = pp_init_ctx(data->ib_dev, data);
		if (!ctx)
			goto err4;
		data->sockfd = connfd;
	}
	freeaddrinfo(res);
	return ctx;

err1:
	rdma_destroy_id(child_cm_id);
err2:
	rdma_ack_cm_event(event);
err3:
	rdma_destroy_id(data->cm_id);
	rdma_destroy_event_channel(data->cm_channel);
err4: 
	freeaddrinfo(res);
err5: 
	return NULL;

}

static int pp_server_exch_dest(struct pp_data *data)
{
	char msg[sizeof "0000:000000:000000:00000000:0000000000000000"];
	int parsed;
	int n;
	
	if (!data->use_cma) {
		n = read(data->sockfd, msg, sizeof msg);
		if (n != sizeof msg) {
			perror("server read");
			fprintf(stderr, "%d:%s: %d/%d Couldn't read remote address\n", 
						pid, __func__, n, (int) sizeof msg);
			goto err;
		}
	
		if (data->rem_dest != NULL)
			free(data->rem_dest);
		data->rem_dest = malloc(sizeof *data->rem_dest);
		if (!data->rem_dest)
			goto err;
	
		parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", &data->rem_dest->lid,
			      &data->rem_dest->qpn, &data->rem_dest->psn,
			      &data->rem_dest->rkey, &data->rem_dest->vaddr);
		if (parsed != 5) {
			fprintf(stderr, "%d:%s: Couldn't parse line <%.*s>\n", pid,
						 __func__, (int)sizeof msg, msg);
			free(data->rem_dest);
			goto err;
		}
	
		sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx", data->my_dest.lid,
					 data->my_dest.qpn, data->my_dest.psn,
					 data->my_dest.rkey, data->my_dest.vaddr);
		if (write(data->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("server write");
			fprintf(stderr, "%d:%s: Couldn't send local address\n", 
					pid, __func__);
			free(data->rem_dest);
			goto err;
		}
	}

	return 0;
err:
	return 1;
}

static struct pingpong_context *pp_init_ctx(void *ptr, struct pp_data *data)
{
	struct pingpong_context *ctx;
	struct ibv_device *ib_dev;
	struct rdma_cm_id *cm_id = NULL;

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = data->size;
	ctx->tx_depth = data->tx_depth;

	ctx->buf = memalign(page_size, ctx->size * 2);
	if (!ctx->buf) {
		fprintf(stderr, "%d:%s: Couldn't allocate work buf.\n",
					 pid, __func__);
		return NULL;
	}

	memset(ctx->buf, 0, ctx->size * 2);

	if (data->use_cma) {
		cm_id = (struct rdma_cm_id *)ptr;
		ctx->context = cm_id->verbs;
		if (!ctx->context) {
			fprintf(stderr, "%d:%s: Unbound cm_id!!\n", pid, 
							__func__);
			return NULL;
		}
		
	} else {
		ib_dev = (struct ibv_device *)ptr;
		ctx->context = ibv_open_device(ib_dev);
		if (!ctx->context) {
			fprintf(stderr, "%d:%s: Couldn't get context for %s\n", 
				pid, __func__, ibv_get_device_name(ib_dev));
			return NULL;
		}
	}

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "%d:%s: Couldn't allocate PD\n", pid, __func__);
		return NULL;
	}

        /* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
         * The Consumer is not allowed to assign Remote Write or Remote Atomic to
         * a Memory Region that has not been assigned Local Write. */
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->size * 2,
			     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "%d:%s: Couldn't allocate MR\n", pid, __func__);
		return NULL;
	}


	ctx->ch = ibv_create_comp_channel(ctx->context);
	if (!ctx->ch) {
		fprintf(stderr, "%d:%s: Couldn't create comp channel\n", pid,
								 __func__);
		return NULL;
	}

	ctx->rcq = ibv_create_cq(ctx->context, 1, NULL, NULL, 0);
	if (!ctx->rcq) {
		fprintf(stderr, "%d:%s: Couldn't create recv CQ\n", pid,
								 __func__);
		return NULL;
	}

	ctx->scq = ibv_create_cq(ctx->context, ctx->tx_depth, ctx, ctx->ch, 0);
	if (!ctx->scq) {
		fprintf(stderr, "%d:%s: Couldn't create send CQ\n", pid,
								 __func__);
		return NULL;
	}


	struct ibv_qp_init_attr attr = {
		.send_cq = ctx->scq,
		.recv_cq = ctx->rcq,
		.cap     = {
			.max_send_wr  = ctx->tx_depth,
			/* Work around:  driver doesnt support
				* recv_wr = 0 */
			.max_recv_wr  = 1,
			.max_send_sge = 1,
			.max_recv_sge = 1,
			.max_inline_data = 0
		},
		.qp_type = IBV_QPT_RC
	};

	if (data->use_cma) {
		if (rdma_create_qp(cm_id, ctx->pd, &attr)) {
			fprintf(stderr, "%d:%s: Couldn't create QP\n", pid, __func__);
			return NULL;
		}
		ctx->qp = cm_id->qp;
		pp_post_recv(ctx);
		return ctx;
	} else {
		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "%d:%s: Couldn't create QP\n", pid, __func__);
			return NULL;
		}
		{
			struct ibv_qp_attr attr;
	
			attr.qp_state        = IBV_QPS_INIT;
			attr.pkey_index      = 0;
			attr.port_num        = data->ib_port;
			attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
	
			if (ibv_modify_qp(ctx->qp, &attr,
					IBV_QP_STATE              |
					IBV_QP_PKEY_INDEX         |
					IBV_QP_PORT               |
					IBV_QP_ACCESS_FLAGS)) {
				fprintf(stderr, "%d:%s: Failed to modify QP to INIT\n", 
						pid, __func__);
				return NULL;
			}
		}
	
		return ctx;
	}
	
}

static int pp_connect_ctx(struct pingpong_context *ctx, struct pp_data data)			  
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof attr);

	attr.qp_state 		= IBV_QPS_RTR;
	attr.path_mtu 		= IBV_MTU_2048;
	attr.dest_qp_num 	= data.rem_dest->qpn;
	attr.rq_psn 		= data.rem_dest->psn;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer 	= 12;
	attr.ah_attr.is_global  = 0;
	attr.ah_attr.dlid       = data.rem_dest->lid;
	attr.ah_attr.sl         = sl;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num   = data.ib_port;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "%d:%s: Failed to modify QP to RTR\n", pid, __func__);
		return 1;
	}

	attr.qp_state 	    = IBV_QPS_RTS;
	attr.timeout 	    = 14;
	attr.retry_cnt 	    = 7;
	attr.rnr_retry 	    = 7;
	attr.sq_psn 	    =  data.my_dest.psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "%d:%s: Failed to modify QP to RTS\n", pid, __func__);
		return 1;
	}

	return 0;
}

static void pp_post_recv(struct pingpong_context *ctx)
{
        struct ibv_sge list;
        struct ibv_recv_wr wr, *bad_wr;
        int rc;

        list.addr = (uintptr_t) ctx->buf;
        list.length = 1;
        list.lkey = ctx->mr->lkey;
        wr.next = NULL;
        wr.wr_id = 0xdeadbeef;
        wr.sg_list = &list;
        wr.num_sge = 1;

        rc = ibv_post_recv(ctx->qp, &wr, &bad_wr);
        if (rc) {
                perror("ibv_post_recv");
                fprintf(stderr, "%d:%s: ibv_post_recv failed %d\n", pid,
				 __func__, rc);
        }
}

static void pp_wait_for_done(struct pingpong_context *ctx)
{
	struct ibv_wc wc;
	int ne;

	do {
		usleep(500);
		ne = ibv_poll_cq(ctx->rcq, 1, &wc);
	} while (ne == 0);

	if (wc.status) 
		fprintf(stderr, "%d:%s: bad wc status %d\n", pid, __func__,
					 wc.status);
	if (!(wc.opcode & IBV_WC_RECV))
		fprintf(stderr, "%d:%s: bad wc opcode %d\n", pid, __func__,
					 wc.opcode);
	if (wc.wr_id != 0xdeadbeef) 
		fprintf(stderr, "%d:%s: bad wc wr_id 0x%x\n", pid, __func__,
					 (int)wc.wr_id);
}

static void pp_send_done(struct pingpong_context *ctx)
{
	struct ibv_send_wr *bad_wr;
	struct ibv_wc wc;
	int ne;

	ctx->list.addr = (uintptr_t) ctx->buf;
	ctx->list.length = 1;
	ctx->list.lkey = ctx->mr->lkey;
	ctx->wr.wr_id      = 0xcafebabe;
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_SEND;
	ctx->wr.send_flags = IBV_SEND_SIGNALED;
	ctx->wr.next       = NULL;
	if (ibv_post_send(ctx->qp, &ctx->wr, &bad_wr)) {
		fprintf(stderr, "%d:%s: ibv_post_send failed\n", pid, __func__);
		return;
	}
	do {
		usleep(500);
		ne = ibv_poll_cq(ctx->scq, 1, &wc);
	} while (ne == 0);

	if (wc.status) 
		fprintf(stderr, "%d:%s: bad wc status %d\n", pid, __func__,
						wc.status);
	if (wc.opcode != IBV_WC_SEND)
		fprintf(stderr, "%d:%s: bad wc opcode %d\n", pid, __func__, 
						wc.opcode);
	if (wc.wr_id != 0xcafebabe) 
		fprintf(stderr, "%d:%s: bad wc wr_id 0x%x\n", pid, __func__,
						(int)wc.wr_id);
}

static void pp_wait_for_start(struct pingpong_context *ctx)
{
	struct ibv_wc wc;
	int ne;

	do {
		usleep(500);
		ne = ibv_poll_cq(ctx->rcq, 1, &wc);
	} while (ne == 0);

	if (wc.status) 
		fprintf(stderr, "%d:%s: bad wc status %d\n", pid, __func__,
					 wc.status);
	if (!(wc.opcode & IBV_WC_RECV))
		fprintf(stderr, "%d:%s: bad wc opcode %d\n", pid, __func__,
					 wc.opcode);
	if (wc.wr_id != 0xdeadbeef) 
		fprintf(stderr, "%d:%s: bad wc wr_id 0x%x\n", pid, __func__,
					 (int)wc.wr_id);
	pp_post_recv(ctx);
}

static void pp_send_start(struct pingpong_context *ctx)
{
	struct ibv_send_wr *bad_wr;
	struct ibv_wc wc;
	int ne;

	ctx->list.addr = (uintptr_t) ctx->buf;
	ctx->list.length = 1;
	ctx->list.lkey = ctx->mr->lkey;
	ctx->wr.wr_id      = 0xabbaabba;
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_SEND;
	ctx->wr.send_flags = IBV_SEND_SIGNALED;
	ctx->wr.next       = NULL;
	if (ibv_post_send(ctx->qp, &ctx->wr, &bad_wr)) {
		fprintf(stderr, "%d:%s: ibv_post_send failed\n", pid, __func__);
		return;
	}
	do {
		usleep(500);
		ne = ibv_poll_cq(ctx->scq, 1, &wc);
	} while (ne == 0);

	if (wc.status) 
		fprintf(stderr, "%d:%s: bad wc status %d\n", pid, __func__,
					 wc.status);
	if (wc.opcode != IBV_WC_SEND)
		fprintf(stderr, "%d:%s: bad wc opcode %d\n", pid, __func__,
					 wc.opcode);
	if (wc.wr_id != 0xabbaabba) 
		fprintf(stderr, "%d:%s: bad wc wr_id 0x%x\n", pid, __func__,
					 (int)wc.wr_id);
}

static void pp_close_cma(struct pp_data data)
{
        struct rdma_cm_event *event;
        int rc;

        if (data.servername) {
                rc = rdma_disconnect(data.cm_id);
                if (rc) {
			perror("rdma_disconnect");
			fprintf(stderr, "%d:%s: rdma disconnect error\n", pid,
								 __func__);
			return;
                }
        }

        rdma_get_cm_event(data.cm_channel, &event);
        if (event->event != RDMA_CM_EVENT_DISCONNECTED)
                fprintf(stderr, "%d:%s: unexpected event during disconnect %d\n", 
			pid, __func__, event->event);
        rdma_ack_cm_event(event);
        rdma_destroy_id(data.cm_id);
        rdma_destroy_event_channel(data.cm_channel);
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
	printf("  -s, --size=<size>      size of message to exchange (default 65536)\n");
	printf("  -t, --tx-depth=<dep>   size of tx queue (default 100)\n");
	printf("  -n, --iters=<iters>    number of exchanges (at least 2, default 1000)\n");
	printf("  -S, --sl=<sl>          SL (default 0)\n");
    printf("  -T, --tos=<tos>        Type Of Service (default 0)\n");
	printf("  -b, --bidirectional    measure bidirectional bandwidth (default unidirectional)\n");
	printf("  -c, --cma		 		 use RDMA CM\n");
}

static void print_report(unsigned int iters, unsigned size, int duplex,
			 cycles_t *tposted, cycles_t *tcompleted)
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

	cycles_to_units = get_cpu_mhz(0) * 1000000;

	tsize = duplex ? 2 : 1;
	tsize = tsize * size;

	printf("\n%d: Bandwidth peak (#%d to #%d): %g MB/sec\n", pid, 
			 opt_posted, opt_completed,
			 tsize * cycles_to_units / opt_delta / 0x100000);
	printf("%d: Bandwidth average: %g MB/sec\n", pid, 
			 tsize * iters * cycles_to_units /
			 (tcompleted[iters - 1] - tposted[0]) / 0x100000);

	printf("%d: Service Demand peak (#%d to #%d): %ld cycles/KB\n", pid, 
			 opt_posted, opt_completed,
			 (unsigned long)opt_delta * 1024 / tsize);
	printf("%d: Service Demand Avg  : %ld cycles/KB\n", pid, 
			 (unsigned long)(tcompleted[iters - 1] - tposted[0]) *
			 1024 / (tsize * iters));	
}


int main(int argc, char *argv[])
{
	struct ibv_device	**dev_list;
	struct pingpong_context *ctx = NULL;
	char                    *ib_devname = NULL;
	int                      iters = 1000;
	int                      scnt, ccnt;
	int                      duplex = 0;
	struct ibv_qp		*qp;
	cycles_t		*tposted;
	cycles_t		*tcompleted;
	struct pp_data	 	 data = {
		.port	    = 18515,
		.ib_port    = 1,
		.size       = 65536,
		.tx_depth   = 100,
		.use_cma    = 0,
		.servername = NULL,
		.rem_dest   = NULL,
		.ib_dev     = NULL,
		.cm_channel = NULL,
		.cm_id      = NULL
		
	};

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
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
            { .name = "tos",            .has_arg = 1, .val = 'T' },
			{ .name = "bidirectional",  .has_arg = 0, .val = 'b' },
			{ .name = "cma", 	    .has_arg = 0, .val = 'c' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:s:n:t:S:T:bc", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			data.port = strtol(optarg, NULL, 0);
			if (data.port < 0 || data.port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			ib_devname = strdupa(optarg);
			break;

		case 'i':
			data.ib_port = strtol(optarg, NULL, 0);
			if (data.ib_port < 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 's':
			data.size = strtoll(optarg, NULL, 0);
			if (data.size < 1 || data.size > UINT_MAX / 2) {
			       	usage(argv[0]);
				return 1;
			}
			break;

		case 't':
			data.tx_depth = strtol(optarg, NULL, 0);
			if (data.tx_depth < 1) { usage(argv[0]); return 1; }
			break;

		case 'n':
			iters = strtol(optarg, NULL, 0);
			if (iters < 2) {
				usage(argv[0]);
				return 1;
			}

			break;

        case 'T':
			tos = strtol(optarg, NULL, 0);
			break;

		case 'S':
			sl = strtol(optarg, NULL, 0);
			if (sl > 15) { usage(argv[0]); return 1; }
			break;

		case 'b':
			duplex = 1;
			break;

		case 'c':
			data.use_cma = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind == argc - 1)
		data.servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	/* Get the PID and prepend it to every output on stdout/stderr
	 * This helps to parse output when multiple client/server are
	 * run from single host
	 */
	pid = getpid();

	printf("%d: | port=%d | ib_port=%d | size=%d | tx_depth=%d | sl=%d | iters=%d | duplex=%d | cma=%d |\n",
		 pid, data.port, data.ib_port, data.size, data.tx_depth,
		 sl, iters, duplex, data.use_cma);
		
	/* Done with parameter parsing. Perform setup. */

	srand48(pid * time(NULL));

	page_size = sysconf(_SC_PAGESIZE);

	if (data.use_cma) {
		data.cm_channel = rdma_create_event_channel();
		if (!data.cm_channel) {
			fprintf(stderr, "%d:%s: rdma_create_event_channel failed\n",
							 pid, __func__);
			return 1;
		}
		if (rdma_create_id(data.cm_channel, &data.cm_id, NULL, RDMA_PS_TCP)) {
			fprintf(stderr, "%d:%s: rdma_create_id failed\n",
							 pid, __func__);
			return 1;
		}
	
		if (data.servername) {
			ctx = pp_client_connect(&data);
			if (!ctx) 
				return 1;			
		} else {
			ctx = pp_server_connect(&data);
			if (!ctx) 
				return 1;			
		}
	} else {
		dev_list = ibv_get_device_list(NULL);
	
		if (!ib_devname) {
			data.ib_dev = dev_list[0];
			if (!data.ib_dev) {
				fprintf(stderr, "%d:%s: No IB devices found\n",
							 pid, __func__);
				return 1;
			}
		} else {
			for (; (data.ib_dev = *dev_list); ++dev_list)
				if (!strcmp(ibv_get_device_name(data.ib_dev), ib_devname))
					break;
			if (!data.ib_dev) {
				fprintf(stderr, "%d:%s: IB device %s not found\n",
						 pid, __func__, ib_devname);
				return 1;
			}
		}
		if (data.servername) {
			ctx = pp_client_connect(&data);
			if (!ctx) 
				return 1;
		} else {
			ctx = pp_server_connect(&data);
			if (!ctx) 
				return 1;
		}
		data.my_dest.lid = pp_get_local_lid(ctx, data.ib_port);
		if (!data.my_dest.lid) {
			fprintf(stderr, "%d:%s: Local lid 0x0 detected. Is an SM running?\n",
						pid, __func__);
			return 1;
		}
		data.my_dest.qpn = ctx->qp->qp_num;
		data.my_dest.psn = lrand48() & 0xffffff;
		data.my_dest.rkey = ctx->mr->rkey;
		data.my_dest.vaddr = (uintptr_t)ctx->buf + ctx->size;
	
		/* Create connection between client and server.
		* We do it by exchanging data over a TCP socket connection. */
		if (data.servername) {
			if (pp_client_exch_dest(&data))
				return 1;
		} else {
			if (pp_server_exch_dest(&data)) 
				return 1;
		}
	}

	printf("%d: Local address:  LID %#04x, QPN %#06x, PSN %#06x "
			"RKey %#08x VAddr %#016Lx\n", pid, 
			data.my_dest.lid, data.my_dest.qpn, data.my_dest.psn,
			data.my_dest.rkey, data.my_dest.vaddr);	

	printf("%d: Remote address: LID %#04x, QPN %#06x, PSN %#06x, "
			"RKey %#08x VAddr %#016Lx\n\n", pid, 
			data.rem_dest->lid, data.rem_dest->qpn, data.rem_dest->psn,
			data.rem_dest->rkey, data.rem_dest->vaddr);

	if (data.use_cma) {
		/*
	 	 * Synch up and force the server to wait for the client to send
		 * the first message (MPA requirement).
		 */
		if (data.servername) {			
			pp_send_start(ctx);
		} else {
			pp_wait_for_start(ctx);
		}

	} else {
		if (pp_connect_ctx(ctx, data))
			return 1;
	
		/* An additional handshake is required *after* moving qp to RTR.
		Arbitrarily reuse exch_dest for this purpose. */
		if (data.servername) {
			if (pp_client_exch_dest(&data))
				return 1;
		} else {
			if (pp_server_exch_dest(&data))
				return 1;
		}
	}

	/* For half duplex tests, server just waits for client to exit */
	if (!data.servername && !duplex) {
		if (data.use_cma) {
			pp_wait_for_done(ctx);
                        pp_send_done(ctx);
			pp_close_cma(data);
		} else {
			pp_server_exch_dest(&data);
			write(data.sockfd, "done", sizeof "done");
			close(data.sockfd);
		}
		return 0;
	}

	ctx->list.addr = (uintptr_t) ctx->buf;
	ctx->list.length = ctx->size;
	ctx->list.lkey = ctx->mr->lkey;
	ctx->wr.wr.rdma.remote_addr = data.rem_dest->vaddr;
	ctx->wr.wr.rdma.rkey = data.rem_dest->rkey;
	ctx->wr.wr_id      = PINGPONG_RDMA_WRID;
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_RDMA_WRITE;
	ctx->wr.send_flags = IBV_SEND_SIGNALED;
	ctx->wr.next       = NULL;

	scnt = 0;
	ccnt = 0;

	qp = ctx->qp;

	tposted = malloc(iters * sizeof *tposted);

	if (!tposted) {
		perror("malloc");
		return 1;
	}

	tcompleted = malloc(iters * sizeof *tcompleted);

	if (!tcompleted) {
		perror("malloc");
		return 1;
	}

	/* Done with setup. Start the test. */

	while (scnt < iters || ccnt < iters) {

		while (scnt < iters && scnt - ccnt < data.tx_depth) {
			struct ibv_send_wr *bad_wr;
			tposted[scnt] = get_cycles();

			if (ibv_post_send(qp, &ctx->wr, &bad_wr)) {
				fprintf(stderr, "%d:%s: Couldn't post send: scnt=%d\n",
					pid, __func__, scnt);
				return 1;
			}
			++scnt;
		}

		if (ccnt < iters) {
			struct ibv_wc wc;
			int ne;
			do {
				ne = ibv_poll_cq(ctx->scq, 1, &wc);
			} while (ne == 0);

			tcompleted[ccnt] = get_cycles();

			if (ne < 0) {
				fprintf(stderr, "%d:%s: poll CQ failed %d\n", pid, 
					__func__, ne);
				return 1;
			}
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "%d:%s: Completion with error at %s:\n",
					pid, __func__, data.servername ? "client" : "server");
				fprintf(stderr, "%d:%s: Failed status %d: wr_id %d\n",
					pid, __func__, wc.status, (int) wc.wr_id);
				fprintf(stderr, "%d:%s: scnt=%d, ccnt=%d\n",
					pid, __func__, scnt, ccnt);
				return 1;
			}
			ccnt += 1;
		}
	}

	if (data.use_cma) {
		/* This is racy when duplex mode is used*/
		pp_send_done(ctx);
               	pp_wait_for_done(ctx);
		pp_close_cma(data);
	} else {
		if (data.servername) 
			pp_client_exch_dest(&data);
		else
			pp_server_exch_dest(&data);
		
		write(data.sockfd, "done", sizeof "done");
		close(data.sockfd);
		
	}
	
	print_report(iters, data.size, duplex, tposted, tcompleted);

	free(tposted);
	free(tcompleted);
	return 0;
}
