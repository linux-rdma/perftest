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

#ifdef _WIN32
#pragma warning( disable : 4242)
#pragma warning( disable : 4244)
#endif

/****************************************************************************** 
 * Beginning
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

#ifndef _WIN32
        free(ctx->buf);
#else
        posix_memfree(ctx->buf);
#endif

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

#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
#endif

	ALLOCATE(ctx->qp,struct ibv_qp*,user_param->num_of_qps);
	ALLOCATE(ctx->scnt,int,user_param->num_of_qps);
	ALLOCATE(ctx->ccnt,int,user_param->num_of_qps);

	memset(ctx->scnt, 0, user_param->num_of_qps * sizeof (int));
	memset(ctx->ccnt, 0, user_param->num_of_qps * sizeof (int));

	flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
	// ctx->size = SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine);
	ctx->size = user_param->size;
	buff_size = SIZE(user_param->connection_type,BUFF_SIZE(ctx->size),1)*2* user_param->num_of_qps;
	
	// Allocating the buffer in BUFF_SIZE size to support max performance.
#ifndef _WIN32
	ctx->buf = memalign(sysconf(_SC_PAGESIZE),buff_size);
#else
	posix_memalign(&(ctx->buf),si.dwPageSize, (int)buff_size);
#endif
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
	
	// Allocating Memory region and assigning our buffer to it.
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
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param)  {

	struct ibv_qp_attr attr;
	int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

	static int portindex=0;  // for dual-port support

	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = 0;


	if (user_param->dualport==ON) {
		if (portindex<user_param->num_of_qps/2) {
	attr.port_num        = user_param->ib_port;
		} else {
			attr.port_num = user_param->ib_port2;
		}
		portindex++;
	} else
	{
		attr.port_num = user_param->ib_port;
	}

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
 * End
 ******************************************************************************/


