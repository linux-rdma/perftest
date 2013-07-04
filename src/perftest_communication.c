#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <getopt.h>
#include <malloc.h>
#include <errno.h>
#include <byteswap.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "perftest_communication.h"

/******************************************************************************
 *
 ******************************************************************************/

static const char *sideArray[]  = {"local", "remote"};
static const char *gidArray[]   = {"GID"  , "MGID"};

/******************************************************************************
 *
 ******************************************************************************/

static int post_one_recv_wqe(struct pingpong_context *ctx) {

	struct ibv_recv_wr wr;
	struct ibv_recv_wr *bad_wr;
	struct ibv_sge list;

	list.addr   = (uintptr_t)ctx->buf;
	list.length = sizeof(struct pingpong_dest);
	list.lkey   = ctx->mr->lkey;

	wr.next = NULL;
	wr.wr_id = SYNC_SPEC_ID;
	wr.sg_list = &list;
	wr.num_sge = 1;

	if (ibv_post_recv(ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_recv failed for RDMA_CM QP\n");
		return FAILURE;
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
static int post_recv_to_get_ah(struct pingpong_context *ctx) {

	struct ibv_recv_wr wr;
	struct ibv_recv_wr *bad_wr;
	struct ibv_sge list;

	list.addr   = (uintptr_t)ctx->buf;
	list.length = UD_ADDITION + sizeof(uint32_t);
	list.lkey   = ctx->mr->lkey;

	wr.next = NULL;
	wr.wr_id = 0;
	wr.sg_list = &list;
	wr.num_sge = 1;

	if (ibv_post_recv(ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_recv failed for RDMA_CM QP\n");
		return FAILURE;
	}

	return SUCCESS;

}

/******************************************************************************
 *
 ******************************************************************************/
static int send_qp_num_for_ah(struct pingpong_context *ctx,
							  struct perftest_parameters *user_param) {

	struct ibv_send_wr wr;
	struct ibv_send_wr *bad_wr;
	struct ibv_sge list;
	struct ibv_wc wc;
	int ne;

	memcpy(ctx->buf,&ctx->qp[0]->qp_num,sizeof(uint32_t));

	list.addr   = (uintptr_t)ctx->buf;
	list.length = sizeof(uint32_t);
	list.lkey   = ctx->mr->lkey;

	wr.wr_id      = 0;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_SEND_WITH_IMM;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.next       = NULL;
    wr.imm_data   = htonl(ctx->qp[0]->qp_num);

	wr.wr.ud.ah = ctx->ah[0];
	wr.wr.ud.remote_qpn  = user_param->rem_ud_qpn;
	wr.wr.ud.remote_qkey = user_param->rem_ud_qkey;


	if (ibv_post_send(ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_send failed\n");
		return 1;
	}

	do { ne = ibv_poll_cq(ctx->send_cq, 1,&wc);} while (ne == 0);

	if (wc.status || wc.opcode != IBV_WC_SEND || wc.wr_id != 0) {
		fprintf(stderr, " Couldn't post send my QP number %d\n",(int)wc.status);
		return 1;
	}

	return 0;

}

/******************************************************************************
 *
 ******************************************************************************/
static int create_ah_from_wc_recv(struct pingpong_context *ctx,
								  struct perftest_parameters *user_parm) {

    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
	struct ibv_wc wc;
	int ne;

	do { ne = ibv_poll_cq(ctx->recv_cq,1,&wc);} while (ne == 0);

	if (wc.status || !(wc.opcode & IBV_WC_RECV) || wc.wr_id != 0) {
		fprintf(stderr, "Bad wc status when trying to create AH -- %d -- %d \n",(int)wc.status,(int)wc.wr_id);
		return 1;
	}

	ctx->ah[0] = ibv_create_ah_from_wc(ctx->pd,&wc,(struct ibv_grh*)ctx->buf,ctx->cm_id->port_num);
    user_parm->rem_ud_qpn = ntohl(wc.imm_data);
	ibv_query_qp(ctx->qp[0],&attr, IBV_QP_QKEY,&init_attr);
    user_parm->rem_ud_qkey = attr.qkey;

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ethernet_write_keys(struct pingpong_dest *my_dest,
							   struct perftest_comm *comm) {

    if (comm->rdma_params->gid_index == -1) {

		char msg[KEY_MSG_SIZE];



		sprintf(msg,KEY_PRINT_FMT,my_dest->lid,my_dest->out_reads,
				my_dest->qpn,my_dest->psn, my_dest->rkey, my_dest->vaddr);

		if (write(comm->rdma_params->sockfd,msg,sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return 1;
		}

    } else {
		char msg[KEY_MSG_SIZE_GID];
		sprintf(msg,KEY_PRINT_FMT_GID, my_dest->lid,my_dest->out_reads,
				my_dest->qpn,my_dest->psn, my_dest->rkey, my_dest->vaddr,
				my_dest->gid.raw[0],my_dest->gid.raw[1],
				my_dest->gid.raw[2],my_dest->gid.raw[3],
				my_dest->gid.raw[4],my_dest->gid.raw[5],
				my_dest->gid.raw[6],my_dest->gid.raw[7],
				my_dest->gid.raw[8],my_dest->gid.raw[9],
				my_dest->gid.raw[10],my_dest->gid.raw[11],
				my_dest->gid.raw[12],my_dest->gid.raw[13],
				my_dest->gid.raw[14],my_dest->gid.raw[15]);

		if (write(comm->rdma_params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return 1;
		}

	}

    return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ethernet_read_keys(struct pingpong_dest *rem_dest,
							  struct perftest_comm *comm)  {

if (comm->rdma_params->gid_index == -1){

        int parsed;
		char msg[KEY_MSG_SIZE];

		if (read(comm->rdma_params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return 1;
		}

		parsed = sscanf(msg,KEY_PRINT_FMT,(unsigned int*)&rem_dest->lid,
						&rem_dest->out_reads,&rem_dest->qpn,
						&rem_dest->psn, &rem_dest->rkey,&rem_dest->vaddr);
		if (parsed != 6) {
			fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg, msg);
			return 1;
		}

	} else {

		char msg[KEY_MSG_SIZE_GID];
		char *pstr = msg, *term;
		char tmp[20];
		int i;

		if (read(comm->rdma_params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return 1;
		}

		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->lid = (int)strtol(tmp, NULL, 16); // LID

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->out_reads = (int)strtol(tmp, NULL, 16); // OUT_READS

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
		rem_dest->rkey = (unsigned)strtoul(tmp, NULL, 16); // RKEY

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

			rem_dest->gid.raw[i] = (unsigned char)strtoll(tmp, NULL, 16);
		}

		pstr += term - pstr + 1;

		strcpy(tmp, pstr);
		rem_dest->gid.raw[15] = (unsigned char)strtoll(tmp, NULL, 16);

	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int rdma_write_keys(struct pingpong_dest *my_dest,
						   struct perftest_comm *comm) {

	struct ibv_send_wr wr;
	struct ibv_send_wr *bad_wr;
	struct ibv_sge list;
	struct ibv_wc wc;
	int ne;

	memcpy(comm->rdma_ctx->buf,my_dest,sizeof(struct pingpong_dest));

	list.addr   = (uintptr_t)comm->rdma_ctx->buf;
	list.length = sizeof(struct pingpong_dest);
	list.lkey   = comm->rdma_ctx->mr->lkey;

	wr.wr_id      = SYNC_SPEC_ID;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.next       = NULL;

	if (ibv_post_send(comm->rdma_ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_send failed\n");
		return 1;
	}

	do {
		ne = ibv_poll_cq(comm->rdma_ctx->send_cq, 1,&wc);
	} while (ne == 0);

	if (wc.status || wc.opcode != IBV_WC_SEND || wc.wr_id != SYNC_SPEC_ID) {
		fprintf(stderr, " Bad wc status %d\n",(int)wc.status);
		return 1;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int rdma_read_keys(struct pingpong_dest *rem_dest,
						  struct perftest_comm *comm) {

	struct ibv_wc wc;
	int ne;

	do {
		ne = ibv_poll_cq(comm->rdma_ctx->recv_cq,1,&wc);
	} while (ne == 0);

	if (wc.status || !(wc.opcode & IBV_WC_RECV) || wc.wr_id != SYNC_SPEC_ID) {
		fprintf(stderr, "Bad wc status -- %d -- %d \n",(int)wc.status,(int)wc.wr_id);
		return 1;
	}

	memcpy(rem_dest,comm->rdma_ctx->buf,sizeof(struct pingpong_dest));

	if (post_one_recv_wqe(comm->rdma_ctx)) {
		fprintf(stderr, "Couldn't post send \n");
		return 1;
	}

	return 0;
}

 /******************************************************************************
 *
 ******************************************************************************/
static int ethernet_client_connect(struct perftest_comm *comm) {

	struct addrinfo *res, *t;
	struct addrinfo hints;
	char *service;

	int sockfd = -1;
	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (check_add_port(&service,comm->rdma_params->port,comm->rdma_params->servername,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic address and port\n");
		return 1;
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
		fprintf(stderr, "Couldn't connect to %s:%d\n",comm->rdma_params->servername,comm->rdma_params->port);
		return 1;
	}

	comm->rdma_params->sockfd = sockfd;
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ethernet_server_connect(struct perftest_comm *comm) {

	struct addrinfo *res, *t;
	struct addrinfo hints;
	char *service;
	int n;

	int sockfd = -1, connfd;
	memset(&hints, 0, sizeof hints);
	hints.ai_flags    = AI_PASSIVE;
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (check_add_port(&service,comm->rdma_params->port,NULL,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic adress and port\n");
		return 1;
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
		fprintf(stderr, "Couldn't listen to port %d\n", comm->rdma_params->port);
		return 1;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);

	if (connfd < 0) {
		perror("server accept");
		fprintf(stderr, "accept() failed\n");
		close(sockfd);
		return 1;
	}
	close(sockfd);
	comm->rdma_params->sockfd = connfd;
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int set_up_connection(struct pingpong_context *ctx,
					  struct perftest_parameters *user_param,
					  struct pingpong_dest *my_dest) {

	int i;
	union ibv_gid temp_gid;
	union ibv_gid temp_gid2;

	srand48(getpid() * time(NULL));

	if (user_param->gid_index != -1) {
		if (ibv_query_gid(ctx->context,user_param->ib_port,user_param->gid_index,&temp_gid)) {
			return -1;
		}
	}

	if (user_param->dualport==ON) { //dual port case
		if (user_param->gid_index2 != -1) {
				if (ibv_query_gid(ctx->context,user_param->ib_port2,user_param->gid_index,&temp_gid2)) {
					return -1;
				}
			}
	}

	for (i = 0; i < user_param->num_of_qps; i++) {

		if (user_param->dualport == ON) {
			// first half of qps are for ib_port and second half are for ib_port2
			if (i < user_param->num_of_qps / 2) {
				my_dest[i].lid   = ctx_get_local_lid(ctx->context,user_param->ib_port);

			} else {
				my_dest[i].lid   = ctx_get_local_lid(ctx->context,user_param->ib_port2);
			}
		// single-port case
		} else {
			my_dest[i].lid   = ctx_get_local_lid(ctx->context,user_param->ib_port);
		}

		my_dest[i].qpn   = ctx->qp[i]->qp_num;
		my_dest[i].psn   = lrand48() & 0xffffff;
		my_dest[i].rkey  = ctx->mr->rkey;

		// Each qp gives his receive buffer address .
		my_dest[i].out_reads = user_param->out_reads;
		my_dest[i].vaddr = (uintptr_t)ctx->buf + (user_param->num_of_qps + i)*BUFF_SIZE(ctx->size);

		if (user_param->dualport==ON) {

			if (i < user_param->num_of_qps / 2)
				memcpy(my_dest[i].gid.raw,temp_gid.raw ,16);

			else {
				memcpy(my_dest[i].gid.raw,temp_gid2.raw ,16);
			}

		} else
			memcpy(my_dest[i].gid.raw,temp_gid.raw ,16);

		// We do not fail test upon lid above RoCE.
		if ( (user_param->gid_index < 0) ||  ((user_param->gid_index2 < 0) && (user_param->dualport == ON))  ){
			if (!my_dest[i].lid) {
				fprintf(stderr," Local lid 0x0 detected. Is an SM running? \n");
				return -1;
			}
		}
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int rdma_client_connect(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

    char *service;
    int temp,num_of_retry= NUM_OF_RETRIES;
    struct sockaddr_in sin;
    struct addrinfo *res;
    struct rdma_cm_event *event;
    struct rdma_conn_param conn_param;
    struct addrinfo hints;

	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (check_add_port(&service,user_param->port,user_param->servername,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic adress and port\n");
		return FAILURE;
	}

	sin.sin_addr.s_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
	sin.sin_family = PF_INET;
	sin.sin_port = htons((unsigned short)user_param->port);

	while (1) {

	if (num_of_retry == 0) {
	    fprintf(stderr, "Received %d times ADDR_ERROR\n",NUM_OF_RETRIES);
	    return FAILURE;
	}

	if (rdma_resolve_addr(ctx->cm_id, NULL,(struct sockaddr *)&sin,2000)) {
	    fprintf(stderr, "rdma_resolve_addr failed\n");
	    return FAILURE;
	}

	if (rdma_get_cm_event(ctx->cm_channel,&event)) {
	    fprintf(stderr, "rdma_get_cm_events failed\n");
	    return FAILURE;
	}

	if (event->event == RDMA_CM_EVENT_ADDR_ERROR) {
	    num_of_retry--;
	    rdma_ack_cm_event(event);
	    continue;
	}

	if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
	    fprintf(stderr, "unexpected CM event %d\n",event->event);
	    rdma_ack_cm_event(event);
	    return FAILURE;
	}

	rdma_ack_cm_event(event);
	break;
   }

	if (user_param->tos != DEF_TOS) {

		if (rdma_set_option(ctx->cm_id,RDMA_OPTION_ID,RDMA_OPTION_ID_TOS,&user_param->tos,sizeof(uint8_t))) {
			fprintf(stderr, " Set TOS option failed: %d\n",event->event);
			return FAILURE;
		}
	}

    while (1) {

		if (num_of_retry <= 0) {
			fprintf(stderr, "Received %d times ADDR_ERROR - aborting\n",NUM_OF_RETRIES);
			return FAILURE;
		}

		if (rdma_resolve_route(ctx->cm_id,2000)) {
			fprintf(stderr, "rdma_resolve_route failed\n");
			return FAILURE;
		}

		if (rdma_get_cm_event(ctx->cm_channel,&event)) {
			fprintf(stderr, "rdma_get_cm_events failed\n");
			return FAILURE;
		}

		if (event->event == RDMA_CM_EVENT_ROUTE_ERROR) {
			num_of_retry--;
			rdma_ack_cm_event(event);
			continue;
		}

		if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
			fprintf(stderr, "unexpected CM event %d\n",event->event);
			rdma_ack_cm_event(event);
			return FAILURE;
		}

		rdma_ack_cm_event(event);
		break;
	}

	ctx->context = ctx->cm_id->verbs;
	temp = user_param->work_rdma_cm;
	user_param->work_rdma_cm = ON;

	if (ctx_init(ctx,user_param)) {
		fprintf(stderr," Unable to create the resources needed by comm struct\n");
		return FAILURE;
	}

	memset(&conn_param, 0, sizeof conn_param);
	if (user_param->verb == READ) {
		conn_param.responder_resources = user_param->out_reads;
		conn_param.initiator_depth = user_param->out_reads;
	}
	user_param->work_rdma_cm = temp;

	if (user_param->work_rdma_cm == OFF) {

		if (post_one_recv_wqe(ctx)) {
			fprintf(stderr, "Couldn't post send \n");
			return 1;
		}
	}

	if (rdma_connect(ctx->cm_id,&conn_param)) {
		fprintf(stderr, "Function rdma_connect failed\n");
		return FAILURE;
	}

	if (rdma_get_cm_event(ctx->cm_channel,&event)) {
		fprintf(stderr, "rdma_get_cm_events failed\n");
		return FAILURE;
	}

	if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
		fprintf(stderr, "Unexpected CM event bl blka %d\n",event->event);
		return FAILURE;
	}

	if (user_param->connection_type == UD) {

        user_param->rem_ud_qpn  = event->param.ud.qp_num;
        user_param->rem_ud_qkey = event->param.ud.qkey;

		ctx->ah[0] = ibv_create_ah(ctx->pd,&event->param.ud.ah_attr);

		if (!ctx->ah) {
			printf(" Unable to create address handler for UD QP\n");
			return FAILURE;
        }

		if (user_param->tst == LAT || (user_param->tst == BW && user_param->duplex)) {

			if (send_qp_num_for_ah(ctx,user_param)) {
				printf(" Unable to send my QP number\n");
				return FAILURE;
			}
		}
	}

	rdma_ack_cm_event(event);
	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
int rdma_server_connect(struct pingpong_context *ctx,
						struct perftest_parameters *user_param) {

	int temp;
	struct addrinfo *res;
	struct rdma_cm_event *event;
	struct rdma_conn_param conn_param;
	struct addrinfo hints;
	char *service;
	struct sockaddr_in sin;

	memset(&hints, 0, sizeof hints);
	hints.ai_flags    = AI_PASSIVE;
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (check_add_port(&service,user_param->port,user_param->servername,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic adress and port\n");
		return FAILURE;
	}

	sin.sin_addr.s_addr = 0;
	sin.sin_family = PF_INET;
	sin.sin_port = htons((unsigned short)user_param->port);

	if (rdma_bind_addr(ctx->cm_id_control,(struct sockaddr *)&sin)) {
		fprintf(stderr," rdma_bind_addr failed\n");
		return 1;
	}

	if (rdma_listen(ctx->cm_id_control,0)) {
		fprintf(stderr, "rdma_listen failed\n");
		return 1;
	}

	if (rdma_get_cm_event(ctx->cm_channel,&event)) {
		fprintf(stderr, "rdma_get_cm_events failed\n");
		return 1;
	}

	if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
		fprintf(stderr, "bad event waiting for connect request %d\n",event->event);
		return 1;
	}

	ctx->cm_id = (struct rdma_cm_id*)event->id;
	ctx->context = ctx->cm_id->verbs;

	if (user_param->work_rdma_cm == ON)
		alloc_ctx(ctx,user_param);

	temp = user_param->work_rdma_cm;
	user_param->work_rdma_cm = ON;

	if (ctx_init(ctx,user_param)) {
		fprintf(stderr," Unable to create the resources needed by comm struct\n");
		return FAILURE;
	}

	memset(&conn_param, 0, sizeof conn_param);
	if (user_param->verb == READ) {
		conn_param.responder_resources = user_param->out_reads;
		conn_param.initiator_depth = user_param->out_reads;
	}
	if (user_param->connection_type == UD)
		conn_param.qp_num = ctx->qp[0]->qp_num;

	user_param->work_rdma_cm = temp;

	if (user_param->work_rdma_cm == OFF) {

		if (post_one_recv_wqe(ctx)) {
			fprintf(stderr, "Couldn't post send \n");
			return 1;
		}

	} else if (user_param->connection_type == UD) {

		if (user_param->tst == LAT || (user_param->tst == BW && user_param->duplex)) {

			if (post_recv_to_get_ah(ctx)) {
				fprintf(stderr, "Couldn't post send \n");
				return 1;
			}
		}
	}

	if (rdma_accept(ctx->cm_id, &conn_param)) {
		fprintf(stderr, "Function rdma_accept failed\n");
		return 1;
    }

	if (user_param->work_rdma_cm && user_param->connection_type == UD) {

		if (user_param->tst == LAT || (user_param->tst == BW && user_param->duplex)) {
			if (create_ah_from_wc_recv(ctx,user_param)) {
				fprintf(stderr, "Unable to create AH from WC\n");
				return 1;
			}
		}
	}

	rdma_ack_cm_event(event);
	rdma_destroy_id(ctx->cm_id_control);
    freeaddrinfo(res);
    return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int create_comm_struct(struct perftest_comm *comm,
					   struct perftest_parameters *user_param) {


	ALLOCATE(comm->rdma_params,struct perftest_parameters,1);
	memset(comm->rdma_params,0,sizeof(struct perftest_parameters));

	comm->rdma_params->port		   = user_param->port;
	comm->rdma_params->sockfd      = -1;
	comm->rdma_params->gid_index   = user_param->gid_index;
	comm->rdma_params->use_rdma_cm = user_param->use_rdma_cm;
	comm->rdma_params->servername  = user_param->servername;
	comm->rdma_params->machine 	   = user_param->machine;
	comm->rdma_params->side		   = LOCAL;
	comm->rdma_params->verb		   = user_param->verb;
	comm->rdma_params->use_mcg	   = user_param->use_mcg;
	comm->rdma_params->duplex	   = user_param->duplex;
	comm->rdma_params->tos             = DEF_TOS;

	if (user_param->use_rdma_cm) {

		ALLOCATE(comm->rdma_ctx,struct pingpong_context,1);
		memset(comm->rdma_ctx,0,sizeof(struct pingpong_context));

		comm->rdma_params->tx_depth = 1;
		comm->rdma_params->rx_depth = 1;
		comm->rdma_params->connection_type = RC;
		comm->rdma_params->num_of_qps = 1;
		comm->rdma_params->verb	= SEND;
		comm->rdma_params->size = sizeof(struct pingpong_dest);
		comm->rdma_ctx->context = NULL;

		ALLOCATE(comm->rdma_ctx->qp,struct ibv_qp*,comm->rdma_params->num_of_qps);
		comm->rdma_ctx->buff_size = cycle_buffer;

		if (create_rdma_resources(comm->rdma_ctx,comm->rdma_params)) {
			fprintf(stderr," Unable to create the resources needed by comm struct\n");
			return FAILURE;
		}
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
int establish_connection(struct perftest_comm *comm) {

	int (*ptr)(struct perftest_comm*);

	if (comm->rdma_params->use_rdma_cm) {

		if (comm->rdma_params->machine == CLIENT) {

			if (rdma_client_connect(comm->rdma_ctx,comm->rdma_params)) {
				fprintf(stderr," Unable to perform rdma_client function\n");
				return 1;
			}

		} else {

			if (rdma_server_connect(comm->rdma_ctx,comm->rdma_params)) {
				fprintf(stderr," Unable to perform rdma_client function\n");
				return 1;
			}
		}

	} else {

		ptr = comm->rdma_params->servername ? &ethernet_client_connect : &ethernet_server_connect;

		if ((*ptr)(comm)) {
			fprintf(stderr,"Unable to open file descriptor for socket connection");
			return 1;
		}

	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_hand_shake(struct perftest_comm *comm,
				   struct pingpong_dest *my_dest,
				   struct pingpong_dest *rem_dest) {

	int (*read_func_ptr) (struct pingpong_dest*,struct perftest_comm*);
	int (*write_func_ptr)(struct pingpong_dest*,struct perftest_comm*);

	if (comm->rdma_params->use_rdma_cm || comm->rdma_params->work_rdma_cm) {
		read_func_ptr  = &rdma_read_keys;
		write_func_ptr = &rdma_write_keys;

	} else {
		read_func_ptr  = &ethernet_read_keys;
		write_func_ptr = &ethernet_write_keys;

	}

	if (comm->rdma_params->servername) {
		if ((*write_func_ptr)(my_dest,comm)) {
			fprintf(stderr," Unable to write to socket/rdam_cm\n");
			return 1;
		}

		if ((*read_func_ptr)(rem_dest,comm)) {
			fprintf(stderr," Unable to read from socket/rdam_cm\n");
			return 1;
		}

	// Server side will wait for the client side to reach the write function.
	} else {

		if ((*read_func_ptr)(rem_dest,comm)) {
			fprintf(stderr," Unable to read to socket/rdam_cm\n");
			return 1;
		}

		if ((*write_func_ptr)(my_dest,comm)) {
			fprintf(stderr," Unable to write from socket/rdam_cm\n");
			return 1;
		}
	}

    return 0;
}




/******************************************************************************
 *
 ******************************************************************************/
void ctx_print_pingpong_data(struct pingpong_dest *element,
							 struct perftest_comm *comm) {

	int is_there_mgid,local_mgid,remote_mgid;

	// First of all we print the basic format.
    printf(BASIC_ADDR_FMT,sideArray[comm->rdma_params->side],element->lid,element->qpn,element->psn);

	switch (comm->rdma_params->verb) {
		case 2  : printf(READ_FMT,element->out_reads);
		case 1  : printf(RDMA_FMT,element->rkey,element->vaddr);
		default : putchar('\n');
	}

	local_mgid    = (comm->rdma_params->side == 0)  && (comm->rdma_params->machine == 0);
	remote_mgid   = (comm->rdma_params->side == 1)  && (comm->rdma_params->machine == 1);
	is_there_mgid =  comm->rdma_params->duplex || remote_mgid || local_mgid;

	if ((comm->rdma_params->gid_index > -1 || (comm->rdma_params->use_mcg && is_there_mgid)) && comm->rdma_params->connection_type != RawEth) {

		printf(PERF_GID_FMT,gidArray[comm->rdma_params->use_mcg && is_there_mgid],
				element->gid.raw[0], element->gid.raw[1],
				element->gid.raw[2], element->gid.raw[3],
			    element->gid.raw[4], element->gid.raw[5],
			    element->gid.raw[6], element->gid.raw[7],
			   	element->gid.raw[8], element->gid.raw[9],
			    element->gid.raw[10],element->gid.raw[11],
			    element->gid.raw[12],element->gid.raw[13],
				element->gid.raw[14],element->gid.raw[15]);
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_close_connection(struct perftest_comm *comm,
						 struct pingpong_dest *my_dest,
						 struct pingpong_dest *rem_dest) {

	// Signal client is finished.
    if (ctx_hand_shake(comm,my_dest,rem_dest)) {
        return 1;
    }

	if (!comm->rdma_params->use_rdma_cm && !comm->rdma_params->work_rdma_cm) {

        if (write(comm->rdma_params->sockfd,"done",sizeof "done") != sizeof "done") {
                perror(" Client write");
                fprintf(stderr,"Couldn't write to socket\n");
                return -1;
        }

		close(comm->rdma_params->sockfd);
		return 0;
	}

	return 0;
}

/******************************************************************************
 * End
 ******************************************************************************/
