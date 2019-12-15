#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !defined(__FreeBSD__)
#include <malloc.h>
#endif
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#if defined(__FreeBSD__)
#include <sys/stat.h>
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifdef HAVE_SRD
#include <infiniband/efadv.h>
#endif

#include "perftest_resources.h"
#include "raw_ethernet_resources.h"

#ifdef HAVE_VERBS_EXP
static enum ibv_exp_wr_opcode exp_opcode_verbs_array[] = {IBV_EXP_WR_SEND,IBV_EXP_WR_RDMA_WRITE,IBV_EXP_WR_RDMA_READ};
static enum ibv_exp_wr_opcode exp_opcode_atomic_array[] = {IBV_EXP_WR_ATOMIC_CMP_AND_SWP,IBV_EXP_WR_ATOMIC_FETCH_AND_ADD};
#endif
static enum ibv_wr_opcode opcode_verbs_array[] = {IBV_WR_SEND,IBV_WR_RDMA_WRITE,IBV_WR_RDMA_READ};
static enum ibv_wr_opcode opcode_atomic_array[] = {IBV_WR_ATOMIC_CMP_AND_SWP,IBV_WR_ATOMIC_FETCH_AND_ADD};

#define CPU_UTILITY "/proc/stat"

struct perftest_parameters* duration_param;
struct check_alive_data check_alive_data;

/******************************************************************************
 * Beginning
 ******************************************************************************/
#ifdef HAVE_CUDA
#define ASSERT(x)										\
	do {											\
	if (!(x)) {										\
		fprintf(stdout, "Assertion \"%s\" failed at %s:%d\n", #x, __FILE__, __LINE__);	\
	}											\
} while (0)

#define CUCHECK(stmt)				\
	do {					\
	CUresult result = (stmt);		\
	ASSERT(CUDA_SUCCESS == result);		\
} while (0)

/*----------------------------------------------------------------------------*/

static CUdevice cuDevice;
static CUcontext cuContext;

static int pp_init_gpu(struct pingpong_context *ctx, size_t _size)
{
	const size_t gpu_page_size = 64*1024;
	size_t size = (_size + gpu_page_size - 1) & ~(gpu_page_size - 1);
	printf("initializing CUDA\n");
	CUresult error = cuInit(0);
	if (error != CUDA_SUCCESS) {
		printf("cuInit(0) returned %d\n", error);
		exit(1);
	}

	int deviceCount = 0;
	error = cuDeviceGetCount(&deviceCount);
	if (error != CUDA_SUCCESS) {
		printf("cuDeviceGetCount() returned %d\n", error);
		exit(1);
	}
	/* This function call returns 0 if there are no CUDA capable devices. */
	if (deviceCount == 0) {
		printf("There are no available device(s) that support CUDA\n");
		return 1;
	} else if (deviceCount == 1)
		printf("There is 1 device supporting CUDA\n");
	else
		printf("There are %d devices supporting CUDA, picking first...\n", deviceCount);

	int devID = 0;

	/* pick up device with zero ordinal (default, or devID) */
	CUCHECK(cuDeviceGet(&cuDevice, devID));

	char name[128];
	CUCHECK(cuDeviceGetName(name, sizeof(name), devID));
	printf("[pid = %d, dev = %d] device name = [%s]\n", getpid(), cuDevice, name);
	printf("creating CUDA Ctx\n");

	/* Create context */
	error = cuCtxCreate(&cuContext, CU_CTX_MAP_HOST, cuDevice);
	if (error != CUDA_SUCCESS) {
		printf("cuCtxCreate() error=%d\n", error);
		return 1;
	}

	printf("making it the current CUDA Ctx\n");
	error = cuCtxSetCurrent(cuContext);
	if (error != CUDA_SUCCESS) {
		printf("cuCtxSetCurrent() error=%d\n", error);
		return 1;
	}

	printf("cuMemAlloc() of a %zd bytes GPU buffer\n", size);
	CUdeviceptr d_A;
	error = cuMemAlloc(&d_A, size);
	if (error != CUDA_SUCCESS) {
		printf("cuMemAlloc error=%d\n", error);
		return 1;
	}
	printf("allocated GPU buffer address at %016llx pointer=%p\n", d_A,
	       (void *) d_A);
	ctx->buf[0] = (void*)d_A;

	return 0;
}

static int pp_free_gpu(struct pingpong_context *ctx)
{
	int ret = 0;
	CUdeviceptr d_A = (CUdeviceptr) ctx->buf[0];

	printf("deallocating RX GPU buffer\n");
	cuMemFree(d_A);
	d_A = 0;

	printf("destroying current CUDA Ctx\n");
	CUCHECK(cuCtxDestroy(cuContext));

	return ret;
}
#endif

static int pp_init_mmap(struct pingpong_context *ctx, size_t size,
			const char *fname, unsigned long offset)
{
	int fd = open(fname, O_RDWR);
	if (fd < 0) {
		printf("Unable to open '%s': %s\n", fname, strerror(errno));
		return 1;
	}

	ctx->buf[0] = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED, fd,
			offset);
	close(fd);

	if (ctx->buf[0] == MAP_FAILED) {
		printf("Unable to mmap '%s': %s\n", fname, strerror(errno));
		return 1;
	}

	printf("allocated mmap buffer of size %zd at %p\n", size, ctx->buf[0]);

	return 0;
}

static int pp_free_mmap(struct pingpong_context *ctx)
{
	munmap(ctx->buf[0], ctx->buff_size);
	return 0;
}

#ifdef HAVE_VERBS_EXP
static void get_verbs_pointers(struct pingpong_context *ctx)
{
	ctx->exp_post_send_func_pointer = ibv_exp_get_provider_func(ctx->context,IBV_EXP_POST_SEND_FUNC);
	if (!ctx->exp_post_send_func_pointer) {
		fprintf(stderr, "Couldn't get ibv_exp_post_send pointer\n");
		ctx->exp_post_send_func_pointer = &ibv_exp_post_send;
	}
	ctx->post_send_func_pointer = ibv_exp_get_provider_func(ctx->context,IBV_POST_SEND_FUNC);
	if (!ctx->post_send_func_pointer) {
		fprintf(stderr, "Couldn't get ibv_post_send pointer\n");
		ctx->post_send_func_pointer = &ibv_post_send;
	}
	ctx->poll_cq_func_pointer = ibv_exp_get_provider_func(ctx->context,IBV_POLL_CQ_FUNC);
	if (!ctx->poll_cq_func_pointer) {
		fprintf(stderr, "Couldn't get ibv_poll_cq pointer\n");
	}
}
#endif

static int next_word_string(char* input, char* output, int from_index)
{
	int i = from_index;
	int j = 0;

	while (input[i] != ' ') {
		output[j] = input[i];
		j++; i++;
	}

	output[j]=0;
	return i+1;
}

static int get_n_word_string(char *input, char *output,int from_index, int iters)
{
	for (;iters > 0; iters--) {
		from_index = next_word_string(input,output,from_index);
	}

	return from_index;
}
static void compress_spaces(char *str, char *dst)
{
	for (; *str; ++str) {
		*dst++ = *str;

		if (isspace(*str)) {
			do ++str;

			while (isspace(*str));

			--str;
		}
	}

	*dst = 0;
}

static void get_cpu_stats(struct perftest_parameters *duration_param,int stat_index)
{
	char* file_name = CPU_UTILITY;
	FILE *fp;
	char line[100];
	char tmp[100];
	int index=0;
	fp = fopen(file_name, "r");
	if (fp != NULL) {
		if (fgets(line,100,fp) != NULL) {
			compress_spaces(line,line);
			index=get_n_word_string(line,tmp,index,2); /* skip first word */
			duration_param->cpu_util_data.ustat[stat_index-1] = atoll(tmp);

			index=get_n_word_string(line,tmp,index,3); /* skip 2 stats */
			duration_param->cpu_util_data.idle[stat_index-1] = atoll(tmp);

			fclose(fp);
		}
	}
}

#ifdef HAVE_VERBS_EXP
static int check_for_contig_pages_support(struct ibv_context *context)
{
	int answer;
	struct ibv_exp_device_attr attr;
	memset(&attr,0,sizeof attr);
	if (ibv_exp_query_device(context,&attr)) {
		fprintf(stderr, "Couldn't get device attributes\n");
		return FAILURE;
	}
	answer = ( attr.exp_device_cap_flags &= IBV_EXP_DEVICE_MR_ALLOCATE) ? SUCCESS : FAILURE;
	return answer;
}
#endif

/* _new_post_send.
 *
 * Description :
 *
 * Does efficient posting of work to a send
 * queue using function calls instead of the struct based *ibv_post_send()*
 * scheme. Inline is used to make sure that switch would be decided on
 * compile time.
 *
 * Parameters :
 *
 *	ctx         - Test Context.
 *	user_param  - user_parameters struct for this test.
 *	inl         - use inline or not.
 *	index       - qp index.
 *	qpt         - qp type.
 *	op          - RDMA operation code.
 *
 * Return Value : int.
 *
 */
#ifdef HAVE_IBV_WR_API
static inline int _new_post_send(struct pingpong_context *ctx,
	struct perftest_parameters *user_param, int inl, int index,
	enum ibv_qp_type qpt, enum ibv_wr_opcode op)
	__attribute__((always_inline));
static inline int _new_post_send(struct pingpong_context *ctx,
	struct perftest_parameters *user_param, int inl, int index,
	enum ibv_qp_type qpt, enum ibv_wr_opcode op)
{
	int rc;
	int wr_index = index*user_param->post_list;
	ibv_wr_start(ctx->qpx[index]);

	ctx->qpx[index]->wr_id = ctx->wr[wr_index].wr_id;

	switch (op) {
	case IBV_WR_SEND:
		ibv_wr_send(ctx->qpx[index]);
		break;
	case IBV_WR_RDMA_WRITE:
		ibv_wr_rdma_write(
			ctx->qpx[index],
			ctx->wr[wr_index].wr.rdma.rkey,
			ctx->wr[wr_index].wr.rdma.remote_addr);
		break;
	case IBV_WR_RDMA_READ:
		ibv_wr_rdma_read(
			ctx->qpx[index],
			ctx->wr[wr_index].wr.rdma.rkey,
			ctx->wr[wr_index].wr.rdma.remote_addr);
		break;
	case IBV_WR_ATOMIC_FETCH_AND_ADD:
		ibv_wr_atomic_fetch_add(
			ctx->qpx[index],
			ctx->wr[wr_index].wr.atomic.rkey,
			ctx->wr[wr_index].wr.atomic.remote_addr,
			ctx->wr[wr_index].wr.atomic.compare_add);
		break;
	case IBV_WR_ATOMIC_CMP_AND_SWP:
		ibv_wr_atomic_cmp_swp(
			ctx->qpx[index],
			ctx->wr[wr_index].wr.atomic.rkey,
			ctx->wr[wr_index].wr.atomic.remote_addr,
			ctx->wr[wr_index].wr.atomic.compare_add,
			ctx->wr[wr_index].wr.atomic.swap);
		break;
	default:
		fprintf(stderr, "Post send failed: unknown operation code.\n");;
	}

	if (qpt == IBV_QPT_UD) {
		ibv_wr_set_ud_addr(
			ctx->qpx[index],
			ctx->wr[wr_index].wr.ud.ah,
			ctx->wr[wr_index].wr.ud.remote_qpn,
			ctx->wr[wr_index].wr.ud.remote_qkey);
	}
	#ifdef HAVE_XRCD
	else if (qpt == IBV_QPT_XRC_SEND) {
		ibv_wr_set_xrc_srqn(
			ctx->qpx[index],
			ctx->wr[wr_index].qp_type.xrc.remote_srqn);
	}
	#endif

	if (inl) {
		ibv_wr_set_inline_data(
			ctx->qpx[index],
			(void*)ctx->wr[wr_index].sg_list->addr,
			user_param->size);
	} else {
		ibv_wr_set_sge(
			ctx->qpx[index],
			ctx->wr[wr_index].sg_list->lkey,
			ctx->wr[wr_index].sg_list->addr,
			user_param->size);
	}
	rc = ibv_wr_complete(ctx->qpx[index]);

	return rc;
}

/* new_post_send_*.
 *
 * Description :
 *
 * Calls _new_post_send to do posting of work to a send
 * queue using function calls instead of the struct based *ibv_post_send()*
 * scheme. We need a lot of functions for each combination to make sure the
 * condition in _new_post_send is decided in compile-time.
 *
 * Parameters :
 *
 *	ctx         - Test Context.
 *	index       - qp index.
 *	user_param  - user_parameters struct for this test.
 *
 * Return Value : int.
 *
 */
static int new_post_send_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_SEND);
}

static int new_post_send_inl_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_RC, IBV_WR_SEND);
}

static int new_post_write_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_RDMA_WRITE);
}

static int new_post_write_inl_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_RC, IBV_WR_RDMA_WRITE);
}

static int new_post_read_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_RDMA_READ);
}

static int new_post_atomic_fa_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_ATOMIC_FETCH_AND_ADD);
}

static int new_post_atomic_cs_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_ATOMIC_CMP_AND_SWP);
}

static int new_post_send_sge_ud(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_UD, IBV_WR_SEND);
}

static int new_post_send_inl_ud(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_UD, IBV_WR_SEND);
}

static int new_post_send_sge_uc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_UC, IBV_WR_SEND);
}

static int new_post_send_inl_uc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_UC, IBV_WR_SEND);
}

static int new_post_write_sge_uc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_UC, IBV_WR_RDMA_WRITE);
}

static int new_post_write_inl_uc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_UC, IBV_WR_RDMA_WRITE);
}

#ifdef HAVE_XRCD
static int new_post_send_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_SEND);
}

static int new_post_send_inl_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_XRC_SEND, IBV_WR_SEND);
}

static int new_post_write_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_RDMA_WRITE);
}

static int new_post_write_inl_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_XRC_SEND, IBV_WR_RDMA_WRITE);
}

static int new_post_read_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_RDMA_READ);
}

static int new_post_atomic_fa_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_ATOMIC_FETCH_AND_ADD);
}

static int new_post_atomic_cs_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_ATOMIC_CMP_AND_SWP);
}
#endif
#endif

#ifdef HAVE_XRCD
/******************************************************************************
 *
 ******************************************************************************/
static int ctx_xrcd_create(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	char *tmp_file_name;
	struct ibv_xrcd_init_attr xrcd_init_attr;

	memset(&xrcd_init_attr , 0 , sizeof xrcd_init_attr);

	tmp_file_name = (user_param->machine == SERVER) ? SERVER_FD : CLIENT_FD;

	ctx->fd = open(tmp_file_name, O_RDONLY | O_CREAT, S_IRUSR | S_IRGRP);
	if (ctx->fd < 0) {
		fprintf(stderr,"Error opening file %s errno: %s", tmp_file_name,strerror(errno));
		return FAILURE;
	}

	xrcd_init_attr.comp_mask = IBV_XRCD_INIT_ATTR_FD | IBV_XRCD_INIT_ATTR_OFLAGS;
	xrcd_init_attr.fd = ctx->fd;
	xrcd_init_attr.oflags = O_CREAT ;

	ctx->xrc_domain = ibv_open_xrcd(ctx->context,&xrcd_init_attr);
	if (ctx->xrc_domain == NULL) {
		fprintf(stderr,"Error opening XRC domain\n");
		return FAILURE;
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_xrc_srq_create(struct pingpong_context *ctx,
			      struct perftest_parameters *user_param)
{
	struct ibv_srq_init_attr_ex srq_init_attr;

	memset(&srq_init_attr, 0, sizeof(srq_init_attr));

	srq_init_attr.attr.max_wr = user_param->rx_depth;
	srq_init_attr.attr.max_sge = 1;
	srq_init_attr.comp_mask = IBV_SRQ_INIT_ATTR_TYPE | IBV_SRQ_INIT_ATTR_XRCD | IBV_SRQ_INIT_ATTR_CQ | IBV_SRQ_INIT_ATTR_PD;
	srq_init_attr.srq_type = IBV_SRQT_XRC;
	srq_init_attr.xrcd = ctx->xrc_domain;

	if(user_param->verb == SEND)
		srq_init_attr.cq = ctx->recv_cq;
	else
		srq_init_attr.cq = ctx->send_cq;

	srq_init_attr.pd = ctx->pd;
	ctx->srq = ibv_create_srq_ex(ctx->context, &srq_init_attr);
	if (ctx->srq == NULL) {
		fprintf(stderr, "Couldn't open XRC SRQ\n");
		return FAILURE;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static struct ibv_qp *ctx_xrc_qp_create(struct pingpong_context *ctx,
					struct perftest_parameters *user_param,
					int qp_index)
{
	struct ibv_qp* qp = NULL;
	int num_of_qps = user_param->num_of_qps / 2;

	#ifdef HAVE_IBV_WR_API
	enum ibv_wr_opcode opcode;
	#endif

	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_qp_init_attr qp_init_attr;
	#else
	struct ibv_qp_init_attr_ex qp_init_attr;
	#endif

	memset(&qp_init_attr, 0, sizeof(qp_init_attr));

	if ( (!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
			|| ((user_param->duplex || user_param->tst == LAT) && (qp_index >= num_of_qps))) {
		qp_init_attr.qp_type = IBV_QPT_XRC_RECV;
		qp_init_attr.comp_mask = IBV_QP_INIT_ATTR_XRCD;
		qp_init_attr.xrcd = ctx->xrc_domain;
		qp_init_attr.cap.max_recv_wr  = user_param->rx_depth;
		qp_init_attr.cap.max_recv_sge = 1;
		qp_init_attr.cap.max_inline_data = user_param->inline_size;

	} else {
		qp_init_attr.qp_type = IBV_QPT_XRC_SEND;
		qp_init_attr.send_cq = ctx->send_cq;
		qp_init_attr.cap.max_send_wr = user_param->tx_depth;
		qp_init_attr.cap.max_send_sge = 1;
		qp_init_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
		qp_init_attr.pd = ctx->pd;
		#ifdef HAVE_IBV_WR_API
		qp_init_attr.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
		#endif
		qp_init_attr.cap.max_inline_data = user_param->inline_size;
	}

	#ifdef HAVE_IBV_WR_API
	if (user_param->verb == ATOMIC) {
		opcode = opcode_atomic_array[user_param->atomicType];
		if (opcode == IBV_WR_ATOMIC_FETCH_AND_ADD)
			qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_ATOMIC_FETCH_AND_ADD;
		else if (opcode == IBV_WR_ATOMIC_CMP_AND_SWP)
			qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_ATOMIC_CMP_AND_SWP;
	}
	else {
		opcode = opcode_verbs_array[user_param->verb];
		if (opcode == IBV_WR_SEND)
			qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_SEND;
		else if (opcode == IBV_WR_RDMA_WRITE)
			qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_RDMA_WRITE;
		else if (opcode == IBV_WR_RDMA_READ)
			qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_RDMA_READ;
	}
	#endif

	#ifdef HAVE_ACCL_VERBS
	if (user_param->use_res_domain) {
		qp_init_attr.comp_mask |= IBV_EXP_QP_INIT_ATTR_RES_DOMAIN;
		qp_init_attr.res_domain = ctx->res_domain;
	}
	#endif

	#ifdef HAVE_VERBS_EXP
	qp = ibv_exp_create_qp(ctx->context, &qp_init_attr);
	#else
	qp = ibv_create_qp_ex(ctx->context, &qp_init_attr);
	#endif

	return qp;
}
#endif

#ifdef HAVE_DC
/******************************************************************************
 *
 ******************************************************************************/
static int ctx_dc_tgt_create(struct pingpong_context *ctx,struct perftest_parameters *user_param,int dct_index)
{
	struct ibv_exp_device_attr dattr;
	int err;
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;
	int port_num;
	int access_flags = 0;

	memset(&dattr,0,sizeof(struct ibv_exp_device_attr));

	/* in dc with bidirectional,
	 * there are send qps and recv qps. the actual number of send/recv qps
	 * is num_of_qps / 2.
	 */
	if (user_param->duplex || user_param->tst == LAT) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	/* first half of qps are for ib_port and second half are for ib_port2
	 * in dc with bidirectional, the first half of qps are dc_ini qps and
	 * the second half are dc_tgts . the first half of the send/recv qps
	 * are for ib_port1 and the second half are for ib_port2
	 */
	if (user_param->dualport == ON && (dct_index % num_of_qps >= num_of_qps_per_port))
		port_num = user_param->ib_port2;
	else
		port_num = user_param->ib_port;

	dattr.comp_mask = IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS |
		IBV_EXP_DEVICE_DC_RD_REQ |
		IBV_EXP_DEVICE_DC_RD_RES;

	err = ibv_exp_query_device(ctx->context, &dattr);
	if (err) {
		printf("couldn't query device extended attributes\n");
		return -1;
	} else {
		if (!(dattr.comp_mask & IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS)) {
			printf("no extended capability flgas\n");
			return -1;
		}
		if (!(dattr.exp_device_cap_flags & IBV_EXP_DEVICE_DC_TRANSPORT)) {
			printf("DC transport not enabled\n");
			return -1;
		}

		if (!(dattr.comp_mask & IBV_EXP_DEVICE_DC_RD_REQ)) {
			printf("no report on max requestor rdma/atomic resources\n");
			return -1;
		}

		if (!(dattr.comp_mask & IBV_EXP_DEVICE_DC_RD_RES)) {
			printf("no report on max responder rdma/atomic resources\n");
			return -1;
		}
	}

	if (user_param->verb == WRITE)
		access_flags |= IBV_ACCESS_REMOTE_WRITE;
	else if (user_param->verb == READ)
		access_flags |= IBV_ACCESS_REMOTE_READ;
	else if (user_param->verb == ATOMIC)
		access_flags |= IBV_ACCESS_REMOTE_ATOMIC;

	struct ibv_exp_dct_init_attr dctattr = {
		.pd = ctx->pd,
		.cq = (user_param->verb == SEND && (user_param->duplex || user_param->tst == LAT)) ? ctx->recv_cq : ctx->send_cq,
		.srq = ctx->srq,
		.dc_key = user_param->dct_key,
		.port = port_num,
		.access_flags = access_flags,
		.min_rnr_timer = 2,
		.tclass = user_param->traffic_class,
		.flow_label = 0,
		.mtu = user_param->curr_mtu,
		.pkey_index = user_param->pkey_index,
		.gid_index = user_param->gid_index,
		.hop_limit = 1,
		.inline_size = user_param->inline_size,
	};

	ctx->dct[dct_index] = ibv_exp_create_dct(ctx->context, &dctattr);
	if (!ctx->dct[dct_index]) {
		printf("create dct failed\n");
		return FAILURE;
	}

	struct ibv_exp_dct_attr dcqattr;
	memset(&dcqattr,0,sizeof(struct ibv_exp_dct_attr));

	err = ibv_exp_query_dct(ctx->dct[dct_index], &dcqattr);
	if (err) {
		printf("query dct failed\n");
		return FAILURE;
	} else if (dcqattr.dc_key != user_param->dct_key) {
		printf("queried dckry (0x%llx) is different then provided at create (0x%llx)\n",
				(unsigned long long)dcqattr.dc_key,
				(unsigned long long)user_param->dct_key);
		return FAILURE;
	} else if (dcqattr.state != IBV_EXP_DCT_STATE_ACTIVE) {
		printf("state is not active %d\n", dcqattr.state);
		return FAILURE;
	}

	return 0;
}
#endif

#ifdef HAVE_RSS_EXP
static struct ibv_qp *ctx_rss_eth_qp_create(struct pingpong_context *ctx,struct perftest_parameters *user_param,int qp_index)
{

	struct ibv_exp_qp_init_attr attr;
	struct ibv_qp* qp = NULL;

	memset(&attr, 0, sizeof(struct ibv_exp_qp_init_attr));

	attr.send_cq = ctx->send_cq;
	attr.recv_cq = ctx->recv_cq;
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_inline_data = user_param->inline_size;
	attr.cap.max_recv_wr  = user_param->rx_depth;
	attr.cap.max_recv_sge = MAX_RECV_SGE;
	attr.qp_type = IBV_QPT_RAW_PACKET;
	attr.comp_mask = IBV_EXP_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_QPG;
	attr.pd = ctx->pd;

	if (qp_index == 0) { /* rss parent */
		#ifdef HAVE_VERBS_EXP
		attr.qpg.qpg_type = IBV_EXP_QPG_PARENT;
		#else
		attr.qpg.qpg_type = IBV_QPG_PARENT;
		#endif
		attr.qpg.qpg_parent = NULL;
		attr.qpg.parent_attrib.tss_child_count = 0;
		attr.qpg.parent_attrib.rss_child_count = user_param->num_of_qps - 1;
	} else { /* rss childs */
		#ifdef HAVE_VERBS_EXP
		attr.qpg.qpg_type = IBV_EXP_QPG_CHILD_RX;
		#else
		attr.qpg.qpg_type = IBV_QPG_CHILD_RX;
		#endif
		attr.qpg.qpg_parent = ctx->qp[0];
	}
	qp = ibv_exp_create_qp(ctx->context,&attr);


	return qp;
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
int check_add_port(char **service,int port,
		const char *servername,
		struct addrinfo *hints,
		struct addrinfo **res)
{
	int number;

	if (asprintf(service,"%d", port) < 0) {
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
		struct perftest_parameters *user_param)
{
	int is_udp_ps = user_param->connection_type == UD || user_param->connection_type == RawEth;
	enum rdma_port_space port_space = (is_udp_ps) ? RDMA_PS_UDP : RDMA_PS_TCP;
	struct rdma_cm_id **cm_id = (user_param->machine == CLIENT) ? &ctx->cm_id : &ctx->cm_id_control;

	ctx->cm_channel = rdma_create_event_channel();
	if (ctx->cm_channel == NULL) {
		fprintf(stderr, " rdma_create_event_channel failed\n");
		return FAILURE;
	}

	if (rdma_create_id(ctx->cm_channel,cm_id,NULL,port_space)) {
		fprintf(stderr,"rdma_create_id failed\n");
		return FAILURE;
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
int destroy_rdma_resources(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int ret;
	if (user_param->machine == CLIENT) {
		ret = rdma_destroy_id(ctx->cm_id);
	} else {
		ret = rdma_destroy_id(ctx->cm_id_control);
	}
	rdma_destroy_event_channel(ctx->cm_channel);
	return ret;
}

/******************************************************************************
  + *
  + ******************************************************************************/
struct ibv_device* ctx_find_dev(char **ib_devname)
{
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
		fprintf(stderr," Internal error, existing.\n");
		return NULL;
	}

	if (!*ib_devname) {
		ib_dev = dev_list[0];
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			exit (1);
		}
	} else {
		for (; (ib_dev = *dev_list); ++dev_list)
			if (!strcmp(ibv_get_device_name(ib_dev), *ib_devname))
				break;
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", *ib_devname);
			return NULL;
		}
	}

	GET_STRING(*ib_devname, ibv_get_device_name(ib_dev));
	return ib_dev;
}

/******************************************************************************
 *
 ******************************************************************************/
void alloc_ctx(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t tarr_size;
	int num_of_qps_factor;
	ctx->cycle_buffer = user_param->cycle_buffer;
	ctx->cache_line_size = user_param->cache_line_size;

	ALLOCATE(user_param->port_by_qp, uint64_t, user_param->num_of_qps);

	tarr_size = (user_param->noPeak) ? 1 : user_param->iters*user_param->num_of_qps;
	ALLOCATE(user_param->tposted, cycles_t, tarr_size);
	memset(user_param->tposted, 0, sizeof(cycles_t)*tarr_size);
	if ((user_param->tst == LAT || user_param->tst == FS_RATE) && user_param->test_type == DURATION)
		ALLOCATE(user_param->tcompleted, cycles_t, 1);

	ALLOCATE(ctx->qp, struct ibv_qp*, user_param->num_of_qps);
	#ifdef HAVE_IBV_WR_API
	ALLOCATE(ctx->qpx, struct ibv_qp_ex*, user_param->num_of_qps);
	#endif
	ALLOCATE(ctx->mr, struct ibv_mr*, user_param->num_of_qps);
	ALLOCATE(ctx->buf, void* , user_param->num_of_qps);

	#ifdef HAVE_ACCL_VERBS
	ALLOCATE(ctx->qp_burst_family, struct ibv_exp_qp_burst_family*, user_param->num_of_qps);
	#endif

	#ifdef HAVE_DC
	if (user_param->connection_type == DC) {
		#ifdef HAVE_VERBS_EXP
		ALLOCATE(ctx->dct, struct ibv_exp_dct*, user_param->num_of_qps);
		#else
		ALLOCATE(ctx->dct, struct ibv_dct*, user_param->num_of_qps);
		#endif
	}
	#endif

	if ((user_param->tst == BW || user_param->tst == LAT_BY_BW) && (user_param->machine == CLIENT || user_param->duplex)) {

		ALLOCATE(user_param->tcompleted,cycles_t,tarr_size);
		memset(user_param->tcompleted, 0, sizeof(cycles_t)*tarr_size);
		ALLOCATE(ctx->my_addr,uint64_t,user_param->num_of_qps);
		ALLOCATE(ctx->rem_addr,uint64_t,user_param->num_of_qps);
		ALLOCATE(ctx->scnt,uint64_t,user_param->num_of_qps);
		ALLOCATE(ctx->ccnt,uint64_t,user_param->num_of_qps);
		memset(ctx->scnt, 0, user_param->num_of_qps * sizeof (uint64_t));
		memset(ctx->ccnt, 0, user_param->num_of_qps * sizeof (uint64_t));

	} else if ((user_param->tst == BW || user_param->tst == LAT_BY_BW)
		   && user_param->verb == SEND && user_param->machine == SERVER) {

		ALLOCATE(ctx->my_addr, uint64_t, user_param->num_of_qps);
		ALLOCATE(user_param->tcompleted, cycles_t, 1);
	} else if (user_param->tst == FS_RATE && user_param->test_type == ITERATIONS) {
		ALLOCATE(user_param->tcompleted, cycles_t, tarr_size);
		memset(user_param->tcompleted, 0, sizeof(cycles_t) * tarr_size);
	}

	if (user_param->machine == CLIENT || user_param->tst == LAT || user_param->duplex) {

		ALLOCATE(ctx->sge_list, struct ibv_sge,user_param->num_of_qps * user_param->post_list);
		#ifdef HAVE_VERBS_EXP
		ALLOCATE(ctx->exp_wr, struct ibv_exp_send_wr, user_param->num_of_qps * user_param->post_list);
		#endif
		ALLOCATE(ctx->wr, struct ibv_send_wr, user_param->num_of_qps * user_param->post_list);
		if ((user_param->verb == SEND && user_param->connection_type == UD) ||
				user_param->connection_type == DC || user_param->connection_type == SRD) {
			ALLOCATE(ctx->ah, struct ibv_ah*, user_param->num_of_qps);
		}
	}

	if (user_param->verb == SEND && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex)) {

		ALLOCATE(ctx->recv_sge_list,struct ibv_sge,user_param->num_of_qps);
		ALLOCATE(ctx->rwr,struct ibv_recv_wr,user_param->num_of_qps);
		ALLOCATE(ctx->rx_buffer_addr,uint64_t,user_param->num_of_qps);
	}
	if (user_param->mac_fwd == ON )
		ctx->cycle_buffer = user_param->size * user_param->rx_depth;

	ctx->size = user_param->size;

	num_of_qps_factor = (user_param->mr_per_qp) ? 1 : user_param->num_of_qps;

	/* holds the size of maximum between msg size and cycle buffer,
	* aligned to cache line,
	* it is multiply by 2 for send and receive
	* with reference to number of flows and number of QPs */
	ctx->buff_size = INC(BUFF_SIZE(ctx->size, ctx->cycle_buffer),
				 ctx->cache_line_size) * 2 * num_of_qps_factor * user_param->flows;
	ctx->send_qp_buff_size = ctx->buff_size / num_of_qps_factor / 2;
	ctx->flow_buff_size = ctx->send_qp_buff_size / user_param->flows;
	user_param->buff_size = ctx->buff_size;
	if (user_param->connection_type == UD)
		ctx->buff_size += ctx->cache_line_size;
}

/******************************************************************************
 *
 ******************************************************************************/
int destroy_ctx(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int i, first, dereg_counter, rc;
	int test_result = 0;
	int num_of_qps = user_param->num_of_qps;

	if (user_param->wait_destroy) {
		printf(" Waiting %u seconds before releasing resources...\n",
		       user_param->wait_destroy);
		sleep(user_param->wait_destroy);
	}

	dereg_counter = (user_param->mr_per_qp) ? user_param->num_of_qps : 1;

	if (user_param->work_rdma_cm == ON) {
		rc = rdma_cm_disconnect_nodes(ctx, user_param);
		if (rc) {
			fprintf(stderr, "Failed to disconnect RDMA CM nodes.\n");
		}
		rdma_cm_destroy_qps(ctx, user_param);
	}

	if (user_param->work_rdma_cm == ON)
		rdma_disconnect(ctx->cm_id);

	/* in dc with bidirectional,
	 * there are send qps and recv qps. the actual number of send/recv qps
	 * is num_of_qps / 2.
	 */
	if (user_param->duplex || user_param->tst == LAT) {
		num_of_qps /= 2;
	}

	/* RSS parent should be last */
	if (user_param->use_rss)
		first = 1;
	else
		first = 0;
	for (i = first; i < user_param->num_of_qps; i++) {

		if (((user_param->connection_type == DC && !((!(user_param->duplex || user_param->tst == LAT) && user_param->machine == SERVER)
							|| ((user_param->duplex || user_param->tst == LAT) && i >= num_of_qps))) ||
					user_param->connection_type == UD || user_param->connection_type == SRD) &&
				(user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex)) {
			if (ibv_destroy_ah(ctx->ah[i])) {
				fprintf(stderr, "Failed to destroy AH\n");
				test_result = 1;
			}
		}
		#ifdef HAVE_DC
		if (user_param->connection_type == DC && ((!(user_param->duplex || user_param->tst == LAT)
						&& (user_param->machine == SERVER)) || ((user_param->duplex || user_param->tst == LAT) && (i >= num_of_qps)))) {
			if (ibv_exp_destroy_dct(ctx->dct[i])) {
				fprintf(stderr, "Failed to destroy dct\n");
				test_result = 1;
			}
			if ( i == user_param->num_of_qps -1 )
				return test_result;
		} else
		#endif
			if (user_param->work_rdma_cm == OFF) {
				if (ibv_destroy_qp(ctx->qp[i])) {
					fprintf(stderr, "Couldn't destroy QP - %s\n", strerror(errno));
					test_result = 1;
				}
			}
	}

	if (user_param->use_rss) {
		if (user_param->connection_type == UD && (user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex)) {
			if (ibv_destroy_ah(ctx->ah[0])) {
				fprintf(stderr, "Failed to destroy AH\n");
				test_result = 1;
			}
		}

		if (ibv_destroy_qp(ctx->qp[0])) {
			fprintf(stderr, "Couldn't destroy QP - %s\n", strerror(errno));
			test_result = 1;
		}
	}
	if (user_param->srq_exists) {
		if (ibv_destroy_srq(ctx->srq)) {
			fprintf(stderr, "Couldn't destroy SRQ\n");
			test_result = 1;
		}
	}

	#ifdef HAVE_XRCD
	if (user_param->use_xrc) {

		if (ibv_close_xrcd(ctx->xrc_domain)) {
			fprintf(stderr, "Couldn't destroy XRC domain\n");
			test_result = 1;
		}

		if (ctx->fd >= 0 && close(ctx->fd)) {
			fprintf(stderr, "Couldn't close the file for the XRC Domain\n");
			test_result = 1;
		}

	}
	#endif

	if (ibv_destroy_cq(ctx->send_cq)) {
		fprintf(stderr, "Failed to destroy CQ - %s\n", strerror(errno));
		test_result = 1;
	}

	if (user_param->verb == SEND && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex || (ctx->channel)) ) {
		if (!(user_param->connection_type == DC && user_param->machine == SERVER)) {
			if (ibv_destroy_cq(ctx->recv_cq)) {
				fprintf(stderr, "Failed to destroy CQ - %s\n", strerror(errno));
				test_result = 1;
			}
		}
	}

	for (i = 0; i < dereg_counter; i++) {
		if (ibv_dereg_mr(ctx->mr[i])) {
			fprintf(stderr, "Failed to deregister MR #%d\n", i+1);
			test_result = 1;
		}
	}

	if (user_param->verb == SEND && user_param->work_rdma_cm == ON && ctx->send_rcredit) {
		if (ibv_dereg_mr(ctx->credit_mr)) {
			fprintf(stderr, "Failed to deregister send credit MR\n");
			test_result = 1;
		}
		free(ctx->ctrl_buf);
		free(ctx->ctrl_sge_list);
		free(ctx->ctrl_wr);
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Failed to deallocate PD - %s\n", strerror(errno));
		test_result = 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Failed to close event channel\n");
			test_result = 1;
		}
	}
	if (user_param->use_rdma_cm == OFF) {

		if (ibv_close_device(ctx->context)) {
			fprintf(stderr, "Failed to close device context\n");
			test_result = 1;
		}
	}

	#ifdef HAVE_CUDA
	if (user_param->use_cuda) {
		pp_free_gpu(ctx);
	}
	else
	#endif
	if (user_param->mmap_file != NULL) {
		pp_free_mmap(ctx);
	} else if (ctx->is_contig_supported == FAILURE) {
		for (i = 0; i < dereg_counter; i++) {
			if (user_param->use_hugepages) {
				shmdt(ctx->buf[i]);
			} else {
				free(ctx->buf[i]);
			}
		}
	}
	free(ctx->qp);
	#ifdef HAVE_IBV_WR_API
	free(ctx->qpx);
	#endif

	if ((user_param->tst == BW || user_param->tst == LAT_BY_BW ) && (user_param->machine == CLIENT || user_param->duplex)) {

		free(user_param->tposted);
		free(user_param->tcompleted);
		free(ctx->my_addr);
		free(ctx->rem_addr);
		free(ctx->scnt);
		free(ctx->ccnt);
	}
	else if ((user_param->tst == BW || user_param->tst == LAT_BY_BW ) && user_param->verb == SEND && user_param->machine == SERVER) {

		free(user_param->tposted);
		free(user_param->tcompleted);
		free(ctx->my_addr);
	}
	if (user_param->machine == CLIENT || user_param->tst == LAT || user_param->duplex) {

		free(ctx->sge_list);
		free(ctx->wr);
		#ifdef HAVE_VERBS_EXP
		free(ctx->exp_wr);
		#endif
	}

	if (user_param->verb == SEND && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex)) {

		free(ctx->rx_buffer_addr);
		free(ctx->recv_sge_list);
		free(ctx->rwr);
	}

	if (user_param->work_rdma_cm == ON) {
		rdma_cm_destroy_cma(ctx, user_param);
	}

	if (user_param->counter_ctx) {
		counters_close(user_param->counter_ctx);
	}

	return test_result;
}

/******************************************************************************
 *
 ******************************************************************************/
#ifdef HAVE_VERBS_EXP
static int check_inline_recv_support(struct pingpong_context *ctx,
					struct perftest_parameters *user_param)
{
	struct ibv_exp_device_attr dattr;
	int ret = 0;

	memset(&dattr, 0, sizeof(dattr));

	dattr.comp_mask |= IBV_EXP_DEVICE_ATTR_INLINE_RECV_SZ;
	ret = ibv_exp_query_device(ctx->context, &dattr);
	if (ret) {
		printf("  Couldn't query device for inline-receive capabilities.\n");
	} else if (!(dattr.comp_mask & IBV_EXP_DEVICE_ATTR_INLINE_RECV_SZ)) {
		printf("  Inline-receive not supported by driver.\n");
		ret = 1;
	} else if (dattr.inline_recv_sz < user_param->inline_recv_size) {
		printf("  Max inline-receive(%d) < Requested inline-receive(%d).\n",
			dattr.inline_recv_sz, user_param->inline_recv_size);
	}

	return ret;

}
#endif

/******************************************************************************
 *
 ******************************************************************************/
#if defined HAVE_EX_ODP || defined HAVE_EXP_ODP
static int check_odp_support(struct pingpong_context *ctx)
{
	#ifdef HAVE_EX_ODP
	struct ibv_device_attr_ex dattr;
	int odp_support_send = IBV_ODP_SUPPORT_SEND;
	int odp_support_recv = IBV_ODP_SUPPORT_RECV;
	int ret = ibv_query_device_ex(ctx->context, NULL, &dattr);
	#elif defined  HAVE_EXP_ODP
	struct ibv_exp_device_attr dattr;
	int ret = ibv_exp_query_device(ctx->context, &dattr);
	int odp_support_send = IBV_EXP_ODP_SUPPORT_SEND;
	int odp_support_recv = IBV_EXP_ODP_SUPPORT_RECV;
	#endif
	if (ret) {
		fprintf(stderr, " Couldn't query device for on-demand paging capabilities.\n");
		return 0;
	} else if (!(dattr.odp_caps.per_transport_caps.rc_odp_caps & odp_support_send)) {
		fprintf(stderr, " Send is not supported for RC transport.\n");
		return 0;
	} else if (!(dattr.odp_caps.per_transport_caps.rc_odp_caps & odp_support_recv)) {
		fprintf(stderr, " Receive is not supported for RC transport.\n");
		return 0;
	}
	return 1;
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
int create_reg_cqs(struct pingpong_context *ctx,
		   struct perftest_parameters *user_param,
		   int tx_buffer_depth, int need_recv_cq)
{
	ctx->send_cq = ibv_create_cq(ctx->context,tx_buffer_depth *
					user_param->num_of_qps, NULL, ctx->channel, user_param->eq_num);
	if (!ctx->send_cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return FAILURE;
	}

	if (need_recv_cq) {
		ctx->recv_cq = ibv_create_cq(ctx->context,user_param->rx_depth *
						user_param->num_of_qps, NULL, ctx->channel, user_param->eq_num);
		if (!ctx->recv_cq) {
			fprintf(stderr, "Couldn't create a receiver CQ\n");
			return FAILURE;
		}
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
#ifdef HAVE_VERBS_EXP
int create_exp_cqs(struct pingpong_context *ctx,
		   struct perftest_parameters *user_param,
		   int tx_buffer_depth, int need_recv_cq)
{
	struct ibv_exp_cq_init_attr attr;
	#ifdef HAVE_ACCL_VERBS
	enum ibv_exp_query_intf_status intf_status;
	struct ibv_exp_query_intf_params intf_params;
	#endif

	memset(&attr, 0, sizeof(attr));

	#ifdef HAVE_ACCL_VERBS
	if (user_param->use_res_domain) {
		attr.res_domain = ctx->res_domain;
	}

	if (user_param->verb_type == ACCL_INTF) {
		memset(&intf_params, 0, sizeof(intf_params));
		intf_params.intf_scope = IBV_EXP_INTF_GLOBAL;
		intf_params.intf = IBV_EXP_INTF_CQ;
	}
	#endif

	ctx->send_cq = ibv_exp_create_cq(ctx->context, tx_buffer_depth *
						user_param->num_of_qps, NULL,
						ctx->channel, 0, &attr);
	if (!ctx->send_cq) {
		fprintf(stderr, "Couldn't create exp CQ\n");
		return FAILURE;
	}

	if (need_recv_cq) {
		ctx->recv_cq = ibv_create_cq(ctx->context,user_param->rx_depth *
						user_param->num_of_qps,NULL,ctx->channel,0);
		if (!ctx->recv_cq) {
			fprintf(stderr, "Couldn't create a receiver CQ\n");
			return FAILURE;
		}
	}

	#ifdef HAVE_ACCL_VERBS
	if (user_param->verb_type == ACCL_INTF) {
		/* Check CQ family */
		intf_params.obj = ctx->send_cq;
		ctx->send_cq_family = ibv_exp_query_intf(ctx->context, &intf_params, &intf_status);
		intf_params.obj = ctx->recv_cq;
		ctx->recv_cq_family = ibv_exp_query_intf(ctx->context, &intf_params, &intf_status);

		if (!ctx->send_cq_family || (!ctx->recv_cq_family && need_recv_cq)) {
			fprintf(stderr, "Couldn't create CQ family.\n");
			return FAILURE;
		}
	}
	#endif

	return SUCCESS;
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
int create_cqs(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	int ret;
	int dct_only = 0, need_recv_cq = 0;
	int tx_buffer_depth = user_param->tx_depth;

	if (user_param->connection_type == DC) {
		dct_only = (user_param->machine == SERVER && !(user_param->duplex || user_param->tst == LAT));
	}

	if (dct_only)
		tx_buffer_depth = user_param->rx_depth;

	if ((user_param->connection_type == DC && !dct_only) || (user_param->verb == SEND))
		need_recv_cq = 1;

	#ifdef HAVE_VERBS_EXP
	if (user_param->is_exp_cq)
		ret = create_exp_cqs(ctx, user_param, tx_buffer_depth, need_recv_cq);
	else
	#endif
		ret = create_reg_cqs(ctx, user_param, tx_buffer_depth, need_recv_cq);

	return ret;
}

/******************************************************************************
 *
 ******************************************************************************/
#ifdef HAVE_ACCL_VERBS
struct ibv_exp_res_domain* create_res_domain(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	struct ibv_exp_res_domain_init_attr res_domain_attr;
	struct ibv_exp_device_attr dattr;
	uint32_t req_comp_mask;

	/* Query device */
	req_comp_mask = IBV_EXP_DEVICE_ATTR_CALC_CAP		|
			IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS	|
			IBV_EXP_DEVICE_ATTR_MAX_CTX_RES_DOMAIN;
	dattr.comp_mask = req_comp_mask;

	if (ibv_exp_query_device(ctx->context, &dattr)) {
		fprintf(stderr, "Couldn't query device capabilities.\n");
		return NULL;
	} else if (dattr.comp_mask !=  req_comp_mask) {
		fprintf(stderr, "This device does not support resource domain / accelerated verbs.\n");
		return NULL;
	}

	/* Allocate resource domain */
	res_domain_attr.comp_mask = IBV_EXP_RES_DOMAIN_THREAD_MODEL | IBV_EXP_RES_DOMAIN_MSG_MODEL;
	res_domain_attr.thread_model = IBV_EXP_THREAD_SINGLE;
	if (user_param->tst == BW)
		res_domain_attr.msg_model = IBV_EXP_MSG_HIGH_BW;
	else if (user_param->tst == LAT)
		res_domain_attr.msg_model = IBV_EXP_MSG_LOW_LATENCY;
	else
		res_domain_attr.msg_model = IBV_EXP_MSG_DEFAULT;

	return ibv_exp_create_res_domain(ctx->context, &res_domain_attr);
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
int create_single_mr(struct pingpong_context *ctx, struct perftest_parameters *user_param, int qp_index)
{
	int i;
	int flags = IBV_ACCESS_LOCAL_WRITE;

	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_reg_mr_in reg_mr_exp_in;
	uint64_t exp_flags = IBV_EXP_ACCESS_LOCAL_WRITE;
	#endif

	#if defined(__FreeBSD__)
	ctx->is_contig_supported = FAILURE;
	#endif

	/* ODP */
	#if defined HAVE_EX_ODP || defined HAVE_EXP_ODP
	if (user_param->use_odp) {
		if ( !check_odp_support(ctx) )
			return FAILURE;

		/* ODP does not support contig pages */
		ctx->is_contig_supported = FAILURE;
		#ifdef HAVE_EX_ODP
		flags |= IBV_ACCESS_ON_DEMAND;
		#elif defined  HAVE_EXP_ODP
		exp_flags |= IBV_EXP_ACCESS_ON_DEMAND;
		#endif
	}
	#endif


	#ifdef HAVE_CUDA
	if (user_param->use_cuda) {
		ctx->is_contig_supported = FAILURE;
		if(pp_init_gpu(ctx, ctx->buff_size)) {
			fprintf(stderr, "Couldn't allocate work buf.\n");
			return FAILURE;
		}
	} else
	#endif

	if (user_param->mmap_file != NULL) {
		#if defined(__FreeBSD__)
		posix_memalign(ctx->buf, user_param->cycle_buffer, ctx->buff_size);
		#else
		ctx->buf = memalign(user_param->cycle_buffer, ctx->buff_size);
		#endif
		if (pp_init_mmap(ctx, ctx->buff_size, user_param->mmap_file,
				 user_param->mmap_offset))
		{
			fprintf(stderr, "Couldn't allocate work buf.\n");
			return FAILURE;
		}

	} else {
		/* Allocating buffer for data, in case driver not support contig pages. */
		if (ctx->is_contig_supported == FAILURE) {
			#if defined(__FreeBSD__)
			posix_memalign(&ctx->buf[qp_index], user_param->cycle_buffer, ctx->buff_size);
			#else
			if (user_param->use_hugepages) {
				if (alloc_hugepage_region(ctx, qp_index) != SUCCESS){
					fprintf(stderr, "Failed to allocate hugepage region.\n");
					return FAILURE;
				}
				memset(ctx->buf[qp_index], 0, ctx->buff_size);
			} else if  (ctx->is_contig_supported == FAILURE) {
				ctx->buf[qp_index] = memalign(user_param->cycle_buffer, ctx->buff_size);
			}
			#endif
			if (!ctx->buf[qp_index]) {
				fprintf(stderr, "Couldn't allocate work buf.\n");
				return FAILURE;
			}

			memset(ctx->buf[qp_index], 0, ctx->buff_size);
		} else {
			ctx->buf[qp_index] = NULL;
			#ifdef HAVE_VERBS_EXP
			exp_flags |= IBV_EXP_ACCESS_ALLOCATE_MR;
			#else
			flags |= (1 << 5);
			#endif
		}
	}

	if (user_param->verb == WRITE) {
		flags |= IBV_ACCESS_REMOTE_WRITE;
		#ifdef HAVE_VERBS_EXP
		exp_flags |= IBV_EXP_ACCESS_REMOTE_WRITE;
		#endif
	} else if (user_param->verb == READ) {
		flags |= IBV_ACCESS_REMOTE_READ;
		#ifdef HAVE_VERBS_EXP
		exp_flags |= IBV_EXP_ACCESS_REMOTE_READ;
		#endif
		if (user_param->transport_type == IBV_TRANSPORT_IWARP)
			flags |= IBV_ACCESS_REMOTE_WRITE;
		#ifdef HAVE_VERBS_EXP
		exp_flags |= IBV_EXP_ACCESS_REMOTE_WRITE;
		#endif
	} else if (user_param->verb == ATOMIC) {
		flags |= IBV_ACCESS_REMOTE_ATOMIC;
		#ifdef HAVE_VERBS_EXP
		exp_flags |= IBV_EXP_ACCESS_REMOTE_ATOMIC;
		#endif
	}

	/* Allocating Memory region and assigning our buffer to it. */
	#ifdef HAVE_VERBS_EXP
	if (ctx->is_contig_supported == SUCCESS || user_param->use_odp) {
		reg_mr_exp_in.pd = ctx->pd;
		reg_mr_exp_in.addr = ctx->buf[qp_index];
		reg_mr_exp_in.length = ctx->buff_size;
		reg_mr_exp_in.exp_access = exp_flags;
		reg_mr_exp_in.comp_mask = 0;

		ctx->mr[qp_index] = ibv_exp_reg_mr(&reg_mr_exp_in);
	}
	else
		ctx->mr[qp_index] = ibv_reg_mr(ctx->pd, ctx->buf[qp_index], ctx->buff_size, flags);
	#else
	ctx->mr[qp_index] = ibv_reg_mr(ctx->pd, ctx->buf[qp_index], ctx->buff_size, flags);
	#endif

	if (!ctx->mr[qp_index]) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return FAILURE;
	}

	if (ctx->is_contig_supported == SUCCESS)
		ctx->buf[qp_index] = ctx->mr[qp_index]->addr;


	/* Initialize buffer with random numbers except in WRITE_LAT test that it 0's */
	if (!user_param->use_cuda) {
		srand(time(NULL));
		if (user_param->verb == WRITE && user_param->tst == LAT) {
			memset(ctx->buf[qp_index], 0, ctx->buff_size);
		} else {
			for (i = 0; i < ctx->buff_size; i++) {
				((char*)ctx->buf[qp_index])[i] = (char)rand();
			}
		}
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
int create_mr(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	int i;

	/* create first MR */
	if (create_single_mr(ctx, user_param, 0)) {
		fprintf(stderr, "failed to create mr\n");
		return 1;
	}

	/* create the rest if needed, or copy the first one */
	for (i = 1; i < user_param->num_of_qps; i++) {
		if (user_param->mr_per_qp) {
			if (create_single_mr(ctx, user_param, i)) {
				fprintf(stderr, "failed to create mr\n");
				return 1;
			}
		} else {
			ALLOCATE(ctx->mr[i], struct ibv_mr, 1);
			memset(ctx->mr[i], 0, sizeof(struct ibv_mr));
			ctx->mr[i] = ctx->mr[0];
			ctx->buf[i] = ctx->buf[0] + (i*BUFF_SIZE(ctx->size, ctx->cycle_buffer));
		}
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
#define HUGEPAGE_ALIGN  (2*1024*1024)
#define SHMAT_ADDR (void *)(0x0UL)
#define SHMAT_FLAGS (0)

#if !defined(__FreeBSD__)
int alloc_hugepage_region (struct pingpong_context *ctx, int qp_index)
{
	int buf_size;
	int alignment = (((ctx->cycle_buffer + HUGEPAGE_ALIGN -1) / HUGEPAGE_ALIGN) * HUGEPAGE_ALIGN);
	buf_size = (((ctx->buff_size + alignment -1 ) / alignment ) * alignment);

	/* create hugepage shared region */
	ctx->huge_shmid = shmget(IPC_PRIVATE, buf_size,
				 SHM_HUGETLB | IPC_CREAT | SHM_R | SHM_W);
	if (ctx->huge_shmid < 0) {
		fprintf(stderr, "Failed to allocate hugepages. Please configure hugepages\n");
		return FAILURE;
	}

	/* attach shared memory */
	ctx->buf[qp_index] = (void *) shmat(ctx->huge_shmid, SHMAT_ADDR, SHMAT_FLAGS);
	if (ctx->buf == (void *) -1) {
		fprintf(stderr, "Failed to attach shared memory region\n");
		return FAILURE;
	}

	/* Mark shmem for removal */
	if (shmctl(ctx->huge_shmid, IPC_RMID, 0) != 0) {
		fprintf(stderr, "Failed to mark shm for removal\n");
		return FAILURE;
	}

	return SUCCESS;
}
#endif

int verify_params_with_device_context(struct ibv_context *context,
				      struct perftest_parameters *user_param)
{
	if(user_param->use_event) {
		if(user_param->eq_num > context->num_comp_vectors) {
			fprintf(stderr, " Completion vector specified is invalid\n");
			fprintf(stderr, " Max completion vector = %d\n",
				context->num_comp_vectors - 1);
			return FAILURE;
		}
	}

	return SUCCESS;
}

#if defined HAVE_OOO_ATTR || defined HAVE_EXP_OOO_ATTR
static int verify_ooo_settings(struct pingpong_context *ctx,
			       struct perftest_parameters *user_param)
{
	#ifdef HAVE_OOO_ATTR
	struct ibv_device_attr_ex attr = { };
	if (ibv_query_device_ex(ctx->context, NULL, &attr))
	#elif HAVE_EXP_OOO_ATTR
	struct ibv_exp_device_attr attr = { };
	attr.comp_mask = IBV_EXP_DEVICE_ATTR_RESERVED - 1;
	if (ibv_exp_query_device(ctx->context, &attr))
	#endif
		return FAILURE;

	if (user_param->connection_type == RC) {
		if (attr.ooo_caps.rc_caps == 0) {
			fprintf(stderr, " OOO unsupported by HCA on RC QP\n");
			return FAILURE;
		} else {
			return SUCCESS;
		}
	} else if (user_param->connection_type == XRC) {
		if (attr.ooo_caps.xrc_caps == 0) {
			fprintf(stderr, " OOO unsupported by HCA on XRC QP\n");
			return FAILURE;
		} else {
			return SUCCESS;
		}
	} else if (user_param->connection_type == UD) {
		if (attr.ooo_caps.ud_caps == 0) {
			fprintf(stderr, " OOO unsupported by HCA on UD QP\n");
			return FAILURE;
		} else {
			return SUCCESS;
		}

	#if HAVE_OOO_ATTR
	} else if (user_param->connection_type == UC) {
		if (attr.ooo_caps.uc_caps == 0) {
			fprintf(stderr, " OOO unsupported by HCA on UC QP\n");
			return FAILURE;
		} else {
			return SUCCESS;
		}
	#elif HAVE_EXP_OOO_ATTR
	} else if (user_param->connection_type == DC) {
		if (attr.ooo_caps.dc_caps == 0) {
			fprintf(stderr, " OOO unsupported by HCA on DC QP\n");
			return FAILURE;
		} else {
			return SUCCESS;
		}
	#endif
	} else {
		return FAILURE;
	}
}
#endif
int ctx_init(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	int i;
	int num_of_qps = user_param->num_of_qps / 2;

	#ifdef HAVE_ACCL_VERBS
	enum ibv_exp_query_intf_status intf_status;
	struct ibv_exp_query_intf_params intf_params;
	#endif

	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_device_attr dattr;
	memset(&dattr, 0, sizeof(dattr));
	get_verbs_pointers(ctx);
	#endif

	#if defined HAVE_OOO_ATTR || defined HAVE_EXP_OOO_ATTR
	if (user_param->use_ooo) {
		if (verify_ooo_settings(ctx, user_param) != SUCCESS) {
			fprintf(stderr, "Incompatible OOO settings\n");
			return FAILURE;
		}
	}
	#endif

	ctx->is_contig_supported = FAILURE;
	#ifdef HAVE_VERBS_EXP
	if (!user_param->use_hugepages)
		ctx->is_contig_supported  = check_for_contig_pages_support(ctx->context);
	#endif

	/* Allocating an event channel if requested. */
	if (user_param->use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			return FAILURE;
		}
	}

	/* Allocating the Protection domain. */
	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return FAILURE;
	}

	#ifdef HAVE_ACCL_VERBS
	if (user_param->use_res_domain) {
		ctx->res_domain = create_res_domain(ctx, user_param);
		if (!ctx->res_domain) {
			fprintf(stderr, "Couldn't create resource domain\n");
			return FAILURE;
		}
	}
	#endif

	if (create_mr(ctx, user_param)) {
		fprintf(stderr, "Failed to create MR\n");
		return FAILURE;
	}

	if (create_cqs(ctx, user_param)) {
		fprintf(stderr, "Failed to create CQs\n");
		return FAILURE;

	}

	#ifdef HAVE_XRCD
	if (user_param->use_xrc) {

		if (ctx_xrcd_create(ctx,user_param)) {
			fprintf(stderr, "Couldn't create XRC resources\n");
			return FAILURE;
		}

		if (ctx_xrc_srq_create(ctx,user_param)) {
			fprintf(stderr, "Couldn't create SRQ XRC resources\n");
			return FAILURE;
		}
	}
	#endif

	if (user_param->use_srq && !user_param->use_xrc && (user_param->tst == LAT ||
				user_param->machine == SERVER || user_param->duplex == ON)) {

		struct ibv_srq_init_attr attr = {
			.attr = {
				/* when using sreq, rx_depth sets the max_wr */
				.max_wr  = user_param->rx_depth,
				.max_sge = 1
			}
		};

		ctx->srq = ibv_create_srq(ctx->pd, &attr);
		if (!ctx->srq)  {
			fprintf(stderr, "Couldn't create SRQ\n");
			return FAILURE;
		}
	}

	#ifdef HAVE_RSS_EXP
	if (user_param->use_rss) {
		struct ibv_exp_device_attr attr;

		attr.comp_mask = IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS |
			IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ;
		if (ibv_exp_query_device(ctx->context, &attr)) {
			fprintf(stderr, "Experimental ibv_exp_query_device.\n");
			exit(1);
		}

		if (!((attr.exp_device_cap_flags & IBV_EXP_DEVICE_QPG) &&
					(attr.exp_device_cap_flags & IBV_EXP_DEVICE_UD_RSS) &&
					(attr.comp_mask & IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ) &&
					(attr.max_rss_tbl_sz > 0))) {
			fprintf(stderr, "RSS not supported .\n");
			exit(1);
		}

		/* num of qps includes the parent */
		if (user_param->num_of_qps > attr.max_rss_tbl_sz + 1) {
			fprintf(stderr, "RSS limit is %d .\n",
					attr.max_rss_tbl_sz);
			exit(1);
		}
	}
	#endif

	/*
	* QPs creation in RDMA CM flow will be done separately.
	* Unless, the function called with RDMA CM connection contexts,
	* need to verify the call with the existence of ctx->cm_id.
	*/
	if (!(user_param->work_rdma_cm == OFF || ctx->cm_id))
		return SUCCESS;

	for (i=0; i < user_param->num_of_qps; i++) {

		if (create_qp_main(ctx, user_param, i, num_of_qps)) {
			fprintf(stderr, "Failed to create QP.\n");
			return FAILURE;
		}

		if (user_param->work_rdma_cm == OFF) {
			modify_qp_to_init(ctx, user_param, i, num_of_qps);
			#ifdef HAVE_ACCL_VERBS
			if (user_param->verb_type == ACCL_INTF) {
				memset(&intf_params, 0, sizeof(intf_params));
				intf_params.intf_scope = IBV_EXP_INTF_GLOBAL;
				intf_params.intf = IBV_EXP_INTF_QP_BURST;
				intf_params.obj = ctx->qp[i];
				ctx->qp_burst_family[i] = ibv_exp_query_intf(ctx->context, &intf_params, &intf_status);
				if (!ctx->qp_burst_family[i]) {
					fprintf(stderr, "Couldn't get QP burst family.\n");
					return FAILURE;
				}
			}
			#endif
		}
	}

	return SUCCESS;
}

int modify_qp_to_init(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int qp_index, int num_of_qps)
{
	uint64_t init_flag = 0;

	#ifdef HAVE_RSS_EXP
	if (qp_index == 0 && user_param->use_rss) {
		init_flag = IBV_EXP_QP_GROUP_RSS;
	}
	else
	#endif
		init_flag = 0;

	if(user_param->connection_type == DC) {
		if ( !((!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
					|| ((user_param->duplex || user_param->tst == LAT) && (qp_index >= num_of_qps)))) {
			#ifdef HAVE_DC
			if (ctx_modify_dc_qp_to_init(ctx->qp[qp_index],user_param)) {
				fprintf(stderr," Unable to create DC QP.\n");
				return FAILURE;
			}
			#endif
		}
	} else {
		if (ctx_modify_qp_to_init(ctx->qp[qp_index],user_param,init_flag)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			return FAILURE;
		}
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
int create_reg_qp_main(struct pingpong_context *ctx,
		       struct perftest_parameters *user_param,
		       int i, int num_of_qps)
{
	if (user_param->use_xrc) {
		#ifdef HAVE_XRCD
		ctx->qp[i] = ctx_xrc_qp_create(ctx, user_param, i);
		#endif
	} else {
		ctx->qp[i] = ctx_qp_create(ctx, user_param);
	}

	if (ctx->qp[i] == NULL) {
		fprintf(stderr, "Unable to create QP.\n");
		return FAILURE;
	}
	#ifdef HAVE_IBV_WR_API
	ctx->qpx[i] = ibv_qp_to_qp_ex(ctx->qp[i]);
	#endif

	return SUCCESS;
}


int create_exp_qp_main(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int i, int num_of_qps)
{
	int is_dc_tgt_query = 0;
	is_dc_tgt_query |= !(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER);
	is_dc_tgt_query |= (user_param->duplex || user_param->tst == LAT) && (i >= num_of_qps);
	is_dc_tgt_query &= user_param->connection_type == DC;

	if (is_dc_tgt_query) {
		#ifdef HAVE_DC
		if(ctx_dc_tgt_create(ctx,user_param,i)) {
			return FAILURE;
		}
		/* in order to not change anything in the test */
		ALLOCATE(ctx->qp[i], struct ibv_qp, 1);
		ctx->qp[i]->qp_num = ctx->dct[i]->dct_num;
		#else
		fprintf(stderr, "DC is not supported.\n");
		return FAILURE;
		#endif
	} else if (user_param->use_rss && user_param->connection_type == RawEth) {
		#ifdef HAVE_RSS_EXP
		ctx->qp[i] = ctx_rss_eth_qp_create(ctx, user_param, i);
		#endif
		if (ctx->qp[i] == NULL) {
			fprintf(stderr," Unable to create RSS QP.\n");
			return FAILURE;
		}
	} else if (user_param->use_xrc) {
		#ifdef HAVE_XRCD
		ctx->qp[i] = ctx_xrc_qp_create(ctx, user_param, i);
		#endif
		if (ctx->qp[i] == NULL) {
			fprintf(stderr," Unable to create XRC QP.\n");
			return FAILURE;
		}
	} else {
		#ifdef HAVE_VERBS_EXP
		ctx->qp[i] = ctx_exp_qp_create(ctx, user_param, i);
		#endif
		if (ctx->qp[i] == NULL) {
			fprintf(stderr, " Unable to create exp QP.\n");
			return FAILURE;
		}
	}
	return SUCCESS;
}

int create_qp_main(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int i, int num_of_qps)
{
	int ret;
	#ifdef HAVE_VERBS_EXP
	int query;

	/* flag that indicates that we are going to use exp QP */
	query = (user_param->connection_type == DC);
	query |= (user_param->use_rss && user_param->connection_type == RawEth);
	query |= user_param->use_xrc;
	query |= user_param->inline_recv_size != 0;
	query |= user_param->masked_atomics;
	query |= user_param->verb_type != NORMAL_INTF;
	query |= user_param->use_res_domain;
	query |= user_param->use_exp;

	if (query == 1)
		user_param->is_exp_qp = 1;

	if (user_param->is_exp_qp)
		ret = create_exp_qp_main(ctx, user_param, i, num_of_qps);
	else
	#endif
		ret = create_reg_qp_main(ctx, user_param, i, num_of_qps);

	return ret;
}

#ifdef HAVE_VERBS_EXP
#ifdef HAVE_SCATTER_FCS
static int check_scatter_fcs_support(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	struct ibv_exp_device_attr dev_attr;

	memset(&dev_attr, 0, sizeof(dev_attr));
	dev_attr.comp_mask = IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS;
	dev_attr.exp_device_cap_flags = IBV_EXP_DEVICE_SCATTER_FCS;
	if (ibv_exp_query_device(ctx->context, &dev_attr)) {
		fprintf(stderr, "ibv_exp_query_device failed\n");
		return 1;
	}

	return MASK_IS_SET(IBV_EXP_DEVICE_SCATTER_FCS, dev_attr.exp_device_cap_flags);
}
#endif
#endif

#ifdef HAVE_VERBS_EXP
struct ibv_qp* ctx_exp_qp_create(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int qp_index)
{
	struct ibv_exp_qp_init_attr attr;
	struct ibv_qp* qp = NULL;
	struct ibv_exp_device_attr dev_attr;

	memset(&attr, 0, sizeof(attr));
	memset(&dev_attr, 0, sizeof(dev_attr));
	attr.comp_mask = IBV_EXP_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;

	if (user_param->masked_atomics) {
		#ifdef HAVE_MASKED_ATOMICS
		dev_attr.comp_mask = IBV_EXP_DEVICE_ATTR_EXT_ATOMIC_ARGS | IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS;

		if (ibv_exp_query_device(ctx->context, &dev_attr)) {
			fprintf(stderr, "ibv_exp_query_device failed\n");
			return NULL;
		}

		attr.max_atomic_arg = pow(2,dev_attr.ext_atom.log_max_atomic_inline);
		attr.exp_create_flags |= IBV_EXP_QP_CREATE_ATOMIC_BE_REPLY;
		attr.comp_mask |= IBV_EXP_QP_INIT_ATTR_ATOMICS_ARG;
		#else
		fprintf(stderr, "Can't create masked atomic QP.\n");
		return NULL;
		#endif
	}

	if (user_param->inline_recv_size) {

		if (check_inline_recv_support(ctx, user_param)) {
			fprintf(stderr, "Failed to create QP with inline receive.\n");
			return NULL;
		}

		attr.comp_mask |= IBV_EXP_QP_INIT_ATTR_INL_RECV;
		attr.max_inl_recv = user_param->inline_recv_size;
		attr.sq_sig_all = (1 == user_param->cq_mod) ? 1 : 0; //inline receive on requestor must QP's sq_sig_all to be applied
	}

	#ifdef HAVE_ACCL_VERBS
	if (user_param->use_res_domain) {
		attr.comp_mask |= IBV_EXP_QP_INIT_ATTR_RES_DOMAIN;
		attr.res_domain = ctx->res_domain;
	}
	#endif

	attr.pd = ctx->pd;
	attr.send_cq = ctx->send_cq;
	attr.recv_cq = (user_param->verb == SEND) ? ctx->recv_cq : ctx->send_cq;
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_inline_data = user_param->inline_size;

	if (user_param->use_srq && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex == ON)) {
		attr.srq = ctx->srq;
		attr.cap.max_recv_wr  = 0;
		attr.cap.max_recv_sge = 0;
	} else {
		attr.srq = NULL;
		attr.cap.max_recv_wr  = user_param->rx_depth;
		attr.cap.max_recv_sge = MAX_RECV_SGE;
	}

	switch (user_param->connection_type) {

		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
		#ifdef HAVE_RAW_ETH
		case RawEth : attr.qp_type = IBV_QPT_RAW_PACKET; break;
		#endif
		case DC : attr.qp_type = IBV_EXP_QPT_DC_INI; break;
		default:  fprintf(stderr, "Unknown connection type \n");
			  return NULL;
	}

	#ifdef HAVE_SCATTER_FCS
	if (!user_param->disable_fcs && (user_param->connection_type == RawEth)) {
		if(check_scatter_fcs_support(ctx, user_param)) {
			attr.exp_create_flags |= IBV_EXP_QP_CREATE_SCATTER_FCS;
		}
	}
	#endif

	qp = ibv_exp_create_qp(ctx->context, &attr);
	if (!qp)
		return NULL;

	if (user_param->inline_recv_size > attr.max_inl_recv)
		printf("  Actual inline-receive(%d) < requested inline-receive(%d)\n",
				attr.max_inl_recv, user_param->inline_recv_size);

	if (user_param->inline_size > attr.cap.max_inline_data) {
		user_param->inline_size = attr.cap.max_inline_data;
		printf("  Actual inline-size(%d) > requested inline-size(%d)\n",
			attr.cap.max_inline_data, user_param->inline_size);
	}

	return qp;
}
#endif

struct ibv_qp* ctx_qp_create(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	struct ibv_qp* qp = NULL;

	#ifdef HAVE_IBV_WR_API
	enum ibv_wr_opcode opcode;
	struct ibv_qp_init_attr_ex attr;
	memset(&attr, 0, sizeof(struct ibv_qp_init_attr_ex));
	#else
	struct ibv_qp_init_attr attr;
	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	#endif

	attr.send_cq = ctx->send_cq;
	attr.recv_cq = (user_param->verb == SEND) ? ctx->recv_cq : ctx->send_cq;
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_inline_data = user_param->inline_size;

	if (user_param->use_srq && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex == ON)) {
		attr.srq = ctx->srq;
		attr.cap.max_recv_wr  = 0;
		attr.cap.max_recv_sge = 0;
	} else {
		attr.srq = NULL;
		attr.cap.max_recv_wr  = user_param->rx_depth;
		attr.cap.max_recv_sge = MAX_RECV_SGE;
	}

	switch (user_param->connection_type) {

		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
		#ifdef HAVE_RAW_ETH
		case RawEth : attr.qp_type = IBV_QPT_RAW_PACKET; break;
		#endif
		#ifdef HAVE_SRD
		case SRD: attr.qp_type = IBV_QPT_DRIVER; break;
		#endif
		default:  fprintf(stderr, "Unknown connection type \n");
			  return NULL;
	}

	#ifdef HAVE_IBV_WR_API
	if (user_param->verb == ATOMIC) {
		opcode = opcode_atomic_array[user_param->atomicType];
		if(0);
		else if (opcode == IBV_WR_ATOMIC_FETCH_AND_ADD)
			attr.send_ops_flags |= IBV_QP_EX_WITH_ATOMIC_FETCH_AND_ADD;
		else if (opcode == IBV_WR_ATOMIC_CMP_AND_SWP)
			attr.send_ops_flags |= IBV_QP_EX_WITH_ATOMIC_CMP_AND_SWP;
	}
	else {
		opcode = opcode_verbs_array[user_param->verb];
		if(0);
		else if (opcode == IBV_WR_SEND)
			attr.send_ops_flags |= IBV_QP_EX_WITH_SEND;
		else if (opcode == IBV_WR_RDMA_WRITE)
			attr.send_ops_flags |= IBV_QP_EX_WITH_RDMA_WRITE;
		else if (opcode == IBV_WR_RDMA_READ)
			attr.send_ops_flags |= IBV_QP_EX_WITH_RDMA_READ;
	}
	attr.pd = ctx->pd;
	attr.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS | IBV_QP_INIT_ATTR_PD;
	#endif

	if (user_param->work_rdma_cm) {
		#if !defined(HAVE_IBV_WR_API)
		if (rdma_create_qp(ctx->cm_id, ctx->pd, &attr)) {
			fprintf(stderr, "Couldn't create rdma QP - %s\n", strerror(errno));
		} else {
			qp = ctx->cm_id->qp;
		}
		#endif
	} else if (user_param->connection_type == SRD) {
		#ifdef HAVE_SRD
		qp = efadv_create_driver_qp(ctx->pd, &attr, EFADV_QP_DRIVER_TYPE_SRD);
		#endif
	} else {
		#ifdef HAVE_IBV_WR_API
		qp = ibv_create_qp_ex(ctx->context, &attr);
		#else
		qp = ibv_create_qp(ctx->pd, &attr);
		#endif
	}

	if (qp == NULL && errno == ENOMEM) {
		fprintf(stderr, "Requested QP size might be too big. Try reducing TX depth and/or inline size.\n");
		fprintf(stderr, "Current TX depth is %d and  inline size is %d .\n", user_param->tx_depth, user_param->inline_size);
	}

	if (user_param->inline_size > attr.cap.max_inline_data) {
		user_param->inline_size = attr.cap.max_inline_data;
		printf("  Actual inline-size(%d) > requested inline-size(%d)\n",
			attr.cap.max_inline_data, user_param->inline_size);
	}

	return qp;
}

#ifdef HAVE_MASKED_ATOMICS
/******************************************************************************
 *
 ******************************************************************************/
struct ibv_qp* ctx_atomic_qp_create(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	struct ibv_exp_qp_init_attr	attr;
	struct ibv_qp* qp = NULL;
	struct ibv_exp_device_attr dev_attr;

	memset(&dev_attr, 0, sizeof(dev_attr));

	dev_attr.comp_mask |= IBV_EXP_DEVICE_ATTR_EXT_ATOMIC_ARGS | IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS;

	if (ibv_exp_query_device(ctx->context, &dev_attr)) {
		fprintf(stderr, "ibv_exp_query_device failed\n");
		exit(1);
	}

	memset(&attr, 0, sizeof(struct ibv_exp_qp_init_attr));
	attr.pd = ctx->pd;
	attr.send_cq = ctx->send_cq;
	attr.recv_cq = (user_param->verb == SEND) ? ctx->recv_cq : ctx->send_cq;
	attr.cap.max_send_wr  = user_param->tx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_inline_data = user_param->inline_size;
	attr.max_atomic_arg = pow(2,dev_attr.ext_atom.log_max_atomic_inline);
	attr.exp_create_flags = IBV_EXP_QP_CREATE_ATOMIC_BE_REPLY;
	attr.comp_mask = IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS | IBV_EXP_QP_INIT_ATTR_PD;
	attr.comp_mask |= IBV_EXP_QP_INIT_ATTR_ATOMICS_ARG;

	if (user_param->use_srq && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex == ON)) {
		attr.srq = ctx->srq;
		attr.cap.max_recv_wr  = 0;
		attr.cap.max_recv_sge = 0;
	} else {
		attr.srq = NULL;
		attr.cap.max_recv_wr  = user_param->rx_depth;
		attr.cap.max_recv_sge = MAX_RECV_SGE;
	}

	switch (user_param->connection_type) {

		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
		#ifdef HAVE_XRCD
		case XRC : attr.qp_type = IBV_QPT_XRC; break;
		#endif
		#ifdef HAVE_RAW_ETH
		case RawEth : attr.qp_type = IBV_QPT_RAW_PACKET; break;
		#endif
		default:  fprintf(stderr, "Unknown connection type \n");
			  return NULL;
	}

	qp = ibv_exp_create_qp(ctx->context, &attr);

	return qp;
}
#endif

#ifdef HAVE_DC
/******************************************************************************
 *
 ******************************************************************************/
int ctx_modify_dc_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param)
{
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;
	int err;
	uint64_t flags;

	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_qp_attr attr;
	memset(&attr, 0, sizeof(struct ibv_exp_qp_attr));
	flags = IBV_EXP_QP_STATE | IBV_EXP_QP_PKEY_INDEX | IBV_EXP_QP_PORT;
	#else
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;
	#endif

	static int portindex=0;  /* for dual-port support */

	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = user_param->pkey_index;
	attr.qp_access_flags = 0;
	attr.dct_key = user_param->dct_key;

	if (user_param->duplex || user_param->tst == LAT) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	if (user_param->dualport==ON) {
		if (portindex % num_of_qps < num_of_qps_per_port) {
			attr.port_num = user_param->ib_port;
			user_param->port_by_qp[portindex] = 0;
		} else {
			attr.port_num = user_param->ib_port2;
			user_param->port_by_qp[portindex] = 1;
		}
		portindex++;

	} else {

		attr.port_num = user_param->ib_port;
	}

	#ifdef HAVE_VERBS_EXP
	flags |= IBV_EXP_QP_DC_KEY;
	err = ibv_exp_modify_qp(qp,&attr,flags);
	#else
	flags |= IBV_QP_DC_KEY;
	attr.comp_mask = IBV_QP_ATTR_DCT_KEY;
	err = ibv_modify_qp(qp,&attr,flags);
	#endif

	if (err) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return 1;
	}
	return 0;
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param, uint64_t init_flag)
{
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;

	struct ibv_qp_attr attr;
	int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_qp_attr exp_attr;
	uint64_t exp_flags = 0;
	#endif

	static int portindex=0;  /* for dual-port support */
	int ret = 0;

	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = user_param->pkey_index;

	#ifdef HAVE_VERBS_EXP
	memset(&exp_attr, 0, sizeof(struct ibv_exp_qp_attr));
	exp_attr.qp_state        = attr.qp_state;
	exp_attr.pkey_index      = attr.pkey_index;
	#endif

	if ( user_param->use_xrc && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	if (user_param->dualport==ON) {
		if (portindex % num_of_qps < num_of_qps_per_port) {
			attr.port_num = user_param->ib_port;
			user_param->port_by_qp[portindex] = 0;
		} else {
			attr.port_num = user_param->ib_port2;
			user_param->port_by_qp[portindex] = 1;
		}
		portindex++;

	} else {

		attr.port_num = user_param->ib_port;
	}

	#ifdef HAVE_VERBS_EXP
	exp_attr.port_num = attr.port_num;
	#endif

	if (user_param->connection_type == RawEth) {
		flags = IBV_QP_STATE | IBV_QP_PORT;
		#ifdef HAVE_VERBS_EXP
		exp_flags = init_flag | IBV_EXP_QP_STATE | IBV_EXP_QP_PORT;
		#endif

	} else if (user_param->connection_type == UD || user_param->connection_type == SRD) {
		attr.qkey = DEFF_QKEY;
		flags |= IBV_QP_QKEY;

	} else {
		switch (user_param->verb) {
			case ATOMIC: attr.qp_access_flags = IBV_ACCESS_REMOTE_ATOMIC; break;
			case READ  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
			case WRITE : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
			case SEND  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
		}
		flags |= IBV_QP_ACCESS_FLAGS;
	}

	#ifdef HAVE_MASKED_ATOMICS
	if (user_param->masked_atomics) {
		exp_attr.qp_access_flags = IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
		exp_flags = IBV_EXP_QP_STATE | IBV_EXP_QP_PKEY_INDEX | IBV_EXP_QP_PORT | IBV_EXP_QP_ACCESS_FLAGS;
	}
	#endif

	#ifdef HAVE_VERBS_EXP
	if ( (init_flag != 0 && user_param->use_rss) || user_param->masked_atomics ) {
		ret = ibv_exp_modify_qp(qp,&exp_attr,exp_flags);
	}
	else
	#endif
		ret = ibv_modify_qp(qp,&attr,flags);

	if (ret) {
		fprintf(stderr, "Failed to modify QP to INIT, ret=%d\n",ret);
		return 1;
	}
	return 0;
}

#ifdef HAVE_DC
/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_dc_qp_to_rtr(struct ibv_qp *qp,
		struct ibv_exp_qp_attr *attr,
		struct perftest_parameters *user_param,
		struct pingpong_dest *dest,
		struct pingpong_dest *my_dest,
		int qpindex)
{
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;

	int flags = IBV_EXP_QP_STATE | IBV_EXP_QP_PATH_MTU | IBV_EXP_QP_AV;
	attr->qp_state = IBV_QPS_RTR;
	attr->ah_attr.src_path_bits = 0;

	/* in DC with bidirectional,
	 * there are send qps and recv qps. the actual number of send/recv qps
	 * is num_of_qps / 2.
	 */
	if (user_param->duplex || user_param->tst == LAT) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	/* first half of qps are for ib_port and second half are for ib_port2
	 * in DC with bidirectional, the first half of qps are DC_INI qps and
	 * the second half are DC_TGT qps. the first half of the send/recv qps
	 * are for ib_port1 and the second half are for ib_port2
	 */
	if (user_param->dualport == ON && (qpindex % num_of_qps >= num_of_qps_per_port))
		attr->ah_attr.port_num = user_param->ib_port2;
	else
		attr->ah_attr.port_num = user_param->ib_port;

	attr->ah_attr.dlid = (user_param->dlid) ? user_param->dlid : dest->lid;
	if (user_param->gid_index == DEF_GID_INDEX) {

		attr->ah_attr.is_global = 0;
		attr->ah_attr.sl = user_param->sl;

	} else {
		attr->ah_attr.is_global  = 1;
		attr->ah_attr.grh.dgid = dest->gid;
		attr->ah_attr.grh.sgid_index = user_param->gid_index;
		attr->ah_attr.grh.hop_limit = 1;
		attr->ah_attr.grh.traffic_class = user_param->traffic_class;
		attr->ah_attr.sl = 0;
	}

	attr->max_dest_rd_atomic = 0;
	attr->min_rnr_timer = 0;
	attr->dct_key = user_param->dct_key;
	attr->path_mtu = user_param->curr_mtu;

	return ibv_exp_modify_qp(qp,attr,flags);
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_qp_to_rtr(struct ibv_qp *qp,
		struct ibv_qp_attr *attr,
		struct perftest_parameters *user_param,
		struct pingpong_dest *dest,
		struct pingpong_dest *my_dest,
		int qpindex)
{
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;

	int flags = IBV_QP_STATE;
	int ooo_flags = 0;

	attr->qp_state = IBV_QPS_RTR;
	attr->ah_attr.src_path_bits = 0;

	/* in xrc with bidirectional,
	 * there are send qps and recv qps. the actual number of send/recv qps
	 * is num_of_qps / 2.
	 */
	if ( user_param->use_xrc && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	/* first half of qps are for ib_port and second half are for ib_port2
	 * in xrc with bidirectional, the first half of qps are xrc_send qps and
	 * the second half are xrc_recv qps. the first half of the send/recv qps
	 * are for ib_port1 and the second half are for ib_port2
	 */
	if (user_param->dualport == ON && (qpindex % num_of_qps >= num_of_qps_per_port))
		attr->ah_attr.port_num = user_param->ib_port2;
	else
		attr->ah_attr.port_num = user_param->ib_port;

	if (user_param->connection_type != RawEth) {

		attr->ah_attr.dlid = (user_param->dlid) ? user_param->dlid : dest->lid;
		attr->ah_attr.sl = user_param->sl;

		if (((attr->ah_attr.port_num == user_param->ib_port) && (user_param->gid_index == DEF_GID_INDEX))
				|| ((attr->ah_attr.port_num == user_param->ib_port2) && (user_param->gid_index2 == DEF_GID_INDEX) && user_param->dualport)) {

			attr->ah_attr.is_global = 0;
		} else {

			attr->ah_attr.is_global  = 1;
			attr->ah_attr.grh.dgid = dest->gid;
			attr->ah_attr.grh.sgid_index = (attr->ah_attr.port_num == user_param->ib_port) ? user_param->gid_index : user_param->gid_index2;
			attr->ah_attr.grh.hop_limit = 0xFF;
			attr->ah_attr.grh.traffic_class = user_param->traffic_class;
		}
		if (user_param->connection_type != UD && user_param->connection_type != SRD) {

			attr->path_mtu = user_param->curr_mtu;
			attr->dest_qp_num = dest->qpn;
			attr->rq_psn = dest->psn;

			flags |= (IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);

			if (user_param->connection_type == RC || user_param->connection_type == XRC) {

				attr->max_dest_rd_atomic = my_dest->out_reads;
				attr->min_rnr_timer = 12;
				flags |= (IBV_QP_MIN_RNR_TIMER | IBV_QP_MAX_DEST_RD_ATOMIC);
			}
		}
	}
	else if (user_param->raw_qos) {
		attr->ah_attr.sl = user_param->sl;
		flags |= IBV_QP_AV;
	}

	#ifdef HAVE_OOO_ATTR
		ooo_flags |= IBV_QP_OOO_RW_DATA_PLACEMENT;
	#elif HAVE_EXP_OOO_ATTR
		ooo_flags |= IBV_EXP_QP_OOO_RW_DATA_PLACEMENT;
	#endif

	if (user_param->use_ooo)
		flags |= ooo_flags;
	return ibv_modify_qp(qp, attr, flags);
}

#ifdef HAVE_DC
/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_dc_qp_to_rts(struct ibv_qp *qp,
		#ifdef HAVE_VERBS_EXP
		struct ibv_exp_qp_attr *attr,
		#else
		struct ibv_qp_attr_ex *attr,
		#endif
		struct perftest_parameters *user_param,
		struct pingpong_dest *dest,
		struct pingpong_dest *my_dest)
{

	#ifdef HAVE_VERBS_EXP
	int flags = IBV_EXP_QP_STATE | IBV_EXP_QP_TIMEOUT | IBV_EXP_QP_RETRY_CNT |
					 IBV_EXP_QP_RNR_RETRY | IBV_EXP_QP_MAX_QP_RD_ATOMIC;
	#else
	int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
	#endif

	attr->qp_state = IBV_QPS_RTS;

	attr->timeout   = user_param->qp_timeout;
	attr->retry_cnt = 7;
	attr->rnr_retry = 7;
	attr->max_rd_atomic  = dest->out_reads;

	#ifdef HAVE_VERBS_EXP
	return ibv_exp_modify_qp(qp,attr,flags);
	#else
	return ibv_modify_qp_ex(qp,attr,flags);
	#endif
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_qp_to_rts(struct ibv_qp *qp,
		void *_attr,
		struct perftest_parameters *user_param,
		struct pingpong_dest *dest,
		struct pingpong_dest *my_dest)
{
	#ifdef HAVE_PACKET_PACING_EXP
	uint64_t flags = IBV_QP_STATE;
	#else
	int flags = IBV_QP_STATE;
	#endif
	struct ibv_qp_attr *attr = (struct ibv_qp_attr*)_attr;

	attr->qp_state = IBV_QPS_RTS;

	if (user_param->connection_type != RawEth) {

		flags |= IBV_QP_SQ_PSN;
		attr->sq_psn = my_dest->psn;

		if (user_param->connection_type == RC || user_param->connection_type == XRC) {

			attr->timeout   = user_param->qp_timeout;
			attr->retry_cnt = 7;
			attr->rnr_retry = 7;
			attr->max_rd_atomic  = dest->out_reads;
			flags |= (IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
		}
	}

	#ifdef HAVE_PACKET_PACING_EXP
	if (user_param->rate_limit_type == PP_RATE_LIMIT) {
		((struct ibv_exp_qp_attr*)_attr)->rate_limit = user_param->rate_limit;

	#ifdef HAVE_PACKET_PACING_EXTENSION_EXP
		((struct ibv_exp_qp_attr*)_attr)->burst_info.max_burst_sz =
			user_param->burst_size;
		((struct ibv_exp_qp_attr*)_attr)->burst_info.typical_pkt_sz =
			user_param->typical_pkt_size;
		((struct ibv_exp_qp_attr*)_attr)->comp_mask |= IBV_EXP_QP_ATTR_BURST_INFO;
	#endif
		flags |= IBV_EXP_QP_RATE_LIMIT;
		return ibv_exp_modify_qp(qp, (struct ibv_exp_qp_attr*)_attr, flags);
	}
	#elif HAVE_PACKET_PACING
	if (user_param->rate_limit_type == PP_RATE_LIMIT) {
		attr->rate_limit = user_param->rate_limit;
		flags |= IBV_QP_RATE_LIMIT;
	}
	#endif

	return ibv_modify_qp(qp, attr, flags);
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_connect(struct pingpong_context *ctx,
		struct pingpong_dest *dest,
		struct perftest_parameters *user_param,
		struct pingpong_dest *my_dest)
{
	int i;
	#ifdef HAVE_DC
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_qp_attr attr_ex;
	#else
	struct ibv_qp_attr_ex attr_ex;
	#endif
	#elif HAVE_PACKET_PACING_EXP
	struct ibv_exp_qp_attr attr_ex;
	#endif
	struct ibv_qp_attr attr;
	int xrc_offset = 0;

	if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT)) {
		xrc_offset = user_param->num_of_qps / 2;
	}
	for (i=0; i < user_param->num_of_qps; i++) {

		if (user_param->connection_type == DC) {
			if ( ((!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
						|| ((user_param->duplex || user_param->tst == LAT) && (i >= user_param->num_of_qps/2)))) {
				continue;
			}
		}
		#if defined (HAVE_DC) || defined (HAVE_PACKET_PACING_EXP)
		memset(&attr_ex, 0, sizeof attr_ex);
		#endif
		memset(&attr, 0, sizeof attr);

		if (user_param->rate_limit_type == HW_RATE_LIMIT)
			attr.ah_attr.static_rate = user_param->valid_hw_rate_limit_index;

		#if defined (HAVE_PACKET_PACING_EXP) || defined (HAVE_PACKET_PACING)
		if (user_param->rate_limit_type == PP_RATE_LIMIT) {
			if (check_packet_pacing_support(ctx) == FAILURE) {
				fprintf(stderr, "Packet Pacing isn't supported.\n");
				return FAILURE;
			}
			#if defined (HAVE_PACKET_PACING_EXTENSION_EXP)
			if (check_packet_pacing_extension_support(ctx) == FAILURE &&
			    (user_param->burst_size || user_param->typical_pkt_size)) {
				fprintf(stderr, "Burst control isn't supported.\n");
				return FAILURE;
			}
			#endif
		}
		#endif

		if ((i >= xrc_offset) && (user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT))
			xrc_offset = -1*xrc_offset;

		if(user_param->connection_type == DC) {
			#ifdef HAVE_DC
			if(ctx_modify_dc_qp_to_rtr(ctx->qp[i],&attr_ex,user_param,&dest[xrc_offset + i],&my_dest[i],i)) {
				fprintf(stderr, "Failed to modify QP %d to RTR\n",ctx->qp[i]->qp_num);
				return FAILURE;
			}
			#endif
		} else {
			if(ctx_modify_qp_to_rtr(ctx->qp[i], &attr, user_param, &dest[xrc_offset + i], &my_dest[i], i)) {
				fprintf(stderr, "Failed to modify QP %d to RTR\n",ctx->qp[i]->qp_num);
				return FAILURE;
			}
		}
		if (user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex) {
			if(user_param->connection_type == DC) {
				#ifdef HAVE_DC
				if(ctx_modify_dc_qp_to_rts(ctx->qp[i], &attr_ex, user_param, &dest[xrc_offset + i], &my_dest[i])) {
					fprintf(stderr, "Failed to modify QP to RTS\n");
					return FAILURE;
				}
				#endif
			} else {
				#ifdef HAVE_PACKET_PACING_EXP
				if (user_param->rate_limit_type == PP_RATE_LIMIT) {
					if(ctx_modify_qp_to_rts(ctx->qp[i], &attr_ex, user_param, &dest[xrc_offset + i], &my_dest[i])) {
						fprintf(stderr, "Failed to modify QP %x to RTS\n", ctx->qp[i]->qp_num);
						return FAILURE;
					}
				} else {
				#endif
					if(ctx_modify_qp_to_rts(ctx->qp[i], &attr, user_param, &dest[xrc_offset + i], &my_dest[i])) {
						fprintf(stderr, "Failed to modify QP to RTS\n");
						return FAILURE;
					}
				#ifdef HAVE_PACKET_PACING_EXP
				}
				#endif
			}
		}

		if ((user_param->connection_type == UD || user_param->connection_type == DC || user_param->connection_type == SRD) &&
				(user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex)) {

			#ifdef HAVE_DC
			if(user_param->connection_type == DC)
				ctx->ah[i] = ibv_create_ah(ctx->pd,&(attr_ex.ah_attr));
			else
			#endif
				ctx->ah[i] = ibv_create_ah(ctx->pd,&(attr.ah_attr));

			if (!ctx->ah[i]) {
				fprintf(stderr, "Failed to create AH\n");
				return FAILURE;
			}
		}

		if (user_param->rate_limit_type == HW_RATE_LIMIT) {
			struct ibv_qp_attr qp_attr;
			struct ibv_qp_init_attr init_attr;
			int err, qp_static_rate = 0;

			memset(&qp_attr,0,sizeof(struct ibv_qp_attr));
			memset(&init_attr,0,sizeof(struct ibv_qp_init_attr));

			err = ibv_query_qp(ctx->qp[i], &qp_attr, IBV_QP_AV, &init_attr);
			if (err)
				fprintf(stderr, "ibv_query_qp failed to get ah_attr\n");
			else
				qp_static_rate = (int)(qp_attr.ah_attr.static_rate);

			//- Fall back to SW Limit only if flag undefined
			if(err ||
			   qp_static_rate != user_param->valid_hw_rate_limit_index ||
			   user_param->link_type != IBV_LINK_LAYER_INFINIBAND) {
				if(!user_param->is_rate_limit_type) {
					user_param->rate_limit_type = SW_RATE_LIMIT;
					fprintf(stderr, "\x1b[31mThe QP failed to accept HW rate limit, providing SW rate limit \x1b[0m\n");
				} else {
					fprintf(stderr, "\x1b[31mThe QP failed to accept HW rate limit  \x1b[0m\n");
					return FAILURE;
				}

			}
		}

		if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT))
			xrc_offset = user_param->num_of_qps / 2;

	}
	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
void ctx_set_send_wqes(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct pingpong_dest *rem_dest)
{

	#ifdef HAVE_VERBS_EXP
	if (user_param->use_exp == 1) {
		ctx_set_send_exp_wqes(ctx,user_param,rem_dest);
	}
	else {
	#endif
		ctx_set_send_reg_wqes(ctx,user_param,rem_dest);
	#ifdef HAVE_VERBS_EXP
	}
	#endif
}

#ifdef HAVE_VERBS_EXP
/******************************************************************************
 *
 ******************************************************************************/
void ctx_set_send_exp_wqes(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct pingpong_dest *rem_dest)
{
	int i,j;
	int num_of_qps = user_param->num_of_qps;
	int xrc_offset = 0;
	uint32_t remote_qpn, remote_qkey;

	if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		xrc_offset = num_of_qps;
	}

	for (i = 0; i < num_of_qps ; i++) {
		memset(&ctx->exp_wr[i*user_param->post_list],0,sizeof(struct ibv_exp_send_wr));
		ctx->sge_list[i*user_param->post_list].addr = (uintptr_t)ctx->buf[i];

		if (user_param->mac_fwd) {
			if (user_param->mr_per_qp) {
				ctx->sge_list[i*user_param->post_list].addr =
					(uintptr_t)ctx->buf[0] + (num_of_qps + i)*BUFF_SIZE(ctx->size,ctx->cycle_buffer);
			} else {
				ctx->sge_list[i*user_param->post_list].addr = (uintptr_t)ctx->buf[i];
			}
		}

		if (user_param->verb == WRITE || user_param->verb == READ)
			ctx->exp_wr[i*user_param->post_list].wr.rdma.remote_addr   = rem_dest[xrc_offset + i].vaddr;

		else if (user_param->verb == ATOMIC)
			ctx->exp_wr[i*user_param->post_list].wr.atomic.remote_addr = rem_dest[xrc_offset + i].vaddr;

		if (user_param->tst == BW || user_param->tst == LAT_BY_BW ) {

			ctx->scnt[i] = 0;
			ctx->ccnt[i] = 0;
			ctx->my_addr[i] = (uintptr_t)ctx->buf[i];
			if (user_param->verb != SEND)
				ctx->rem_addr[i] = rem_dest[xrc_offset + i].vaddr;
		}

		for (j = 0; j < user_param->post_list; j++) {

			ctx->sge_list[i*user_param->post_list + j].length =
				(user_param->connection_type == RawEth) ? (user_param->size - HW_CRC_ADDITION) : user_param->size;

			ctx->sge_list[i*user_param->post_list + j].lkey = ctx->mr[i]->lkey;

			if (j > 0) {

				ctx->sge_list[i*user_param->post_list +j].addr = ctx->sge_list[i*user_param->post_list + (j-1)].addr;

				if ((user_param->tst == BW || user_param->tst == LAT_BY_BW) && user_param->size <= (ctx->cycle_buffer / 2))
					increase_loc_addr(&ctx->sge_list[i*user_param->post_list +j],user_param->size,
							j-1,ctx->my_addr[i],0,ctx->cache_line_size,ctx->cycle_buffer);
			}

			ctx->exp_wr[i*user_param->post_list + j].sg_list = &ctx->sge_list[i*user_param->post_list + j];
			ctx->exp_wr[i*user_param->post_list + j].num_sge = MAX_SEND_SGE;
			ctx->exp_wr[i*user_param->post_list + j].wr_id   = i;

			if (j == (user_param->post_list - 1)) {
				ctx->exp_wr[i*user_param->post_list + j].next = NULL;
			} else {
				ctx->exp_wr[i*user_param->post_list + j].next = &ctx->exp_wr[i*user_param->post_list+j+1];
			}

			if ((j + 1) % user_param->cq_mod == 0) {
				#ifdef HAVE_ACCL_VERBS
				if (user_param->verb_type == ACCL_INTF)
					ctx->exp_wr[i*user_param->post_list + j].exp_send_flags = IBV_EXP_QP_BURST_SIGNALED;
				else
				#endif
					ctx->exp_wr[i*user_param->post_list + j].exp_send_flags = IBV_EXP_SEND_SIGNALED;
			} else {
				ctx->exp_wr[i*user_param->post_list + j].exp_send_flags = 0;
			}

			if (user_param->verb == ATOMIC) {
				ctx->exp_wr[i*user_param->post_list + j].exp_opcode = exp_opcode_atomic_array[user_param->atomicType];
			}
			else {
				ctx->exp_wr[i*user_param->post_list + j].exp_opcode = exp_opcode_verbs_array[user_param->verb];
			}
			if (user_param->verb == WRITE || user_param->verb == READ) {

				ctx->exp_wr[i*user_param->post_list + j].wr.rdma.rkey = rem_dest[xrc_offset + i].rkey;

				if (j > 0) {

					ctx->exp_wr[i*user_param->post_list + j].wr.rdma.remote_addr = ctx->exp_wr[i*user_param->post_list + (j-1)].wr.rdma.remote_addr;

					if ((user_param->tst == BW || user_param->tst == LAT_BY_BW) && user_param->size <= (ctx->cycle_buffer / 2))
						increase_exp_rem_addr(&ctx->exp_wr[i*user_param->post_list + j],user_param->size,
								j-1,ctx->rem_addr[i],WRITE,ctx->cache_line_size,ctx->cycle_buffer);
				}

			} else if (user_param->verb == ATOMIC) {

				ctx->exp_wr[i*user_param->post_list + j].wr.atomic.rkey = rem_dest[xrc_offset + i].rkey;

				if (j > 0) {

					ctx->exp_wr[i*user_param->post_list + j].wr.atomic.remote_addr = ctx->exp_wr[i*user_param->post_list + j-1].wr.atomic.remote_addr;
					if (user_param->tst == BW || user_param->tst == LAT_BY_BW)
						increase_exp_rem_addr(&ctx->exp_wr[i*user_param->post_list + j],user_param->size,
								j-1,ctx->rem_addr[i],ATOMIC,ctx->cache_line_size,ctx->cycle_buffer);
				}

				if (user_param->atomicType == FETCH_AND_ADD)
					ctx->exp_wr[i*user_param->post_list + j].wr.atomic.compare_add = ATOMIC_ADD_VALUE;

				else
					ctx->exp_wr[i*user_param->post_list + j].wr.atomic.swap = ATOMIC_SWAP_VALUE;


			} else if (user_param->verb == SEND) {

				if (user_param->connection_type == UD) {

					ctx->exp_wr[i*user_param->post_list + j].wr.ud.ah = ctx->ah[i];
					if (user_param->work_rdma_cm) {
						remote_qpn = ctx->cma_master.nodes[i].remote_qpn;
						remote_qkey = ctx->cma_master.nodes[i].remote_qkey;
					} else {
						remote_qpn = rem_dest[xrc_offset + i].qpn;
						remote_qkey = DEF_QKEY;
					}
					ctx->exp_wr[i*user_param->post_list + j].wr.ud.remote_qkey = remote_qkey;
					ctx->exp_wr[i*user_param->post_list + j].wr.ud.remote_qpn = remote_qpn;

				#ifdef HAVE_DC
				} else if (user_param->connection_type == DC) {
					ctx->exp_wr[i*user_param->post_list + j].dc.ah = ctx->ah[i];
					ctx->exp_wr[i*user_param->post_list + j].dc.dct_access_key = user_param->dct_key;
					ctx->exp_wr[i*user_param->post_list + j].dc.dct_number = rem_dest[xrc_offset + i].qpn;
				#endif
				}
			}

			#ifdef HAVE_DC
			if (user_param->connection_type == DC) {
				ctx->exp_wr[i*user_param->post_list + j].dc.ah = ctx->ah[i];
				ctx->exp_wr[i*user_param->post_list + j].dc.dct_access_key = user_param->dct_key;
				ctx->exp_wr[i*user_param->post_list + j].dc.dct_number = rem_dest[xrc_offset + i].qpn;
			}
			#endif


			if ((user_param->verb == SEND || user_param->verb == WRITE) && user_param->size <= user_param->inline_size)
				ctx->exp_wr[i*user_param->post_list + j].exp_send_flags |= IBV_EXP_SEND_INLINE;

			#ifdef HAVE_XRCD
			if (user_param->use_xrc)
				ctx->exp_wr[i*user_param->post_list + j].qp_type.xrc.remote_srqn = rem_dest[xrc_offset + i].srqn;
			#endif
		}
	}
}
#endif

/* ctx_post_send_work_request_func_pointer.
 *
 * Description :
 *
 * Chooses the correct pointer to be used to call funtion for posting WR
 * using new posting API.
 *
 * Parameters :
 *
 *	ctx         - Test Context.
 *	user_param  - user_parameters struct for this test.
 *
 * Return Value : void.
 *
 */
#ifdef HAVE_IBV_WR_API
static void ctx_post_send_work_request_func_pointer(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int use_inl = user_param->size <= user_param->inline_size;
	switch (user_param->connection_type) {
	case RC:
		switch (user_param->verb) {
			case SEND:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_inl_rc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_sge_rc;
				}
				break;
			case WRITE:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_inl_rc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_sge_rc;
				}
				break;
			case READ:
				ctx->new_post_send_work_request_func_pointer = &new_post_read_sge_rc;
				break;
			case ATOMIC:
				if (user_param->atomicType == FETCH_AND_ADD) {
					ctx->new_post_send_work_request_func_pointer = &new_post_atomic_fa_sge_rc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_atomic_cs_sge_rc;
				}
				break;
			default:
				fprintf(stderr, "The post send properties are not supported on RC. \n");
		}
		break;
	case UD:
		switch (user_param->verb) {
			case SEND:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_inl_ud;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_sge_ud;
				}
				break;
			default:
				fprintf(stderr, "The post send properties are not supported on UD. \n");
		}
		break;
	case UC:
		switch (user_param->verb) {
			case SEND:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_inl_uc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_sge_uc;
				}
				break;
			case WRITE:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_inl_uc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_sge_uc;
				}
				break;
			default:
				fprintf(stderr, "The post send properties are not supported on UD. \n");
		}
		break;
	case XRC:
		switch (user_param->verb) {
			case SEND:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_inl_xrc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_sge_xrc;
				}
				break;
			case WRITE:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_inl_xrc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_sge_xrc;
				}
				break;
			case READ:
				ctx->new_post_send_work_request_func_pointer = &new_post_read_sge_xrc;
				break;
			case ATOMIC:
				if (user_param->atomicType == FETCH_AND_ADD) {
					ctx->new_post_send_work_request_func_pointer = &new_post_atomic_fa_sge_xrc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_atomic_cs_sge_xrc;
				}
				break;
			default:
				fprintf(stderr, "The post send properties are not supported on RC. \n");
		}
		break;
	default:
		fprintf(stderr, "Unsupported transport. \n");
	}
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
void ctx_set_send_reg_wqes(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct pingpong_dest *rem_dest)
{
	int i,j;
	int num_of_qps = user_param->num_of_qps;
	int xrc_offset = 0;
	uint32_t remote_qpn, remote_qkey;

	if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		xrc_offset = num_of_qps;
	}
	#ifdef HAVE_IBV_WR_API
	ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif
	for (i = 0; i < num_of_qps ; i++) {
		memset(&ctx->wr[i*user_param->post_list],0,sizeof(struct ibv_send_wr));
		ctx->sge_list[i*user_param->post_list].addr = (uintptr_t)ctx->buf[i];

		if (user_param->mac_fwd) {
			if (user_param->mr_per_qp) {
				ctx->sge_list[i*user_param->post_list].addr =
					(uintptr_t)ctx->buf[0] + (num_of_qps + i)*BUFF_SIZE(ctx->size,ctx->cycle_buffer);
			} else {
				ctx->sge_list[i*user_param->post_list].addr = (uintptr_t)ctx->buf[i];
			}
		}

		if (user_param->verb == WRITE || user_param->verb == READ)
			ctx->wr[i*user_param->post_list].wr.rdma.remote_addr   = rem_dest[xrc_offset + i].vaddr;

		else if (user_param->verb == ATOMIC)
			ctx->wr[i*user_param->post_list].wr.atomic.remote_addr = rem_dest[xrc_offset + i].vaddr;

		if (user_param->tst == BW || user_param->tst == LAT_BY_BW) {

			ctx->scnt[i] = 0;
			ctx->ccnt[i] = 0;
			ctx->my_addr[i] = (uintptr_t)ctx->buf[i];
			if (user_param->verb != SEND)
				ctx->rem_addr[i] = rem_dest[xrc_offset + i].vaddr;
		}

		for (j = 0; j < user_param->post_list; j++) {

			ctx->sge_list[i*user_param->post_list + j].length =
				(user_param->connection_type == RawEth) ? (user_param->size - HW_CRC_ADDITION) : user_param->size;

			ctx->sge_list[i*user_param->post_list + j].lkey = ctx->mr[i]->lkey;

			if (j > 0) {

				ctx->sge_list[i*user_param->post_list +j].addr = ctx->sge_list[i*user_param->post_list + (j-1)].addr;

				if ((user_param->tst == BW || user_param->tst == LAT_BY_BW) && user_param->size <= (ctx->cycle_buffer / 2))
					increase_loc_addr(&ctx->sge_list[i*user_param->post_list +j],user_param->size,
							j-1,ctx->my_addr[i],0,ctx->cache_line_size,ctx->cycle_buffer);
			}

			ctx->wr[i*user_param->post_list + j].sg_list = &ctx->sge_list[i*user_param->post_list + j];
			ctx->wr[i*user_param->post_list + j].num_sge = MAX_SEND_SGE;
			ctx->wr[i*user_param->post_list + j].wr_id   = i;

			if (j == (user_param->post_list - 1)) {
				ctx->wr[i*user_param->post_list + j].next = NULL;
			} else {
				ctx->wr[i*user_param->post_list + j].next = &ctx->wr[i*user_param->post_list+j+1];
			}

			if ((j + 1) % user_param->cq_mod == 0) {
				ctx->wr[i*user_param->post_list + j].send_flags = IBV_SEND_SIGNALED;
				#ifdef HAVE_IBV_WR_API
				ctx->qpx[i]->wr_flags = IBV_SEND_SIGNALED;
				#endif
			} else {
				ctx->wr[i*user_param->post_list + j].send_flags = 0;
			}

			if (user_param->verb == ATOMIC) {
				ctx->wr[i*user_param->post_list + j].opcode = opcode_atomic_array[user_param->atomicType];
			}
			else {
				ctx->wr[i*user_param->post_list + j].opcode = opcode_verbs_array[user_param->verb];
			}
			if (user_param->verb == WRITE || user_param->verb == READ) {

				ctx->wr[i*user_param->post_list + j].wr.rdma.rkey = rem_dest[xrc_offset + i].rkey;

				if (j > 0) {

					ctx->wr[i*user_param->post_list + j].wr.rdma.remote_addr =
						ctx->wr[i*user_param->post_list + (j-1)].wr.rdma.remote_addr;

					if ((user_param->tst == BW || user_param->tst == LAT_BY_BW ) && user_param->size <= (ctx->cycle_buffer / 2))
						increase_rem_addr(&ctx->wr[i*user_param->post_list + j],user_param->size,
								j-1,ctx->rem_addr[i],WRITE,ctx->cache_line_size,ctx->cycle_buffer);
				}

			} else if (user_param->verb == ATOMIC) {

				ctx->wr[i*user_param->post_list + j].wr.atomic.rkey = rem_dest[xrc_offset + i].rkey;

				if (j > 0) {

					ctx->wr[i*user_param->post_list + j].wr.atomic.remote_addr =
						ctx->wr[i*user_param->post_list + j-1].wr.atomic.remote_addr;
					if (user_param->tst == BW || user_param->tst == LAT_BY_BW)
						increase_rem_addr(&ctx->wr[i*user_param->post_list + j],user_param->size,
								j-1,ctx->rem_addr[i],ATOMIC,ctx->cache_line_size,ctx->cycle_buffer);
				}

				if (user_param->atomicType == FETCH_AND_ADD)
					ctx->wr[i*user_param->post_list + j].wr.atomic.compare_add = ATOMIC_ADD_VALUE;

				else
					ctx->wr[i*user_param->post_list + j].wr.atomic.swap = ATOMIC_SWAP_VALUE;


			} else if (user_param->verb == SEND) {

				if (user_param->connection_type == UD || user_param->connection_type == SRD) {

					ctx->wr[i*user_param->post_list + j].wr.ud.ah = ctx->ah[i];
					if (user_param->work_rdma_cm) {
						remote_qpn = ctx->cma_master.nodes[i].remote_qpn;
						remote_qkey = ctx->cma_master.nodes[i].remote_qkey;
					} else {
						remote_qpn = rem_dest[xrc_offset + i].qpn;
						remote_qkey = DEF_QKEY;
					}
					ctx->wr[i*user_param->post_list + j].wr.ud.remote_qkey = remote_qkey;
					ctx->wr[i*user_param->post_list + j].wr.ud.remote_qpn = remote_qpn;
				}
			}

			if ((user_param->verb == SEND || user_param->verb == WRITE) && user_param->size <= user_param->inline_size)
				ctx->wr[i*user_param->post_list + j].send_flags |= IBV_SEND_INLINE;

			#ifdef HAVE_XRCD
			if (user_param->use_xrc)
				ctx->wr[i*user_param->post_list + j].qp_type.xrc.remote_srqn = rem_dest[xrc_offset + i].srqn;
			#endif
		}
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_set_recv_wqes(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	int			i = 0,j,k;
	int			num_of_qps = user_param->num_of_qps;
	struct ibv_recv_wr	*bad_wr_recv;
	int			size_per_qp = user_param->rx_depth;

	if((user_param->use_xrc || user_param->connection_type == DC) &&
				(user_param->duplex || user_param->tst == LAT)) {

		i = user_param->num_of_qps / 2;
		num_of_qps /= 2;
	}

	if (user_param->use_srq)
		size_per_qp /= user_param->num_of_qps;

	if (user_param->use_rss) {
		i = 1;
		num_of_qps = 1;
	}
	for (k = 0; i < user_param->num_of_qps; i++,k++) {
		if (!user_param->mr_per_qp) {
			ctx->recv_sge_list[i].addr  = (uintptr_t)ctx->buf[0] +
				(num_of_qps + k) * ctx->send_qp_buff_size;
		} else {
			ctx->recv_sge_list[i].addr  = (uintptr_t)ctx->buf[i];
		}

		if (user_param->connection_type == UD)
			ctx->recv_sge_list[i].addr += (ctx->cache_line_size - UD_ADDITION);

		ctx->recv_sge_list[i].length = SIZE(user_param->connection_type,user_param->size,1);
		ctx->recv_sge_list[i].lkey   = ctx->mr[i]->lkey;

		ctx->rwr[i].sg_list = &ctx->recv_sge_list[i];
		ctx->rwr[i].wr_id   = i;
		ctx->rwr[i].next    = NULL;
		ctx->rwr[i].num_sge	= MAX_RECV_SGE;

		ctx->rx_buffer_addr[i] = ctx->recv_sge_list[i].addr;

		for (j = 0; j < size_per_qp ; ++j) {

			if (user_param->use_srq) {

				if (ibv_post_srq_recv(ctx->srq,&ctx->rwr[i], &bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv SRQ = %d: counter=%d\n",i,j);
					return 1;
				}

			} else {

				if (ibv_post_recv(ctx->qp[i],&ctx->rwr[i],&bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv Qp = %d: counter=%d\n",i,j);
					return 1;
				}
			}

			if ((user_param->tst == BW || user_param->tst == LAT_BY_BW) && user_param->size <= (ctx->cycle_buffer / 2)) {

				increase_loc_addr(&ctx->recv_sge_list[i],
						user_param->size,
						j,
						ctx->rx_buffer_addr[i],
						user_param->connection_type,ctx->cache_line_size,ctx->cycle_buffer);
			}
		}
		ctx->recv_sge_list[i].addr = ctx->rx_buffer_addr[i];
	}
	return 0;
}

int ctx_alloc_credit(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct pingpong_dest *my_dest)
{
	int buf_size = 2*user_param->num_of_qps*sizeof(uint32_t);
	int flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
	int i;

	ALLOCATE(ctx->ctrl_buf,uint32_t,user_param->num_of_qps);
	memset(&ctx->ctrl_buf[0],0,buf_size);

	ctx->credit_buf = (uint32_t *)ctx->ctrl_buf + user_param->num_of_qps;
	ctx->credit_cnt = user_param->rx_depth/3;

	ctx->credit_mr = ibv_reg_mr(ctx->pd,ctx->ctrl_buf,buf_size,flags);
	if (!ctx->credit_mr) {
		fprintf(stderr, "Couldn't allocate MR\n");
		return FAILURE;
	}
	for (i = 0; i < user_param->num_of_qps; i++) {
		my_dest[i].rkey  = ctx->credit_mr->rkey;
		my_dest[i].vaddr = (uintptr_t)ctx->credit_buf + i*sizeof(uint32_t);
	}
	return 0;
}

/* Should be called after the remote keys have been exchanged */
int ctx_set_credit_wqes(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct pingpong_dest *rem_dest)
{
	int i;
	ALLOCATE(ctx->ctrl_wr,struct ibv_send_wr,user_param->num_of_qps);
	ALLOCATE(ctx->ctrl_sge_list,struct ibv_sge,user_param->num_of_qps);

	for (i = 0; i < user_param->num_of_qps; i++) {
		memset(&ctx->ctrl_wr[i],0,sizeof(struct ibv_send_wr));

		ctx->ctrl_sge_list[i].addr = (uintptr_t)ctx->ctrl_buf + (i*sizeof(uint32_t));
		ctx->ctrl_sge_list[i].length = sizeof(uint32_t);
		ctx->ctrl_sge_list[i].lkey = ctx->credit_mr->lkey;

		ctx->ctrl_wr[i].opcode = IBV_WR_RDMA_WRITE;
		ctx->ctrl_wr[i].sg_list = &ctx->ctrl_sge_list[i];
		ctx->ctrl_wr[i].num_sge = 1;
		ctx->ctrl_wr[i].wr_id = i;
		ctx->ctrl_wr[i].send_flags = IBV_SEND_SIGNALED;
		ctx->ctrl_wr[i].next = NULL;

		ctx->ctrl_wr[i].wr.rdma.remote_addr = rem_dest[i].vaddr;
		ctx->ctrl_wr[i].wr.rdma.rkey = rem_dest[i].rkey;
	}
	return 0;
}

static int clean_scq_credit(int send_cnt,struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	int 		i= 0, sne = 0;
	struct ibv_wc 	*swc = NULL;
	int		return_value = 0;
	if (!send_cnt)
		return 0;

	ALLOCATE(swc,struct ibv_wc,user_param->tx_depth);
	do {
		sne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth,swc);
		if (sne > 0) {
			for (i = 0; i < sne; i++) {
				if (swc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr, "Poll send CQ error status=%u qp %d\n",
							swc[i].status,(int)swc[i].wr_id);
					return_value = FAILURE;
					goto cleaning;
				}
				send_cnt--;
			}

		} else if (sne < 0) {
			fprintf(stderr, "Poll send CQ to clean credit failed ne=%d\n",sne);
			return_value = FAILURE;
			goto cleaning;
		}
	} while(send_cnt > 0);

cleaning:
	free(swc);
	return return_value;
}

/******************************************************************************
 *
 ******************************************************************************/
int perform_warm_up(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	int 			ne,index,warmindex,warmupsession;
	int 			err = 0;
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr 	*bad_exp_wr = NULL;
	#endif
	struct ibv_send_wr 	*bad_wr = NULL;
	struct ibv_wc 		wc;
	struct ibv_wc 		*wc_for_cleaning = NULL;
	int 			num_of_qps = user_param->num_of_qps;
	int			return_value = 0;

	if(user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	warmupsession = (user_param->post_list == 1) ? user_param->tx_depth : user_param->post_list;
	ALLOCATE(wc_for_cleaning,struct ibv_wc,user_param->tx_depth);

	/* Clean up the pipe */
	ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth,wc_for_cleaning);

	for (index=0 ; index < num_of_qps ; index++) {

		for (warmindex = 0 ;warmindex < warmupsession ;warmindex += user_param->post_list) {

			#ifdef HAVE_VERBS_EXP
			if (user_param->use_exp == 1)
				err = (ctx->exp_post_send_func_pointer)(ctx->qp[index],
					&ctx->exp_wr[index*user_param->post_list], &bad_exp_wr);
			else
				err = (ctx->post_send_func_pointer)(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr);
			#else
			err = ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr);
			#endif

			if (err) {
				fprintf(stderr,"Couldn't post send during warm up: qp %d scnt=%d \n",index,warmindex);
				return_value = FAILURE;
				goto cleaning;
			}
		}

		do {

			ne = ibv_poll_cq(ctx->send_cq,1,&wc);
			if (ne > 0) {

				if (wc.status != IBV_WC_SUCCESS) {
					return_value = FAILURE;
					goto cleaning;
				}

				warmindex -= user_param->post_list;

			} else if (ne < 0) {
				return_value = FAILURE;
				goto cleaning;
			}

		} while (warmindex);
	}

cleaning:
	free(wc_for_cleaning);
	return return_value;
}

/* post_send_method.
 *
 * Description :
 *
 * Does posting of work to a send queue.
 *
 * Parameters :
 *
 *	ctx         - Test Context.
 *	index       - qp index.
 *	user_param  - user_parameters struct for this test.
 *
 * Return Value : int.
 *
 */
static inline int post_send_method(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	#ifdef HAVE_IBV_WR_API
	return (*ctx->new_post_send_work_request_func_pointer)(ctx, index, user_param);
	#else
	struct ibv_send_wr 	*bad_wr = NULL;
	return ibv_post_send(ctx->qp[index], &ctx->wr[index*user_param->post_list], &bad_wr);
	#endif
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t           	totscnt = 0;
	uint64_t       	   	totccnt = 0;
	int                	i = 0;
	int                	index,ne;
	uint64_t	   	tot_iters;
	int			err = 0;
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr 	*bad_exp_wr = NULL;
	struct ibv_send_wr 	*bad_wr = NULL;
	#ifdef HAVE_ACCL_VERBS
	int pl_index;
	struct ibv_sge		*sg_l;
	#endif
	#endif
	struct ibv_wc 	   	*wc = NULL;
	int 			num_of_qps = user_param->num_of_qps;
	/* Rate Limiter*/
	int 			rate_limit_pps = 0;
	double 			gap_time = 0;	/* in usec */
	cycles_t 		gap_cycles = 0;	/* in cycles */
	cycles_t 		gap_deadline = 0;
	unsigned int 		number_of_bursts = 0;
	int 			burst_iter = 0;
	int 			is_sending_burst = 0;
	int 			cpu_mhz = 0;
	int 			return_value = 0;
	int			wc_id;
	int			send_flows_index = 0;
	uintptr_t		primary_send_addr = ctx->sge_list[0].addr;
	int			address_offset = 0;
	int			flows_burst_iter = 0;

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);

	if (user_param->test_type == DURATION) {
		duration_param=user_param;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		if (user_param->margin > 0 )
			alarm(user_param->margin);
		else
			catch_alarm(0); /* move to next state */

		user_param->iters = 0;
	}

	if (user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	/* Will be 0, in case of Duration (look at force_dependencies or in the exp above). */
	tot_iters = (uint64_t)user_param->iters*num_of_qps;

	if (user_param->test_type == DURATION && user_param->state != START_STATE && user_param->margin > 0) {
		fprintf(stderr, "Failed: margin is not long enough (taking samples before warmup ends)\n");
		fprintf(stderr, "Please increase margin or decrease tx_depth\n");
		return_value = FAILURE;
		goto cleaning;
	}

	if (user_param->test_type == ITERATIONS && user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	/* If using rate limiter, calculate gap time between bursts */
	if (user_param->rate_limit_type == SW_RATE_LIMIT ) {
		/* Calculate rate limit in pps */
		switch (user_param->rate_units) {
			case MEGA_BYTE_PS:
				rate_limit_pps = ((double)(user_param->rate_limit) / user_param->size) * 1048576;
				break;
			case GIGA_BIT_PS:
				rate_limit_pps = ((double)(user_param->rate_limit) / (user_param->size * 8)) * 1000000000;
				break;
			case PACKET_PS:
				rate_limit_pps = user_param->rate_limit;
				break;
			default:
				fprintf(stderr, " Failed: Unknown rate limit units\n");
				return_value = FAILURE;
				goto cleaning;
		}
		cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
		if (cpu_mhz <= 0) {
			fprintf(stderr, "Failed: couldn't acquire cpu frequency for rate limiter.\n");
		}
		number_of_bursts = rate_limit_pps / user_param->burst_size;
		gap_time = 1000000 * (1.0 / number_of_bursts);
		gap_cycles = cpu_mhz * gap_time;
	}

	/* main loop for posting */
	while (totscnt < tot_iters  || totccnt < tot_iters ||
		(user_param->test_type == DURATION && user_param->state != END_STATE) ) {

		/* main loop to run over all the qps and post each time n messages */
		for (index =0 ; index < num_of_qps ; index++) {
			if (user_param->rate_limit_type == SW_RATE_LIMIT && is_sending_burst == 0) {
				if (gap_deadline > get_cycles()) {
					/* Go right to cq polling until gap time is over. */
					continue;
				}
				gap_deadline = get_cycles() + gap_cycles;
				is_sending_burst = 1;
				burst_iter = 0;
			}
			while ((ctx->scnt[index] < user_param->iters || user_param->test_type == DURATION) &&
					(ctx->scnt[index] - ctx->ccnt[index] + user_param->post_list) <= (user_param->tx_depth) &&
					!((user_param->rate_limit_type == SW_RATE_LIMIT ) && is_sending_burst == 0)) {

				if (ctx->send_rcredit) {
					uint32_t swindow = ctx->scnt[index] + user_param->post_list - ctx->credit_buf[index];
					if (swindow >= user_param->rx_depth)
						break;
				}
				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)
					&& !(ctx->scnt[index] == (user_param->iters - 1) && user_param->test_type == ITERATIONS)) {

					#ifdef HAVE_VERBS_EXP
					#ifdef HAVE_ACCL_VERBS
					if (user_param->verb_type == ACCL_INTF)
						ctx->exp_wr[index].exp_send_flags &= ~IBV_EXP_QP_BURST_SIGNALED;
					else {
					#endif
						if (user_param->use_exp == 1)
							ctx->exp_wr[index].exp_send_flags &= ~IBV_EXP_SEND_SIGNALED;
						else
					#endif
						#ifdef HAVE_IBV_WR_API
						ctx->qpx[index]->wr_flags &= ~IBV_SEND_SIGNALED;
						#else
						ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;
						#endif

					#ifdef HAVE_ACCL_VERBS
					}
					#endif
				}

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && user_param->state == END_STATE)
					break;

				#ifdef HAVE_VERBS_EXP
				#ifdef HAVE_ACCL_VERBS
				if (user_param->verb_type == ACCL_INTF) {
					for (pl_index = 0; pl_index < user_param->post_list; pl_index++) {
						sg_l = ctx->exp_wr[index*user_param->post_list + pl_index].sg_list;
						ctx->qp_burst_family[index]->send_pending(ctx->qp[index], sg_l->addr, sg_l->length, sg_l->lkey,
											ctx->exp_wr[index*user_param->post_list + pl_index].exp_send_flags);
					}
					ctx->qp_burst_family[index]->send_flush(ctx->qp[index]);
				} else {
				#endif
					if (user_param->use_exp == 1) {
						err = (ctx->exp_post_send_func_pointer)(ctx->qp[index],
						&ctx->exp_wr[index*user_param->post_list],&bad_exp_wr);
					}
					else {
						err = (ctx->post_send_func_pointer)(ctx->qp[index],
							&ctx->wr[index*user_param->post_list],&bad_wr);
					}
					if (err) {
						fprintf(stderr,"Couldn't post send: qp %d scnt=%lu \n",index,ctx->scnt[index]);
						return_value = FAILURE;
						goto cleaning;
					}

				#ifdef HAVE_ACCL_VERBS
				}
				#endif
				#else

				err = post_send_method(ctx, index, user_param);
				if (err) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%lu \n",index,ctx->scnt[index]);
					return_value = FAILURE;
					goto cleaning;
				}
				#endif

				/* if we have more than single flow and the burst iter is the last one */
				if (user_param->flows != DEF_FLOWS) {
					if (++flows_burst_iter == user_param->flows_burst) {
						flows_burst_iter = 0;
						/* inc the send_flows_index and update the address */
						if (++send_flows_index == user_param->flows)
							send_flows_index = 0;
						address_offset = send_flows_index * ctx->flow_buff_size;
						ctx->sge_list[0].addr = primary_send_addr + address_offset;
					}
				}

				/* in multiple flow scenarios we will go to next cycle buffer address in the main buffer*/
				if (user_param->post_list == 1 && user_param->size <= (ctx->cycle_buffer / 2)) {
					#ifdef HAVE_VERBS_EXP
					if (user_param->use_exp == 1)
						increase_loc_addr(ctx->exp_wr[index].sg_list,user_param->size,
								ctx->scnt[index], ctx->my_addr[index] + address_offset, 0,
									ctx->cache_line_size, ctx->cycle_buffer);
					else
					#endif
						increase_loc_addr(ctx->wr[index].sg_list,user_param->size, ctx->scnt[index],
								ctx->my_addr[index] + address_offset , 0, ctx->cache_line_size,
								ctx->cycle_buffer);

					if (user_param->verb != SEND) {
						#ifdef HAVE_VERBS_EXP
						if (user_param->use_exp == 1)
							increase_exp_rem_addr(&ctx->exp_wr[index], user_param->size,
									ctx->scnt[index], ctx->rem_addr[index], user_param->verb,
									ctx->cache_line_size, ctx->cycle_buffer);
						else
						#endif
							increase_rem_addr(&ctx->wr[index], user_param->size,
									ctx->scnt[index], ctx->rem_addr[index], user_param->verb,
									ctx->cache_line_size, ctx->cycle_buffer);
					}
				}

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				/* ask for completion on this wr */
				if (user_param->post_list == 1 &&
						(ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 ||
							(user_param->test_type == ITERATIONS && ctx->scnt[index] == user_param->iters - 1))) {
					#ifdef HAVE_VERBS_EXP
					#ifdef HAVE_ACCL_VERBS
					if (user_param->verb_type == ACCL_INTF)
						ctx->exp_wr[index].exp_send_flags |= IBV_EXP_QP_BURST_SIGNALED;
					else {
					#endif
						if (user_param->use_exp == 1) {
							ctx->exp_wr[index].exp_send_flags |= IBV_EXP_SEND_SIGNALED;
						}
						else
					#endif
						#ifdef HAVE_IBV_WR_API
						ctx->qpx[index]->wr_flags |= IBV_SEND_SIGNALED;
						#else
						ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
						#endif
					#ifdef HAVE_ACCL_VERBS
					}
					#endif
				}

				/* Check if a full burst was sent. */
				if (user_param->rate_limit_type == SW_RATE_LIMIT) {
					burst_iter += user_param->post_list;
					if (burst_iter >= user_param->burst_size) {
						is_sending_burst = 0;
					}
				}
			}
		}
		if (totccnt < tot_iters || (user_param->test_type == DURATION &&  totccnt < totscnt)) {
				if (user_param->use_event) {
					if (ctx_notify_events(ctx->channel)) {
						fprintf(stderr, "Couldn't request CQ notification\n");
						return_value = FAILURE;
						goto cleaning;
					}
				}

				#ifdef HAVE_ACCL_VERBS
				if (user_param->verb_type == ACCL_INTF)
					ne = ctx->send_cq_family->poll_cnt(ctx->send_cq, CTX_POLL_BATCH);
				else
				#endif
					ne = ibv_poll_cq(ctx->send_cq, CTX_POLL_BATCH, wc);
				if (ne > 0) {
					for (i = 0; i < ne; i++) {
						wc_id = (user_param->verb_type == ACCL_INTF) ?
							0 : (int)wc[i].wr_id;

						if (user_param->verb_type != ACCL_INTF) {
							if (wc[i].status != IBV_WC_SUCCESS) {
								NOTIFY_COMP_ERROR_SEND(wc[i],totscnt,totccnt);
								return_value = FAILURE;
								goto cleaning;
							}
						}

						ctx->ccnt[wc_id] += user_param->cq_mod;
						totccnt += user_param->cq_mod;
						if (user_param->noPeak == OFF) {
							if (totccnt > tot_iters)
								user_param->tcompleted[user_param->iters*num_of_qps - 1] = get_cycles();
							else
								user_param->tcompleted[totccnt-1] = get_cycles();
						}

						if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE) {
							if (user_param->report_per_port) {
								user_param->iters_per_port[user_param->port_by_qp[wc_id]] += user_param->cq_mod;
							}
							user_param->iters += user_param->cq_mod;
						}
					}

				} else if (ne < 0) {
					fprintf(stderr, "poll CQ failed %d\n",ne);
					return_value = FAILURE;
					goto cleaning;
					}
		}
	}
	if (user_param->noPeak == ON && user_param->test_type == ITERATIONS)
		user_param->tcompleted[0] = get_cycles();

cleaning:

	free(wc);
	return return_value;
}

/******************************************************************************
 *
 ******************************************************************************/
static inline void set_on_first_rx_packet(struct perftest_parameters *user_param)
{
	if (user_param->test_type == DURATION) {

		duration_param=user_param;
		user_param->iters=0;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		if (user_param->margin > 0)
			alarm(user_param->margin);
		else
			catch_alarm(0);

	} else if (user_param->tst == BW) {
		user_param->tposted[0] = get_cycles();
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw_server(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	uint64_t		rcnt = 0;
	int 			ne = 0;
	int			i;
	uint64_t		tot_iters;
	uint64_t                *rcnt_for_qp = NULL;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr  	*bad_wr_recv = NULL;
	struct ibv_wc 		*swc = NULL;
	long 			*scredit_for_qp = NULL;
	int 			tot_scredit = 0;
	int 			firstRx = 1;
	int 			size_per_qp = (user_param->use_srq) ?
					user_param->rx_depth/user_param->num_of_qps : user_param->rx_depth;
	int 			return_value = 0;
	int			wc_id;
	int			recv_flows_index = 0;
	uintptr_t		primary_recv_addr = ctx->recv_sge_list[0].addr;
	int			recv_flows_burst = 0;
	int			address_flows_offset =0;

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);
	ALLOCATE(swc ,struct ibv_wc ,user_param->tx_depth);

	ALLOCATE(rcnt_for_qp,uint64_t,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	ALLOCATE(scredit_for_qp,long,user_param->num_of_qps);
	memset(scredit_for_qp,0,sizeof(long)*user_param->num_of_qps);

	if (user_param->use_rss)
		tot_iters = (uint64_t)user_param->iters*(user_param->num_of_qps-1);
	else
		tot_iters = (uint64_t)user_param->iters*user_param->num_of_qps;

	if (user_param->test_type == ITERATIONS) {
		check_alive_data.is_events = user_param->use_event;
		signal(SIGALRM, check_alive);
		alarm(60);
	}

	check_alive_data.g_total_iters = tot_iters;

	while (rcnt < tot_iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr ," Failed to notify events to CQ");
				return_value = FAILURE;
				goto cleaning;
			}
		}

		do {
			if (user_param->test_type == DURATION && user_param->state == END_STATE)
				break;

			#ifdef HAVE_ACCL_VERBS
			if (user_param->verb_type == ACCL_INTF)
				ne = ctx->recv_cq_family->poll_cnt(ctx->recv_cq, CTX_POLL_BATCH);
			else {
			#endif
				if (user_param->connection_type == DC)
					ne = ibv_poll_cq(ctx->send_cq,CTX_POLL_BATCH,wc);
				else
					ne = ibv_poll_cq(ctx->recv_cq,CTX_POLL_BATCH,wc);
			#ifdef HAVE_ACCL_VERBS
			}
			#endif

			if (ne > 0) {
				if (firstRx) {
					set_on_first_rx_packet(user_param);
					firstRx = 0;
				}

				for (i = 0; i < ne; i++) {
					wc_id = (user_param->verb_type == ACCL_INTF) ?
						0 : (int)wc[i].wr_id;

					if (user_param->verb_type != ACCL_INTF) {
						if (wc[i].status != IBV_WC_SUCCESS) {

							NOTIFY_COMP_ERROR_RECV(wc[i],rcnt_for_qp[wc_id]);
							return_value = FAILURE;
							goto cleaning;
						}
					}
					rcnt_for_qp[wc_id]++;
					rcnt++;
					check_alive_data.current_totrcnt = rcnt;

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE) {
						if (user_param->report_per_port) {
							user_param->iters_per_port[user_param->port_by_qp[wc_id]]++;
						}
						user_param->iters++;
					}
					if (user_param->test_type==DURATION || rcnt_for_qp[wc_id] + size_per_qp <= user_param->iters) {
						#ifdef HAVE_ACCL_VERBS
						if (user_param->verb_type == ACCL_INTF) {
							if (ctx->qp_burst_family[wc_id]->recv_burst(ctx->qp[wc_id], ctx->rwr[wc_id].sg_list, 1)) {
								fprintf(stderr, "Couldn't post recv burst (accelerated verbs).\n");
								return_value = FAILURE;
								goto cleaning;
							}
						} else {
						#endif
							if (user_param->use_srq) {
								if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc_id],&bad_wr_recv)) {
									fprintf(stderr, "Couldn't post recv SRQ. QP = %d: counter=%lu\n", wc_id,rcnt);
									return_value = FAILURE;
									goto cleaning;
								}

							} else {
								if (ibv_post_recv(ctx->qp[wc_id],&ctx->rwr[wc_id],&bad_wr_recv)) {
									fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%ld\n",wc_id,rcnt_for_qp[wc_id]);
									return_value = 15;
									goto cleaning;
								}

							}
							if (user_param->flows != DEF_FLOWS) {
								if (++recv_flows_burst == user_param->flows_burst) {
									recv_flows_burst = 0;
									if (++recv_flows_index == user_param->flows)
										recv_flows_index = 0;
									address_flows_offset = recv_flows_index * ctx->cycle_buffer;
									ctx->recv_sge_list[0].addr = primary_recv_addr + address_flows_offset;
								}
							}
						#ifdef HAVE_ACCL_VERBS
						}
						#endif
						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (ctx->cycle_buffer / 2)) {
							increase_loc_addr(ctx->rwr[wc_id].sg_list,
									user_param->size,
									rcnt_for_qp[wc_id] + size_per_qp,
									ctx->rx_buffer_addr[wc_id] + address_flows_offset,
									user_param->connection_type,ctx->cache_line_size,ctx->cycle_buffer);
						}
					}

					if (ctx->send_rcredit) {
						int credit_cnt = rcnt_for_qp[wc_id]%user_param->rx_depth;

						if (credit_cnt%ctx->credit_cnt == 0) {
							struct ibv_send_wr *bad_wr = NULL;
							int sne = 0, j = 0;
							ctx->ctrl_buf[wc_id] = rcnt_for_qp[wc_id];

							while (scredit_for_qp[wc_id] == user_param->tx_depth) {
								sne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth,swc);
								if (sne > 0) {
									for (j = 0; j < sne; j++) {
										if (swc[j].status != IBV_WC_SUCCESS) {
											fprintf(stderr, "Poll send CQ error status=%u qp %d credit=%lu scredit=%lu\n",
													swc[j].status,(int)swc[j].wr_id,
													rcnt_for_qp[swc[j].wr_id],scredit_for_qp[swc[j].wr_id]);
											return_value = FAILURE;
											goto cleaning;
										}
										scredit_for_qp[swc[j].wr_id]--;
										tot_scredit--;
									}
								} else if (sne < 0) {
									fprintf(stderr, "Poll send CQ failed ne=%d\n",sne);
									return_value = FAILURE;
									goto cleaning;
								}
							}
							if (ibv_post_send(ctx->qp[wc_id],&ctx->ctrl_wr[wc_id],&bad_wr)) {
								fprintf(stderr,"Couldn't post send qp %d credit = %lu\n",
										wc_id,rcnt_for_qp[wc_id]);
								return_value = FAILURE;
								goto cleaning;
							}
							scredit_for_qp[wc_id]++;
							tot_scredit++;
						}
					}
				}
			}

		} while (ne > 0);

		if (ne < 0) {
			fprintf(stderr, "Poll Receive CQ failed %d\n", ne);
			return_value = FAILURE;
			goto cleaning;
		}
		else if (ne == 0) {
			if (check_alive_data.to_exit) {
				user_param->check_alive_exited = 1;
				return_value = FAILURE;
				goto cleaning;
			}
		}

	}
	if (user_param->test_type == ITERATIONS)
		user_param->tcompleted[0] = get_cycles();

cleaning:
	if (ctx->send_rcredit) {
		if (clean_scq_credit(tot_scredit, ctx, user_param))
			return_value = FAILURE;
	}

	check_alive_data.last_totrcnt=0;
	free(wc);
	free(rcnt_for_qp);
	free(swc);
	free(scredit_for_qp);

	return return_value;
}
/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw_infinitely(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t		totscnt = 0;
	uint64_t		totccnt = 0;
	int 			i = 0;
	int 			index = 0,ne;
	int 			err = 0;
	int			wc_id;
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr 	*bad_exp_wr = NULL;
	struct ibv_send_wr 	*bad_wr = NULL;
	#endif
	uint64_t		*scnt_for_qp = NULL;
	struct ibv_wc 		*wc = NULL;
	int 			num_of_qps = user_param->num_of_qps;
	int 			return_value = 0;
	int 			single_thread_handler;

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);
	ALLOCATE(scnt_for_qp,uint64_t,user_param->num_of_qps);
	memset(scnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	duration_param=user_param;
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	single_thread_handler = pthread_sigmask(SIG_BLOCK, &set, NULL);
	if (single_thread_handler != 0){
		printf("error when try to mask alram for signal to thread\n");
		return FAILURE;
	}

	pthread_t print_thread;
	if (pthread_create(&print_thread, NULL, &handle_signal_print_thread,(void *)&set) != 0){
		printf("Fail to create thread \n");
		return FAILURE;
	}

	alarm(user_param->duration);
	user_param->iters = 0;

	/* Will be 0, in case of Duration (look at force_dependencies or in the exp above) */
	if (user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	user_param->tposted[0] = get_cycles();

	/* main loop for posting */
	while (1) {
	/* main loop to run over all the qps and post each time n messages */
		for (index =0 ; index < num_of_qps ; index++) {

			while ((ctx->scnt[index] - ctx->ccnt[index] + user_param->post_list) <= user_param->tx_depth) {
				if (ctx->send_rcredit) {
					uint32_t swindow = scnt_for_qp[index] + user_param->post_list - ctx->credit_buf[index];
					if (swindow >= user_param->rx_depth)
						break;
				}

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)) {

					#ifdef HAVE_VERBS_EXP
					#ifdef HAVE_ACCL_VERBS
					if (user_param->verb_type == ACCL_INTF)
						ctx->exp_wr[index].exp_send_flags &= ~IBV_EXP_QP_BURST_SIGNALED;
					else {
					#endif
						if (user_param->use_exp == 1)
							ctx->exp_wr[index].exp_send_flags &= ~IBV_EXP_SEND_SIGNALED;
						else
					#endif
						#ifdef HAVE_IBV_WR_API
						ctx->qpx[index]->wr_flags &= ~IBV_SEND_SIGNALED;
						#else
						ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;
						#endif
					#ifdef HAVE_ACCL_VERBS
					}
					#endif
				}

				#ifdef HAVE_VERBS_EXP
				if (user_param->use_exp == 1)
					err = (ctx->exp_post_send_func_pointer)(ctx->qp[index],&ctx->exp_wr[index*user_param->post_list],&bad_exp_wr);
				else
					err = (ctx->post_send_func_pointer)(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr);
				#else
				err = post_send_method(ctx, index, user_param);
				#endif
				if (err) {
					fprintf(stderr,"Couldn't post send: %d scnt=%lu \n",index,ctx->scnt[index]);
					return_value = FAILURE;
					goto cleaning;
				}
				ctx->scnt[index] += user_param->post_list;
				scnt_for_qp[index] += user_param->post_list;
				totscnt += user_param->post_list;

				/* ask for completion on this wr */
				if (user_param->post_list == 1 &&
						(ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 ||
							(user_param->test_type == ITERATIONS && ctx->scnt[index] == user_param->iters - 1))) {
					#ifdef HAVE_VERBS_EXP
					#ifdef HAVE_ACCL_VERBS
					if (user_param->verb_type == ACCL_INTF)
						ctx->exp_wr[index].exp_send_flags |= IBV_EXP_QP_BURST_SIGNALED;
					else {
					#endif
						if (user_param->use_exp == 1)
							ctx->exp_wr[index].exp_send_flags |= IBV_EXP_SEND_SIGNALED;
						else
					#endif
						#ifdef HAVE_IBV_WR_API
						ctx->qpx[index]->wr_flags |= IBV_SEND_SIGNALED;
						#else
						ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
						#endif
					#ifdef HAVE_ACCL_VERBS
					}
					#endif
				}
			}
		}
		if (totccnt < totscnt) {
			ne = ibv_poll_cq(ctx->send_cq,CTX_POLL_BATCH,wc);

			if (ne > 0) {

				for (i = 0; i < ne; i++) {
					if (wc[i].status != IBV_WC_SUCCESS) {
						NOTIFY_COMP_ERROR_SEND(wc[i],ctx->scnt[(int)wc[i].wr_id],ctx->scnt[(int)wc[i].wr_id]);
						return_value = FAILURE;
						goto cleaning;
					}
					wc_id = (user_param->verb_type == ACCL_INTF) ?
							0 : (int)wc[i].wr_id;
					user_param->iters += user_param->cq_mod;
					totccnt += user_param->cq_mod;
					ctx->ccnt[wc_id] += user_param->cq_mod;
				}

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n",ne);
				return_value = FAILURE;
				goto cleaning;
			}
		}
	}
cleaning:
	free(scnt_for_qp);
	free(wc);
	return return_value;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw_infinitely_server(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	int 			i,ne;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_wc 		*swc         = NULL;
	struct ibv_recv_wr 	*bad_wr_recv = NULL;
	uint64_t                *rcnt_for_qp = NULL;
	uint64_t                *ccnt_for_qp = NULL;
	int                     *scredit_for_qp = NULL;
	int 			return_value = 0;

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);
	ALLOCATE(swc ,struct ibv_wc ,user_param->tx_depth);

	ALLOCATE(rcnt_for_qp,uint64_t,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	ALLOCATE(ccnt_for_qp,uint64_t,user_param->num_of_qps);
	memset(ccnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	ALLOCATE(scredit_for_qp,int,user_param->num_of_qps);
	memset(scredit_for_qp,0,sizeof(int)*user_param->num_of_qps);

	while (1) {

		ne = ibv_poll_cq(ctx->recv_cq,CTX_POLL_BATCH,wc);

		if (ne > 0) {

			for (i = 0; i < ne; i++) {

				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr,"A completion with Error in run_infinitely_bw_server function");
					return_value = FAILURE;
					goto cleaning;
				}

				if (user_param->use_srq) {

					if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv SRQ. QP = %d:\n",(int)wc[i].wr_id);
						return_value = FAILURE;
						goto cleaning;
					}

				} else {

					if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv Qp=%d\n",(int)wc[i].wr_id);
						return_value = 15;
						goto cleaning;
					}
					if (ctx->send_rcredit) {
						rcnt_for_qp[wc[i].wr_id]++;
						scredit_for_qp[wc[i].wr_id]++;

						if (scredit_for_qp[wc[i].wr_id] == ctx->credit_cnt) {
							struct ibv_send_wr *bad_wr = NULL;
							ctx->ctrl_buf[wc[i].wr_id] = rcnt_for_qp[wc[i].wr_id];

							while (ccnt_for_qp[wc[i].wr_id] == user_param->tx_depth) {
								int sne, j = 0;

								sne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth,swc);
								if (sne > 0) {
									for (j = 0; j < sne; j++) {
										if (swc[j].status != IBV_WC_SUCCESS) {
											fprintf(stderr, "Poll send CQ error status=%u qp %d credit=%lu scredit=%lu\n",
													swc[j].status,(int)swc[j].wr_id,
													rcnt_for_qp[swc[j].wr_id],ccnt_for_qp[swc[j].wr_id]);
											return_value = FAILURE;
											goto cleaning;
										}
										ccnt_for_qp[swc[j].wr_id]--;
									}

								} else if (sne < 0) {
									fprintf(stderr, "Poll send CQ failed ne=%d\n",sne);
									return_value = FAILURE;
									goto cleaning;
								}
							}
							if (ibv_post_send(ctx->qp[wc[i].wr_id],&ctx->ctrl_wr[wc[i].wr_id],&bad_wr)) {
								fprintf(stderr,"Couldn't post send qp %d credit=%lu\n",
										(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
								return_value = FAILURE;
								goto cleaning;
							}
							ccnt_for_qp[wc[i].wr_id]++;
							scredit_for_qp[wc[i].wr_id] = 0;
						}
					}
				}
			}

		} else if (ne < 0) {
			fprintf(stderr, "Poll Receive CQ failed %d\n", ne);
			return_value = FAILURE;
			goto cleaning;
		}
	}

cleaning:
	free(wc);
	free(swc);
	free(rcnt_for_qp);
	free(ccnt_for_qp);
	free(scredit_for_qp);
	return return_value;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bi(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)  {

	uint64_t 		totscnt    = 0;
	uint64_t 		totccnt    = 0;
	uint64_t 		totrcnt    = 0;
	int 			i,index      = 0;
	int 			ne = 0;
	int 			err = 0;
	uint64_t 		*rcnt_for_qp = NULL;
	uint64_t 		tot_iters = 0;
	uint64_t 		iters = 0;
	int 			tot_scredit = 0;
	int 			*scredit_for_qp = NULL;
	struct ibv_wc 		*wc = NULL;
	struct ibv_wc 		*wc_tx = NULL;
	struct ibv_recv_wr 	*bad_wr_recv = NULL;
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr 	*bad_exp_wr      = NULL;
	struct ibv_send_wr 	*bad_wr      = NULL;
	#endif
	int 			num_of_qps = user_param->num_of_qps;
	/* This is to ensure SERVER will not start to send packets before CLIENT start the test. */
	int 			before_first_rx = ON;
	int 			size_per_qp = (user_param->use_srq) ? user_param->rx_depth/user_param->num_of_qps : user_param->rx_depth;
	int 			return_value = 0;

	ALLOCATE(wc_tx,struct ibv_wc,CTX_POLL_BATCH);
	ALLOCATE(rcnt_for_qp,uint64_t,user_param->num_of_qps);
	ALLOCATE(scredit_for_qp,int,user_param->num_of_qps);
	ALLOCATE(wc,struct ibv_wc,user_param->rx_depth);

	memset(rcnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);
	memset(scredit_for_qp,0,sizeof(int)*user_param->num_of_qps);

	if (user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	/* This is a very important point. Since this function do RX and TX
	   in the same time, we need to give some priority to RX to avoid
	   deadlock in UC/UD test scenarios (Recv WQEs depleted due to fast TX) */
	if (user_param->machine == CLIENT) {

		before_first_rx = OFF;
		if (user_param->test_type == DURATION) {
			duration_param=user_param;
			user_param->iters=0;
			duration_param->state = START_STATE;
			signal(SIGALRM, catch_alarm);

			if (user_param->margin > 0 )
				alarm(user_param->margin);
			else
				catch_alarm(0); /* move to next state */
		}
	}

	if (user_param->test_type == ITERATIONS) {
		check_alive_data.is_events = user_param->use_event;
		signal(SIGALRM, check_alive);
		alarm(60);
	}


	if(user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	tot_iters = (uint64_t)user_param->iters*num_of_qps;
	iters=user_param->iters;
	check_alive_data.g_total_iters = tot_iters;

	while ((user_param->test_type == DURATION && user_param->state != END_STATE) ||
							totccnt < tot_iters || totrcnt < tot_iters ) {

		for (index=0; index < num_of_qps; index++) {
			while (before_first_rx == OFF && (ctx->scnt[index] < iters || user_param->test_type == DURATION) &&
					((ctx->scnt[index] + scredit_for_qp[index] - ctx->ccnt[index]) < user_param->tx_depth)) {
				if (ctx->send_rcredit) {
					uint32_t swindow = ctx->scnt[index] + user_param->post_list - ctx->credit_buf[index];
					if (swindow >= user_param->rx_depth)
						break;
				}
				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)
					&& !(ctx->scnt[index] == (user_param->iters - 1) && user_param->test_type == ITERATIONS)) {
					#ifdef HAVE_VERBS_EXP
					if (user_param->use_exp ==1)
						ctx->exp_wr[index].exp_send_flags &= ~IBV_EXP_SEND_SIGNALED;
					else
					#endif
						#ifdef HAVE_IBV_WR_API
						ctx->qpx[index]->wr_flags &= ~IBV_SEND_SIGNALED;
						#else
						ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;
						#endif
				}
				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && duration_param->state == END_STATE)
					break;

				#ifdef HAVE_VERBS_EXP
				if (user_param->use_exp == 1)
					err = (ctx->exp_post_send_func_pointer)(ctx->qp[index],
						&ctx->exp_wr[index*user_param->post_list],&bad_exp_wr);
				else
					err = (ctx->post_send_func_pointer)(ctx->qp[index],
						&ctx->wr[index*user_param->post_list],&bad_wr);
				#else
				err = post_send_method(ctx, index, user_param);
				#endif
				if (err) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%lu \n",index,ctx->scnt[index]);
					return_value = FAILURE;
					goto cleaning;
				}

				if (user_param->post_list == 1 && user_param->size <= (ctx->cycle_buffer / 2)) {
					#ifdef HAVE_VERBS_EXP
					if (user_param->use_exp == 1)
						increase_loc_addr(ctx->exp_wr[index].sg_list,user_param->size,ctx->scnt[index],
								ctx->my_addr[index],0,ctx->cache_line_size,ctx->cycle_buffer);
					else
					#endif
						increase_loc_addr(ctx->wr[index].sg_list,user_param->size,ctx->scnt[index],
								ctx->my_addr[index],0,ctx->cache_line_size,ctx->cycle_buffer);
				}

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->post_list == 1 &&
					(ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 ||
						(user_param->test_type == ITERATIONS && ctx->scnt[index] == iters-1))) {

					#ifdef HAVE_VERBS_EXP
					if (user_param->use_exp == 1)
						ctx->exp_wr[index].exp_send_flags |= IBV_EXP_SEND_SIGNALED;
					else
					#endif
						#ifdef HAVE_IBV_WR_API
						ctx->qpx[index]->wr_flags |= IBV_SEND_SIGNALED;
						#else
						ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
						#endif
				}
			}
		}
		if (user_param->use_event) {

			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr,"Failed to notify events to CQ");
				return_value = FAILURE;
				goto cleaning;
			}
		}

		ne = ibv_poll_cq(ctx->recv_cq,user_param->rx_depth,wc);
		if (ne > 0) {

			if (user_param->machine == SERVER && before_first_rx == ON) {
				before_first_rx = OFF;
				if (user_param->test_type == DURATION) {
					duration_param=user_param;
					user_param->iters=0;
					duration_param->state = START_STATE;
					signal(SIGALRM, catch_alarm);
					if (user_param->margin > 0 )
						alarm(user_param->margin);
					else
						catch_alarm(0); /* move to next state */
				}
			}

			for (i = 0; i < ne; i++) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_RECV(wc[i],totrcnt);
					return_value = FAILURE;
					goto cleaning;
				}

				rcnt_for_qp[wc[i].wr_id]++;
				totrcnt++;
				check_alive_data.current_totrcnt = totrcnt;

				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE) {
					if (user_param->report_per_port) {
						user_param->iters_per_port[user_param->port_by_qp[(int)wc[i].wr_id]]++;
					}
					user_param->iters++;
				}

				if (user_param->test_type==DURATION || rcnt_for_qp[wc[i].wr_id] + size_per_qp <= user_param->iters) {
					if (user_param->use_srq) {
						if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv SRQ. QP = %d: counter=%d\n",(int)wc[i].wr_id,(int)totrcnt);
							return_value = FAILURE;
							goto cleaning;
						}

					} else {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%lu\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
							return_value = 15;
							goto cleaning;
						}
					}

					if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (ctx->cycle_buffer / 2)) {
						increase_loc_addr(ctx->rwr[wc[i].wr_id].sg_list,
								user_param->size,
								rcnt_for_qp[wc[i].wr_id] + size_per_qp -1,
								ctx->rx_buffer_addr[wc[i].wr_id],user_param->connection_type,
								ctx->cache_line_size,ctx->cycle_buffer);
					}
				}
				if (ctx->send_rcredit) {
					int credit_cnt = rcnt_for_qp[wc[i].wr_id]%user_param->rx_depth;

					if (credit_cnt%ctx->credit_cnt == 0) {
						int sne = 0;
						struct ibv_wc credit_wc;
						struct ibv_send_wr *bad_wr = NULL;
						ctx->ctrl_buf[wc[i].wr_id] = rcnt_for_qp[wc[i].wr_id];

						while ((ctx->scnt[wc[i].wr_id] + scredit_for_qp[wc[i].wr_id] - ctx->ccnt[wc[i].wr_id]) >= user_param->tx_depth) {
							sne = ibv_poll_cq(ctx->send_cq, 1, &credit_wc);
							if (sne > 0) {
								if (credit_wc.status != IBV_WC_SUCCESS) {
									fprintf(stderr, "Poll send CQ error status=%u qp %d credit=%lu scredit=%d\n",
											credit_wc.status,(int)credit_wc.wr_id,
											rcnt_for_qp[credit_wc.wr_id],scredit_for_qp[credit_wc.wr_id]);
									return_value = FAILURE;
									goto cleaning;
								}

								if (credit_wc.opcode == IBV_WC_RDMA_WRITE) {
									scredit_for_qp[credit_wc.wr_id]--;
									tot_scredit--;
								} else  {
									totccnt += user_param->cq_mod;
									ctx->ccnt[(int)credit_wc.wr_id] += user_param->cq_mod;

									if (user_param->noPeak == OFF) {
										if ((user_param->test_type == ITERATIONS && (totccnt >= tot_iters - 1)))
											user_param->tcompleted[tot_iters - 1] = get_cycles();
										else
											user_param->tcompleted[totccnt-1] = get_cycles();
									}
									if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
										user_param->iters += user_param->cq_mod;
								}
							} else if (sne < 0) {
								fprintf(stderr, "Poll send CQ ne=%d\n",sne);
								return_value = FAILURE;
								goto cleaning;
							}
						}
						if (ibv_post_send(ctx->qp[wc[i].wr_id],&ctx->ctrl_wr[wc[i].wr_id],&bad_wr)) {
							fprintf(stderr,"Couldn't post send: qp%lu credit=%lu\n",wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
							return_value = FAILURE;
							goto cleaning;
						}
						scredit_for_qp[wc[i].wr_id]++;
						tot_scredit++;
					}
				}
			}

		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return_value = FAILURE;
			goto cleaning;
		}
		else if (ne == 0) {
			if (check_alive_data.to_exit) {
				user_param->check_alive_exited = 1;
				return_value = FAILURE;
				goto cleaning;
			}
		}

		ne = ibv_poll_cq(ctx->send_cq,CTX_POLL_BATCH,wc_tx);

		if (ne > 0) {
			for (i = 0; i < ne; i++) {
				if (wc_tx[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_SEND(wc_tx[i],totscnt,totccnt);
					return_value = FAILURE;
					goto cleaning;
				}

				if (wc_tx[i].opcode == IBV_WC_RDMA_WRITE) {
					if (!ctx->send_rcredit) {
						fprintf(stderr, "Polled RDMA_WRITE completion without recv credit request\n");
						return_value = FAILURE;
						goto cleaning;
					}
					scredit_for_qp[wc_tx[i].wr_id]--;
					tot_scredit--;
				} else  {
					totccnt += user_param->cq_mod;
					ctx->ccnt[(int)wc_tx[i].wr_id] += user_param->cq_mod;

					if (user_param->noPeak == OFF) {

						if ((user_param->test_type == ITERATIONS && (totccnt >= tot_iters - 1)))
							user_param->tcompleted[tot_iters - 1] = get_cycles();
						else
							user_param->tcompleted[totccnt-1] = get_cycles();
					}

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE) {
						if (user_param->report_per_port) {
							user_param->iters_per_port[user_param->port_by_qp[(int)wc[i].wr_id]] += user_param->cq_mod;
						}
						user_param->iters += user_param->cq_mod;
					}
				}
			}

		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return_value = FAILURE;
			goto cleaning;
		}
	}

	if (user_param->noPeak == ON && user_param->test_type == ITERATIONS) {
		user_param->tcompleted[0] = get_cycles();
	}

	if (ctx->send_rcredit) {
		if (clean_scq_credit(tot_scredit, ctx, user_param)) {
			return_value = FAILURE;
			goto cleaning;
		}
	}

cleaning:
	check_alive_data.last_totrcnt=0;
	free(rcnt_for_qp);
	free(scredit_for_qp);
	free(wc);
	free(wc_tx);
	return return_value;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_lat_write(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t                scnt = 0;
	uint64_t                ccnt = 0;
	uint64_t                rcnt = 0;
	int                     ne;
	int			err = 0;
	int 			poll_buf_offset = 0;
	volatile char           *poll_buf = NULL;
	volatile char           *post_buf = NULL;
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr  *bad_exp_wr = NULL;
	struct ibv_send_wr      *bad_wr = NULL;
	#endif
	struct ibv_wc           wc;

	int 			cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
	int 			total_gap_cycles = user_param->latency_gap * cpu_mhz;
	cycles_t 		end_cycle, start_gap=0;

	#ifdef HAVE_VERBS_EXP
	if (user_param->use_exp == 1) {
		ctx->exp_wr[0].sg_list->length = user_param->size;
		ctx->exp_wr[0].exp_send_flags      = IBV_EXP_SEND_SIGNALED;
		if (user_param->size <= user_param->inline_size)
			ctx->exp_wr[0].exp_send_flags |= IBV_EXP_SEND_INLINE;
	} else {
	#endif
		ctx->wr[0].sg_list->length = user_param->size;
		#ifdef HAVE_IBV_WR_API
		ctx->qpx[0]->wr_flags |= IBV_SEND_SIGNALED;
		#else
		ctx->wr[0].send_flags = IBV_SEND_SIGNALED;
		#endif
		if (user_param->size <= user_param->inline_size)
			ctx->wr[0].send_flags |= IBV_SEND_INLINE;
	#ifdef HAVE_VERBS_EXP
	}
	#endif

	if((user_param->use_xrc || user_param->connection_type == DC))
		poll_buf_offset = 1;

	post_buf = (char*)ctx->buf[0] + user_param->size - 1;
	poll_buf = (char*)ctx->buf[0] + (user_param->num_of_qps + poll_buf_offset)*BUFF_SIZE(ctx->size, ctx->cycle_buffer) + user_param->size - 1;

	/* Duration support in latency tests. */
	if (user_param->test_type == DURATION) {
		duration_param=user_param;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		user_param->iters = 0;
		if (user_param->margin > 0)
			alarm(user_param->margin);
		else
			catch_alarm(0);
	}

	/* Done with setup. Start the test. */
	while (scnt < user_param->iters || ccnt < user_param->iters || rcnt < user_param->iters
			|| ((user_param->test_type == DURATION && user_param->state != END_STATE))) {

		if ((rcnt < user_param->iters || user_param->test_type == DURATION) && !(scnt < 1 && user_param->machine == SERVER)) {
			rcnt++;
			while (*poll_buf != (char)rcnt && user_param->state != END_STATE);
		}

		if (scnt < user_param->iters || user_param->test_type == DURATION) {

			if (user_param->latency_gap) {
				start_gap = get_cycles();
				end_cycle = start_gap + total_gap_cycles;
				while (get_cycles() < end_cycle) {
					continue;
				}
			}

			if (user_param->test_type == ITERATIONS)
				user_param->tposted[scnt] = get_cycles();

			*post_buf = (char)++scnt;
			#ifdef HAVE_VERBS_EXP
			if (user_param->use_exp == 1)
				err = (ctx->exp_post_send_func_pointer)(ctx->qp[0],&ctx->exp_wr[0],&bad_exp_wr);
			else
				err = (ctx->post_send_func_pointer)(ctx->qp[0],&ctx->wr[0],&bad_wr);
			#else
			err = post_send_method(ctx, 0, user_param);
			#endif
			if (err) {
				fprintf(stderr,"Couldn't post send: scnt=%lu\n",scnt);
				return 1;
			}
		}

		if (user_param->test_type == DURATION && user_param->state == END_STATE)
			break;

		if (ccnt < user_param->iters || user_param->test_type == DURATION) {

			do { ne = ibv_poll_cq(ctx->send_cq, 1, &wc); } while (ne == 0);

			if(ne > 0) {

				if (wc.status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_SEND(wc,scnt,ccnt);
					return 1;
				}

				ccnt++;
				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
					user_param->iters++;

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return FAILURE;
			}
		}
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_lat(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t	scnt = 0;
	int 		ne;
	int		err = 0;
	#ifdef HAVE_VERBS_EXP
	struct 		ibv_exp_send_wr *bad_exp_wr = NULL;
	struct 		ibv_send_wr *bad_wr = NULL;
	#endif
	struct 		ibv_wc wc;

	int 		cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
	int 		total_gap_cycles = user_param->latency_gap * cpu_mhz;
	cycles_t 	end_cycle, start_gap=0;

	#ifdef HAVE_VERBS_EXP
	if (user_param->use_exp == 1) {
		ctx->exp_wr[0].sg_list->length = user_param->size;
		ctx->exp_wr[0].exp_send_flags = IBV_EXP_SEND_SIGNALED;
	} else {
	#endif
		ctx->wr[0].sg_list->length = user_param->size;
		#ifdef HAVE_IBV_WR_API
		ctx->qpx[0]->wr_flags |= IBV_SEND_SIGNALED;
		#else
		ctx->wr[0].send_flags = IBV_SEND_SIGNALED;
		#endif
	#ifdef HAVE_VERBS_EXP
	}
	#endif

	/* Duration support in latency tests. */
	if (user_param->test_type == DURATION) {
		duration_param=user_param;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		user_param->iters = 0;
		if (user_param->margin > 0)
			alarm(user_param->margin);
		else
			catch_alarm(0);
	}
	while (scnt < user_param->iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
		if (user_param->latency_gap) {
			start_gap = get_cycles();
			end_cycle = start_gap + total_gap_cycles;
			while (get_cycles() < end_cycle) {
				continue;
			}
		}
		if (user_param->test_type == ITERATIONS)
			user_param->tposted[scnt++] = get_cycles();

		#ifdef HAVE_VERBS_EXP
		if (user_param->use_exp == 1)
			err = (ctx->exp_post_send_func_pointer)(ctx->qp[0],&ctx->exp_wr[0],&bad_exp_wr);
		else
			err = (ctx->post_send_func_pointer)(ctx->qp[0],&ctx->wr[0],&bad_wr);
		#else
		err = post_send_method(ctx, 0, user_param);
		#endif
		if (err) {
			fprintf(stderr,"Couldn't post send: scnt=%lu\n",scnt);
			return 1;
		}

		if (user_param->test_type == DURATION && user_param->state == END_STATE)
			break;

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return 1;
			}
		}

		do {
			ne = ibv_poll_cq(ctx->send_cq, 1, &wc);
			if(ne > 0) {
				if (wc.status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_SEND(wc,scnt,scnt);
					return 1;
				}
				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
					user_param->iters++;

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return FAILURE;
			}

		} while (!user_param->use_event && ne == 0);
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_lat_send(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t		scnt = 0; /* sent packets counter */
	uint64_t		rcnt = 0; /* received packets counter */
	int			poll = 0;
	int			ne;
	int			err = 0;
	struct ibv_wc		wc;
	struct ibv_recv_wr	*bad_wr_recv;
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr	*bad_exp_wr;
	struct ibv_send_wr	*bad_wr;
	#endif
	int  			firstRx = 1;
	int 			size_per_qp = (user_param->use_srq) ?
					user_param->rx_depth/user_param->num_of_qps : user_param->rx_depth;
	int 			cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
	int			total_gap_cycles = user_param->latency_gap * cpu_mhz;
	int			send_flows_index = 0;
	int			recv_flows_index = 0;
	cycles_t 		end_cycle, start_gap=0;
	uintptr_t		primary_send_addr = ctx->sge_list[0].addr;
	uintptr_t		primary_recv_addr = ctx->recv_sge_list[0].addr;
	if (user_param->connection_type != RawEth) {
		#ifdef HAVE_VERBS_EXP
		if (user_param->use_exp == 1) {
			ctx->exp_wr[0].sg_list->length = user_param->size;
			ctx->exp_wr[0].exp_send_flags = 0;
		} else {
		#endif
			ctx->wr[0].sg_list->length = user_param->size;
			ctx->wr[0].send_flags = 0;
			#ifdef HAVE_IBV_WR_API
			ctx->qpx[0]->wr_flags |= IBV_SEND_SIGNALED;
			#endif
		#ifdef HAVE_VERBS_EXP
		}
		#endif

	}
	if (user_param->size <= user_param->inline_size) {
		#ifdef HAVE_VERBS_EXP
		if (user_param->use_exp == 1)
			ctx->exp_wr[0].exp_send_flags |= IBV_EXP_SEND_INLINE;
		else
		#endif
			ctx->wr[0].send_flags |= IBV_SEND_INLINE;
	}
	while (scnt < user_param->iters || rcnt < user_param->iters ||
			( (user_param->test_type == DURATION && user_param->state != END_STATE))) {

		/*
		 * Get the received packet. make sure that the client won't enter here until he sends
		 * his first packet (scnt < 1)
		 * server will enter here first and wait for a packet to arrive (from the client)
		 */
		if ((rcnt < user_param->iters || user_param->test_type == DURATION) && !(scnt < 1 && user_param->machine == CLIENT)) {
			if (user_param->use_event) {
				if (ctx_notify_events(ctx->channel)) {
					fprintf(stderr , " Failed to notify events to CQ");
					return 1;
				}
			}
			do {
				ne = ibv_poll_cq(ctx->recv_cq,1,&wc);
				if (user_param->test_type == DURATION && user_param->state == END_STATE)
					break;

				if (ne > 0) {
					if (firstRx) {
						set_on_first_rx_packet(user_param);
						firstRx = 0;
					}

					if (wc.status != IBV_WC_SUCCESS) {
						NOTIFY_COMP_ERROR_RECV(wc,rcnt);
						return 1;
					}

					rcnt++;

					if (user_param->test_type == DURATION && user_param->state == SAMPLE_STATE)
						user_param->iters++;

					/*if we're in duration mode or there
					 * is enough space in the rx_depth,
					 * post that you received a packet.
					 */
					if (user_param->test_type == DURATION || (rcnt + size_per_qp <= user_param->iters)) {
						if (user_param->use_srq) {

							if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc.wr_id], &bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv SRQ. QP = %d: counter=%lu\n",(int)wc.wr_id, rcnt);
								return 1;
							}

						} else {
							if (ibv_post_recv(ctx->qp[wc.wr_id], &ctx->rwr[wc.wr_id], &bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv: rcnt=%lu\n", rcnt);
								return 15;
							}
						}
						if (user_param->flows != DEF_FLOWS) {
							if (++recv_flows_index == user_param->flows) {
								recv_flows_index = 0;
								ctx->recv_sge_list[0].addr = primary_recv_addr;
							} else {
								ctx->recv_sge_list[0].addr += INC(user_param->size, ctx->cache_line_size);
							}
						}
					}
				} else if (ne < 0) {
					fprintf(stderr, "poll CQ failed %d\n", ne);
					return 1;
				}
			} while (!user_param->use_event && ne == 0);
		}

		if (scnt < user_param->iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

			if (user_param->latency_gap) {
				start_gap = get_cycles();
				end_cycle = start_gap + total_gap_cycles;
				while (get_cycles() < end_cycle) {
					continue;
				}
			}

			if (user_param->test_type == ITERATIONS)
				user_param->tposted[scnt] = get_cycles();

			scnt++;

			if (scnt % user_param->cq_mod == 0 || (user_param->test_type == ITERATIONS && scnt == user_param->iters)) {
				poll = 1;
				#ifdef HAVE_VERBS_EXP
				if (user_param->use_exp == 1)
					ctx->exp_wr[0].exp_send_flags |= IBV_EXP_SEND_SIGNALED;
				else
				#endif
					ctx->wr[0].send_flags |= IBV_SEND_SIGNALED;
			}

			/* if we're in duration mode and the time is over, exit from this function */
			if (user_param->test_type == DURATION && user_param->state == END_STATE)
				break;

			/* send the packet that's in index 0 on the buffer */
			#ifdef HAVE_VERBS_EXP
			if (user_param->use_exp == 1)
				err = (ctx->exp_post_send_func_pointer)(ctx->qp[0],&ctx->exp_wr[0],&bad_exp_wr);
			else
				err = (ctx->post_send_func_pointer)(ctx->qp[0],&ctx->wr[0],&bad_wr);
			#else
			err = post_send_method(ctx, 0, user_param);
			#endif
			if (err) {
				fprintf(stderr,"Couldn't post send: scnt=%lu \n",scnt);
				return 1;
			}
			if (user_param->flows != DEF_FLOWS) {
				if (++send_flows_index == user_param->flows) {
					send_flows_index = 0;
					ctx->sge_list[0].addr = primary_send_addr;
				} else {
					ctx->sge_list[0].addr = primary_send_addr + (ctx->flow_buff_size * send_flows_index);
				}
			}
			if (poll == 1) {

				struct ibv_wc s_wc;
				int s_ne;

				if (user_param->use_event) {
					if (ctx_notify_events(ctx->channel)) {
						fprintf(stderr , " Failed to notify events to CQ");
						return FAILURE;
					}
				}

				/* wait until you get a cq for the last packet */
				do {
					s_ne = ibv_poll_cq(ctx->send_cq, 1, &s_wc);
				} while (!user_param->use_event && s_ne == 0);

				if (s_ne < 0) {
					fprintf(stderr, "poll on Send CQ failed %d\n", s_ne);
					return FAILURE;
				}

				if (s_wc.status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_SEND(s_wc,scnt,scnt)
						return 1;
				}
				poll = 0;

				#ifdef HAVE_VERBS_EXP
				if (user_param->use_exp == 1)
					ctx->exp_wr[0].exp_send_flags &= ~IBV_EXP_SEND_SIGNALED;
				else
				#endif
					ctx->wr[0].send_flags &= ~IBV_SEND_SIGNALED;
			}
		}
	}

	return 0;
}
/******************************************************************************
 *Server
 ******************************************************************************/
int run_iter_lat_burst_server(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	int i;
	int ne = 0;
	int err = 0;
	uint64_t scnt = 0;
	uint64_t rcnt = 0;
	uint64_t ccnt = 0;
	struct ibv_wc		*wc = NULL;
	struct ibv_send_wr	*bad_wr;
	struct ibv_recv_wr      *bad_wr_recv = NULL;
	int wc_id;

	ALLOCATE(wc, struct ibv_wc, user_param->burst_size);

	/* main loop for polling */
	while (rcnt < user_param->iters) {

		ne = ibv_poll_cq(ctx->recv_cq, user_param->burst_size, wc);
		if (ne > 0) {
			for (i = 0; i < ne; i++) {
				wc_id = (int)wc[i].wr_id;
				if (wc[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_RECV(wc[i], rcnt);
					return FAILURE;
				}
				rcnt++;
				if (rcnt%user_param->reply_every == 0 && scnt - ccnt < user_param->tx_depth) {
					err = ibv_post_send(ctx->qp[0], &ctx->wr[0], &bad_wr);
					if (err) {
						fprintf(stderr, "Couldn't post send: scnt=%lu\n", scnt);
						return FAILURE;
					}
					scnt++;
				}

				if (ibv_post_recv(ctx->qp[wc_id], &ctx->rwr[wc_id], &bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%ld\n", wc_id, rcnt);
					return FAILURE;
				}
			}
		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return FAILURE;
		}
		ne = ibv_poll_cq(ctx->send_cq, CTX_POLL_BATCH, wc);
		if (ne > 0) {
			for (i = 0; i < ne; i++) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_SEND(wc[i], scnt, ccnt);
					return FAILURE;
				}
				ccnt++;
			}

		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return FAILURE;
		}
	}
	free(wc);
	return SUCCESS;
}
/******************************************************************************
 *Client
 ******************************************************************************/
int run_iter_lat_burst(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	uint64_t		totscnt = 0; /* sent packets counter */
	uint64_t		totccnt = 0; /* complete sent packets counter */
	uint64_t		totrcnt = 0; /* received packets counter */
	uint64_t	   	tot_iters;
	uint64_t		pong_cnt = 0; /* counts how many pongs arrived */
	int			ne, ns;
	int			err = 0;
	int			i = 0;
	int			wc_id;
	struct ibv_wc		*wc;
	#ifdef HAVE_VERBS_EXP
	struct ibv_exp_send_wr	*bad_exp_wr;
	#endif
	struct ibv_send_wr	*bad_wr;
	int			cpu_mhz;
	int			return_value = 0;
	/* Rate Limiter*/
	int			rate_limit_pps = 0;
	double			gap_time = 0;   /* in usec */
	cycles_t		gap_cycles = 0; /* in cycles */
	cycles_t		gap_deadline = 0;
	unsigned int		number_of_bursts = 0;
	int			burst_iter = 0;
	int			is_sending_burst = 0;
	struct ibv_recv_wr      *bad_wr_recv = NULL;
	ALLOCATE(wc, struct ibv_wc, user_param->burst_size);

	tot_iters = (uint64_t)user_param->iters;

	/* If using rate limiter, calculate gap time between bursts */
	cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
	if (cpu_mhz <= 0) {
		fprintf(stderr, "Failed: couldn't acquire cpu frequency for rate limiter.\n");
		return_value = FAILURE;
		goto cleaning;
	}
	if (user_param->rate_limit > 0 ) {
		if (user_param->rate_limit_type == SW_RATE_LIMIT) {
			switch (user_param->rate_units) {
				case MEGA_BYTE_PS:
					rate_limit_pps = ((double)(user_param->rate_limit) / user_param->size) * 1048576;
					break;
				case GIGA_BIT_PS:
					rate_limit_pps = ((double)(user_param->rate_limit) / (user_param->size * 8)) * 1000000000;
					break;
				case PACKET_PS:
					rate_limit_pps = user_param->rate_limit;
					break;
				default:
					fprintf(stderr, " Failed: Unknown rate limit units\n");
					return_value = FAILURE;
					goto cleaning;
			}
			number_of_bursts = rate_limit_pps / user_param->burst_size;
			gap_time = 1000000 * (1.0 / number_of_bursts);
		}
	}

	gap_cycles = cpu_mhz * gap_time;

	/* main loop for posting */
	while (totrcnt < (totscnt / user_param->reply_every) || totccnt < tot_iters) {

		if (is_sending_burst == 0) {
			if (gap_deadline > get_cycles() && user_param->rate_limit_type == SW_RATE_LIMIT) {
				/* Go right to cq polling until gap time is over. */
				goto polling;
			}
			gap_deadline = get_cycles() + gap_cycles;
			is_sending_burst = 1;
			burst_iter = 0;
		}
		while ((totscnt < user_param->iters)
			&& (totscnt - totccnt) < (user_param->tx_depth) && !(is_sending_burst == 0 )) {
			#ifdef HAVE_VERBS_EXP
			if (user_param->use_exp == 1)
				err = (ctx->exp_post_send_func_pointer)(ctx->qp[0],
					&ctx->exp_wr[0], &bad_exp_wr);
			else
				err = (ctx->post_send_func_pointer)(ctx->qp[0],&ctx->wr[0],&bad_wr);
			#else
			err = ibv_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr);
                        #endif
			if (err) {
				fprintf(stderr, "Couldn't post send: scnt=%lu\n", totscnt);
				return 1;
			}
			if (user_param->post_list == 1 && user_param->size <= (ctx->cycle_buffer / 2)) {
				#ifdef HAVE_VERBS_EXP
				if (user_param->use_exp == 1)
					increase_loc_addr(ctx->exp_wr[0].sg_list, user_param->size,
								totscnt, ctx->my_addr[0], 0, ctx->cache_line_size, ctx->cycle_buffer);
				else
				#endif
					increase_loc_addr(ctx->wr[0].sg_list, user_param->size, totscnt,
							ctx->my_addr[0], 0, ctx->cache_line_size, ctx->cycle_buffer);
			}
			totscnt += user_param->post_list;
			if (totscnt % user_param->reply_every == 0 && totscnt != 0) {
				user_param->tposted[pong_cnt] = get_cycles();
				pong_cnt++;
			}
			if (++burst_iter == user_param->burst_size) {
				is_sending_burst = 0;
			}
		}
polling:
		do {
			ne = ibv_poll_cq(ctx->recv_cq, CTX_POLL_BATCH, wc);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					wc_id = (user_param->verb_type == ACCL_INTF) ?
							0 : (int)wc[i].wr_id;
					user_param->tcompleted[totrcnt] = get_cycles();
					totrcnt++;
					if (wc[i].status != IBV_WC_SUCCESS) {
						NOTIFY_COMP_ERROR_SEND(wc[i], totscnt, totccnt);
						return_value = FAILURE;
						goto cleaning;
					}
					if (ibv_post_recv(ctx->qp[wc_id], &ctx->rwr[wc_id], &bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%ld\n", wc_id, totrcnt);
						return FAILURE;
					}
				}
			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return_value = 1;
				goto cleaning;
			}
			ns = ibv_poll_cq(ctx->send_cq, user_param->burst_size, wc);
			if (ns > 0) {
				for (i = 0; i < ns; i++) {
					wc_id = (user_param->verb_type == ACCL_INTF) ?
						0 : (int)wc[i].wr_id;
					if (wc[i].status != IBV_WC_SUCCESS) {
						NOTIFY_COMP_ERROR_SEND(wc[i], totscnt, totccnt);
						return_value = FAILURE ;
						goto cleaning;
					}
					totccnt += user_param->cq_mod;
				}
			} else if (ns < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return_value = 1;
				goto cleaning;
			}
		} while (ne != 0);
	}

	return SUCCESS;
cleaning:
	free(wc);
	return return_value;
}
/******************************************************************************
 *
 ******************************************************************************/
uint16_t ctx_get_local_lid(struct ibv_context *context,int port)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(context,port,&attr))
		return 0;

	return attr.lid;
}

/******************************************************************************
 *
 ******************************************************************************/
void catch_alarm(int sig)
{
	switch (duration_param->state) {
		case START_STATE:
			duration_param->state = SAMPLE_STATE;
			get_cpu_stats(duration_param,1);
			duration_param->tposted[0] = get_cycles();
			alarm(duration_param->duration - 2*(duration_param->margin));
			break;
		case SAMPLE_STATE:
			duration_param->state = STOP_SAMPLE_STATE;
			duration_param->tcompleted[0] = get_cycles();
			get_cpu_stats(duration_param,2);
			if (duration_param->margin > 0)
				alarm(duration_param->margin);
			else
				catch_alarm(0);

			break;
		case STOP_SAMPLE_STATE:
			duration_param->state = END_STATE;
			break;
		default:
			fprintf(stderr,"unknown state\n");
	}
}

void check_alive(int sig)
{
	if (check_alive_data.current_totrcnt > check_alive_data.last_totrcnt) {
		check_alive_data.last_totrcnt = check_alive_data.current_totrcnt;
		alarm(60);
	} else if (check_alive_data.current_totrcnt == check_alive_data.last_totrcnt && check_alive_data.current_totrcnt < check_alive_data.g_total_iters) {
		fprintf(stderr," Did not get Message for 120 Seconds, exiting..\n Total Received=%d, Total Iters Required=%d\n",check_alive_data.current_totrcnt, check_alive_data.g_total_iters);

		if (check_alive_data.is_events) {
			/* Can't report BW, as we are stuck in event_loop */
			fprintf(stderr," Due to this issue, Perftest cannot produce a report when in event mode.\n");
			exit(FAILURE);
		}
		else {
			/* exit nice from run_iter function and report known bw/mr */
			check_alive_data.to_exit = 1;
		}
	}
}

/******************************************************************************
 *
 ******************************************************************************/
void catch_alarm_infintely()
{
	print_report_bw(duration_param,NULL);
	duration_param->iters = 0;
	alarm(duration_param->duration);
	duration_param->tposted[0] = get_cycles();
}

/******************************************************************************
 *
 ******************************************************************************/
void *handle_signal_print_thread(void *sigmask)
{
	sigset_t *set = (sigset_t*)sigmask;
	int rc;
	int sig_caught;
	while(1){
		rc = sigwait(set, &sig_caught);
		if (rc != 0){
			printf("Error when try to wait for SIGALRM\n");
			exit(EXIT_FAILURE);
		}
		if(sig_caught == SIGALRM)
				catch_alarm_infintely();
		else {
			printf("Unsupported signal caught %d, only signal %d is supported\n", sig_caught, SIGALRM);
			exit(EXIT_FAILURE);
		}
	}

}

/******************************************************************************
 *
 ******************************************************************************/
#ifdef HAVE_MASKED_ATOMICS
int check_masked_atomics_support(struct pingpong_context *ctx)
{
	struct ibv_exp_device_attr attr;
        memset(&attr,0,sizeof (struct ibv_exp_device_attr));

	attr.comp_mask = IBV_EXP_DEVICE_ATTR_EXT_ATOMIC_ARGS | IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS;
	attr.exp_atomic_cap = IBV_EXP_ATOMIC_HCA_REPLY_BE;

	if (ibv_exp_query_device(ctx->context, &attr)) {
		fprintf(stderr, "ibv_exp_query_device failed\n");
		return -1;
	}

	return MASK_IS_SET(IBV_EXP_ATOMIC_HCA_REPLY_BE, attr.exp_atomic_cap) &&
		MASK_IS_SET(IBV_EXP_DEVICE_EXT_ATOMICS, attr.exp_device_cap_flags);
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
#ifdef HAVE_PACKET_PACING_EXP
int check_packet_pacing_support(struct pingpong_context *ctx)
{
	struct ibv_exp_device_attr attr;
	memset(&attr, 0, sizeof (struct ibv_exp_device_attr));

	attr.comp_mask = IBV_EXP_DEVICE_ATTR_PACKET_PACING_CAPS;

	if (ibv_exp_query_device(ctx->context, &attr)) {
		fprintf(stderr, "ibv_exp_query_device failed\n");
		return FAILURE;
	}

	return MASK_IS_SET(IBV_EXP_DEVICE_ATTR_PACKET_PACING_CAPS, attr.comp_mask) ?
		SUCCESS : FAILURE;
}

#ifdef HAVE_PACKET_PACING_EXTENSION_EXP
int check_packet_pacing_extension_support(struct pingpong_context *ctx)
{
	struct ibv_exp_device_attr attr;
	memset(&attr, 0, sizeof (struct ibv_exp_device_attr));

	attr.comp_mask = IBV_EXP_DEVICE_ATTR_PACKET_PACING_CAPS;

	if (ibv_exp_query_device(ctx->context, &attr)) {
		fprintf(stderr, "ibv_exp_query_device failed\n");
		return FAILURE;
	}

	if (!MASK_IS_SET(IBV_EXP_DEVICE_ATTR_PACKET_PACING_CAPS, attr.comp_mask))
		return FAILURE;

	if (attr.packet_pacing_caps.cap_flags & IBV_EXP_QP_SUPPORT_BURST)
		return SUCCESS;

	return FAILURE;
}
#endif

#elif HAVE_PACKET_PACING
int check_packet_pacing_support(struct pingpong_context *ctx)
{
	struct ibv_device_attr_ex attr;
	memset(&attr, 0, sizeof (struct ibv_device_attr_ex));

	if (ibv_query_device_ex(ctx->context, NULL, &attr)) {
		fprintf(stderr, "ibv_query_device_ex failed\n");
		return FAILURE;
	}

	/* qp_rate_limit_max > 0 if PP is supported */
	return attr.packet_pacing_caps.qp_rate_limit_max > 0 ? SUCCESS : FAILURE;
}
#endif

int run_iter_fs(struct pingpong_context *ctx, struct perftest_parameters *user_param) {

	struct raw_ethernet_info	*my_dest_info = NULL;
	struct raw_ethernet_info	*rem_dest_info = NULL;

	#ifdef HAVE_RAW_ETH_EXP
	struct ibv_exp_flow		**flow_create_result;
	struct ibv_exp_flow_attr	**flow_rules;
	#else
	struct ibv_flow			**flow_create_result;
	struct ibv_flow_attr		**flow_rules;
	#endif
	int 				flow_index = 0;
	int				qp_index = 0;
	int				retval = SUCCESS;
	uint64_t			tot_fs_cnt    = 0;
	uint64_t			allocated_flows = 0;
	uint64_t			tot_iters = 0;

	/* Allocate user input dependable structs */
	ALLOCATE(my_dest_info, struct raw_ethernet_info, user_param->num_of_qps);
	memset(my_dest_info, 0, sizeof(struct raw_ethernet_info) * user_param->num_of_qps);
	ALLOCATE(rem_dest_info, struct raw_ethernet_info, user_param->num_of_qps);
	memset(rem_dest_info, 0, sizeof(struct raw_ethernet_info) * user_param->num_of_qps);

	if (user_param->test_type == ITERATIONS) {
		user_param->flows = user_param->iters * user_param->num_of_qps;
		allocated_flows = user_param->iters;
	} else if (user_param->test_type == DURATION) {
		allocated_flows = (2 * MAX_FS_PORT) - (user_param->server_port + user_param->client_port);
	}


	#ifdef HAVE_RAW_ETH_EXP
        ALLOCATE(flow_create_result, struct ibv_exp_flow*, allocated_flows * user_param->num_of_qps);
        ALLOCATE(flow_rules, struct ibv_exp_flow_attr*, allocated_flows * user_param->num_of_qps);
	#else
        ALLOCATE(flow_create_result, struct ibv_flow*, allocated_flows * user_param->num_of_qps);
        ALLOCATE(flow_rules, struct ibv_flow_attr*, allocated_flows * user_param->num_of_qps);
	#endif

	if(user_param->test_type == DURATION) {
		duration_param = user_param;
		user_param->iters = 0;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		alarm(user_param->margin);
		if (user_param->margin > 0)
			alarm(user_param->margin);
		else
			catch_alarm(0); /* move to next state */
	}
	if (set_up_fs_rules(flow_rules, ctx, user_param, allocated_flows)) {
			fprintf(stderr, "Unable to set up flow rules\n");
			retval = FAILURE;
			goto cleaning;
	}

	do {/* This loop runs once in Iteration mode */
		for (qp_index = 0; qp_index < user_param->num_of_qps; qp_index++) {

			for (flow_index = 0; flow_index < allocated_flows; flow_index++) {

				if (user_param->test_type == ITERATIONS)
					user_param->tposted[tot_fs_cnt] = get_cycles();
				else if (user_param->test_type == DURATION && duration_param->state == END_STATE)
					break;
				#ifdef HAVE_RAW_ETH_EXP
				flow_create_result[flow_index] =
					ibv_exp_create_flow(ctx->qp[qp_index], flow_rules[(qp_index * allocated_flows) + flow_index]);
				#else
				flow_create_result[flow_index] =
					ibv_create_flow(ctx->qp[qp_index], flow_rules[(qp_index * allocated_flows) + flow_index]);
				#endif
				if (user_param->test_type == ITERATIONS)
					user_param->tcompleted[tot_fs_cnt] = get_cycles();
				if (!flow_create_result[flow_index]) {
					perror("error");
					fprintf(stderr, "Couldn't attach QP\n");
					retval = FAILURE;
					goto cleaning;
				}
				if (user_param->test_type == ITERATIONS ||
				   (user_param->test_type == DURATION && duration_param->state == SAMPLE_STATE))
					tot_fs_cnt++;
				tot_iters++;
			}
		}
	} while (user_param->test_type == DURATION && duration_param->state != END_STATE);

	if (user_param->test_type == DURATION && user_param->state == END_STATE)
		user_param->iters = tot_fs_cnt;

cleaning:
	/* destroy open flows */
	for (flow_index = 0; flow_index < tot_iters; flow_index++) {
		#ifdef HAVE_RAW_ETH_EXP
		if (ibv_exp_destroy_flow(flow_create_result[flow_index])) {
		#else
		if (ibv_destroy_flow(flow_create_result[flow_index])) {
		#endif
			perror("error");
			fprintf(stderr, "Couldn't destroy flow\n");
		}
	}
	free(flow_rules);
	free(flow_create_result);
	free(my_dest_info);
	free(rem_dest_info);

	return retval;
}


/******************************************************************************
*
******************************************************************************/
int rdma_cm_allocate_nodes(struct pingpong_context *ctx,
	struct perftest_parameters *user_param, struct rdma_addrinfo *hints)
{
	int rc = SUCCESS, i = 0;
	char *error_message = "";

	if (user_param->connection_type == UD
		|| user_param->connection_type == RawEth)
		hints->ai_port_space = RDMA_PS_UDP;
	else
		hints->ai_port_space = RDMA_PS_TCP;

	ALLOCATE(ctx->cma_master.nodes, struct cma_node, user_param->num_of_qps);
	if (!ctx->cma_master.nodes) {
		error_message = "Failed to allocate memory for RDMA CM nodes.";
		goto error;
	}

	memset(ctx->cma_master.nodes, 0,
		(sizeof *ctx->cma_master.nodes) * user_param->num_of_qps);

	for (i = 0; i < user_param->num_of_qps; i++) {
		ctx->cma_master.nodes[i].id = i;
		if (user_param->machine == CLIENT) {
			rc = rdma_create_id(ctx->cma_master.channel,
				&ctx->cma_master.nodes[i].cma_id, NULL, hints->ai_port_space);
			if (rc) {
				error_message = "Failed to create RDMA CM ID.";
				goto error;
			}
		}
	}

	return rc;

error:
	while (--i >= 0) {
		rc = rdma_destroy_id(ctx->cma_master.nodes[i].cma_id);
		if (rc) {
			error_message = "Failed to destroy RDMA CM ID.";
			break;
		}
	}

	free(ctx->cma_master.nodes);
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
void rdma_cm_destroy_qps(struct pingpong_context *ctx,
	struct perftest_parameters *user_param)
{
	int i;
	struct cma_node *cm_node;

	for (i = 0; i < user_param->num_of_qps; i++) {
		cm_node = &ctx->cma_master.nodes[i];
		if (cm_node->cma_id->qp) {
			rdma_destroy_qp(cm_node->cma_id);
		}
	}
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_destroy_cma(struct pingpong_context *ctx,
	struct perftest_parameters *user_param)
{
	int rc = SUCCESS, i;
	char error_message[ERROR_MSG_SIZE] = "";
	struct cma_node *cm_node;

	for (i = 0; i < user_param->num_of_qps; i++) {
		cm_node = &ctx->cma_master.nodes[i];
		rc = rdma_destroy_id(cm_node->cma_id);
		if (rc) {
			sprintf(error_message,
				"Failed to destroy RDMA CM ID number %d.", i);
			goto error;
		}
	}

	rdma_destroy_event_channel(ctx->cma_master.channel);
	if (ctx->cma_master.rai) {
		rdma_freeaddrinfo(ctx->cma_master.rai);
	}

	free(ctx->cma_master.nodes);
	return rc;

error:
	return error_handler(error_message);
}

int error_handler(char *error_message)
{
	fprintf(stderr, "%s\nERRNO: %s.\n", error_message, strerror(errno));
	return FAILURE;
}

/******************************************************************************
 * End
 ******************************************************************************/
