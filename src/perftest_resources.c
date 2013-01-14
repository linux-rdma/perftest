#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <stdarg.h>
#include <time.h>
#include <sys/types.h>
#include <winsock2.h>
#include <Winsock2.h>
#include "l2w.h"
#else
#include <unistd.h>
#include <malloc.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#endif
#include "perftest_resources.h"

#include <signal.h>
//#include "get_clock.h"

#ifdef _WIN32
#pragma warning( disable : 4242)
#pragma warning( disable : 4244)
#endif


static enum ibv_wr_opcode opcode_verbs_array[] = {IBV_WR_SEND,IBV_WR_RDMA_WRITE,IBV_WR_RDMA_READ};
static enum ibv_wr_opcode opcode_atomic_array[] = {IBV_WR_ATOMIC_CMP_AND_SWP,IBV_WR_ATOMIC_FETCH_AND_ADD};

struct perftest_parameters* duration_param;

/****************************************************************************** 
 * Beginning
 ******************************************************************************/
static uint16_t ip_checksum	(void * buf,size_t 	  hdr_len)
{
       unsigned long sum = 0;
       const uint16_t *ip1;
        ip1 = buf;
        while (hdr_len > 1)
        {
                 sum += *ip1++;
                if (sum & 0x80000000)
                         sum = (sum & 0xFFFF) + (sum >> 16);
                 hdr_len -= 2;
        }
        while (sum >> 16)
                sum = (sum & 0xFFFF) + (sum >> 16);
        return(~sum);
 }

/******************************************************************************
 *
 ******************************************************************************/
/*uint16_t udp_checksum	( void * buff,size_t len,in_addr_t src_addr,in_addr_t dest_addr)
 {
         const uint16_t *buf=buff;
         uint16_t *ip_src=(void *)&src_addr, *ip_dst=(void *)&dest_addr;
         uint32_t sum;
         size_t length=len;
         sum = 0;
         while (len > 1)
         {
                 sum += *buf++;
                 if (sum & 0x80000000)
                         sum = (sum & 0xFFFF) + (sum >> 16);
                 len -= 2;
         }
         if ( len & 1 )
                 sum += *((uint8_t *)buf);
         sum += *(ip_src++);
         sum += *ip_src;
         sum += *(ip_dst++);
         sum += *ip_dst;
         sum += htons(UDP_PROTOCOL);
         sum += htons(length);
         while (sum >> 16)
                 sum = (sum & 0xFFFF) + (sum >> 16);
         return ( (uint16_t)(~sum)  );
 }*/

/******************************************************************************
 *
 ******************************************************************************/
int check_add_port(char **service,int port,
				   const char *servername,
				   struct addrinfo *hints,
				   struct addrinfo **res) {

	int number;

#ifndef _WIN32
	if (asprintf(service,"%d", port) < 0) { 
#else
	*service = (char*)malloc(6*sizeof(char));
	if (sprintf_s(*service,(6*sizeof(char)), "%d", port) < 0) {
#endif
		return FAILURE;
	 }

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
int create_rdma_resources(struct pingpong_context *ctx,
						  struct perftest_parameters *user_param) { 

	enum rdma_port_space port_space = (user_param->connection_type == UD) ? RDMA_PS_UDP : RDMA_PS_TCP;

	ctx->cm_channel = rdma_create_event_channel();
	if (ctx->cm_channel == NULL) {
		fprintf(stderr, " rdma_create_event_channel failed\n");
		return FAILURE;
	}

	if (user_param->machine == CLIENT) {

		if (rdma_create_id(ctx->cm_channel,&ctx->cm_id,NULL,port_space)) {
			fprintf(stderr,"rdma_create_id failed\n");
			return FAILURE;
		}

	} else {

		if (rdma_create_id(ctx->cm_channel,&ctx->cm_id_control,NULL,port_space)) {
			fprintf(stderr,"rdma_create_id failed\n");
			return FAILURE;
		}

	}

	return SUCCESS;
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
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			exit (1);
		}
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
void alloc_ctx(struct pingpong_context *ctx,struct perftest_parameters *user_param) { 

	int tarr_size; 
	
	tarr_size = (user_param->noPeak) ? 1 : user_param->iters*user_param->num_of_qps;

	ALLOCATE(ctx->qp,struct ibv_qp*,user_param->num_of_qps);

	if (user_param->tst == BW && (user_param->machine == CLIENT || user_param->duplex)) { 

		ALLOCATE(user_param->tposted,cycles_t,tarr_size);
		memset(user_param->tposted, 0, sizeof(cycles_t)*tarr_size);
		ALLOCATE(user_param->tcompleted,cycles_t,tarr_size);
		memset(user_param->tcompleted, 0, sizeof(cycles_t)*tarr_size);

		ALLOCATE(ctx->my_addr,uint64_t,user_param->num_of_qps);
		ALLOCATE(ctx->rem_addr,uint64_t,user_param->num_of_qps);
		

		ALLOCATE(ctx->scnt,int,user_param->num_of_qps);
		ALLOCATE(ctx->ccnt,int,user_param->num_of_qps);
		memset(ctx->scnt, 0, user_param->num_of_qps * sizeof (int));
		memset(ctx->ccnt, 0, user_param->num_of_qps * sizeof (int));

	} else if (user_param->tst == BW && user_param->verb == SEND && user_param->machine == SERVER) {

		ALLOCATE(ctx->my_addr,uint64_t,user_param->num_of_qps);
		ALLOCATE(user_param->tposted,cycles_t,1);
		ALLOCATE(user_param->tcompleted,cycles_t,1);

	}

	if (user_param->machine == CLIENT || user_param->tst == LAT || user_param->duplex) { 
		
		ALLOCATE(ctx->sge_list,struct ibv_sge,user_param->num_of_qps*user_param->post_list);
		ALLOCATE(ctx->wr,struct ibv_send_wr,user_param->num_of_qps*user_param->post_list);

		if ((user_param->verb == SEND && user_param->connection_type == UD)) {
			ALLOCATE(ctx->ah,struct ibv_ah*,user_param->num_of_qps);
		}
	}

	if (user_param->verb == SEND && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex)) { 

		ALLOCATE(ctx->recv_sge_list,struct ibv_sge,user_param->num_of_qps);
		ALLOCATE(ctx->rwr,struct ibv_recv_wr,user_param->num_of_qps);
		ALLOCATE(ctx->rx_buffer_addr,uint64_t,user_param->num_of_qps);
	}

	ctx->size = user_param->size;
	ctx->buff_size = BUFF_SIZE(ctx->size)*2*user_param->num_of_qps;

	if (user_param->connection_type == UD)
		ctx->buff_size += CACHE_LINE_SIZE;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int destroy_ctx(struct pingpong_context *ctx,
				struct perftest_parameters *user_parm)  {

	int i;
	int test_result = 0;

	for (i = 0; i < user_parm->num_of_qps; i++) {
		if (ibv_destroy_qp(ctx->qp[i])) {
			fprintf(stderr, "failed to destroy QP\n");
			test_result = 1;
		}
	}

	if (ibv_destroy_cq(ctx->send_cq)) {
		fprintf(stderr, "failed to destroy CQ\n");
		test_result = 1;
	}

	if (user_parm->verb == SEND && (user_parm->tst == LAT || user_parm->machine == SERVER || user_parm->duplex) ) {
		if (ibv_destroy_cq(ctx->recv_cq)) {
			fprintf(stderr, "failed to destroy CQ\n");
			test_result = 1;
		}
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "failed to deregister MR\n");
		test_result = 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "failed to deallocate PD\n");
		test_result = 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			test_result = 1;
		}
	}

	if (user_parm->work_rdma_cm == OFF) { 

		if (ibv_close_device(ctx->context)) {
			fprintf(stderr, "failed to close device context\n");
			test_result = 1;
		}
	}

	
#ifndef _WIN32
        free(ctx->buf);
#else
        posix_memfree(ctx->buf);
#endif

	free(ctx->qp);

	if (user_parm->tst == BW && (user_parm->machine == CLIENT || user_parm->duplex)) {

		free(user_parm->tposted);
		free(user_parm->tcompleted);
		free(ctx->my_addr);
		free(ctx->rem_addr);
		free(ctx->scnt);
		free(ctx->ccnt);
	}
	else if (user_parm->tst == BW && user_parm->verb == SEND && user_parm->machine == SERVER) {

		free(user_parm->tposted);
		free(user_parm->tcompleted);
		free(ctx->my_addr);
	}

	if (user_parm->machine == CLIENT || user_parm->tst == LAT || user_parm->duplex) {

		free(ctx->sge_list);
		free(ctx->wr);
	}

	if (user_parm->verb == SEND && (user_parm->tst == LAT || user_parm->machine == SERVER || user_parm->duplex)) {

		free(ctx->rx_buffer_addr);
		free(ctx->recv_sge_list);
		free(ctx->rwr);
	}

	return test_result;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_init(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

	int i,flags;
#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
#endif

	flags = IBV_ACCESS_LOCAL_WRITE;

#ifndef _WIN32
	ctx->buf = memalign(sysconf(_SC_PAGESIZE),ctx->buff_size);
#else
	posix_memalign(&(ctx->buf),si.dwPageSize, (int)ctx->buff_size);
#endif

	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		exit(1);
	}

	memset(ctx->buf, 0,ctx->buff_size);

	// Allocating an event channel if requested.
	if (user_param->use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			return FAILURE;
		}
	}

	// Allocating the Protection domain.
	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return FAILURE;
	}

	if (user_param->verb == WRITE) { 
		flags |= IBV_ACCESS_REMOTE_WRITE;
		
	} else if (user_param->verb == READ) {
		flags |= IBV_ACCESS_REMOTE_READ;

	} else if (user_param->verb == ATOMIC) {
		flags |= IBV_ACCESS_REMOTE_ATOMIC;
	}
	
	// Allocating Memory region and assigning our buffer to it.
	ctx->mr = ibv_reg_mr(ctx->pd,ctx->buf,ctx->buff_size,flags);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return FAILURE;
	}

	ctx->send_cq = ibv_create_cq(ctx->context,user_param->tx_depth*user_param->num_of_qps,NULL,ctx->channel,0);
	if (!ctx->send_cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return FAILURE;
	}

	if (user_param->verb == SEND) { 
		ctx->recv_cq = ibv_create_cq(ctx->context,user_param->rx_depth*user_param->num_of_qps,NULL,ctx->channel,0);
		if (!ctx->recv_cq) {
			fprintf(stderr, "Couldn't create a receiver CQ\n");
			return FAILURE;
		}
	}

	for (i=0; i < user_param->num_of_qps; i++) {

		ctx->qp[i] = ctx_qp_create(ctx,user_param);
		if (ctx->qp[i] == NULL) {
			fprintf(stderr," Unable to create QP.\n");
			return FAILURE;
		}

        if (user_param->work_rdma_cm == OFF) {

			if (ctx_modify_qp_to_init(ctx->qp[i],user_param)) {
				fprintf(stderr, "Failed to modify QP to INIT\n");
				return FAILURE;
			}	
		}
	}

	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_qp* ctx_qp_create(struct pingpong_context *ctx,
							 struct perftest_parameters *user_param) {

	struct ibv_qp_init_attr attr;
	struct ibv_qp* qp = NULL;

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.send_cq = ctx->send_cq;
	attr.recv_cq = (user_param->verb == SEND) ? ctx->recv_cq : ctx->send_cq;
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_recv_wr  = user_param->rx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_recv_sge = MAX_RECV_SGE;
	attr.cap.max_inline_data = user_param->inline_size;

	switch (user_param->connection_type) {
		
		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
#ifdef HAVE_RAW_ETH
		case RawEth : attr.qp_type = IBV_QPT_RAW_PACKET; break;
#endif
		default:  fprintf(stderr, "Unknown connection type \n");
			return NULL;
	}

	if (user_param->work_rdma_cm) {

		if (rdma_create_qp(ctx->cm_id,ctx->pd,&attr)) {
			fprintf(stderr, " Couldn't create rdma QP\n");
			return NULL;
		}
		qp = ctx->cm_id->qp;

	} else {
		qp = ibv_create_qp(ctx->pd,&attr);
	}
	return qp;
}
 
/****************************************************************************** 
 *
 ******************************************************************************/
void mac_from_gid(uint8_t   *mac, uint8_t *gid ){
	memcpy(mac, gid + 8, 3);
	memcpy(mac + 3, gid + 13, 3);
	mac[0] ^= 2;
}

void mac_from_user(uint8_t   *mac, uint8_t *gid,int size )
{
	memcpy(mac,gid,size);
}
/******************************************************************************
 *
 ******************************************************************************/
void dump_buffer(unsigned char *bufptr, int len) {
	int i;

	for (i = 0; i < len; i++) {
		printf("%02x:", *bufptr);
		bufptr++;
	}
	printf("\n");
}
/******************************************************************************
 *
 ******************************************************************************/
void gen_eth_header(struct ETH_header* eth_header,uint8_t* src_mac,uint8_t* dst_mac, uint16_t eth_type) {
	memcpy(eth_header->src_mac, src_mac, 6);
	memcpy(eth_header->dst_mac, dst_mac, 6);
	eth_header->eth_type = htons(eth_type);

}
/******************************************************************************
 *
 ******************************************************************************/
void gen_ip_header(void* ip_header_buffer,uint32_t* saddr ,uint32_t* daddr , uint8_t protocol,int sizePkt) {

	struct IP_V4_header ip_header;

	memset(&ip_header,0,sizeof(struct IP_V4_header));

	ip_header.version = 4;
	ip_header.ihl = 5;
	ip_header.tos = 0;
	ip_header.tot_len = htons(sizePkt);
	ip_header.id = htons(0);
	ip_header.frag_off = htons(0);
	ip_header.ttl = DEFAULT_TTL;
	ip_header.protocol = protocol;
	ip_header.saddr = *saddr;
	ip_header.daddr = *daddr;
	ip_header.check = ip_checksum((void*)&ip_header,sizeof(struct IP_V4_header));

	memcpy(ip_header_buffer, &ip_header, sizeof(struct IP_V4_header));
}

/******************************************************************************
 *
 ******************************************************************************/
void gen_udp_header(void* UDP_header_buffer,int* sPort ,int* dPort,uint32_t saddr,uint32_t daddr,int sizePkt) {

	struct UDP_header udp_header;

	memset(&udp_header,0,sizeof(struct UDP_header));

	udp_header.uh_sport = htons(*sPort);
	udp_header.uh_dport = htons(*dPort);
	udp_header.uh_ulen = htons(sizePkt - sizeof(struct IP_V4_header));
	udp_header.uh_sum = 0;

	memcpy(UDP_header_buffer, &udp_header, sizeof(struct UDP_header));


}
 /******************************************************************************
 *
 ******************************************************************************/
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param)  {

	struct ibv_qp_attr attr;
	int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

	static int portindex=0;  // for dual-port support

	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = 0;


	if (user_param->dualport==ON) {
		if (portindex<user_param->num_of_qps/2) {
	        attr.port_num = user_param->ib_port;
		} else {
			attr.port_num = user_param->ib_port2;
		}
		portindex++;

	} else {

		attr.port_num = user_param->ib_port;
	}

	if (user_param->connection_type == RawEth) {
		flags = IBV_QP_STATE | IBV_QP_PORT;

	} else if (user_param->connection_type == UD) {
		attr.qkey = DEFF_QKEY;
		flags |= IBV_QP_QKEY;

	} else {
		switch (user_param->verb) {
			case ATOMIC: attr.qp_access_flags = IBV_ACCESS_REMOTE_ATOMIC; break;
			case READ  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
			case WRITE : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
			case SEND  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
												IBV_ACCESS_LOCAL_WRITE;
		}
		flags |= IBV_QP_ACCESS_FLAGS;
	}

	if (ibv_modify_qp(qp,&attr,flags)) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ctx_modify_qp_to_rtr(struct ibv_qp *qp,
								struct ibv_qp_attr *attr,
								struct perftest_parameters *user_parm,
								struct pingpong_dest *dest,
								int qpindex,
								int my_reads)  {

	int flags = IBV_QP_STATE;

	attr->qp_state = IBV_QPS_RTR;
	attr->ah_attr.dlid = dest->lid;
		
	if (user_parm->gid_index == DEF_GID_INDEX) {

		attr->ah_attr.is_global  = 0;
		attr->ah_attr.sl         = user_parm->sl;

	} else {

		attr->ah_attr.is_global  = 1;
		attr->ah_attr.grh.dgid   = dest->gid;
		attr->ah_attr.grh.sgid_index = user_parm->gid_index;
		attr->ah_attr.grh.hop_limit = 1;
		attr->ah_attr.sl         = 0;
	}

	attr->ah_attr.src_path_bits = 0;
	attr->ah_attr.port_num = (user_parm->dualport == ON && qpindex >= (user_parm->num_of_qps/2)) ? user_parm->ib_port2 : user_parm->ib_port;

	if (user_parm->connection_type != UD) { 

		attr->path_mtu     = user_parm->curr_mtu;
		attr->dest_qp_num  = dest->qpn;
		attr->rq_psn 	   = dest->psn;

		flags |= (IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);

		if(user_parm->connection_type == RawEth)
		{
			flags = IBV_QP_STATE;
		}

		if (user_parm->connection_type == RC) { 

			attr->max_dest_rd_atomic = my_reads;
			attr->min_rnr_timer 	 = 12;
			flags |= (IBV_QP_MIN_RNR_TIMER | IBV_QP_MAX_DEST_RD_ATOMIC);
		}
	}

	return ibv_modify_qp(qp,attr,flags);
	
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ctx_modify_qp_to_rts(struct ibv_qp *qp,
								struct ibv_qp_attr *attr,
								struct perftest_parameters *user_parm,
								int dest_read,
								int my_psn)  {

	int flags = IBV_QP_STATE | IBV_QP_SQ_PSN;

	attr->qp_state 	    = IBV_QPS_RTS;
	attr->sq_psn 	    = my_psn;

	if (user_parm->connection_type == RC) {

		attr->timeout   = user_parm->qp_timeout;
		attr->retry_cnt = 7;
		attr->rnr_retry = 7;
		attr->max_rd_atomic  = dest_read;

		flags |= (IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);

	}
	if (user_parm->connection_type == RawEth) {

			flags = IBV_QP_STATE;
	}

	return ibv_modify_qp(qp,attr,flags);		
}


/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_connect(struct pingpong_context *ctx,
				struct pingpong_dest *dest, 
				struct perftest_parameters *user_parm,
				struct pingpong_dest *my_dest) {

	int i;
	struct ibv_qp_attr attr;
	
	for (i=0; i < user_parm->num_of_qps; i++) { 
	
		memset(&attr, 0, sizeof attr);

		if(ctx_modify_qp_to_rtr(ctx->qp[i],&attr,user_parm,&dest[i],i,my_dest[i].out_reads)) { 
			fprintf(stderr, "Failed to modify QP %d to RTR\n",ctx->qp[i]->qp_num);
			return FAILURE;
		}


		if (user_parm->tst == LAT || user_parm->machine == CLIENT || user_parm->duplex) { 
			if(ctx_modify_qp_to_rts(ctx->qp[i],&attr,user_parm,dest[i].out_reads,my_dest[i].psn)) { 
				fprintf(stderr, "Failed to modify QP %d to RTS\n",ctx->qp[i]->qp_num);
				return FAILURE;
			}
		}
		
		if (user_parm->connection_type == UD && (user_parm->tst == LAT || user_parm->machine == CLIENT || user_parm->duplex)) {
			ctx->ah[i] = ibv_create_ah(ctx->pd,&(attr.ah_attr));
			if (!ctx->ah[i]) {
				fprintf(stderr, "Failed to create AH for UD\n");
				return FAILURE;
			}
		}
	}
	
	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
void ctx_set_send_wqes(struct pingpong_context *ctx,
					   struct perftest_parameters *user_param,
					   struct pingpong_dest *rem_dest) { 

	int i,j;

	for (i = 0 ; i < user_param->num_of_qps ; i++) {

		ctx->sge_list[i*user_param->post_list].addr = (uintptr_t)ctx->buf + (i*BUFF_SIZE(ctx->size));

		if (user_param->verb == WRITE || user_param->verb == READ)
			ctx->wr[i*user_param->post_list].wr.rdma.remote_addr   = rem_dest[i].vaddr;

		else if (user_param->verb == ATOMIC)
			ctx->wr[i*user_param->post_list].wr.atomic.remote_addr = rem_dest[i].vaddr;

		if (user_param->tst == BW) { 

			ctx->scnt[i] = 0;
			ctx->ccnt[i] = 0;
			ctx->my_addr[i]	= (uintptr_t)ctx->buf + (i*BUFF_SIZE(ctx->size));
			ctx->rem_addr[i] = rem_dest[i].vaddr;
		}
		
		for (j = 0; j < user_param->post_list; j++) { 

			ctx->sge_list[i*user_param->post_list + j].length =  (user_param->connection_type == RawEth) ? (user_param->size - HW_CRC_ADDITION) : user_param->size;
			ctx->sge_list[i*user_param->post_list + j].lkey = ctx->mr->lkey;

			if (j > 0) { 

				ctx->sge_list[i*user_param->post_list +j].addr = ctx->sge_list[i*user_param->post_list + (j-1)].addr;

				if (user_param->tst == BW && user_param->size <= (CYCLE_BUFFER / 2))
					increase_loc_addr(&ctx->sge_list[i*user_param->post_list +j],user_param->size,j-1,ctx->my_addr[i],0);
			}

			ctx->wr[i*user_param->post_list + j].sg_list = &ctx->sge_list[i*user_param->post_list + j]; 
			ctx->wr[i*user_param->post_list + j].num_sge = MAX_SEND_SGE;
			ctx->wr[i*user_param->post_list + j].wr_id   = i;

			if (j == (user_param->post_list - 1)) {
				ctx->wr[i*user_param->post_list + j].send_flags = IBV_SEND_SIGNALED;
				ctx->wr[i*user_param->post_list + j].next = NULL;
			}

			else {
				ctx->wr[i*user_param->post_list + j].next = &ctx->wr[i*user_param->post_list+j+1];
				ctx->wr[i*user_param->post_list + j].send_flags = 0;
			}

			if (user_param->verb == ATOMIC) 
				ctx->wr[i*user_param->post_list + j].opcode = opcode_atomic_array[user_param->atomicType];

			else 
				ctx->wr[i*user_param->post_list + j].opcode = opcode_verbs_array[user_param->verb];

			if (user_param->verb == WRITE || user_param->verb == READ) {

				ctx->wr[i*user_param->post_list + j].wr.rdma.rkey = rem_dest[i].rkey;

				if (j > 0) {

					ctx->wr[i*user_param->post_list + j].wr.rdma.remote_addr = ctx->wr[i*user_param->post_list + (j-1)].wr.rdma.remote_addr;

					if (user_param->tst == BW && user_param->size <= (CYCLE_BUFFER / 2))
						increase_rem_addr(&ctx->wr[i*user_param->post_list + j],user_param->size,j-1,ctx->rem_addr[i],WRITE);
				}

			} else if (user_param->verb == ATOMIC) {

				ctx->wr[i*user_param->post_list + j].wr.atomic.rkey = rem_dest[i].rkey;

				if (j > 0) {

					ctx->wr[i*user_param->post_list + j].wr.atomic.remote_addr = ctx->wr[i*user_param->post_list + j-1].wr.atomic.remote_addr;
					if (user_param->tst == BW)
						increase_rem_addr(&ctx->wr[i*user_param->post_list + j],user_param->size,j-1,ctx->rem_addr[i],ATOMIC);
				}
			
				if (user_param->atomicType == FETCH_AND_ADD) 
					ctx->wr[i*user_param->post_list + j].wr.atomic.compare_add = ATOMIC_ADD_VALUE;

				else 
					ctx->wr[i*user_param->post_list + j].wr.atomic.swap = ATOMIC_SWAP_VALUE;

	
			} else if (user_param->verb == SEND && user_param->connection_type == UD) { 

				ctx->wr[i*user_param->post_list + j].wr.ud.ah = ctx->ah[i];
				ctx->wr[i*user_param->post_list + j].wr.ud.remote_qkey = DEF_QKEY;
				ctx->wr[i*user_param->post_list + j].wr.ud.remote_qpn  = rem_dest[i].qpn;
			}

			if ((user_param->verb == SEND || user_param->verb == WRITE) && user_param->size <= user_param->inline_size)
				ctx->wr[i*user_param->post_list + j].send_flags |= IBV_SEND_INLINE;
		}
	}
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_set_recv_wqes(struct pingpong_context *ctx,struct perftest_parameters *user_param) {
						
	int					i,j;
	struct ibv_recv_wr  *bad_wr_recv;

	for (i=0; i < user_param->num_of_qps; i++) {

		ctx->recv_sge_list[i].addr  = (uintptr_t)ctx->buf + (user_param->num_of_qps + i)*BUFF_SIZE(ctx->size);

		if (user_param->connection_type == UD)
			ctx->recv_sge_list[i].addr += (CACHE_LINE_SIZE - UD_ADDITION);

		ctx->recv_sge_list[i].length = SIZE(user_param->connection_type,user_param->size,1);
		ctx->recv_sge_list[i].lkey   = ctx->mr->lkey;
		
		ctx->rwr[i].sg_list = &ctx->recv_sge_list[i];
		ctx->rwr[i].wr_id   = i;
		ctx->rwr[i].next    = NULL;
		ctx->rwr[i].num_sge	= MAX_RECV_SGE;

		if (user_param->tst == BW)
			ctx->rx_buffer_addr[i] = ctx->recv_sge_list[i].addr;
		
		for (j = 0; j < user_param->rx_depth; ++j) {

			if (ibv_post_recv(ctx->qp[i],&ctx->rwr[i],&bad_wr_recv)) {
				fprintf(stderr, "Couldn't post recv Qp = %d: counter=%d\n",i,j);
				return 1;
			}

			if (user_param->tst == BW && user_param->size <= (CYCLE_BUFFER / 2)) {

				increase_loc_addr(&ctx->recv_sge_list[i],
								  user_param->size,
								  j,
								  ctx->rx_buffer_addr[i],
								  user_param->connection_type);
			}
		}
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int perform_warm_up(struct pingpong_context *ctx,struct perftest_parameters *user_param) { 

	int ne,index,warmindex,warmupsession;
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_wc wc;
	struct ibv_wc *wc_for_cleaning = NULL;

	warmupsession = (user_param->post_list == 1) ? user_param->tx_depth : user_param->post_list;
	ALLOCATE(wc_for_cleaning,struct ibv_wc,user_param->tx_depth);

	// Clean up the pipe
	ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth,wc_for_cleaning);

	for (index=0 ; index < user_param->num_of_qps ; index++) {

		for (warmindex = 0 ;warmindex < warmupsession ;warmindex += user_param->post_list) {
	  
            if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
                fprintf(stderr,"Couldn't post send during warm up: qp %d scnt=%d \n",index,warmindex);
                return 1;
            }
		}

		do {

			ne = ibv_poll_cq(ctx->send_cq,1,&wc);
			if (ne > 0) { 
				
				if (wc.status != IBV_WC_SUCCESS)
					return 1;

				warmindex -= user_param->post_list;

			} else if (ne < 0)
				return 1;

		} while (warmindex);

	}
	free(wc_for_cleaning);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter_bw(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

    int                totscnt = 0;
    int 	       	   totccnt = 0;
    int                i = 0;
    int                index,ne,tot_iters;
    struct ibv_send_wr *bad_wr = NULL;
    struct ibv_wc 	   *wc = NULL;

	ALLOCATE(wc ,struct ibv_wc ,user_param->tx_depth*user_param->num_of_qps);
	
	if (user_param->test_type == DURATION) {
		duration_param=user_param;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		alarm(user_param->margin);
		user_param->iters = 0;
	}

	// Will be 0, in case of Duration (look at force_dependencies or in the exp above).
	tot_iters = user_param->iters * user_param->num_of_qps;

	if (user_param->verb != SEND) { 

		if(perform_warm_up(ctx,user_param)) { 
			fprintf(stderr,"Problems with warm up\n");
			return 1;
		}
	}

	if (user_param->test_type == DURATION && user_param->state != START_STATE) {
		fprintf(stderr, "Failed: margin is not long enough (taking samples before warmup ends)\n");
		fprintf(stderr, "Please increase margin or decrease tx_depth\n");
		return 1;
	}	

	if (user_param->test_type == ITERATIONS && user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	// main loop for posting 
	while (totscnt < tot_iters  || totccnt < tot_iters || (user_param->test_type == DURATION && user_param->state != END_STATE) ) {

		// main loop to run over all the qps and post each time n messages 
		for (index =0 ; index < user_param->num_of_qps ; index++) {

			while ((ctx->scnt[index] < user_param->iters || user_param->test_type == DURATION) && (ctx->scnt[index] - ctx->ccnt[index]) < (user_param->tx_depth)) {

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1))
					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;

				if (user_param->noPeak == OFF) 
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && user_param->state == END_STATE) 
					break;

				if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}     
				
				if (user_param->post_list == 1 && user_param->size <= (CYCLE_BUFFER / 2)) { 

						increase_loc_addr(ctx->wr[index].sg_list,user_param->size,ctx->scnt[index],ctx->my_addr[index],0);

						if (user_param->verb != SEND)
							increase_rem_addr(&ctx->wr[index],user_param->size,ctx->scnt[index],ctx->rem_addr[index],user_param->verb);
				}

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->post_list == 1 &&
				   (ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 || (user_param->test_type == ITERATIONS && ctx->scnt[index] == user_param->iters - 1)))
					ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
			}
		}

		if (totccnt < tot_iters || (user_param->test_type == DURATION &&  totccnt < totscnt)) {
		
			if (user_param->use_event) {
				if (ctx_notify_events(ctx->channel)) {
					fprintf(stderr, "Couldn't request CQ notification\n");
					return 1;
				}
			}

			ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth*user_param->num_of_qps,wc);

			if (ne > 0) {

				for (i = 0; i < ne; i++) {	
					if (wc[i].status != IBV_WC_SUCCESS) 
						NOTIFY_COMP_ERROR_SEND(wc[i],totscnt,totccnt);

					ctx->ccnt[(int)wc[i].wr_id] += user_param->cq_mod;
					totccnt += user_param->cq_mod;

					if (user_param->noPeak == OFF) {

						if (totccnt >=  tot_iters - 1)
							user_param->tcompleted[user_param->iters*user_param->num_of_qps - 1] = get_cycles();
						else 
							user_param->tcompleted[totccnt-1] = get_cycles();
					}
							
					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE) 
						user_param->iters += user_param->cq_mod;
				}

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n",ne);
				return 1;
			}
		}
	}

	if (user_param->noPeak == ON && user_param->test_type == ITERATIONS)
		user_param->tcompleted[0] = get_cycles();

	free(wc);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter_bw_server(struct pingpong_context *ctx, struct perftest_parameters *user_param) {

	int 				rcnt = 0;
	int 				ne,i,tot_iters;
	int                 *rcnt_for_qp = NULL;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr  *bad_wr_recv = NULL;
	int firstRx = 1;

	ALLOCATE(wc ,struct ibv_wc ,user_param->rx_depth*user_param->num_of_qps);

	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	if(user_param->connection_type != RawEth){
		if (user_param->test_type == DURATION) {
			duration_param=user_param;
			duration_param->state = START_STATE;
			signal(SIGALRM, catch_alarm);
			alarm(user_param->margin);
			user_param->iters = 0;
		}
	}

	if (user_param->test_type == ITERATIONS)
		user_param->tposted[0] = get_cycles();

	tot_iters = user_param->iters*user_param->num_of_qps;

	while (rcnt < tot_iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr ," Failed to notify events to CQ");
				return 1;
			}
		}
		
		do {
			ne = ibv_poll_cq(ctx->recv_cq,user_param->rx_depth*user_param->num_of_qps,wc);

			if (ne > 0) {
				if(user_param->connection_type == RawEth){
					if (firstRx && user_param->test_type == DURATION) {
						firstRx = 0;
						duration_param=user_param;
						user_param->iters=0;
						duration_param->state = START_STATE;
						signal(SIGALRM, catch_alarm);
						alarm(user_param->margin);
					}
				}

				for (i = 0; i < ne; i++) {
					
					if (wc[i].status != IBV_WC_SUCCESS) {
				
						NOTIFY_COMP_ERROR_RECV(wc[i],rcnt_for_qp[0]);
					}
						
					rcnt_for_qp[wc[i].wr_id]++;
					rcnt++;

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE) 
						user_param->iters++;

					if (user_param->test_type==DURATION || rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth <= user_param->iters) {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (CYCLE_BUFFER / 2)) {
							increase_loc_addr(ctx->rwr[wc[i].wr_id].sg_list,
											  user_param->size,
											  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth,
											  ctx->rx_buffer_addr[wc[i].wr_id],
											  user_param->connection_type);
						}
					}
				}
			}

		} while (ne > 0);

		if (ne < 0) {
			fprintf(stderr, "Poll Recieve CQ failed %d\n", ne);
			return 1;
		}
	}

	if (user_param->test_type == ITERATIONS)
		user_param->tcompleted[0] = get_cycles();

	free(wc);
	free(rcnt_for_qp);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw_infinitely(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
    int i,j = 0;
    int index = 0,ne;
    struct ibv_send_wr *bad_wr = NULL;
    struct ibv_wc *wc = NULL;

	ALLOCATE(wc ,struct ibv_wc ,user_param->tx_depth*user_param->num_of_qps);
	
	duration_param=user_param;
	signal(SIGALRM,catch_alarm_infintely);
	alarm(5);
	user_param->iters = 0;
	
	for (i=0; i < user_param->num_of_qps; i++)
		for (j=0 ; j < user_param->post_list; j++)
			ctx->wr[i*user_param->post_list +j].send_flags |= IBV_SEND_SIGNALED;
	
	user_param->tposted[0] = get_cycles();

	// main loop for posting 
	while (1) {

		// main loop to run over all the qps and post each time n messages 
		for (index =0 ; index < user_param->num_of_qps ; index++) {

			while (ctx->scnt[index] < user_param->tx_depth) {

				if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}     
				
				ctx->scnt[index] += user_param->post_list;
			}
		}

	
		ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth*user_param->num_of_qps,wc);

		if (ne > 0) {

			for (i = 0; i < ne; i++) {	
				if (wc[i].status != IBV_WC_SUCCESS) 
					NOTIFY_COMP_ERROR_SEND(wc[i],ctx->scnt[(int)wc[i].wr_id],ctx->scnt[(int)wc[i].wr_id]);

				ctx->scnt[(int)wc[i].wr_id]--;
				user_param->iters++;
			}

		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n",ne);
			return 1;
		}
	}
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter_bw_infinitely_server(struct pingpong_context *ctx, struct perftest_parameters *user_param) {

	int 				i,ne;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr  *bad_wr_recv = NULL;

	ALLOCATE(wc ,struct ibv_wc ,user_param->rx_depth*user_param->num_of_qps);

	while (1) {

		ne = ibv_poll_cq(ctx->recv_cq,user_param->rx_depth*user_param->num_of_qps,wc);

		if (ne > 0) {

			for (i = 0; i < ne; i++) {
					
				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr,"A completion with Error in run_infinitely_bw_server function");
					return 1;
				}

				if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv Qp=%d\n",(int)wc[i].wr_id);
					return 1;
				}
			}

		} else if (ne < 0) {
			fprintf(stderr, "Poll Recieve CQ failed %d\n", ne);
			return 1;
		}
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bi(struct pingpong_context *ctx,
				struct perftest_parameters *user_param)  {

	uint64_t				totscnt    = 0;
	uint64_t				totccnt    = 0;
	uint64_t				totrcnt    = 0;
	int 					i,index      = 0;
	int 					ne = 0;
	int						*rcnt_for_qp = NULL;
	int 					tot_iters = 0;
	int 					iters = 0;
	struct ibv_wc 			*wc          = NULL;
	struct ibv_wc 			*wc_tx		 = NULL;
	struct ibv_recv_wr      *bad_wr_recv = NULL;
	struct ibv_send_wr 		*bad_wr      = NULL;
	int  					firstRx = 1;

	ALLOCATE(wc,struct ibv_wc,user_param->rx_depth*user_param->num_of_qps);
	ALLOCATE(wc_tx,struct ibv_wc,user_param->tx_depth*user_param->num_of_qps);
	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);

	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	tot_iters = user_param->iters*user_param->num_of_qps;
	iters=user_param->iters;

	if (user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	if((user_param->test_type == DURATION )&& (user_param->connection_type != RawEth || (user_param->machine == CLIENT && firstRx)))
	{
			firstRx = OFF;
			duration_param=user_param;
			user_param->iters=0;
			duration_param->state = START_STATE;
			signal(SIGALRM, catch_alarm);
			alarm(user_param->margin);
	}
	while ( (user_param->test_type == DURATION && user_param->state != END_STATE) || totccnt < tot_iters || totrcnt < tot_iters) {

		for (index=0; index < user_param->num_of_qps; index++) {

			while (((ctx->scnt[index] < iters) || ((firstRx == OFF) && (user_param->test_type == DURATION)))&& ((ctx->scnt[index] - ctx->ccnt[index]) < user_param->tx_depth)) {

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1))
					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && duration_param->state == END_STATE)
					break;

				if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}

				if (user_param->post_list == 1 && user_param->size <= (CYCLE_BUFFER / 2))
					increase_loc_addr(ctx->wr[index].sg_list,user_param->size,ctx->scnt[index],ctx->my_addr[index],0);

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->post_list == 1 && (ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 || (user_param->test_type == ITERATIONS && ctx->scnt[index] == iters-1)))
					ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
			}
		}

		if (user_param->use_event) {

			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr,"Failed to notify events to CQ");
				return 1;
			}
		}

		if ((user_param->test_type == ITERATIONS && (totrcnt < tot_iters)) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
			ne = ibv_poll_cq(ctx->recv_cq,user_param->rx_depth*user_param->num_of_qps,wc);
			if (ne > 0) {
				if(user_param->connection_type == RawEth)
				{
					if (user_param->machine == SERVER && firstRx && user_param->test_type == DURATION) {
						firstRx = 0;
						duration_param=user_param;
						user_param->iters=0;
						duration_param->state = START_STATE;
						signal(SIGALRM, catch_alarm);
						alarm(user_param->margin);
					}
				}

				for (i = 0; i < ne; i++) {
					if (wc[i].status != IBV_WC_SUCCESS) {

						NOTIFY_COMP_ERROR_RECV(wc[i],(int)totrcnt);
					}

					rcnt_for_qp[wc[i].wr_id]++;
					totrcnt++;

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
						user_param->iters++;

					if (user_param->test_type==DURATION || rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth <= user_param->iters) {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (CYCLE_BUFFER / 2)) {
							increase_loc_addr(ctx->rwr[wc[i].wr_id].sg_list,
											  user_param->size,
											  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth -1,
											  ctx->rx_buffer_addr[wc[i].wr_id],user_param->connection_type);
						}
					}
				}
			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}

		if ((totccnt < tot_iters) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

			ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth*user_param->num_of_qps,wc_tx);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					if (wc_tx[i].status != IBV_WC_SUCCESS)
						 NOTIFY_COMP_ERROR_SEND(wc_tx[i],(int)totscnt,(int)totccnt);

					totccnt += user_param->cq_mod;
					ctx->ccnt[(int)wc_tx[i].wr_id] += user_param->cq_mod;

					if (user_param->noPeak == OFF) {

						if ((user_param->test_type == ITERATIONS && (totccnt >= tot_iters - 1)))
							user_param->tcompleted[tot_iters - 1] = get_cycles();
						else
							user_param->tcompleted[totccnt-1] = get_cycles();
					}

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
						user_param->iters += user_param->cq_mod;
				}

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}
	}

	if (user_param->noPeak == ON)
		user_param->tcompleted[0] = get_cycles();

	free(rcnt_for_qp);
	free(wc);
	free(wc_tx);
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
void catch_alarm(int sig) {
	switch (duration_param->state) {
		case START_STATE:
			duration_param->state = SAMPLE_STATE;
			duration_param->tposted[0] = get_cycles();
			alarm(duration_param->duration - 2*(duration_param->margin));
			break;
		case SAMPLE_STATE:
			duration_param->state = STOP_SAMPLE_STATE;
			duration_param->tcompleted[0] = get_cycles();
			alarm(duration_param->margin);
			break;
		case STOP_SAMPLE_STATE:
			duration_param->state = END_STATE;
			break;
		default:
			fprintf(stderr,"unknown state\n");
	} 
}

/****************************************************************************** 
 *
 ******************************************************************************/
void catch_alarm_infintely(int sig) 
{
	duration_param->tcompleted[0] = get_cycles();
	print_report_bw(duration_param);
	duration_param->iters = 0;
	alarm(5);
	duration_param->tposted[0] = get_cycles();
}

/****************************************************************************** 
 * End
 ******************************************************************************/

