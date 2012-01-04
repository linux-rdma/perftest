#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include "perftest_resources.h"

/****************************************************************************** 
 * Begining
 ******************************************************************************/
int check_add_port(char **service,int port,
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
int create_rdma_resources(struct pingpong_context *ctx,
						  struct perftest_parameters *user_param) { 

	ctx->cm_channel = rdma_create_event_channel();
	if (ctx->cm_channel == NULL) {
		fprintf(stderr, " rdma_create_event_channel failed\n");
		return FAILURE;
	}

	if (user_param->machine == CLIENT) {

		if (rdma_create_id(ctx->cm_channel,&ctx->cm_id,NULL,RDMA_PS_TCP)) {
			fprintf(stderr,"rdma_create_id failed\n");
			return FAILURE;
		}

	} else {

		if (rdma_create_id(ctx->cm_channel,&ctx->cm_id_control,NULL,RDMA_PS_TCP)) {
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

	if (user_parm->verb == SEND) {
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

	} else {

		rdma_destroy_id(ctx->cm_id);
		rdma_destroy_event_channel(ctx->cm_channel);
	}

	free(ctx->buf);
	free(ctx->qp);
	free(ctx->scnt);
	free(ctx->ccnt);
	return test_result;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_init(struct pingpong_context *ctx,struct perftest_parameters *user_param) {

	int i,flags;
	uint64_t buff_size;

	ALLOCATE(ctx->qp,struct ibv_qp*,user_param->num_of_qps);
	ALLOCATE(ctx->scnt,int,user_param->num_of_qps);
	ALLOCATE(ctx->ccnt,int,user_param->num_of_qps);

	memset(ctx->scnt, 0, user_param->num_of_qps * sizeof (int));
	memset(ctx->ccnt, 0, user_param->num_of_qps * sizeof (int));

	flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
	// ctx->size = SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine);
	ctx->size = user_param->size;
	buff_size = BUFF_SIZE(ctx->size) * 2 * user_param->num_of_qps;
	
	// Allocating the buffer in BUFF_SIZE size to support max performance.
	ctx->buf = memalign(sysconf(_SC_PAGESIZE),buff_size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return FAILURE;
	}
	memset(ctx->buf, 0,buff_size);

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

	if (user_param->verb == READ) {
		flags |= IBV_ACCESS_REMOTE_READ;

	} else if (user_param->verb == ATOMIC) {
		flags |= IBV_ACCESS_REMOTE_ATOMIC;
	}
	
	// Alocating Memory region and assiging our buffer to it.
	ctx->mr = ibv_reg_mr(ctx->pd,ctx->buf,buff_size,flags);
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
			fprintf(stderr, "Couldn't create a recevier CQ\n");
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
		default:  fprintf(stderr, "Unknown connection type \n");
			return NULL;
	}

	if (user_param->work_rdma_cm) {

		if (rdma_create_qp(ctx->cm_id,ctx->pd,&attr)) {
			fprintf(stderr, " Couldn't create rdma QP - %s\n",strerror(errno));
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
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param)  {

	struct ibv_qp_attr attr;
	int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = 0;
	attr.port_num        = user_param->ib_port;

	if (user_param->connection_type == UD) {
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
uint16_t ctx_get_local_lid(struct ibv_context *context,int port) {

	struct ibv_port_attr attr;

	if (ibv_query_port(context,port,&attr))
		return 0;

	return attr.lid;
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline int ctx_notify_events(struct ibv_comp_channel *channel) {

	struct ibv_cq       *ev_cq;
	void                *ev_ctx;

	if (ibv_get_cq_event(channel,&ev_cq,&ev_ctx)) {
		fprintf(stderr, "Failed to get cq_event\n");
		return 1;
	}

	ibv_ack_cq_events(ev_cq,1);

	if (ibv_req_notify_cq(ev_cq, 0)) {
		fprintf(stderr, "Couldn't request CQ notification\n");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline void increase_rem_addr(struct ibv_send_wr *wr,
							  int size,
							  int scnt,
							  uint64_t prim_addr,
							  VerbType verb) {

	if (verb == ATOMIC) 
		wr->wr.atomic.remote_addr += INC(size);

	else
		wr->wr.rdma.remote_addr += INC(size);           

	if ( ((scnt+1) % (CYCLE_BUFFER/ INC(size))) == 0) {

		if (verb == ATOMIC) 
			wr->wr.atomic.remote_addr = prim_addr;

		else 
			wr->wr.rdma.remote_addr = prim_addr;
	}
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline void increase_loc_addr(struct ibv_sge *sg,int size,int rcnt,
							  uint64_t prim_addr,int server_is_ud) {


	//if (server_is_ud)
		//sg->addr -= (CACHE_LINE_SIZE - UD_ADDITION);

	sg->addr  += INC(size);

	if ( ((rcnt+1) % (CYCLE_BUFFER/ INC(size))) == 0 )
		sg->addr = prim_addr;

	//if (server_is_ud)
		//sg->addr += (CACHE_LINE_SIZE - UD_ADDITION);
}


/****************************************************************************** 
 * End
 ******************************************************************************/


