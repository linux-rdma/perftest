
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <math.h>
// #include <byteswap.h>
#include "perftest_resources.h"


/****************************************************************************** 
 *
 ******************************************************************************/

static const char *sideArray[] = {"local", "remote"};

static const char *gidArray[] =  {"GID", "MGID"};

static const char *portStates[] = {"Nop","Down","Init","Armed","","Active Defer"};

/****************************************************************************** 
 *
 ******************************************************************************/

const char *link_layer_str(uint8_t link_layer) {

	switch (link_layer) {

        case IBV_LINK_LAYER_UNSPECIFIED:
        case IBV_LINK_LAYER_INFINIBAND:	
			return "IB";
        case IBV_LINK_LAYER_ETHERNET:	
			return "Ethernet";
        default:
		    return "Unknown";
    }
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ctx_write_keys(const struct pingpong_dest *my_dest,
						  struct perftest_parameters *params) {
    
    if (params->gid_index == -1 && !params->use_mcg) {

		char msg[KEY_MSG_SIZE];
		sprintf(msg,KEY_PRINT_FMT,my_dest->lid,my_dest->out_reads,
				my_dest->qpn,my_dest->psn, my_dest->rkey, my_dest->vaddr);

		if (write(params->sockfd,msg,sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return -1;
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

		if (write(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return -1;
		}	
	}
    return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ctx_read_keys(struct pingpong_dest *rem_dest, 
                         struct perftest_parameters *params)  {
    
	if (params->gid_index == -1 && !params->use_mcg) {

        int parsed;
		char msg[KEY_MSG_SIZE];

		if (read(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return -1;
		}

		parsed = sscanf(msg,KEY_PRINT_FMT,&rem_dest->lid,
						&rem_dest->out_reads,&rem_dest->qpn,
						&rem_dest->psn, &rem_dest->rkey,&rem_dest->vaddr);

		if (parsed != 6) {
			fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg, msg);
			return -1;
		}
        
	} else {

		char msg[KEY_MSG_SIZE_GID];
		char *pstr = msg, *term;
		char tmp[20];
		int i;

		if (read(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return -1;
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
struct ibv_device* ctx_find_dev(const char *ib_devname) {

	int num_of_device;
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev = NULL;

	dev_list = ibv_get_device_list(&num_of_device);

	if (num_of_device <= 0) {
		fprintf(stderr," Did not detect devices \n");
		fprintf(stderr," If device exists, check if driver is up\n");
		return NULL;
	}

	if (!ib_devname) {
		ib_dev = dev_list[0];
		if (!ib_dev)
			fprintf(stderr, "No IB devices found\n");
	} else {
		for (; (ib_dev = *dev_list); ++dev_list)
			if (!strcmp(ibv_get_device_name(ib_dev), ib_devname))
				break;
		if (!ib_dev)
			fprintf(stderr, "IB device %s not found\n", ib_devname);
	}
	return ib_dev;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_set_mtu(struct ibv_context *context,struct perftest_parameters *params) {

	struct ibv_port_attr port_attr;

	if (ibv_query_port(context,params->ib_port,&port_attr)) {
		fprintf(stderr,"Unable to query port\n");
		return -1;
	}

	// User did not ask for specific mtu.
	if (params->mtu == 0) {
		params->curr_mtu = port_attr.active_mtu;

	} else {

		switch (params->mtu) {

			case 256  :	params->curr_mtu = IBV_MTU_256;	 break;
			case 512  : params->curr_mtu = IBV_MTU_512;	 break;
			case 1024 :	params->curr_mtu = IBV_MTU_1024; break;
			case 2048 :	params->curr_mtu = IBV_MTU_2048; break;
			case 4096 :	params->curr_mtu = IBV_MTU_4096; break;
			default   :	
				fprintf(stderr," Invalid MTU - %d \n",params->mtu);
				fprintf(stderr," Please choose valid mtu\n");
				return -1;
		}
		
		if (params->curr_mtu > port_attr.active_mtu) {
			fprintf(stdout,"Requested mtu is higher than active mtu \n");
			fprintf(stdout,"Changing to active mtu \n");
			params->curr_mtu = port_attr.active_mtu;
		}
	}
	printf(" Mtu : %d\n",MTU_SIZE(params->curr_mtu)); 
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_set_link_layer(struct ibv_context *context,
					   struct perftest_parameters *params) {

	struct ibv_port_attr port_attr;

	if (ibv_query_port(context,params->ib_port,&port_attr)) {
		fprintf(stderr,"Unable to query port\n");
		return -1;
	}

	if (port_attr.state != IBV_PORT_ACTIVE) {
		fprintf(stderr," Port number %d state is %s\n"
					  ,params->ib_port
					  ,portStates[port_attr.state]);
		return -1;
	}

	params->link_type = port_attr.link_layer; 

	if (!strcmp(link_layer_str(params->link_type),"Unknown")) {
		fprintf(stderr," Unable to determine link layer \n");
		return -1;
	}
	else {
		printf(" Link type is %s\n",link_layer_str(params->link_type));
	}
	
	if (!strcmp(link_layer_str(params->link_type),"Ethernet") &&  params->gid_index == -1) {
			params->gid_index = 0;
	}

	if (params->use_mcg &&  params->gid_index == -1) {
			params->gid_index = 0;
	}

	if (params->gid_index > -1 && (params->machine == CLIENT || params->duplex)) {
		fprintf(stdout," Using gid index %d as source GID\n",params->gid_index);
	}

	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_cq* ctx_cq_create(struct ibv_context *context, 
							 struct ibv_comp_channel *channel,
							 struct perftest_parameters *param) {

	int cq_depth;
	struct ibv_cq *curr_cq = NULL;

	if (param->verb == WRITE || param->verb == READ)
		cq_depth = param->tx_depth*param->num_of_qps;

	else if (param->duplex) 		
		 cq_depth = param->tx_depth + param->rx_depth*(param->num_of_qps); 
	else if (param->machine == CLIENT) 
		cq_depth = param->tx_depth;

	else 
		cq_depth = param->rx_depth*param->num_of_qps;

	curr_cq = ibv_create_cq(context,cq_depth,NULL,channel,0);

	return curr_cq;
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_qp* ctx_qp_create(struct ibv_pd *pd,
							 struct ibv_cq *send_cq,
							 struct ibv_cq *recv_cq,
							 struct perftest_parameters *param) {
	
	struct ibv_qp_init_attr attr;
	struct ibv_qp* qp = NULL;
	
	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.send_cq = send_cq;
	attr.recv_cq = recv_cq; 
	attr.cap.max_send_wr  = param->tx_depth;
	attr.cap.max_recv_wr  = param->rx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_recv_sge = MAX_RECV_SGE;
	attr.cap.max_inline_data = param->inline_size;
		
	switch (param->connection_type) {
		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
		default:  fprintf(stderr, "Unknown connection type \n");
				  return NULL;
	}

	qp = ibv_create_qp(pd,&attr);
	if (!qp)  {
		fprintf(stderr, "Couldn't create QP\n");
		return NULL;
	}
	return qp;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *param)  {

    struct ibv_qp_attr attr;
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;
    
    memset(&attr, 0, sizeof(struct ibv_qp_attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = param->ib_port;
    
    if (param->connection_type == UD) {
		// assert(param->verb == SEND);
		attr.qkey = DEF_QKEY;
		flags |= IBV_QP_QKEY;

	} else {
		switch(param->verb) {
			case READ  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
			case WRITE : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
			case SEND  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE	|
											    IBV_ACCESS_LOCAL_WRITE;
		}
		flags |= IBV_QP_ACCESS_FLAGS;
    }
    
	if (ibv_modify_qp(qp,&attr,flags))  {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
uint16_t ctx_get_local_lid(struct ibv_context *context,int port) {

    struct ibv_port_attr attr;

    if (ibv_query_port(context,port,&attr))
	return 0;

    return attr.lid;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_set_out_reads(struct ibv_context *context,int num_user_reads) {

	int is_hermon = 0;
	int max_reads;
	struct ibv_device_attr attr;

	if (ibv_query_device(context,&attr)) {
		return -1;
	}
	// Checks the devide type for setting the max outstanding reads.
	if (attr.vendor_part_id == 25408  || attr.vendor_part_id == 25418  ||
		attr.vendor_part_id == 25448  || attr.vendor_part_id == 26418  || 
		attr.vendor_part_id == 26428  || attr.vendor_part_id == 26438  ||
		attr.vendor_part_id == 26448  || attr.vendor_part_id == 26458  ||
		attr.vendor_part_id == 26468  || attr.vendor_part_id == 26478) {
			is_hermon = 1;		
	}

	max_reads = (is_hermon == 1) ? MAX_OUT_READ_HERMON : MAX_OUT_READ;

	if (num_user_reads > max_reads) {
		fprintf(stderr," Number of outstanding reads is above max = %d\n",max_reads);
		fprintf(stderr," Changing to that max value\n");
		num_user_reads = max_reads;
	}
	else if (num_user_reads <= 0) {
		num_user_reads = max_reads;
	}

	printf(" Number of outstanding reads is %d\n",num_user_reads);

	return num_user_reads;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_client_connect(const char *servername,int port) {
    
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

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_server_connect(int port)
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


/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_hand_shake(struct perftest_parameters  *params,
				   struct pingpong_dest *my_dest,
				   struct pingpong_dest *rem_dest) {

    // Client.
    if (params->machine == CLIENT) {
		if (ctx_write_keys(my_dest,params)) {
			fprintf(stderr,"Unable to write on the socket\n");
			return -1;
		}
		if (ctx_read_keys(rem_dest,params)) {
			fprintf(stderr,"Unable to Read from the socket\n");
			return -1;
		}
    }
    // Server.
    else {
		if (ctx_read_keys(rem_dest,params)) {
			fprintf(stderr,"Unable to Read from the socket\n");
			return -1;
		}
		if (ctx_write_keys(my_dest,params)) {
			fprintf(stderr,"Unable to write on the socket\n");
			return -1;
		}
    }
    return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
void ctx_print_pingpong_data(struct pingpong_dest *element,
							 struct perftest_parameters *params) {

	int is_there_mgid,local_mgid,remote_mgid;

	// First of all we print the basic format.
    printf(BASIC_ADDR_FMT,sideArray[params->side],element->lid,element->qpn,element->psn);

	switch (params->verb) {

		case READ  : printf(READ_FMT,element->out_reads);
		case WRITE : printf(RDMA_FMT,element->rkey,element->vaddr);
		default    : putchar('\n');
	}

	local_mgid    = (params->side == LOCAL)  && (params->machine == SERVER);
	remote_mgid   = (params->side == REMOTE) && (params->machine == CLIENT);
	is_there_mgid =  params->duplex || remote_mgid || local_mgid;

	if (params->gid_index > -1 || (params->use_mcg && is_there_mgid)) {

		printf(GID_FMT,gidArray[params->use_mcg && is_there_mgid],
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
inline int ctx_notify_events(struct ibv_cq *cq,struct ibv_comp_channel *channel) {

	struct ibv_cq 		*ev_cq;
	void          		*ev_ctx;

	if (ibv_get_cq_event(channel,&ev_cq,&ev_ctx)) {
		fprintf(stderr, "Failed to get cq_event\n");
		return 1;
	}

	if (ev_cq != cq) {
		fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
		return 1;
	}

	ibv_ack_cq_events(cq,1);

	if (ibv_req_notify_cq(cq, 0)) {
		fprintf(stderr, "Couldn't request CQ notification\n");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline void	increase_rem_addr(struct ibv_send_wr *wr,int size,
							  int scnt,uint64_t prim_addr) {

	wr->wr.rdma.remote_addr += INC(size);		    

	if( ((scnt+1) % (CYCLE_BUFFER/ INC(size))) == 0)
		wr->wr.rdma.remote_addr = prim_addr;
}
		     		
/****************************************************************************** 
 *
 ******************************************************************************/
inline void increase_loc_addr(struct ibv_sge *sg,int size,int rcnt,
							  uint64_t prim_addr,int server_is_ud) {


	if (server_is_ud) 
		sg->addr -= (CACHE_LINE_SIZE - UD_ADDITION);

	sg->addr  += INC(size);

    if( ((rcnt+1) % (CYCLE_BUFFER/ INC(size))) == 0 )
		sg->addr = prim_addr;

    if (server_is_ud) 
		sg->addr += (CACHE_LINE_SIZE - UD_ADDITION);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_close_connection(struct perftest_parameters  *params,
				         struct pingpong_dest *my_dest,
				         struct pingpong_dest *rem_dest) {


	// Signal client is finished.
    if (ctx_hand_shake(params,my_dest,rem_dest)) {
        return -1;
        
    }

	// Close the Socket file descriptor.
	if (write(params->sockfd,"done",sizeof "done") != sizeof "done") {
		perror(" Client write");
		fprintf(stderr,"Couldn't write to socket\n");
		return -1;
	}
	close(params->sockfd);
	return 0;
}
/****************************************************************************** 
 * End
 ******************************************************************************/
