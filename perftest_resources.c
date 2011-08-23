#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "perftest_resources.h"

/* Addin



*/

/****************************************************************************** 
 *
 ******************************************************************************/

static const char *portStates[] = {"Nop","Down","Init","Armed","","Active Defer"};

/****************************************************************************** 
 *
 ******************************************************************************/
Device is_dev_hermon(struct ibv_context *context) { 

	Device is_hermon = NOT_HERMON;
	struct ibv_device_attr attr;

	if (ibv_query_device(context,&attr)) {
		is_hermon = ERROR;
	}
	// Checks the devide type for setting the max outstanding reads.
	else if (attr.vendor_part_id == 25408  || attr.vendor_part_id == 25418  ||
			 attr.vendor_part_id == 25448  || attr.vendor_part_id == 26418  || 
			 attr.vendor_part_id == 26428  || attr.vendor_part_id == 26438  ||
			 attr.vendor_part_id == 26448  || attr.vendor_part_id == 26458  ||
			 attr.vendor_part_id == 26468  || attr.vendor_part_id == 26478  ||
			 attr.vendor_part_id == 4099 ) {
				is_hermon = HERMON;		
	}
	return is_hermon;
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
enum ibv_mtu ctx_set_mtu(struct ibv_context *context,int ib_port,int user_mtu) {

	struct ibv_port_attr port_attr;
	enum ibv_mtu curr_mtu;

	ibv_query_port(context,ib_port,&port_attr);

	// User did not ask for specific mtu.
	if (user_mtu == 0) 
		curr_mtu = port_attr.active_mtu;

	else {

		switch (user_mtu) {

			case 256  :	curr_mtu = IBV_MTU_256;	 break;
			case 512  : curr_mtu = IBV_MTU_512;	 break;
			case 1024 :	curr_mtu = IBV_MTU_1024; break;
			case 2048 :	curr_mtu = IBV_MTU_2048; break;
			case 4096 :	curr_mtu = IBV_MTU_4096; break;
			default   :	
				fprintf(stderr," Invalid MTU - %d \n",user_mtu);
				fprintf(stderr," Please choose mtu form {256,512,1024,2048,4096}\n");
				fprintf(stderr," Will run with the port active mtu - %d\n",port_attr.active_mtu);
				curr_mtu = port_attr.active_mtu;
		}
		
		if (curr_mtu > port_attr.active_mtu) {
			fprintf(stdout,"Requested mtu is higher than active mtu \n");
			fprintf(stdout,"Changing to active mtu - %d\n",port_attr.active_mtu);
			curr_mtu = port_attr.active_mtu;
		}
	} 
	return curr_mtu;
}

/****************************************************************************** 
 *
 ******************************************************************************/
uint8_t ctx_set_link_layer(struct ibv_context *context,int ib_port) {

	struct ibv_port_attr port_attr;
	uint8_t curr_link;

	if (ibv_query_port(context,ib_port,&port_attr)) {
		fprintf(stderr,"Unable to query port attributes\n");
		return LINK_FAILURE;
	}

	if (port_attr.state != IBV_PORT_ACTIVE) {
		fprintf(stderr," Port number %d state is %s\n"
					  ,ib_port
					  ,portStates[port_attr.state]);
		return LINK_FAILURE;
	}

	curr_link = port_attr.link_layer; 

	if (curr_link != IBV_LINK_LAYER_UNSPECIFIED &&
		curr_link != IBV_LINK_LAYER_INFINIBAND  &&
		curr_link != IBV_LINK_LAYER_ETHERNET) {
			fprintf(stderr," Unable to determine link layer \n");
			return LINK_FAILURE;
	}

	return curr_link;
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_qp* ctx_qp_create(struct ibv_pd *pd,
							 struct ibv_cq *send_cq,
							 struct ibv_cq *recv_cq,
							 int tx_depth,
							 int rx_depth,
							 int inline_size,
							 int connection_type) {
	
	struct ibv_qp_init_attr attr;
	struct ibv_qp* qp = NULL;
	
	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.send_cq = send_cq;
	attr.recv_cq = recv_cq; 
	attr.cap.max_send_wr  = tx_depth;
	attr.cap.max_recv_wr  = rx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_recv_sge = MAX_RECV_SGE;
	attr.cap.max_inline_data = inline_size;
		
	switch (connection_type) {
		case 0 : attr.qp_type = IBV_QPT_RC; break;
		case 1 : attr.qp_type = IBV_QPT_UC; break;
		case 2 : attr.qp_type = IBV_QPT_UD; break;
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
int ctx_modify_qp_to_init(struct ibv_qp *qp,int ib_port,int conn,int verb)  {

    struct ibv_qp_attr attr;
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;
    
    memset(&attr, 0, sizeof(struct ibv_qp_attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = ib_port;
    
    if (conn == 2) {
		attr.qkey = DEFF_QKEY;
		flags |= IBV_QP_QKEY;

	} else {
		switch(verb) {
			case 2  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
			case 1  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
			case 0  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE	|
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


	int max_reads;

	max_reads = (is_dev_hermon(context) == HERMON) ? MAX_OUT_READ_HERMON : MAX_OUT_READ;

	if (num_user_reads > max_reads) {
		fprintf(stderr," Number of outstanding reads is above max = %d\n",max_reads);
		fprintf(stderr," Changing to that max value\n");
		num_user_reads = max_reads;
	}
	else if (num_user_reads <= 0) {
		num_user_reads = max_reads;
	}

	return num_user_reads;
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
 * End
 ******************************************************************************/
