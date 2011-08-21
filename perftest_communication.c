#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include <malloc.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <math.h>
#include <byteswap.h>
#include "perftest_communication.h"

/****************************************************************************** 
 *
 ******************************************************************************/

static const char *sideArray[]  = {"local", "remote"};
static const char *gidArray[]   = {"GID"  , "MGID"};

/****************************************************************************** 
 *
 ******************************************************************************/
static int check_add_port(char **service,int port,
						  const char *servername,
						  struct addrinfo *hints,
						  struct addrinfo **res) {

	int number;

	if (asprintf(service,"%d", port) < 0)
		return FAILURE;

	number = getaddrinfo(servername,*service,hints,res);

	if (number < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(number), servername, port);
		return FAILURE;
	}

	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ethernet_write_keys(struct pingpong_dest *my_dest,
							   struct perftest_comm *comm) {

    if (comm->gid_index == -1) {

		char msg[KEY_MSG_SIZE];
		sprintf(msg,KEY_PRINT_FMT,my_dest->lid,my_dest->out_reads,
				my_dest->qpn,my_dest->psn, my_dest->rkey, my_dest->vaddr);
		if (write(comm->sockfd,msg,sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return FAILURE;
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
		if (write(comm->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return FAILURE;
		}	
	}
    return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ethernet_read_keys(struct pingpong_dest *rem_dest,
							  struct perftest_comm *comm)  {
    
	if (comm->gid_index == -1) {

        int parsed;
		char msg[KEY_MSG_SIZE];

		if (read(comm->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return FAILURE;
		}

		parsed = sscanf(msg,KEY_PRINT_FMT,&rem_dest->lid,
						&rem_dest->out_reads,&rem_dest->qpn,
						&rem_dest->psn, &rem_dest->rkey,&rem_dest->vaddr);

		if (parsed != 6) {
			fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg, msg);
			return FAILURE;
		}
        
	} else {

		char msg[KEY_MSG_SIZE_GID];
		char *pstr = msg, *term;
		char tmp[20];
		int i;

		if (read(comm->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return FAILURE;
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
	return SUCCESS;
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

	memcpy(comm->my_dest_buff,my_dest,sizeof(struct pingpong_dest));

	list.addr   = (uintptr_t)comm->my_dest_buff;
	list.length = sizeof(struct pingpong_dest);
	list.lkey   = comm->my_cm_mr->lkey;

	wr.wr_id      = 1;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.next       = NULL;

	if (ibv_post_send(comm->cm_qp,&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_send failed\n");
		return FAILURE;
	}

	do {
		ne = ibv_poll_cq(comm->cm_cq, 1,&wc);
	} while (ne == 0);

	if (wc.status || wc.opcode != IBV_WC_SEND || wc.wr_id != 1) {
		fprintf(stderr, "Bad wc status %d\n",(int)wc.status);
		return FAILURE;
	}

	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int rdma_read_keys(struct pingpong_dest *rem_dest,
						  struct perftest_comm *comm) {

	struct ibv_recv_wr wr;
	struct ibv_recv_wr *bad_wr;
	struct ibv_sge list;
	struct ibv_wc wc;
	int ne;

	do {
		ne = ibv_poll_cq(comm->cm_cq,1,&wc);
	} while (ne == 0);

	if (wc.status || !(wc.opcode & IBV_WC_RECV) || wc.wr_id != 0) {
		fprintf(stderr, "Bad wc status -- %d -- %d \n",(int)wc.status,(int)wc.wr_id);
		return FAILURE;
	}

	memcpy(rem_dest,comm->rem_dest_buff,sizeof(struct pingpong_dest));

	list.addr   = (uintptr_t)comm->rem_dest_buff;
    list.length = sizeof(struct pingpong_dest);
    list.lkey   = comm->rem_cm_mr->lkey;
    wr.next = NULL;
    wr.wr_id = 0;
    wr.sg_list = &list;
    wr.num_sge = 1;

    if (ibv_post_recv(comm->cm_qp,&wr,&bad_wr)) {
        fprintf(stderr, "Function ibv_post_recv failed for RDMA_CM QP\n");
		return FAILURE;
	}
	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int comm_rdma_resources(struct perftest_comm *comm) {

	struct ibv_qp_init_attr attr;
	struct ibv_sge list;
    struct ibv_recv_wr wr,*bad_wr;
	struct rdma_cm_id *curr_cm_id = NULL;

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	curr_cm_id = (comm->servername) ? comm->cm_id : comm->child_cm_id;
	
	// Create resources - should come from a library.
	comm->my_dest_buff  = memalign(sysconf(_SC_PAGESIZE),sizeof(struct pingpong_dest));
	comm->rem_dest_buff = memalign(sysconf(_SC_PAGESIZE),sizeof(struct pingpong_dest));

	if (!comm->my_dest_buff || !comm->rem_dest_buff) {
		fprintf(stderr, "Couldn't allocate buffers for RDMA_CM communication method\n");
		return FAILURE;
	}

	if (!curr_cm_id->verbs) {
		fprintf(stderr, " Unbound cm_id!! - aborting\n");
		return FAILURE;
	}

	comm->cm_pd = ibv_alloc_pd(curr_cm_id->verbs);
	if (!comm->cm_pd) {
		fprintf(stderr," Couldn't allocate PD for RDMA_CM communication method\n");
		return FAILURE;
	}

	comm->my_cm_mr  = ibv_reg_mr(comm->cm_pd,
								 comm->my_dest_buff,
								 sizeof(struct pingpong_dest),
								 IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);

	comm->rem_cm_mr = ibv_reg_mr(comm->cm_pd,
								 comm->rem_dest_buff,
								 sizeof(struct pingpong_dest),
								 IBV_ACCESS_REMOTE_WRITE |IBV_ACCESS_LOCAL_WRITE);

	if (!comm->my_cm_mr || !comm->rem_cm_mr) {
		fprintf(stderr," Couldn't allocate MRs for RDMA_CM communication method\n");
		return FAILURE;
	}

	comm->cm_cq = ibv_create_cq(curr_cm_id->verbs,1,NULL,NULL,0);

	if (!comm->cm_cq) {
		fprintf(stderr, " Couldn't allocate CQ for RDMA_CM communication method\n");
		return FAILURE;
	}

	attr.send_cq = comm->cm_cq;
	attr.recv_cq = comm->cm_cq;
	attr.cap.max_send_wr  = 1;
	attr.cap.max_recv_wr  = 1;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	attr.cap.max_inline_data = 0;
	attr.qp_type = IBV_QPT_RC;

	if (rdma_create_qp(curr_cm_id,comm->cm_pd,&attr)) {
		fprintf(stderr, " rdma_create_qp function failed\n");
		return FAILURE;
	}

	comm->cm_qp = curr_cm_id->qp;

	list.addr   = (uintptr_t)comm->rem_dest_buff;
    list.length = sizeof(struct pingpong_dest);
    list.lkey   = comm->rem_cm_mr->lkey;
    wr.next = NULL;
    wr.wr_id = 0;
    wr.sg_list = &list;
    wr.num_sge = 1;

    if (ibv_post_recv(comm->cm_qp, &wr,&bad_wr)) {
        fprintf(stderr, "Function ibv_post_recv failed for RDMA_CM QP\n");
		return FAILURE;
	}

	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int rdma_cm_client_connect(struct perftest_comm *comm) {

	char *service;
	int num_of_retry = 10;
	struct sockaddr_in sin;
	struct addrinfo *res;
	struct rdma_cm_event *event;
	struct rdma_conn_param conn_param;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	
	if (check_add_port(&service,comm->port,comm->servername,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic adress and port\n");
		return FAILURE;
	}

	sin.sin_addr.s_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
	sin.sin_family = AF_INET;
	sin.sin_port = htons(comm->port);

	while (1) {

		if (num_of_retry == 0) {
			fprintf(stderr, "Received 10 times ADDR_ERROR - aborting\n");
			return FAILURE;
		}

		if (rdma_resolve_addr(comm->cm_id, NULL,(struct sockaddr *)&sin,2000)) {
			fprintf(stderr, "rdma_resolve_addr failed\n");
			return FAILURE;
		}

		if (rdma_get_cm_event(comm->cm_channel,&event)) {
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
	
	while (1) {

		if (num_of_retry <= 0) {
			fprintf(stderr, "Received %d times ADDR_ERROR - aborting\n",num_of_retry);
			return FAILURE;
		}

		if (rdma_resolve_route(comm->cm_id,2000)) {
			fprintf(stderr, "rdma_resolve_route failed\n");
			return FAILURE;
		}
	
		if (rdma_get_cm_event(comm->cm_channel,&event)) {
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

	if (comm_rdma_resources(comm)) {
		fprintf(stderr, " Failed to create some of the rdma resources\n");
		return FAILURE;
	}

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.retry_count = 5;

	if (rdma_connect(comm->cm_id, &conn_param)) {
		fprintf(stderr, "Function rdma_connect failed\n");
		return FAILURE;
	}

	if (rdma_get_cm_event(comm->cm_channel,&event)) {
		fprintf(stderr, "rdma_get_cm_events failed\n"); 
		return FAILURE; 
	}

	if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
		fprintf(stderr, "Unexpected CM event %d\n",event->event);
		return FAILURE;
	}

	rdma_ack_cm_event(event);
	freeaddrinfo(res);
	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int rdma_cm_server_connect(struct perftest_comm *comm) {

	struct addrinfo *res;
	struct rdma_cm_event *event;
	struct rdma_conn_param conn_param;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	struct sockaddr_in sin;

	if (check_add_port(&service,comm->port,NULL,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic adress and port\n");
		return FAILURE;
	}

	sin.sin_addr.s_addr = 0;
	sin.sin_family = AF_INET;
	sin.sin_port = htons(comm->port);
	
	if (rdma_bind_addr(comm->cm_id,(struct sockaddr *)&sin)) {
		fprintf(stderr," rdma_bind_addr failed\n");
		return FAILURE;
	}

	if (rdma_listen(comm->cm_id,0)) {
		fprintf(stderr, "rdma_listen failed\n");
		return FAILURE;
	}
	
	if (rdma_get_cm_event(comm->cm_channel,&event)) {
		fprintf(stderr, "rdma_get_cm_events failed\n"); 
		return FAILURE; 
	}

	if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
		fprintf(stderr, "bad event waiting for connect request %d\n",event->event);
		return FAILURE;
	}

	comm->child_cm_id = (struct rdma_cm_id*)event->id;

	if (comm_rdma_resources(comm)) {
		fprintf(stderr, "Failed to create some of the rdma resources\n");
		return FAILURE;
	}

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.retry_count = 5;

	if (rdma_accept(comm->child_cm_id, &conn_param)) {
		fprintf(stderr, "Function rdma_accept failed\n");
		return FAILURE;
	}

	rdma_ack_cm_event(event);

	if (rdma_get_cm_event(comm->cm_channel,&event)) {
		fprintf(stderr, "rdma_get_cm_events failed\n"); 
		return FAILURE; 
	}

	if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
		fprintf(stderr, "Bad event waiting for established connection\n");
		return FAILURE;
	}

	rdma_ack_cm_event(event);
	freeaddrinfo(res);
	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ethernet_client_connect(struct perftest_comm *comm) {
    
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int sockfd = -1;

	if (check_add_port(&service,comm->port,comm->servername,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic adress and port\n");
		return FAILURE;
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
		fprintf(stderr, "Couldn't connect to %s:%d\n", comm->servername, comm->port);
		return FAILURE;
	}
	comm->sockfd = sockfd;
	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ethernet_server_connect(struct perftest_comm *comm) {

	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int sockfd = -1, connfd,n;

	if (check_add_port(&service,comm->port,NULL,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic adress and port\n");
		return FAILURE;
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
		fprintf(stderr, "Couldn't listen to port %d\n", comm->port);
		return FAILURE;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);
	if (connfd < 0) {
		perror("server accept");
		fprintf(stderr, "accept() failed\n");
		close(sockfd);
		return FAILURE;
	}

	close(sockfd);
	comm->sockfd = connfd;
	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int create_comm_struct(struct perftest_comm *comm,
					   int port,
					   int index,
	 				   int use_rdma,
					   const char *servername) {

	comm->port        = port;
	comm->sockfd      = -1;
	comm->gid_index   = index;
	comm->use_rdma_cm = use_rdma;
	comm->cm_channel  = NULL;
	comm->cm_id       = NULL;
	comm->servername  = servername;
	comm->cm_pd 	  = NULL;
	comm->my_cm_mr 	  = NULL;
	comm->rem_cm_mr	  = NULL;
	comm->cm_cq 	  = NULL;
	comm->cm_qp		  = NULL;

	if (use_rdma) {

		comm->cm_channel = rdma_create_event_channel();
		if (comm->cm_channel == NULL) {
			fprintf(stderr, "rdma_create_event_channel failed\n");
			return FAILURE;
		}

		if (rdma_create_id(comm->cm_channel,&comm->cm_id,NULL,RDMA_PS_TCP)) {
			fprintf(stderr,"rdma_create_id failed\n");
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

	if (comm->use_rdma_cm) {

		ptr = comm->servername ? &rdma_cm_client_connect : &rdma_cm_server_connect;

		if ((*ptr)(comm)) {
			rdma_destroy_id(comm->cm_id);
			rdma_destroy_event_channel(comm->cm_channel);
			return FAILURE;
		}

	} else {

		ptr = comm->servername ? &ethernet_client_connect : &ethernet_server_connect;

		if ((*ptr)(comm)) {
			fprintf(stderr,"Unable to open file descriptor for socket connection");
			return FAILURE;
		}

	}
	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_hand_shake(struct perftest_comm *comm,
				   struct pingpong_dest *my_dest,
				   struct pingpong_dest *rem_dest) {

	int (*read_func_ptr) (struct pingpong_dest*,struct perftest_comm*);
	int (*write_func_ptr)(struct pingpong_dest*,struct perftest_comm*);

	if (comm->use_rdma_cm) {

		read_func_ptr  = &rdma_read_keys;
		write_func_ptr = &rdma_write_keys;

	} else {

		read_func_ptr  = &ethernet_read_keys;
		write_func_ptr = &ethernet_write_keys;
		
	}

	if (comm->servername) { 

		if ((*write_func_ptr)(my_dest,comm)) {
			fprintf(stderr,"Unable to write on socket/rdam_cm\n");
			return FAILURE;
		}

		if ((*read_func_ptr)(rem_dest,comm)) {
			fprintf(stderr,"Unable to write on the socket\n");
			return FAILURE;
		}

	} else {

		if ((*read_func_ptr)(rem_dest,comm)) {
			fprintf(stderr,"Unable to write on the socket\n");
			return FAILURE;
		}

		if ((*write_func_ptr)(my_dest,comm)) {
			fprintf(stderr,"Unable to write with RDMA_CM QP\n");
			return FAILURE;
		}
	}

    return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
void ctx_print_pingpong_data(struct pingpong_dest *element,
							 struct perftest_comm *comm,
							 int side,
							 int verb,
							 int machine,
							 int duplex,
							 int use_mcg) {

	int is_there_mgid,local_mgid,remote_mgid;

	// First of all we print the basic format.
    printf(BASIC_ADDR_FMT,sideArray[side],element->lid,element->qpn,element->psn);

	switch (verb) {

		case 2  : printf(READ_FMT,element->out_reads);
		case 1  : printf(RDMA_FMT,element->rkey,element->vaddr);
		default : putchar('\n');
	}

	local_mgid    = (side == 0)  && (machine == 0);
	remote_mgid   = (side == 1)  && (machine == 1);
	is_there_mgid =  duplex || remote_mgid || local_mgid;

	if (comm->gid_index > -1 || (use_mcg && is_there_mgid)) {

		printf(GID_FMT,gidArray[use_mcg && is_there_mgid],
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
        return FAILURE;
        
    }

	if (!comm->use_rdma_cm) {

		close(comm->sockfd);
		return SUCCESS;
	}

	return SUCCESS;
}

/****************************************************************************** 
 * End
 ******************************************************************************/
