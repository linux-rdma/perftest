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
#include <errno.h>

#include <infiniband/verbs.h>

#include "get_clock.h"

#define PINGPONG_SEND_WRID  1
#define PINGPONG_RECV_WRID  2
#define MAX_SEND_SGE 1
#define MAX_RECV_SGE 1
#define RC 0
#define UC 1
#define UD 3
#define VERSION 1.2
#define SIGNAL 1
#define MAX_INLINE 400
#define ALL 1
#define MAX_POLL_ITERATION_TIMEOUT 100000000
#define QPNUM_MCAST 0xffffff
#define DEF_QKEY 0x11111111
#define MCG_LID 0xc001
#define MCG_GID {255,1,0,0,0,2,201,133,0,0,1,1,0,0,0,0}

struct user_parameters {
	const char *servername;
	const char *user_mgid;
	int connection_type;
	int mtu;
	int all; /* run all msg size */
	int signal_comp;
	int iters;
	int tx_depth;
	int rx_depth;
	int duplex;
	int use_event;
	int use_mcg;
	int use_user_mgid;
	int num_of_clients_mcg;
	int inline_size;
	int qp_timeout;
	int gid_index; /* if value not negative, we use gid AND gid_index=value */
};

static int sl = 0;
static int page_size;
cycles_t	*tposted;
cycles_t	*tcompleted;
int post_recv;

struct pingpong_context {
	struct ibv_context *context;
	struct ibv_comp_channel *channel;
	struct ibv_pd      *pd;
	struct ibv_mr      *mr;
	struct ibv_cq      *cq;
	struct ibv_qp      *qp;
	struct mcg_qp	   *qp_mcg;
	void               *buf;
	unsigned            size;
	int                 tx_depth;
	int                 rx_depth;
	struct ibv_sge      list;
	struct ibv_sge recv_list;
	struct ibv_send_wr  wr;
	struct ibv_recv_wr  rwr;
	struct ibv_ah		*ah;
	union ibv_gid       dgid;
};

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	unsigned rkey;
	unsigned long long vaddr;
	union ibv_gid       dgid;
};

struct mcg_qp {

	struct ibv_qp *qp;
	unsigned int package_counter;
	cycles_t *completion_array;		 
};


/*
 *
 */
static int set_multicast_gid(struct pingpong_context *ctx,struct user_parameters *param) {

	uint8_t mcg_gid[16] = MCG_GID;
	const char *pstr = param->user_mgid;
	char *term = NULL;
	char tmp[20];
	int i;

	if (param->use_user_mgid) {
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr+1);
		tmp[term - pstr] = 0;
		mcg_gid[0] = (unsigned char)strtoll(tmp, NULL, 0);
		for (i = 1; i < 15; ++i) {
			pstr += term - pstr + 1;
			term = strpbrk(pstr, ":");
			memcpy(tmp, pstr, term - pstr+1);
			tmp[term - pstr] = 0;
			mcg_gid[i] = (unsigned char)strtoll(tmp, NULL, 0);
		}
		pstr += term - pstr + 1;
		strcpy(tmp, pstr);
		mcg_gid[15] = (unsigned char)strtoll(tmp, NULL, 0);
	}
	memcpy(ctx->dgid.raw,mcg_gid,16);
	return 0;
}

/*
 *
 */
static int 
qp_create(struct pingpong_context *ctx, struct user_parameters *param) {
	
	struct ibv_qp_init_attr attr;
	int i;
	
	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.send_cq = ctx->cq;
	attr.recv_cq = ctx->cq; 
	attr.cap.max_send_wr  = ctx->tx_depth;
	attr.cap.max_recv_wr  = ctx->rx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_recv_sge = MAX_RECV_SGE;
	attr.cap.max_inline_data = param->inline_size;
	
	switch (param->connection_type) {
		case RC :
			attr.qp_type = IBV_QPT_RC;
			break;
		case UC :
			attr.qp_type = IBV_QPT_UC;
			break;
		case UD :
			attr.qp_type = IBV_QPT_UD;
			break;
		default:
			fprintf(stderr, "Unknown connection type \n");
			return 1;
	}	
	if (param->use_mcg && !param->servername) {
		ctx->qp_mcg = malloc(sizeof(struct mcg_qp)*param->num_of_clients_mcg);
		if(ctx->qp_mcg == NULL) {
			fprintf(stdout, "Couldn't create QP\n");
			return 1;
		}
		for (i=0; i < param->num_of_clients_mcg; i++ ) {
			ctx->qp_mcg[i].qp = ibv_create_qp(ctx->pd, &attr);
			if (!ctx->qp_mcg[i].qp)  {
				fprintf(stderr, "Couldn't create QP\n");
				return 1;
			}
		}
	}
	else {
		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			return 1;
		}
	}
	return 0;
}

/*
 *
 */
static int modify_qp_to_init(struct ibv_qp *qp,int port,int connection_type)  {

	struct ibv_qp_attr attr;
	int flags;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = 0;
	attr.port_num        = port;
		
	if (connection_type == UD) {

		attr.qkey = DEF_QKEY;
		flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
	}
	else {
		
		attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
		flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
	}
	if (ibv_modify_qp(qp,&attr,flags))  {

		fprintf(stderr, "Failed to modify UD QP to INIT\n");
		return 1;
	}
	return 0;
}

/*
 *
 */
static int destroy_ctx_resources(struct pingpong_context *ctx, 
								 struct user_parameters *param)  {

	int i;
	int test_result = 0;

	if (ctx->ah) {
		if (ibv_destroy_ah(ctx->ah)) {
			fprintf(stderr, "failed to destroy AH\n");
			test_result = 1;
		}
	}
	if (param->use_mcg && !param->servername) {
		for(i=0; i < param->num_of_clients_mcg; i++) {

			if (ibv_detach_mcast(ctx->qp_mcg[i].qp, &ctx->dgid,MCG_LID)) {
				fprintf(stderr, "failed to deatttached QP\n");
				test_result = 1;
			}
			if (ibv_destroy_qp(ctx->qp_mcg[i].qp)) {
				fprintf(stderr, "failed to destroy QP\n");
				test_result = 1;
			}
            free(ctx->qp_mcg[i].completion_array);
		}
		free(ctx->qp_mcg);
		
	} else {
		if (ibv_destroy_qp(ctx->qp)) {
			fprintf(stderr, "failed to destroy QP\n");
			test_result = 1;
		}
	}
	if (ctx->mr) {
		if (ibv_dereg_mr(ctx->mr)) {
			fprintf(stderr, "failed to deregister MR\n");
			test_result = 1;
		}
	}

	if (ctx->buf)
		free(ctx->buf);

	if (ctx->cq) {
		if (ibv_destroy_cq(ctx->cq)) {
			fprintf(stderr, "failed to destroy CQ\n");
			test_result = 1;
		}
	}
	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "failed to destroy channel \n");
			test_result = 1;
		}
	}
	if (ctx->pd) {
		if (ibv_dealloc_pd(ctx->pd)) {
			fprintf(stderr, "failed to deallocate PD\n");
			test_result = 1;
		}
	}
	if (ctx->context) {
		if (ibv_close_device(ctx->context)) {
			fprintf(stderr, "failed to close device context\n");
			test_result = 1;
		}
	}
    if(!param->use_mcg || param->servername) {       
        free(tposted);
        free(tcompleted);
    }

	return test_result;
}

/*
 *
 */
static int 
alloc_time_res(struct pingpong_context *ctx, struct user_parameters *param ) {

	int i;

	if(param->use_mcg && !param->servername ) {
		for(i=0; i < param->num_of_clients_mcg; i++){
			ctx->qp_mcg[i].completion_array = malloc(sizeof(cycles_t)*param->iters);
			if(ctx->qp_mcg[i].completion_array == NULL) {
				perror("malloc");
				return 1;
			}
		}
	}
	else  {
		tposted = malloc(sizeof(cycles_t)*param->iters);
		if (!tposted) {
			perror("malloc");
			return 1;
		}
	
		tcompleted = malloc(sizeof(cycles_t)*param->iters);
		if (!tcompleted) {
			perror("malloc");
			return 1;
		}
	}

	return 0;
}

/*
 *
 */
static uint16_t pp_get_local_lid(struct pingpong_context *ctx, int port)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(ctx->context, port, &attr))
		return 0;

	return attr.lid;
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

	if (asprintf(&service, "%d", port) < 0)
		return -1;

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
					   const struct pingpong_dest *my_dest, struct user_parameters *user_parm)
{
	struct pingpong_dest *rem_dest = NULL;
	char msg[sizeof "0000:000000:000000:00000000:0000000000000000:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00"];
	int parsed;

	sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
		my_dest->lid, my_dest->qpn, my_dest->psn,my_dest->rkey,my_dest->vaddr,
		my_dest->dgid.raw[0], my_dest->dgid.raw[1], my_dest->dgid.raw[2],
		my_dest->dgid.raw[3], my_dest->dgid.raw[4], my_dest->dgid.raw[5],
		my_dest->dgid.raw[6], my_dest->dgid.raw[7], my_dest->dgid.raw[8],
		my_dest->dgid.raw[9], my_dest->dgid.raw[10], my_dest->dgid.raw[11],
		my_dest->dgid.raw[12], my_dest->dgid.raw[13], my_dest->dgid.raw[14],
		my_dest->dgid.raw[15]);
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

	if (user_parm->gid_index < 0 && !user_parm->use_mcg) {
			parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, &rem_dest->rkey, &rem_dest->vaddr);
		if (parsed != 5) {
			fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg, msg);
			free(rem_dest);
			rem_dest = NULL;
			goto out;
		}
	}else{
		char *pstr = msg, *term;
		char tmp[20];
		int i;

		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->lid = (int)strtol(tmp, NULL, 16); // LID

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->qpn = (int)strtol(tmp, NULL, 16); // QPN

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->psn = (int)strtol(tmp, NULL, 16); // PSN

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->rkey = (unsigned)strtol(tmp, NULL, 16); // RKEY

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->vaddr = strtoull(tmp, NULL, 16); // VA

		for (i = 0; i < 15; ++i) {
			pstr += term - pstr + 1;
			term = strpbrk(pstr, ":");
			memcpy(tmp, pstr, term - pstr);
			tmp[term - pstr] = 0;
			rem_dest->dgid.raw[i] = (unsigned char)strtoll(tmp, NULL, 16);
		}
		pstr += term - pstr + 1;
		strcpy(tmp, pstr);
		rem_dest->dgid.raw[15] = (unsigned char)strtoll(tmp, NULL, 16);
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

	if (asprintf(&service, "%d", port) < 0)
		return -1;

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

static struct pingpong_dest *pp_server_exch_dest(int connfd, const struct pingpong_dest *my_dest, struct user_parameters *user_parm)
{
	char msg[sizeof "0000:000000:000000:00000000:0000000000000000:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00"];
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

	if (user_parm->gid_index < 0) {
		parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", &rem_dest->lid, &rem_dest->qpn,
				&rem_dest->psn, &rem_dest->rkey, &rem_dest->vaddr);
		if (parsed != 5) {
			fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg, msg);
			free(rem_dest);
			rem_dest = NULL;
			goto out;
		}
	}else{
		char *pstr = msg, *term;
		char tmp[20];
		int i;

		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->lid = (int)strtol(tmp, NULL, 16); // LID

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->qpn = (int)strtol(tmp, NULL, 16); // QPN

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->psn = (int)strtol(tmp, NULL, 16); // PSN

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->rkey = (unsigned)strtol(tmp, NULL, 16); // RKEY

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->vaddr = strtoull(tmp, NULL, 16); // VA

		for (i = 0; i < 15; ++i) {
			pstr += term - pstr + 1;
			term = strpbrk(pstr, ":");
			memcpy(tmp, pstr, term - pstr);
			tmp[term - pstr] = 0;
			rem_dest->dgid.raw[i] = (unsigned char)strtoll(tmp, NULL, 16);
			}
		pstr += term - pstr + 1;
		strcpy(tmp, pstr);
		rem_dest->dgid.raw[15] = (unsigned char)strtoll(tmp, NULL, 16);
	}

	sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
		my_dest->lid, my_dest->qpn, my_dest->psn, my_dest->rkey, my_dest->vaddr,
		my_dest->dgid.raw[0], my_dest->dgid.raw[1], my_dest->dgid.raw[2],
		my_dest->dgid.raw[3], my_dest->dgid.raw[4], my_dest->dgid.raw[5],
		my_dest->dgid.raw[6], my_dest->dgid.raw[7], my_dest->dgid.raw[8],
		my_dest->dgid.raw[9], my_dest->dgid.raw[10], my_dest->dgid.raw[11],
		my_dest->dgid.raw[12], my_dest->dgid.raw[13], my_dest->dgid.raw[14],
		my_dest->dgid.raw[15]);
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

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
                                            unsigned size,int tx_depth, 
                                            int rx_depth, int port,
                                            struct user_parameters *user_parm)
{
	struct pingpong_context *ctx;
	struct ibv_device_attr device_attr;
	int i,cq_rx_depth;

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->ah       = NULL;
	ctx->channel  = NULL;
	ctx->size     = size;
	ctx->tx_depth = tx_depth;
	ctx->rx_depth = tx_depth + rx_depth;
	cq_rx_depth = ctx->rx_depth;
	/* in case of UD need space for the GRH */
	if (user_parm->connection_type==UD) {
		ctx->buf = memalign(page_size, ( size + 40 ) * 2);
		if (!ctx->buf) {
			fprintf(stderr, "Couldn't allocate work buf.\n");
			return NULL;
		}
		memset(ctx->buf, 0, ( size + 40 ) * 2);
	} else {
		ctx->buf = memalign(page_size, size * 2);
		if (!ctx->buf) {
			fprintf(stderr, "Couldn't allocate work buf.\n");
			return NULL;
		}
		memset(ctx->buf, 0, size * 2);
	}

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		return NULL;
	}
	if (user_parm->mtu == 0) {/*user did not ask for specific mtu */
		if (ibv_query_device(ctx->context, &device_attr)) {
			fprintf(stderr, "Failed to query device props");
			return NULL;
		}
		if (device_attr.vendor_part_id == 23108 || user_parm->gid_index > -1) {
			user_parm->mtu = 1024;
		} else {
			user_parm->mtu = 2048;
		}
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
	if (user_parm->connection_type==UD) {
		ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, (size + 40 ) * 2,
				     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
		if (!ctx->mr) {
			fprintf(stderr, "Couldn't allocate MR\n");
			return NULL;
		}
	} else {
		ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size * 2,
				     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
		if (!ctx->mr) {
			fprintf(stderr, "Couldn't allocate MR\n");
			return NULL;
		}
	}
	if (user_parm->use_mcg && !user_parm->servername) {
		cq_rx_depth *= user_parm->num_of_clients_mcg;
	}
	ctx->cq = ibv_create_cq(ctx->context,cq_rx_depth, NULL, ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}
	if (qp_create(ctx,user_parm)) {
		fprintf(stderr, "Couldn't create QP\n");
		return NULL;
	}

	if (user_parm->use_mcg && !user_parm->servername) {
		if (set_multicast_gid(ctx,user_parm)) {
			fprintf(stderr, "Couldn't Set the Multicast M_GID\n");
			return NULL;
		}

		for(i=0; i < user_parm->num_of_clients_mcg; i++) {
			if (modify_qp_to_init(ctx->qp_mcg[i].qp,port,user_parm->connection_type)) {
				fprintf(stderr, "Failed to modify UD QP to INIT\n");
				return NULL;
			}
			if (ibv_attach_mcast(ctx->qp_mcg[i].qp,&ctx->dgid,MCG_LID)) {
				fprintf(stderr, "Couldn't attach QP to mcg\n");
				return NULL;
			}
		}
		printf("Successfully created and attached %d QPs to a Mcast group\n",i);
	} else {
		if (modify_qp_to_init(ctx->qp,port,user_parm->connection_type)) {
				fprintf(stderr, "Failed to modify UD QP to INIT\n");
				return NULL;
		}
	}
	return ctx;
}

/*
 *
 */
static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			              struct pingpong_dest *dest, 
						  struct user_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof attr);
	int i;

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
    attr.dest_qp_num = (user_parm->use_mcg && user_parm->servername) ? 
                        QPNUM_MCAST : dest->qpn;
	attr.rq_psn = dest->psn;
	if (user_parm->connection_type == RC) {
		attr.max_dest_rd_atomic     = 1;
		attr.min_rnr_timer          = 12;
	}
	if (user_parm->gid_index < 0) {
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
	attr.ah_attr.port_num   = port;
	if (user_parm->use_mcg && user_parm->servername) {
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.sl         = 0;
		attr.ah_attr.dlid       = MCG_LID;
		attr.ah_attr.is_global  = 1;
		attr.ah_attr.grh.sgid_index = 0;
		memcpy(attr.ah_attr.grh.dgid.raw,dest->dgid.raw, 16);
	}

	if (user_parm->connection_type == RC) {
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
	} else if (user_parm->connection_type == UC) {
		if (ibv_modify_qp(ctx->qp, &attr,
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
		
		if (user_parm->use_mcg && !user_parm->servername) {
			for(i=0; i < user_parm->num_of_clients_mcg; i++)  {
				if (ibv_modify_qp(ctx->qp_mcg[i].qp, &attr, IBV_QP_STATE )) {
					printf("Failed to modify UD QP to RTR\n");
					return 1;
				}
			}
		}
		else {
			if (ibv_modify_qp(ctx->qp,&attr, IBV_QP_STATE )) {
					fprintf(stderr, "Failed to modify UD QP to RTR\n");
					return 1;
			}
		}
        ctx->ah = ibv_create_ah(ctx->pd, &attr.ah_attr);
		    if (!ctx->ah) {
                fprintf(stderr, "Failed to create AH for UD\n");
                return 1;
            }
	}
	attr.qp_state 	    = IBV_QPS_RTS;
	attr.sq_psn 	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (user_parm->connection_type == RC) {
		attr.max_rd_atomic  = 1;
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
	} else { /*both UC and UD */
		if(!user_parm->use_mcg || user_parm->servername) {
			if(ibv_modify_qp(ctx->qp, &attr,IBV_QP_STATE |IBV_QP_SQ_PSN)) {
			fprintf(stderr, "Failed to modify UC QP to RTS\n");
			return 1;
			}
		}
	}
	/* post recieve max msg size*/
	{
		int i,j;
		struct ibv_recv_wr  *bad_wr_recv;
		//recieve
		ctx->rwr.wr_id      = PINGPONG_RECV_WRID;
		ctx->rwr.sg_list    = &ctx->recv_list;
		ctx->rwr.num_sge    = 1;
		ctx->rwr.next       = NULL;
		ctx->recv_list.addr = (uintptr_t) ctx->buf;
		if (user_parm->connection_type==UD) {
			ctx->recv_list.length = ctx->size + 40;
		} else {
			ctx->recv_list.length = ctx->size;
		}
		ctx->recv_list.lkey = ctx->mr->lkey;
		if ( user_parm->use_mcg && !user_parm->servername) {
			for (i=0; i < user_parm->num_of_clients_mcg; i++)  {
				for (j = 0; j < ctx->rx_depth; ++j) {
					if (ibv_post_recv(ctx->qp_mcg[i].qp, &ctx->rwr, &bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv: counter=%d\n", j);
						return 14;
					}
				}
			}
		} else {
			for (i = 0; i < ctx->rx_depth; ++i) {
				if (ibv_post_recv(ctx->qp, &ctx->rwr, &bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv: counter=%d\n", i);
					return 14;
				}
			}
		}
	}
	post_recv = ctx->rx_depth;
	return 0;
}

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
	printf("  -g, --mcg=<num_of_qps>      Send messages to multicast group with <num_of_qps> qps attached to it.\n");
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
	printf("  -M, --MGID=<multicast_gid>  In case of multicast, uses <multicast_gid> as the group MGID.\n");
	printf("                              The format must be '255:1:X:X:X:X:X:X:X:X:X:X:X:X:X:X', where X is a vlaue within [0,255]\n");
	printf("                              In decimal digits. You should specify it on the server.\n");
}

/*
 *
 */
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
	printf("%7d        %d            %7.2f               %7.2f\n",
	       size,iters,!(noPeak) * tsize * cycles_to_units / opt_delta / 0x100000,
	       tsize * iters * cycles_to_units /(tcompleted[iters - 1] - tposted[0]) / 0x100000);
}

/*
 *
 */
static void print_mcg_report(struct pingpong_context *ctx,
							 struct user_parameters *param,
							 unsigned size,
							 int no_cpu_freq_fail) {

	int i,packets_loss = 0;
	unsigned long packets_sum = 0;
	unsigned int lower , upper;
	int last_pos;
	double cycles_to_units;
	cycles_t max_first_comp, min_last_comp;

	cycles_to_units = get_cpu_mhz(no_cpu_freq_fail) * 1000000;

	last_pos = (ctx->qp_mcg[0].package_counter) ? ctx->qp_mcg[0].package_counter -1 : 0;
	max_first_comp = ctx->qp_mcg[0].completion_array[0];
	min_last_comp = ctx->qp_mcg[0].completion_array[last_pos];

	for (i=0; i < param->num_of_clients_mcg; i++) {
		if (ctx->qp_mcg[i].completion_array[0] > max_first_comp) {
			max_first_comp = ctx->qp_mcg[i].completion_array[0];
		}
		last_pos = (ctx->qp_mcg[i].package_counter) ? ctx->qp_mcg[i].package_counter -1 : 0;
		if (ctx->qp_mcg[i].completion_array[last_pos] < min_last_comp) {
			min_last_comp = ctx->qp_mcg[i].completion_array[last_pos];
		}
	}
	for (i=0; i < param->num_of_clients_mcg; i++ ) {

		upper = lower = 0;
		last_pos = (ctx->qp_mcg[i].package_counter) ? ctx->qp_mcg[i].package_counter -1 : 0;
		while (ctx->qp_mcg[i].completion_array[lower] < max_first_comp) {
			lower++;
		}
		while (ctx->qp_mcg[i].completion_array[last_pos] > min_last_comp) {
			last_pos++;
		}
		packets_sum += ctx->qp_mcg[i].package_counter - upper -lower;
		packets_loss += (param->iters - ctx->qp_mcg[i].package_counter);
	}
	printf("%7d        %d            %4d               %10.2f\n",size,param->iters,packets_loss,
		   packets_sum*size*cycles_to_units/(min_last_comp - max_first_comp) / 0x100000);
}

/*
 *
 */
int run_iter_bi(struct pingpong_context *ctx, struct user_parameters *user_param,
		struct pingpong_dest *rem_dest, int size)
{
	struct ibv_qp           *qp;
	int                      scnt, ccnt, rcnt;
	struct ibv_recv_wr      *bad_wr_recv;
	if (user_param->connection_type == UD) {
		if (size > 2048) {
			if (user_param->gid_index < 0) {
				size = 2048;
			} else {
				size = 1024;
			}
		}
	}
	/*********************************************
	 * Important note :
	 * In case of UD/UC this is NOT the way to measure
	 * BW sicen we are running with loop on the send side
	 * while we should run on the recieve side or enable retry in SW
	 * Since the sender may be faster than the reciver than although
	 * we had posted recieve it is not enough and might end this will
	 * result in deadlock of test since both sides are stuck on poll cq
	 * In this test i do not solve this for the general test ,need to write
	 * seperate test for UC/UD but in case the tx_depth is ~1/3 from the
	 * number of iterations this should be ok .
	 * Also note that the sender is limited in the number of send, ans
	 * i try to make the reciver full 
	 *********************************************/

	if (user_param->connection_type == UD)
		ctx->recv_list.length = ctx->size + 40;
	else
		ctx->recv_list.length = ctx->size;
	if (size > user_param->inline_size) /*complaince to perf_main */
		ctx->wr.send_flags = IBV_SEND_SIGNALED;
	else
		ctx->wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

	ctx->list.length = size;
	scnt = 0;
	ccnt = 0;
	rcnt = 0;
	qp = ctx->qp;

	while (ccnt < user_param->iters || rcnt < user_param->iters ) {
		struct ibv_wc wc;
		int ne;
		while (scnt < user_param->iters &&
		       (scnt - ccnt) < user_param->tx_depth / 2) {
			struct ibv_send_wr *bad_wr;
			tposted[scnt] = get_cycles();
			if (ibv_post_send(qp, &ctx->wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",
					scnt);
				return 1;
			}
			++scnt;
		}
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
		for (;;) {
			ne = ibv_poll_cq(ctx->cq, 1, &wc);
			if (ne <= 0)
				break;

			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Completion wth error at %s:\n",
					user_param->servername ? "client" : "server");
				fprintf(stderr, "Failed status %d: wr_id %d syndrom 0x%x\n",
					wc.status, (int) wc.wr_id, wc.vendor_err);
				fprintf(stderr, "scnt=%d, ccnt=%d\n",
					scnt, ccnt);
				return 1;
			}
			switch ((int) wc.wr_id) {
			case PINGPONG_SEND_WRID:
				tcompleted[ccnt] = get_cycles();
				ccnt += 1;
				break;
			case PINGPONG_RECV_WRID:
				if (--post_recv <= ctx->rx_depth - 2) {
					while (rcnt < user_param->iters &&
					       (ctx->rx_depth - post_recv) > 0 ) {
						++post_recv;
						if (ibv_post_recv(qp, &ctx->rwr, &bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv: rcnt=%d\n",
								rcnt);
							return 15;
						}
					}
				}
				rcnt += 1;
				break;
			default:
				fprintf(stderr, "Completion for unknown wr_id %d\n",
					(int) wc.wr_id);
				break;
			}
		}

		if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return 1;
		}
	}

	return(0);
}

/*
 *
 */
int run_iter_uni(struct pingpong_context *ctx, 
				 struct user_parameters *user_param,
				 struct pingpong_dest *rem_dest, 
				 int size)
{
	int i,ne,num_of_iters; 
	int scnt, ccnt, rcnt;
	struct ibv_recv_wr *bad_wr_recv;
	struct ibv_send_wr *bad_wr;
	struct ibv_wc wc;
	

	if (user_param->connection_type == UD) {
		if (size > 2048) {
			if (user_param->gid_index < 0) {
				size = 2048;
			} else {
				size = 1024;
			}
		}
	}

	if (user_param->connection_type == UD)
		ctx->recv_list.length = ctx->size + 40;
	else
		ctx->recv_list.length = ctx->size;

	if (size > user_param->inline_size) { /*complaince to perf_main */
		ctx->wr.send_flags = IBV_SEND_SIGNALED;
	} else {
		ctx->wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
	}
	ctx->list.length = size;
	scnt = 0;
	ccnt = 0;
	rcnt = 0;
	if (user_param->use_mcg && !user_param->servername ) {
		for (i=0; i < user_param->num_of_clients_mcg; i++) {
			 ctx->qp_mcg[i].package_counter = 0;
		}
	}
	// Server side.
	if (!user_param->servername) {

		num_of_iters = user_param->use_mcg ? 
					   user_param->iters*user_param->num_of_clients_mcg : user_param->iters;
		while (rcnt < num_of_iters && scnt < MAX_POLL_ITERATION_TIMEOUT) {
			
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
				ne = ibv_poll_cq(ctx->cq, 1 , &wc); 
				if (ne > 0) {
					if (wc.status != IBV_WC_SUCCESS) {
						fprintf(stderr, "Completion wth error at server :\n");
						fprintf(stderr, "Failed status %d: wr_id %d syndrom 0x%x\n",
								wc.status, (int) wc.wr_id, wc.vendor_err);
						fprintf(stderr, "scnt=%d, ccnt=%d\n",
							    scnt, ccnt);
						return 1;
					}
					++rcnt;
					scnt = 0;
					if (user_param->use_mcg) {
						for (i=0; i < user_param->num_of_clients_mcg; i++) {
							if ( wc.qp_num == ctx->qp_mcg[i].qp->qp_num) {
								
								ccnt = ctx->qp_mcg[i].package_counter; 
								ctx->qp_mcg[i].completion_array[ccnt] = get_cycles();
								ctx->qp_mcg[i].package_counter++;

								if (ibv_post_recv(ctx->qp_mcg[i].qp, &ctx->rwr, &bad_wr_recv)) {
									fprintf(stderr, "Couldn't post recv: rcnt=%d\n",rcnt);
									return 15;
								}
								break;
							}
						}
					}
					else {
						if (ibv_post_recv(ctx->qp, &ctx->rwr, &bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv: rcnt=%d\n",rcnt);
							return 15;
						}
					}
				}
				else {
					scnt++;
					//deal with stat ne = 0
				}
			} while (ne > 0 );

			if (ne < 0) {
				fprintf(stderr, "Poll Recieve CQ failed %d\n", ne);
				return 12;
			}
		}
	}
	else {
		/* client is posting and not receiving. */
		while (scnt < user_param->iters || ccnt < user_param->iters) {
			while (scnt < user_param->iters && (scnt - ccnt) < user_param->tx_depth ) {
				tposted[scnt] = get_cycles();
				if (ibv_post_send(ctx->qp, &ctx->wr, &bad_wr)) {
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
				for (;;) {
					ne = ibv_poll_cq(ctx->cq, 1, &wc);
					if (ne <= 0)
						break;

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
					ccnt += ne;
				}

				if (ne < 0) {
					fprintf(stderr, "poll CQ failed %d\n", ne);
					return 1;
				}
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
	struct pingpong_dest     my_dest;
	struct pingpong_dest    *rem_dest;
	struct user_parameters  user_param;
	struct ibv_device_attr device_attribute;
	char                    *ib_devname = NULL;
	int                      port = 18515;
	int                      ib_port = 1;
	long long                size = 65536;
	int			             sockfd;
	int                      i = 0;
	int                      size_max_pow = 24;
	int                      noPeak = 0;/*noPeak == 0: regular peak-bw calculation done*/
	int                      inline_given_in_cmd = 0;
	struct ibv_context       *context;
	int                      no_cpu_freq_fail = 0;
	union ibv_gid            gid;

	/* init default values to user's parameters */
	memset(&user_param, 0, sizeof(struct user_parameters));
	user_param.mtu = 0;
	user_param.num_of_clients_mcg = 1;
	user_param.iters = 1000;
	user_param.tx_depth = 300;
	user_param.servername = NULL;
	user_param.user_mgid = NULL;
	user_param.use_event = 0;
	user_param.duplex = 0;
	user_param.inline_size = 0;
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
			{ .name = "connection",     .has_arg = 1, .val = 'c' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "inline_size",    .has_arg = 1, .val = 'I' },
			{ .name = "rx-depth",       .has_arg = 1, .val = 'r' },
			{ .name = "qp-timeout",     .has_arg = 1, .val = 'u' },
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
			{ .name = "gid-index",      .has_arg = 1, .val = 'x' },
			{ .name = "mcg",            .has_arg = 1, .val = 'g' },
			{ .name = "MGID",           .has_arg = 1, .val = 'M' },
			{ .name = "all",            .has_arg = 0, .val = 'a' },
			{ .name = "bidirectional",  .has_arg = 0, .val = 'b' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "events",         .has_arg = 0, .val = 'e' },
			{ .name = "noPeak",         .has_arg = 0, .val = 'N' },
			{ .name = "CPU-freq",       .has_arg = 0, .val = 'F' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:m:c:s:n:t:I:r:u:S:x:g:M:ebaVNF", long_options, NULL);
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
		case 'e':
			++user_param.use_event;
			break;
        case 'g':
			++user_param.use_mcg;
			user_param.num_of_clients_mcg = strtol(optarg, NULL, 0);
            if (user_param.num_of_clients_mcg < 0) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'M' :
			user_param.use_user_mgid = 1;
			user_param.user_mgid = strdupa(optarg);
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
			user_param.all = ALL;
			break;
		case 'V':
			printf("send_bw version : %.2f\n",VERSION);
			return 0;
			break;
		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 0) {
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

	if (optind == argc - 1)
		user_param.servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	printf("------------------------------------------------------------------\n");

	if (user_param.use_mcg && user_param.duplex) {
		printf("Bidirectional connection in Multicast mode is not available yet\n");
		return 1;
	}

    if (user_param.use_mcg) {
		user_param.connection_type = UD;
        printf("                    Send BW  -  Multicast Test\n");
    }
	else if (user_param.duplex == 1)
		printf("                    Send Bidirectional BW Test\n");
	else 
		printf("                    Send BW Test\n");

	if (user_param.connection_type == RC)
		printf("Connection type : RC\n");
	else if (user_param.connection_type == UC)
		printf("Connection type : UC\n");
	else{
		printf("Connection type : UD\n");
	}
	if (user_param.gid_index > -1) {
		printf("Using GID to support RDMAoE configuration. Refer to port type as Ethernet, default MTU 1024B\n");
	}

	/* Done with parameter parsing. Perform setup. */
	if (user_param.all == ALL)
		/*since we run all sizes */
		size = 8388608; /*2^23 */
	else if (user_param.connection_type == UD && size > 2048) {
		printf("Max msg size in UD is 2048 changing to 2048\n");
		size = 2048;
	}
	if (user_param.connection_type == UD && user_param.gid_index > -1 && size > 1024) {
		printf("Max msg size in UD RDMAoE is 1024. changing to 1024\n");
		size = 1024;
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
	printf("Inline data is used up to %d bytes message\n", user_param.inline_size);
	
	ctx = pp_init_ctx(ib_dev, size, user_param.tx_depth, user_param.rx_depth,ib_port, &user_param);
	if (!ctx)
		return 1;

	/* Create connection between client and server.
	 * We do it by exchanging data over a TCP socket connection. */
	
	my_dest.lid = pp_get_local_lid(ctx, ib_port);
	my_dest.psn = lrand48() & 0xffffff;
    my_dest.qpn = (user_param.use_mcg && !user_param.servername) ? QPNUM_MCAST : ctx->qp->qp_num;

	if (user_param.gid_index != -1) {
		int err=0;
		err = ibv_query_gid (ctx->context, ib_port, user_param.gid_index, &gid);
		if (err) {
			return -1;
		}
		ctx->dgid=gid;
	}

	if (user_param.gid_index < 0) {/*We do not fail test upon lid in RDMA0E/Eth conf*/
			if (!my_dest.lid) {
				fprintf(stderr, "Local lid 0x0 detected. Is an SM running? If you are running on an RMDAoE interface you must use GIDs\n");
			return 1;
		}
	}
	my_dest.dgid = ctx->dgid;
    my_dest.rkey = ctx->mr->rkey;
	my_dest.vaddr = (uintptr_t)ctx->buf + size;

	printf("  local address:  LID %#04x, QPN %#06x, PSN %#06x\n" ,my_dest.lid,my_dest.qpn,my_dest.psn);

	if (user_param.use_mcg && !user_param.servername)  {
		printf("  M_"); 	   
	} 
	
	if (user_param.gid_index > -1 || (user_param.use_mcg && !user_param.servername) ) {
		printf("GID %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		my_dest.dgid.raw[0],my_dest.dgid.raw[1],
		my_dest.dgid.raw[2], my_dest.dgid.raw[3], my_dest.dgid.raw[4],
		my_dest.dgid.raw[5], my_dest.dgid.raw[6], my_dest.dgid.raw[7],
		my_dest.dgid.raw[8], my_dest.dgid.raw[9], my_dest.dgid.raw[10],
		my_dest.dgid.raw[11], my_dest.dgid.raw[12], my_dest.dgid.raw[13],
		my_dest.dgid.raw[14], my_dest.dgid.raw[15]);
	}
	
	if (user_param.servername) {
		sockfd = pp_client_connect(user_param.servername, port);
		if (sockfd < 0)
			return 1;
		rem_dest = pp_client_exch_dest(sockfd, &my_dest, &user_param);
	} else {
		sockfd = pp_server_connect(port);
		if (sockfd < 0)
			return 1;
		rem_dest = pp_server_exch_dest(sockfd, &my_dest, &user_param);
	}

	if (!rem_dest)
		return 1;

	printf("  remote address: LID %#04x, QPN %#06x, PSN %#06x\n",rem_dest->lid,rem_dest->qpn,rem_dest->psn);
	if (user_param.use_mcg && user_param.servername)  {
		printf("  M_"); 	   
	}

	if (user_param.gid_index > -1 || (user_param.use_mcg && user_param.servername) ) {
		printf("GID %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		rem_dest->dgid.raw[0],rem_dest->dgid.raw[1],rem_dest->dgid.raw[2], rem_dest->dgid.raw[3], rem_dest->dgid.raw[4],
		rem_dest->dgid.raw[5],rem_dest->dgid.raw[6], rem_dest->dgid.raw[7],rem_dest->dgid.raw[8], rem_dest->dgid.raw[9], 
		rem_dest->dgid.raw[10],rem_dest->dgid.raw[11], rem_dest->dgid.raw[12], rem_dest->dgid.raw[13],
		rem_dest->dgid.raw[14], rem_dest->dgid.raw[15]);
	}
	
	
	if (pp_connect_ctx(ctx, ib_port, my_dest.psn, rem_dest, &user_param))
		return 1;

	/* An additional handshake is required *after* moving qp to RTR.
	   Arbitrarily reuse exch_dest for this purpose. */
	if (user_param.servername) {
		rem_dest = pp_client_exch_dest(sockfd, &my_dest, &user_param);
	} else {
		rem_dest = pp_server_exch_dest(sockfd, &my_dest, &user_param);
	}

	if (user_param.use_event) {
		printf("Test with events.\n");
		if (ibv_req_notify_cq(ctx->cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		} 
	}
    printf("------------------------------------------------------------------\n");
	if (user_param.servername || user_param.duplex) {
		printf(" #bytes #iterations    BW peak[MB/sec]    BW average[MB/sec] \n");
	}
	else if (user_param.use_mcg) {
		printf(" #bytes #iterations    Packets Loss       BW Aggreg.[MB/sec]\n");
	}
	else { 
		printf(" Results are in the send side , Have a good day \n");
	}
	

	if(alloc_time_res(ctx,&user_param)) {
		fprintf(stderr, "Couldn't allocate the resources for the tests\n");
		return 1;
	}

	/* send */
	if (user_param.connection_type == UD) {
		ctx->list.addr = (uintptr_t)ctx->buf + 40;
		ctx->wr.wr.ud.ah          = ctx->ah;
		ctx->wr.wr.ud.remote_qkey = 0x11111111;
		if (user_param.use_mcg && user_param.servername) {
			ctx->wr.wr.ud.remote_qpn = QPNUM_MCAST;
		} else {
			ctx->wr.wr.ud.remote_qpn = rem_dest->qpn;
		}
	} else {
        ctx->list.addr = (uintptr_t)ctx->buf;
    }
	
	ctx->list.lkey = ctx->mr->lkey;
	ctx->wr.wr_id      = PINGPONG_SEND_WRID;
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_SEND;
	ctx->wr.next       = NULL;

	/* recieve */
	ctx->rwr.wr_id      = PINGPONG_RECV_WRID;
	ctx->rwr.sg_list    = &ctx->recv_list;
	ctx->rwr.num_sge    = 1;
	ctx->rwr.next       = NULL;
	ctx->recv_list.addr = (uintptr_t) ctx->buf;
	ctx->recv_list.lkey = ctx->mr->lkey;

	if (user_param.all == ALL) {
		if (user_param.connection_type == UD) {
			if (user_param.gid_index < 0) {
				size_max_pow = 12;
			} else {
				size_max_pow = 11;
			}
		}

		for (i = 1; i < size_max_pow ; ++i) {
			size = 1 << i;
			if (user_param.duplex) {
				if(run_iter_bi(ctx, &user_param, rem_dest, size))
					return 17;
			} else {
				if(run_iter_uni(ctx, &user_param, rem_dest, size))
					return 17;
			}
			if (user_param.servername) {
				print_report(user_param.iters, size, user_param.duplex, tposted, tcompleted, noPeak, no_cpu_freq_fail);	
			}
			else {
				if (user_param.use_mcg) {
					print_mcg_report(ctx,&user_param,size,no_cpu_freq_fail);
				}
			}
			if (user_param.servername) {
				rem_dest = pp_client_exch_dest(sockfd, &my_dest, &user_param);
			} else {
				rem_dest = pp_server_exch_dest(sockfd, &my_dest, &user_param);
			}
				
		}
	} else {
		if (user_param.duplex) {
			if (run_iter_bi(ctx, &user_param, rem_dest, size))
				return 18;
		}
		else {
			if(run_iter_uni(ctx, &user_param, rem_dest, size))
				return 18;
		}

		if (user_param.use_mcg && !user_param.servername) {
			print_mcg_report(ctx,&user_param,size,no_cpu_freq_fail);
		}

		if (user_param.servername || user_param.duplex)
			print_report(user_param.iters, size, user_param.duplex, tposted, tcompleted, noPeak, no_cpu_freq_fail);
	}
		
	/* close sockets */
	if (user_param.servername)
		rem_dest = pp_client_exch_dest(sockfd, &my_dest, &user_param);
	else
		rem_dest = pp_server_exch_dest(sockfd, &my_dest, &user_param);

	if (write(sockfd, "done", sizeof "done") != sizeof "done"){
		perror("write");
		fprintf(stderr, "Couldn't write to socket\n");
		return 1;
	}
	close(sockfd);
	if (!user_param.use_event) {
		if (destroy_ctx_resources(ctx,&user_param)) { 
			fprintf(stderr, "Couldn't destroy all of the resources\n");
			return 1;
		}
	}
	printf("------------------------------------------------------------------\n");
	return 0;
}
