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

static enum ibv_wr_opcode opcode_verbs_array[] = {IBV_WR_SEND,IBV_WR_RDMA_WRITE,IBV_WR_RDMA_WRITE_WITH_IMM,IBV_WR_RDMA_READ};
static enum ibv_wr_opcode opcode_atomic_array[] = {IBV_WR_ATOMIC_CMP_AND_SWP,IBV_WR_ATOMIC_FETCH_AND_ADD};

#define CPU_UTILITY "/proc/stat"
#define DC_KEY 0xffeeddcc

struct perftest_parameters* duration_param;
struct check_alive_data check_alive_data;

/******************************************************************************
 * Beginning
 ******************************************************************************/

#ifdef HAVE_AES_XTS
int set_valid_dek(char *dst, struct perftest_parameters *user_param)
{
	char * line = NULL;
	size_t len = 0;
	ssize_t read;
	int index = 0;
	char* eptr;
	char* execute = NULL;
	int i;
	int size;

	const char file_name_letters[] = "abcdefghijklmnopqrstuvwxyz1234567890";
	char file_path[AES_XTS_DEK_FILE_NAME_SIZE];
	int file_letters_size = sizeof(file_name_letters)-1;
	FILE* dek_file = NULL;

	srand (time (NULL));

	sprintf(file_path, "/tmp/");

	/* + 5 because we begin to randomize after folder path /tmp/ */
	for(i = 5; i < AES_XTS_DEK_FILE_NAME_SIZE - 1; i++) {
		file_path[i] = file_name_letters[rand() % file_letters_size];
	}

	/* + 1 for the '\0' at the end of the char array*/
	size = strlen(user_param->data_enc_key_app_path) + 1
		+ strlen(user_param->kek_path) + 1 + strlen(file_path) + 1;

	ALLOCATE(execute, char, size);

	strcpy(execute, user_param->data_enc_key_app_path);
	strcat(execute, " ");
	strcat(execute, user_param->kek_path);
	strcat(execute, " ");
	strcat(execute, file_path);

	if(system(execute)) {
		fprintf(stderr, "Execution of %s has failed\n", execute);
		free(execute);
		return FAILURE;
	}

	free(execute);

	dek_file = fopen(file_path, "r");

	if(dek_file == NULL) {
		fprintf(stderr, "Can not open the data_encryption_key file\n");
		return FAILURE;
	}

	while((read = getline(&line, &len, dek_file)) != -1) {

		if(index >= AES_XTS_DEK_SIZE) {
			fprintf(stderr, "Invalid data_encryption_key file\n");
			fclose(dek_file);
			return FAILURE;
		}

		dst[index] = strtol(line, &eptr, 16);
		index++;
	}

	fclose(dek_file);

	remove(file_path);

	return 0;
}

int set_valid_cred(char *dst, struct perftest_parameters *user_param)
{
	char valid_credential[48];
	char * line = NULL;
	size_t len = 0;
	ssize_t read;
	char* eptr;
	int index = 0;
	FILE* cred = NULL;

	cred = fopen(user_param->credentials_path, "r");

	if(cred == NULL) {
		fprintf(stderr, "Can not open the credentials file\n");
		return FAILURE;
	}

	while((read = getline(&line, &len, cred)) != -1) {

		if(index >= AES_XTS_CREDENTIALS_SIZE) {
			fprintf(stderr, "Invalid credentials file\n");
			fclose(cred);
			return FAILURE;
		}

		valid_credential[index] = strtol(line, &eptr, 16);
		index++;
	}

	fclose(cred);

	//coverity[uninit_use_in_call]
	memcpy(dst, valid_credential, sizeof(valid_credential));

	return 0;
}
#endif

// cppcheck-suppress constParameter
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
	fp = fopen(file_name, "r");

	if (fp != NULL) {
		char line[100];
		if (fgets(line,100,fp) != NULL) {
			char tmp[100];
			int index=0;
			compress_spaces(line,line);
			index=get_n_word_string(line,tmp,index,2); /* skip first word */
			duration_param->cpu_util_data.ustat[stat_index-1] = atoll(tmp);

			get_n_word_string(line,tmp,index,3); /* skip 2 stats */
			duration_param->cpu_util_data.idle[stat_index-1] = atoll(tmp);

		}
		fclose(fp);
	}
}

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
 *	ctx             - Test Context.
 *	user_param      - user_parameters struct for this test.
 *	inl             - use inline or not.
 *	index           - qp index.
 *	qpt             - qp type.
 *	op              - RDMA operation code.
 *	connection_type - Type of the connection.
 *	enc             - use encryption/decryption or not.
 *
 * Return Value : int.
 *
 */
#ifdef HAVE_IBV_WR_API
static inline int _new_post_send(struct pingpong_context *ctx,
	struct perftest_parameters *user_param, int inl, int index,
	enum ibv_qp_type qpt, enum ibv_wr_opcode op, int connection_type, int enc)
	__attribute__((always_inline));
static inline int _new_post_send(struct pingpong_context *ctx,
	struct perftest_parameters *user_param, int inl, int index,
	enum ibv_qp_type qpt, enum ibv_wr_opcode op, int connection_type, int enc)
{
	int rc;
	int wr_index = index * user_param->post_list;
	struct ibv_send_wr *wr = &ctx->wr[wr_index];

#ifdef HAVE_AES_XTS
	if(enc) {
		int i;
		struct ibv_sge sgl;
		struct mlx5dv_mkey_conf_attr mkey_attr = {};
		struct mlx5dv_crypto_attr crypto_attr = {};

		ibv_wr_start(ctx->qpx[index]);

		ctx->qpx[index]->wr_flags = IBV_SEND_INLINE;
		crypto_attr.crypto_standard = MLX5DV_CRYPTO_STANDARD_AES_XTS;
		sgl.addr = (uintptr_t)ctx->mr[index]->addr;
		sgl.lkey = ctx->mr[index]->lkey;
		sgl.length = user_param->buff_size;

		mlx5dv_wr_mkey_configure(ctx->dv_qp[index], ctx->mkey[index], 3, &mkey_attr);
		mlx5dv_wr_set_mkey_access_flags(ctx->dv_qp[index], IBV_ACCESS_REMOTE_READ |
                              IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
		mlx5dv_wr_set_mkey_layout_list(ctx->dv_qp[index], 1, &sgl);

		if(user_param->sig_before) {
			crypto_attr.signature_crypto_order = MLX5DV_SIGNATURE_CRYPTO_ORDER_SIGNATURE_BEFORE_CRYPTO_ON_TX;
		}
		else {
			crypto_attr.signature_crypto_order = MLX5DV_SIGNATURE_CRYPTO_ORDER_SIGNATURE_AFTER_CRYPTO_ON_TX;
		}

		if(user_param->encrypt_on_tx) {
			crypto_attr.encrypt_on_tx = true;
		}
		else {
			crypto_attr.encrypt_on_tx = false;
		}

		for(i=0; i < AES_XTS_TWEAK_SIZE; i++) {
			crypto_attr.initial_tweak[i] = i;
		}

		for(i=0; i < AES_XTS_KEYTAG_SIZE; i++) {
			crypto_attr.keytag[i] = 0;
		}

		crypto_attr.data_unit_size = user_param->aes_block_size;
		crypto_attr.dek = ctx->dek[(ctx->dek_number%user_param->data_enc_keys_number)];
		ctx->dek_number++;

		mlx5dv_wr_set_mkey_crypto(ctx->dv_qp[index], &crypto_attr);
		ibv_wr_complete(ctx->qpx[index]);
	}
#endif



	ibv_wr_start(ctx->qpx[index]);
	while (wr)
	{
		ctx->qpx[index]->wr_id = wr->wr_id;
		ctx->qpx[index]->wr_flags = wr->send_flags;

		switch (op)
		{
		case IBV_WR_SEND:
			ibv_wr_send(ctx->qpx[index]);
			break;
		case IBV_WR_RDMA_WRITE:
			ibv_wr_rdma_write(
				ctx->qpx[index],
				wr->wr.rdma.rkey,
				wr->wr.rdma.remote_addr);
			break;
		case IBV_WR_RDMA_WRITE_WITH_IMM:
			ibv_wr_rdma_write_imm(
				ctx->qpx[index],
				wr->wr.rdma.rkey,
				wr->wr.rdma.remote_addr, 0);
			break;
		case IBV_WR_RDMA_READ:
			ibv_wr_rdma_read(
				ctx->qpx[index],
				wr->wr.rdma.rkey,
				wr->wr.rdma.remote_addr);
			break;
		case IBV_WR_ATOMIC_FETCH_AND_ADD:
			ibv_wr_atomic_fetch_add(
				ctx->qpx[index],
				wr->wr.atomic.rkey,
				wr->wr.atomic.remote_addr,
				wr->wr.atomic.compare_add);
			break;
		case IBV_WR_ATOMIC_CMP_AND_SWP:
			ibv_wr_atomic_cmp_swp(
				ctx->qpx[index],
				wr->wr.atomic.rkey,
				wr->wr.atomic.remote_addr,
				wr->wr.atomic.compare_add,
				wr->wr.atomic.swap);
			break;
		default:
			fprintf(stderr, "Post send failed: unknown operation code.\n");
		}
		#ifdef HAVE_MLX5DV
		if (qpt == IBV_QPT_DRIVER && connection_type == DC)
		{
			#ifdef HAVE_DCS
			mlx5dv_wr_set_dc_addr_stream(
				ctx->dv_qp[index],
				ctx->ah[index],
				ctx->r_dctn[index],
				DC_KEY,
				ctx->dci_stream_id[index]);
			ctx->dci_stream_id[index] = (ctx->dci_stream_id[index] + 1) & (0xffffffff >> (32 - (user_param->log_active_dci_streams)));
			#else
			mlx5dv_wr_set_dc_addr(
				ctx->dv_qp[index],
				ctx->ah[index],
				ctx->r_dctn[index],
				DC_KEY);
			#endif
		}
		else
		#endif
		if (qpt == IBV_QPT_UD) {
			ibv_wr_set_ud_addr(
				ctx->qpx[index],
				wr->wr.ud.ah,
				wr->wr.ud.remote_qpn,
				wr->wr.ud.remote_qkey);
		} else if (qpt == IBV_QPT_DRIVER && connection_type == SRD) {
			ibv_wr_set_ud_addr(
				ctx->qpx[index],
				ctx->ah[index],
				ctx->rem_qpn[index],
				DEF_QKEY);
		}

		#ifdef HAVE_XRCD
		else if (qpt == IBV_QPT_XRC_SEND)
		{
			ibv_wr_set_xrc_srqn(
				ctx->qpx[index],
				wr->qp_type.xrc.remote_srqn);
		}
		#endif

		if (inl)
		{
			ibv_wr_set_inline_data(
				ctx->qpx[index],
				(void*) wr->sg_list->addr,
				user_param->size);
		}
		else
		{
			#ifdef HAVE_AES_XTS
			if(enc) {
				ctx->qpx[index]->wr_flags = ctx->qpx[index]->wr_flags | IBV_SEND_SIGNALED;
				ibv_wr_set_sge(ctx->qpx[index],
					ctx->mkey[index]->lkey,
					(uintptr_t)0,
					user_param->size);
			}
			else
			#endif
			ibv_wr_set_sge(
				ctx->qpx[index],
				wr->sg_list->lkey,
				wr->sg_list->addr,
				user_param->size);
		}
		wr = wr->next;
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
static int new_post_write_sge_dc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_DRIVER, opcode_verbs_array[user_param->verb], DC, 0);
}

static int new_post_write_inl_dc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_DRIVER, opcode_verbs_array[user_param->verb], DC, 0);
}

static int new_post_read_sge_dc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_DRIVER, IBV_WR_RDMA_READ, DC, 0);
}

static int new_post_send_sge_dc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_DRIVER, IBV_WR_SEND, DC, 0);
}

static int new_post_send_inl_dc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_DRIVER, IBV_WR_SEND, DC, 0);
}

static int new_post_atomic_fa_sge_dc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_DRIVER, IBV_WR_ATOMIC_FETCH_AND_ADD, DC, 0);
}

static int new_post_atomic_cs_sge_dc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_DRIVER, IBV_WR_ATOMIC_CMP_AND_SWP, DC, 0);
}

static int new_post_send_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_SEND, RC, 0);
}

static int new_post_send_sge_enc_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_SEND, RC, 1);
}

static int new_post_send_inl_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_RC, IBV_WR_SEND, RC, 0);
}

static int new_post_write_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, opcode_verbs_array[user_param->verb], RC, 0);
}

static int new_post_write_sge_enc_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, opcode_verbs_array[user_param->verb], RC, 1);
}

static int new_post_write_inl_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_RC, opcode_verbs_array[user_param->verb], RC, 0);
}

static int new_post_read_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_RDMA_READ, RC, 0);
}

static int new_post_read_sge_enc_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_RDMA_READ, RC, 1);
}

static int new_post_atomic_fa_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_ATOMIC_FETCH_AND_ADD, RC, 0);
}

static int new_post_atomic_cs_sge_rc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_RC, IBV_WR_ATOMIC_CMP_AND_SWP, RC, 0);
}

static int new_post_send_sge_ud(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_UD, IBV_WR_SEND, UD, 0);
}

static int new_post_send_inl_ud(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_UD, IBV_WR_SEND, UD, 0);
}

static int new_post_send_sge_uc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_UC, IBV_WR_SEND, UC, 0);
}

static int new_post_send_inl_uc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_UC, IBV_WR_SEND, UC, 0);
}

static int new_post_write_sge_uc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_UC, opcode_verbs_array[user_param->verb], UC, 0);
}

static int new_post_write_inl_uc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_UC, opcode_verbs_array[user_param->verb], UC, 0);
}

static int new_post_send_sge_srd(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_DRIVER, IBV_WR_SEND, SRD, 0);
}

static int new_post_send_inl_srd(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_DRIVER, IBV_WR_SEND, SRD, 0);
}

static int new_post_read_sge_srd(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_DRIVER, IBV_WR_RDMA_READ, SRD, 0);
}

static int new_post_write_sge_srd(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_DRIVER, opcode_verbs_array[user_param->verb], SRD, 0);
}

static int new_post_write_inl_srd(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
       return _new_post_send(ctx, user_param, 1, index, IBV_QPT_DRIVER, opcode_verbs_array[user_param->verb], SRD, 0);
}

#ifdef HAVE_XRCD
static int new_post_send_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_SEND, XRC, 0);
}

static int new_post_send_inl_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_XRC_SEND, IBV_WR_SEND, XRC, 0);
}

static int new_post_write_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, opcode_verbs_array[user_param->verb], XRC, 0);
}

static int new_post_write_inl_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 1, index, IBV_QPT_XRC_SEND, opcode_verbs_array[user_param->verb], XRC, 0);
}

static int new_post_read_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_RDMA_READ, XRC, 0);
}

static int new_post_atomic_fa_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_ATOMIC_FETCH_AND_ADD, XRC, 0);
}

static int new_post_atomic_cs_sge_xrc(struct pingpong_context *ctx, int index,
	struct perftest_parameters *user_param)
{
	return _new_post_send(ctx, user_param, 0, index, IBV_QPT_XRC_SEND, IBV_WR_ATOMIC_CMP_AND_SWP, XRC, 0);
}
#endif
#endif

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
	if (!user_param->use_old_post_send)
		return (*ctx->new_post_send_work_request_func_pointer)(ctx, index, user_param);
	#endif
	struct ibv_send_wr 	*bad_wr = NULL;
	return ibv_post_send(ctx->qp[index], &ctx->wr[index*user_param->post_list], &bad_wr);

}

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
		fprintf(stderr,"Error opening file %s errno: %s\n", tmp_file_name,strerror(errno));
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

	if(user_param->verb == SEND || user_param->verb == WRITE_IMM)
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

	struct ibv_qp_init_attr_ex qp_init_attr;

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
		if (!user_param->use_old_post_send)
			qp_init_attr.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
		#endif
		qp_init_attr.cap.max_inline_data = user_param->inline_size;
	}

	#ifdef HAVE_IBV_WR_API
	if (!user_param->use_old_post_send)
	{
		if (user_param->verb == ATOMIC)
		{
			opcode = opcode_atomic_array[user_param->atomicType];
			if (opcode == IBV_WR_ATOMIC_FETCH_AND_ADD)
				qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_ATOMIC_FETCH_AND_ADD;
			else if (opcode == IBV_WR_ATOMIC_CMP_AND_SWP)
				qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_ATOMIC_CMP_AND_SWP;
		}
		else
		{
			opcode = opcode_verbs_array[user_param->verb];
			if (opcode == IBV_WR_SEND)
				qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_SEND;
			else if (opcode == IBV_WR_RDMA_WRITE)
				qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_RDMA_WRITE;
			else if (opcode == IBV_WR_RDMA_WRITE_WITH_IMM)
				qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM;
			else if (opcode == IBV_WR_RDMA_READ)
				qp_init_attr.send_ops_flags |= IBV_QP_EX_WITH_RDMA_READ;
		}
	}
	#endif

	qp = ibv_create_qp_ex(ctx->context, &qp_init_attr);

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

	free(*service);

	if (number < 0) {
		fprintf(stderr, "%s for ai_family: %x service: %s port: %d\n",
				gai_strerror(number), hints->ai_family, servername, port);
		return FAILURE;
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
int sockaddr_set_port(struct sockaddr *sin,int port)
{
	switch (sin->sa_family) {
	case AF_INET:  ((struct sockaddr_in*) sin)->sin_port = htons(port);
		break;
	case AF_INET6:  ((struct sockaddr_in6*) sin)->sin6_port = htons(port);
		break;
	default:
		fprintf(stderr, "ai_family: %x is not yet supported\n", sin->sa_family);
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
		goto destroy_event_channel;
	}

	return SUCCESS;

destroy_event_channel:
	rdma_destroy_event_channel(ctx->cm_channel);
	return FAILURE;

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

	//coverity[uninit_use]
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
struct ibv_context* ctx_open_device(struct ibv_device *ib_dev, struct perftest_parameters *user_param)
{
	struct ibv_context *context;

#ifdef HAVE_AES_XTS
	if(user_param->aes_xts){
		struct mlx5dv_crypto_login_attr login_attr = {};
		struct mlx5dv_context_attr attr = {};
		attr.flags = MLX5DV_CONTEXT_FLAGS_DEVX;

		if(set_valid_cred(login_attr.credential, user_param)){
			fprintf(stderr, "Couldn't set credentials\n");
			return NULL;
		}

		context = mlx5dv_open_device(ib_dev, &attr);

		if(!context){
			fprintf(stderr, "Couldn't get context for the device\n");
			return NULL;
		}

		int ret = mlx5dv_crypto_login(context, &login_attr);

		if (ret) {
			fprintf(stderr,"Couldn't login. err=%d.\n", ret);
			return NULL;
		}

		return context;
	}
#endif

	context = ibv_open_device(ib_dev);

	if (!context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		return NULL;
	}

	return context;
}
/******************************************************************************
 *
 ******************************************************************************/
int alloc_ctx(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t tarr_size;
	int num_of_qps_factor;
	ctx->cycle_buffer = user_param->cycle_buffer;
	ctx->cache_line_size = user_param->cache_line_size;

	ALLOC(user_param->port_by_qp, uint64_t, user_param->num_of_qps);

	tarr_size = (user_param->noPeak) ? 1 : user_param->iters*user_param->num_of_qps;
	ALLOC(user_param->tposted, cycles_t, tarr_size);
	memset(user_param->tposted, 0, sizeof(cycles_t)*tarr_size);
	if ((user_param->tst == LAT || user_param->tst == FS_RATE) && user_param->test_type == DURATION)
		ALLOC(user_param->tcompleted, cycles_t, 1);

	ALLOC(ctx->qp, struct ibv_qp*, user_param->num_of_qps);
	#ifdef HAVE_IBV_WR_API
	ALLOC(ctx->qpx, struct ibv_qp_ex*, user_param->num_of_qps);
	#ifdef HAVE_MLX5DV
	ALLOC(ctx->dv_qp, struct mlx5dv_qp_ex*, user_param->num_of_qps);
	#endif
	ALLOC(ctx->r_dctn, uint32_t, user_param->num_of_qps);
	#ifdef HAVE_DCS
	ALLOC(ctx->dci_stream_id, uint32_t, user_param->num_of_qps);
	#endif
	#ifdef HAVE_AES_XTS
	ALLOC(ctx->dek, struct mlx5dv_dek*, user_param->data_enc_keys_number);
	ALLOC(ctx->mkey, struct mlx5dv_mkey*, user_param->num_of_qps);
	#endif
	#endif
	ALLOC(ctx->mr, struct ibv_mr*, user_param->num_of_qps);
	ALLOC(ctx->buf, void*, user_param->num_of_qps);

	if ((user_param->tst == BW || user_param->tst == LAT_BY_BW) && (user_param->machine == CLIENT || user_param->duplex)) {

		ALLOC(user_param->tcompleted,cycles_t,tarr_size);
		memset(user_param->tcompleted, 0, sizeof(cycles_t)*tarr_size);
		ALLOC(ctx->my_addr,uint64_t,user_param->num_of_qps);
		ALLOC(ctx->rem_addr,uint64_t,user_param->num_of_qps);
		ALLOC(ctx->scnt,uint64_t,user_param->num_of_qps);
		ALLOC(ctx->ccnt,uint64_t,user_param->num_of_qps);
		memset(ctx->scnt, 0, user_param->num_of_qps * sizeof (uint64_t));
		memset(ctx->ccnt, 0, user_param->num_of_qps * sizeof (uint64_t));

	} else if ((user_param->tst == BW || user_param->tst == LAT_BY_BW)
		   && (user_param->verb == SEND || user_param->verb == WRITE_IMM) && user_param->machine == SERVER) {

		ALLOC(ctx->my_addr, uint64_t, user_param->num_of_qps);
		ALLOC(user_param->tcompleted, cycles_t, 1);
	} else if (user_param->tst == FS_RATE && user_param->test_type == ITERATIONS) {
		ALLOC(user_param->tcompleted, cycles_t, tarr_size);
		memset(user_param->tcompleted, 0, sizeof(cycles_t) * tarr_size);
	}

	if (user_param->machine == CLIENT || user_param->tst == LAT || user_param->duplex) {

		ALLOC(ctx->sge_list, struct ibv_sge,user_param->num_of_qps * user_param->post_list);
		ALLOC(ctx->wr, struct ibv_send_wr, user_param->num_of_qps * user_param->post_list);
		ALLOC(ctx->rem_qpn, uint32_t, user_param->num_of_qps);
		if ((user_param->verb == SEND && user_param->connection_type == UD) ||
				user_param->connection_type == DC || user_param->connection_type == SRD) {
			ALLOC(ctx->ah, struct ibv_ah*, user_param->num_of_qps);
		}
	} else if ((user_param->verb == READ || user_param->verb == WRITE || user_param->verb == WRITE_IMM) && user_param->connection_type == SRD) {
		ALLOC(ctx->ah, struct ibv_ah*, user_param->num_of_qps);
	}

	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex)) {
		ALLOC(ctx->recv_sge_list, struct ibv_sge,
			 user_param->num_of_qps * user_param->recv_post_list);
		ALLOC(ctx->rwr, struct ibv_recv_wr,
			 user_param->num_of_qps * user_param->recv_post_list);
		ALLOC(ctx->rx_buffer_addr, uint64_t, user_param->num_of_qps);
	}
	if (user_param->mac_fwd == ON )
		ctx->cycle_buffer = user_param->size * user_param->rx_depth;

	ctx->size = user_param->size;

	num_of_qps_factor = (user_param->mr_per_qp) ? 1 : user_param->num_of_qps;

	/* holds the size of maximum between msg size and cycle buffer,
	 * aligned to cache line, it is multiplied by 2 to be used as
	 * send buffer(first half) and receive buffer(second half)
	 * with reference to number of flows and number of QPs
	 */
	ctx->buff_size = INC(BUFF_SIZE(ctx->size, ctx->cycle_buffer),
				 ctx->cache_line_size) * 2 * num_of_qps_factor * user_param->flows;
	ctx->send_qp_buff_size = ctx->buff_size / num_of_qps_factor / 2;
	ctx->flow_buff_size = ctx->send_qp_buff_size / user_param->flows;
	user_param->buff_size = ctx->buff_size;
	if (user_param->connection_type == UD)
		ctx->buff_size += ctx->cache_line_size;

	ctx->memory = user_param->memory_create(user_param);

	return SUCCESS;
}
/******************************************************************************
 *
 ******************************************************************************/
void dealloc_ctx(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{

	if (user_param->port_by_qp != NULL)
		free(user_param->port_by_qp);

	if (user_param->tposted != NULL)
		free(user_param->tposted);

	if (((user_param->tst == LAT || user_param->tst == FS_RATE) && user_param->test_type == DURATION) ||
		((user_param->tst == BW || user_param->tst == LAT_BY_BW) && (user_param->machine == CLIENT || user_param->duplex)) ||
		((user_param->tst == BW || user_param->tst == LAT_BY_BW) && user_param->verb == SEND && user_param->machine == SERVER) ||
		(user_param->tst == FS_RATE && user_param->test_type == ITERATIONS))
		if (user_param->tcompleted != NULL)
			free(user_param->tcompleted);

	if (ctx->qp != NULL)
		free(ctx->qp);

	#ifdef HAVE_IBV_WR_API
	if (ctx->qpx != NULL)
		free(ctx->qpx);
	#ifdef HAVE_MLX5DV
	if (ctx->dv_qp != NULL)
		free(ctx->dv_qp);
	#endif
	if (ctx->r_dctn != NULL)
		free(ctx->r_dctn);
	#ifdef HAVE_DCS
	if (ctx->dci_stream_id != NULL)
		free(ctx->dci_stream_id);
	#endif
	#ifdef HAVE_AES_XTS
	if (ctx->dek != NULL)
		free(ctx->dek);
	if (ctx->mkey != NULL)
		free(ctx->mkey);
	#endif
	#endif
	if (ctx->mr != NULL)
		free(ctx->mr);
	if (ctx->buf != NULL)
		free(ctx->buf);
	if ((user_param->tst == BW || user_param->tst == LAT_BY_BW) && (user_param->machine == CLIENT || user_param->duplex)) {
		if (ctx->my_addr != NULL)
			free(ctx->my_addr);
		if (ctx->rem_addr != NULL)
			free(ctx->rem_addr);
		if (ctx->scnt != NULL)
			free(ctx->scnt);
		if (ctx->ccnt != NULL)
			free(ctx->ccnt);

	} else if ((user_param->tst == BW || user_param->tst == LAT_BY_BW)
		   && user_param->verb == SEND && user_param->machine == SERVER) {
		if (ctx->my_addr != NULL)
			free(ctx->my_addr);
	}

	if (user_param->machine == CLIENT || user_param->tst == LAT || user_param->duplex) {
		if (ctx->sge_list != NULL)
			free(ctx->sge_list);
		if (ctx->wr != NULL)
			free(ctx->wr);
		if (ctx->rem_qpn != NULL)
			free(ctx->rem_qpn);

		if ((user_param->verb == SEND && user_param->connection_type == UD) ||
				user_param->connection_type == DC || user_param->connection_type == SRD) {
		if (ctx->ah != NULL)
			free(ctx->ah);
		}
	} else if ((user_param->verb == READ || user_param->verb == WRITE || user_param->verb == WRITE_IMM) && user_param->connection_type == SRD) {
		if (ctx->ah != NULL)
			free(ctx->ah);
	}

	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex)) {
		if (ctx->recv_sge_list != NULL)
			free(ctx->recv_sge_list);
		if (ctx->rwr != NULL)
			free(ctx->rwr);
		if (ctx->rx_buffer_addr != NULL)
			free(ctx->rx_buffer_addr);
	}

	if (ctx->memory != NULL) {
		ctx->memory->destroy(ctx->memory);
		ctx->memory = NULL;
	}
}

/******************************************************************************
 *
 ******************************************************************************/
int destroy_ctx(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int i, dereg_counter, rc;
	int test_result = 0;
	int num_of_qps = user_param->num_of_qps;
	int dct_only = (user_param->machine == SERVER && !(user_param->duplex || user_param->tst == LAT));

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

	for (i = 0; i < user_param->num_of_qps; i++) {

		if ((((user_param->connection_type == DC && !((!(user_param->duplex || user_param->tst == LAT) && user_param->machine == SERVER)
							|| ((user_param->duplex || user_param->tst == LAT) && i >= num_of_qps))) ||
					user_param->connection_type == UD || user_param->connection_type == SRD) &&
				(user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex)) ||
				(user_param->connection_type == SRD && (user_param->verb == READ || user_param->verb == WRITE || user_param->verb == WRITE_IMM))) {

			if (user_param->ah_allocated == 1 && ibv_destroy_ah(ctx->ah[i])) {
				fprintf(stderr, "Failed to destroy AH\n");
				test_result = 1;
			}
		}
		if (user_param->work_rdma_cm == OFF) {
			if (ibv_destroy_qp(ctx->qp[i])) {
				fprintf(stderr, "Couldn't destroy QP - %s\n", strerror(errno));
				test_result = 1;
			}
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

	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) || (user_param->connection_type == DC && !dct_only)){
		if (ibv_destroy_cq(ctx->recv_cq)) {
				fprintf(stderr, "Failed to destroy CQ - %s\n", strerror(errno));
				test_result = 1;
			}
	}

	for (i = 0; i < dereg_counter; i++) {
		if (ibv_dereg_mr(ctx->mr[i])) {
			fprintf(stderr, "Failed to deregister MR #%d\n", i+1);
			test_result = 1;
		}
	}

	if (ctx->send_rcredit) {
		if (ibv_dereg_mr(ctx->credit_mr)) {
			fprintf(stderr, "Failed to deregister send credit MR\n");
			test_result = 1;
		}
		free(ctx->ctrl_buf);
		free(ctx->ctrl_sge_list);
		free(ctx->ctrl_wr);
	}

	#ifdef HAVE_AES_XTS
	if(user_param->aes_xts){
		for(i = 0; i < user_param->data_enc_keys_number; i++) {
			if (mlx5dv_dek_destroy(ctx->dek[i]))
				fprintf(stderr, "Failed to destroy data encryption key.\n");
		}

		for(i = 0; i < user_param->num_of_qps; i++) {
			if (mlx5dv_destroy_mkey(ctx->mkey[i]))
				fprintf(stderr, "Failed to destroy MKey.\n");
		}
	}
	#endif

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Failed to deallocate PD - %s\n", strerror(errno));
		test_result = 1;
	}

	if (ctx->send_channel) {
		if (ibv_destroy_comp_channel(ctx->send_channel)) {
			fprintf(stderr, "Failed to close send event channel\n");
			test_result = 1;
		}
	}

	if (ctx->recv_channel) {
		if (ibv_destroy_comp_channel(ctx->recv_channel)) {
			fprintf(stderr, "Failed to close receive event channel\n");
			test_result = 1;
		}
	}

	if (user_param->use_rdma_cm == OFF) {

		if (ibv_close_device(ctx->context)) {
			fprintf(stderr, "Failed to close device context\n");
			test_result = 1;
		}
	}

	for (i = 0; i < dereg_counter; i++) {
		ctx->memory->free_buffer(ctx->memory, 0, ctx->buf[i], ctx->buff_size);
	}

	free(ctx->qp);
	#ifdef HAVE_IBV_WR_API
	free(ctx->qpx);
	free(ctx->r_dctn);
	#ifdef HAVE_DCS
	free(ctx->dci_stream_id);
	#endif
	#endif
	#ifdef HAVE_AES_XTS
	if(user_param->aes_xts) {
		free(ctx->dek);
		free(ctx->mkey);
	}
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
	}

	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex)) {

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

	if (ctx->memory != NULL) {
		ctx->memory->destroy(ctx->memory);
		ctx->memory = NULL;
	}

	return test_result;
}

/******************************************************************************
 *
 ******************************************************************************/
#ifdef HAVE_EX_ODP
static int check_odp_transport_caps(struct perftest_parameters *user_param, uint32_t caps)
{
	static char conn_str[][7] = {"RC", "UC", "UD", "RawEth", "XRC", "DC", "SRD"};
	int conn = user_param->connection_type;
	VerbType verb = user_param->verb;
	MachineType machine = user_param->machine;
	int duplex = user_param->duplex;

	if (verb == SEND && duplex == OFF && machine == CLIENT) {
		if (!(caps & IBV_ODP_SUPPORT_SEND)) {
			fprintf(stderr, " ODP Send is not supported for %s transport.\n", conn_str[conn]);
			return 0;
		}
	} else if (verb == SEND && duplex == OFF && machine == SERVER) {
		if (!(caps & (IBV_ODP_SUPPORT_RECV | IBV_ODP_SUPPORT_SRQ_RECV))) {
			fprintf(stderr, " ODP Recv is not supported for %s transport.\n", conn_str[conn]);
			return 0;
		}
	} else if (verb == SEND && duplex == ON) {
		if (!(caps & IBV_ODP_SUPPORT_SEND &&
		      caps & (IBV_ODP_SUPPORT_RECV | IBV_ODP_SUPPORT_SRQ_RECV))) {
			fprintf(stderr, " ODP bidirectional Send is not supported for %s transport.\n", conn_str[conn]);
			return 0;
		}
	} else if ((verb == WRITE || verb == WRITE_IMM) && !(caps & IBV_ODP_SUPPORT_WRITE)) {
		fprintf(stderr, " ODP Write is not supported for %s transport.\n", conn_str[conn]);
		return 0;
	} else if (verb == READ && !(caps & IBV_ODP_SUPPORT_READ)) {
		fprintf(stderr, " ODP Read is not supported for %s transport.\n", conn_str[conn]);
		return 0;
	} else if (verb == ATOMIC && !(caps & IBV_ODP_SUPPORT_ATOMIC)) {
		fprintf(stderr, " ODP Atomics are not supported for %s transport.\n", conn_str[conn]);
		return 0;
	}

	return 1;
}

static int check_odp_support(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	struct ibv_device_attr_ex dattr;
	int conn = user_param->connection_type;
	int ret = ibv_query_device_ex(ctx->context, NULL, &dattr);

	if (ret) {
		fprintf(stderr, " Couldn't query device for On-Demand Paging capabilities.\n");
		return 0;
	}

	/* These capabilities must be set by device drivers. */
	if (!(dattr.odp_caps.general_caps & IBV_ODP_SUPPORT)) {
		fprintf(stderr, " The device does not support On-Demand Paging.\n");
		return 0;
	}

	switch (conn) {
	case RC:
		if ( !check_odp_transport_caps(user_param,
					       dattr.odp_caps.per_transport_caps.rc_odp_caps) )
			return 0;
		break;

	case UC:
		fprintf(stderr," ODP is not available on UC transport.\n");
		return 0;

	case UD:
		/* Checking UD caps is problematic for some devices because UD recive supports ODP from NIC
		* perspective but capabilaties are registered as off, so let's skip checking them. */
		break;

	case XRC:
		if ( !check_odp_transport_caps(user_param, dattr.xrc_odp_caps) )
			return 0;
		break;

	case DC:
		/* A Dynamically Connected transport service is specific to mlx5 devices.
		 * ODP is available, but the device driver does not register the capabilities,
		 * so we cannot get them with ibv_query_device_ex(). They are configured in
		 * libmlx5 and can be gained with an experimental API ibv_exp_query_device(),
		 * but we should stick to generic functions, so let's skip checking them. */
		break;

	case SRD:
		/* Scalable Reliable Datagram is Ethernet-based transport protocol specific to
		 * AWS Elastic Fabric Adapter. ODP is not implemented. */
		fprintf(stderr, " ODP is not available on SRD transport.\n");
		return 0;

	default:
		fprintf(stderr, " Unsupported connection type.\n");
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
					user_param->num_of_qps, NULL, ctx->send_channel, user_param->eq_num);
	if (!ctx->send_cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return FAILURE;
	}

	if (need_recv_cq) {
		ctx->recv_cq = ibv_create_cq(ctx->context,user_param->rx_depth *
						user_param->num_of_qps, NULL, ctx->recv_channel, user_param->eq_num);
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

	if ((user_param->connection_type == DC && !dct_only) || (user_param->verb == SEND || user_param->verb == WRITE_IMM))
		need_recv_cq = 1;

	ret = create_reg_cqs(ctx, user_param, tx_buffer_depth, need_recv_cq);

	return ret;
}

/******************************************************************************
 *
 ******************************************************************************/
int create_single_mr(struct pingpong_context *ctx, struct perftest_parameters *user_param, int qp_index)
{
	int flags = IBV_ACCESS_LOCAL_WRITE;
	bool can_init_mem = true;
	int dmabuf_fd = 0;
	uint64_t dmabuf_offset;

	#if defined(__FreeBSD__)
	ctx->is_contig_supported = FAILURE;
	#endif

	/* ODP */
	#ifdef HAVE_EX_ODP
	if (user_param->use_odp) {
		if ( !check_odp_support(ctx, user_param) )
			return FAILURE;

		/* ODP does not support contig pages */
		ctx->is_contig_supported = FAILURE;
		flags |= IBV_ACCESS_ON_DEMAND;
	}
	#endif

	if (user_param->memory_type == MEMORY_MMAP) {
		#if defined(__FreeBSD__)
		posix_memalign(ctx->buf, user_param->cycle_buffer, ctx->buff_size);
		#else
		ctx->buf = memalign(user_param->cycle_buffer, ctx->buff_size);
		#endif
	}

	if (ctx->is_contig_supported == SUCCESS)
	{
		ctx->buf[qp_index] = NULL;
		flags |= (1 << 5);
	} else if (ctx->memory->allocate_buffer(ctx->memory, user_param->cycle_buffer, ctx->buff_size,
						&dmabuf_fd, &dmabuf_offset, &ctx->buf[qp_index],
						&can_init_mem)) {
		return FAILURE;
	}

	if (user_param->verb == WRITE || user_param->verb == WRITE_IMM) {
		flags |= IBV_ACCESS_REMOTE_WRITE;
	} else if (user_param->verb == READ) {
		flags |= IBV_ACCESS_REMOTE_READ;
		if (user_param->transport_type == IBV_TRANSPORT_IWARP)
			flags |= IBV_ACCESS_REMOTE_WRITE;
	} else if (user_param->verb == ATOMIC) {
		flags |= IBV_ACCESS_REMOTE_ATOMIC;
	}

#ifdef HAVE_RO
	if (user_param->disable_pcir == 0) {
		flags |= IBV_ACCESS_RELAXED_ORDERING;
	}
#endif

#ifdef HAVE_REG_DMABUF_MR
	/* Allocating Memory region and assigning our buffer to it. */
	if (dmabuf_fd) {
		printf("Calling ibv_reg_dmabuf_mr(offset=%lu, size=%lu, addr=%p, fd=%d) for QP #%d\n",
			dmabuf_offset, ctx->buff_size, ctx->buf[qp_index], dmabuf_fd, qp_index);
		ctx->mr[qp_index] = ibv_reg_dmabuf_mr(
			ctx->pd, dmabuf_offset,
			ctx->buff_size, (uint64_t)ctx->buf[qp_index],
			dmabuf_fd,
			flags
		);
		if (!ctx->mr[qp_index]) {
			int error = errno;
			fprintf(stderr, "Couldn't allocate MR with error=%d\n", error);
			if (error == EOPNOTSUPP)
				fprintf(stderr, "OFED stack does not support DMA-BUF\n");
			return FAILURE;
		}
		// MPI immediately closes the fd.
		// We emulate the behavior here.
		close(dmabuf_fd);
	}
	else
#endif
	{
		ctx->mr[qp_index] = ibv_reg_mr(ctx->pd, ctx->buf[qp_index], ctx->buff_size, flags);
		if (!ctx->mr[qp_index]) {
			fprintf(stderr, "Couldn't allocate MR\n");
			return FAILURE;
		}
	}

	if (user_param->use_null_mr) {
		ctx->null_mr = ibv_alloc_null_mr(ctx->pd);
		if (!ctx->null_mr) {
			fprintf(stderr, "Couldn't create null MR\n");
			return FAILURE;
		}
	}

	if (ctx->is_contig_supported == SUCCESS)
		ctx->buf[qp_index] = ctx->mr[qp_index]->addr;


	/* Initialize buffer with random numbers except in WRITE_LAT test that it 0's */
	if (can_init_mem) {
		srand(time(NULL));
		if ((user_param->verb == WRITE || user_param->verb == WRITE_IMM) && user_param->tst == LAT) {
			memset(ctx->buf[qp_index], 0, ctx->buff_size);
		} else {
			uint64_t i;
			if (user_param->has_payload_modification) {
				for (i = 0; i < ctx->buff_size; i++) {
					((char*)ctx->buf[qp_index])[i] = user_param->payload_content[i % user_param->payload_length];
				}
			} else {
				for (i = 0; i < ctx->buff_size; i++) {
				((char*)ctx->buf[qp_index])[i] = (char)rand();
				}
			}
		}
	}
	return SUCCESS;
}


static int create_payload(struct perftest_parameters *user_param)
{
	char* file_content;
	char* token;
	int payload_file_size;
	int counter = 0;
	FILE* fptr;

	/* read payload text file */
	fptr = fopen(user_param->payload_file_path, "r");
	if (!fptr)
	{
		fprintf(stderr, "Failed to open '%s'\n", user_param->payload_file_path);
		return 1;
	}

	/* get payload file size*/
	fseek(fptr, 0, SEEK_END);
	payload_file_size = ftell(fptr);
	fseek(fptr, 0, SEEK_SET);

	if (payload_file_size <= 0) {
		fprintf(stderr, "Payload size should be greater than 0\n");
		fclose(fptr);
		return 1;
	}

	/* read payload file content*/
	ALLOCATE(file_content, char, payload_file_size + 1);
	if (payload_file_size != fread(file_content, 1, payload_file_size, fptr)) {
		fprintf(stderr, "Failed to read payload file\n");
		free(file_content);
		fclose(fptr);
		return 1;
	}

	file_content[payload_file_size] = '\0';
	/* allocate buffer for the payload*/
	ALLOCATE(user_param->payload_content, char, user_param->size + 1);

	/* get token in DWORD form: '0xaaaaaaaa' */
	token = strtok(file_content, ",");

	do {
			int i;
			char current_byte_chars[2];
			if (strlen(token) != 10) {
				fprintf(stderr, "Failed to parse DWORD number: %d\n", counter/4);
				free(user_param->payload_content);
				free(file_content);
				fclose(fptr);
				return 1;
			}
			for(i = 0; i < 8; i += 2){
				current_byte_chars[0] = token[8-i];
				current_byte_chars[1] = token[9-i];
				if (!isxdigit(current_byte_chars[0]) || !isxdigit(current_byte_chars[1])) {
					fprintf(stderr, "Invalid hex char in DWORD number: %d\n", counter/4);
					free(user_param->payload_content);
					free(file_content);
					fclose(fptr);
					return 1;
				}
				user_param->payload_content[counter] = (char) strtol(current_byte_chars, NULL, 16);

				counter++;
				if (counter == user_param->size)
					break;
			}
		token = strtok(NULL, ",\n");
	} while (token != NULL && counter < user_param->size);

	user_param->payload_content[counter] = '\0';
	user_param->payload_length = counter;
	free(file_content);
	fclose(fptr);

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int create_mr(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	int i;
	int mr_index = 0;

	if (user_param->has_payload_modification){
		if (create_payload(user_param)){
			return 1;
		}
	}

	/* create first MR */
	if (create_single_mr(ctx, user_param, 0)) {
		fprintf(stderr, "failed to create mr\n");
		return 1;
	}
	mr_index++;

	/* create the rest if needed, or copy the first one */
	for (i = 1; i < user_param->num_of_qps; i++) {
		if (user_param->mr_per_qp) {
			if (create_single_mr(ctx, user_param, i)) {
				fprintf(stderr, "failed to create mr\n");
				goto destroy_mr;
			}
			mr_index++;
		} else {
			ctx->mr[i] = ctx->mr[0];
			// cppcheck-suppress arithOperationsOnVoidPointer
			ctx->buf[i] = ctx->buf[0] + (i*BUFF_SIZE(ctx->size, ctx->cycle_buffer));
		}
	}

	return 0;

destroy_mr:
	for (i = 0; i < mr_index; i++)
		ibv_dereg_mr(ctx->mr[i]);

	return FAILURE;
}

/******************************************************************************
 *
 ******************************************************************************/
int verify_params_with_device_context(struct ibv_context *context,
				      struct perftest_parameters *user_param)
{
	enum ctx_device current_dev = ib_dev_name(context);
	if(user_param->use_event) {
		if(user_param->eq_num > context->num_comp_vectors) {
			fprintf(stderr, " Completion vector specified is invalid\n");
			fprintf(stderr, " Max completion vector = %d\n",
				context->num_comp_vectors - 1);
			return FAILURE;
		}
	}

	// those are devices supporting new post send
	if (current_dev != CONNECTIB &&
		current_dev != CONNECTX4 &&
		current_dev != CONNECTX4LX &&
		current_dev != CONNECTX5 &&
		current_dev != CONNECTX5EX &&
		current_dev != CONNECTX6 &&
		current_dev != CONNECTX6DX &&
		current_dev != CONNECTX6LX &&
		current_dev != CONNECTX7 &&
		current_dev != CONNECTX8 &&
		current_dev != MLX5GENVF &&
		current_dev != BLUEFIELD &&
		current_dev != BLUEFIELD2 &&
		current_dev != BLUEFIELD3 &&
		current_dev != EFA &&
		current_dev != HNS)
	{
		if (!user_param->use_old_post_send)
		{
			user_param->use_old_post_send = 1;
		}
	}

	return SUCCESS;
}

#if defined HAVE_OOO_ATTR
static int verify_ooo_settings(struct pingpong_context *ctx,
			       struct perftest_parameters *user_param)
{
	#ifdef HAVE_OOO_ATTR
	struct ibv_device_attr_ex attr = { };
	if (ibv_query_device_ex(ctx->context, NULL, &attr))
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
	#endif
	} else {
		return FAILURE;
	}
}
#endif
int ctx_init(struct pingpong_context *ctx, struct perftest_parameters *user_param)
{
	int i;
	int dct_only = (user_param->machine == SERVER && !(user_param->duplex || user_param->tst == LAT));
	int qp_index = 0, dereg_counter;
	#ifdef HAVE_AES_XTS
	int mkey_index = 0, dek_index = 0;
	#endif

	#if defined HAVE_OOO_ATTR
	if (user_param->use_ooo) {
		if (verify_ooo_settings(ctx, user_param) != SUCCESS) {
			fprintf(stderr, "Incompatible OOO settings\n");
			return FAILURE;
		}
	}
	#endif
	ctx->is_contig_supported = FAILURE;

	/* Allocating event channels if requested. */
	if (user_param->use_event) {
		ctx->send_channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->send_channel) {
			fprintf(stderr, "Couldn't create send completion channel\n");
			return FAILURE;
		}

		ctx->recv_channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->recv_channel) {
			fprintf(stderr, "Couldn't create receive completion channel\n");
			return FAILURE;
		}
	}

	/* Allocating the Protection domain. */
	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto comp_channel;
	}

	#ifdef HAVE_AES_XTS
	if(user_param->aes_xts){
		struct mlx5dv_dek_init_attr dek_attr = {};
		struct mlx5dv_mkey_init_attr mkey_init_attr = {};

		ctx->dek_number = 0;
		dek_attr.key_size = MLX5DV_CRYPTO_KEY_SIZE_128;
		dek_attr.has_keytag = 0;
		dek_attr.key_purpose = MLX5DV_CRYPTO_KEY_PURPOSE_AES_XTS;
		dek_attr.pd = ctx->pd;
		dek_attr.opaque[0] = 0x11;
		mkey_init_attr.pd = ctx->pd;
		mkey_init_attr.create_flags = MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
		MLX5DV_MKEY_INIT_ATTR_FLAGS_CRYPTO;

		mkey_init_attr.max_entries = 1;
		for(i = 0; i < user_param->data_enc_keys_number; i++) {

			if (set_valid_dek(dek_attr.key, user_param)) {
				fprintf(stderr, "Failed to set dek\n");
				goto dek;
			}

			ctx->dek[i] = mlx5dv_dek_create(ctx->context, &dek_attr);

			if(!ctx->dek[i]) {
				fprintf(stderr, "Failed to create dek\n");
				goto dek;
			}

			dek_index++;
		}

		for(i = 0; i < user_param->num_of_qps; i++) {
			ctx->mkey[i] = mlx5dv_create_mkey(&mkey_init_attr);

			if(!ctx->mkey[i]) {
				fprintf(stderr, "Failed to create mkey\n");
				goto mkey;
			}

			mkey_index++;
		}
	}
	#endif

	if (ctx->memory->init(ctx->memory)) {
		fprintf(stderr, "Failed to init memory\n");
		goto mkey;
	}

	if (create_mr(ctx, user_param)) {
		fprintf(stderr, "Failed to create MR\n");
		goto mkey;
	}

	if (create_cqs(ctx, user_param)) {
		fprintf(stderr, "Failed to create CQs\n");
		goto mr;

	}

	#ifdef HAVE_XRCD
	if (user_param->use_xrc) {

		if (ctx_xrcd_create(ctx,user_param)) {
			fprintf(stderr, "Couldn't create XRC resources\n");
			goto cqs;
		}

		if (ctx_xrc_srq_create(ctx,user_param)) {
			fprintf(stderr, "Couldn't create SRQ XRC resources\n");
			goto xrcd;
		}
	}
	#endif

	if (user_param->use_srq && user_param->connection_type == DC &&
			(user_param->tst == LAT ||
			user_param->machine == SERVER ||
			user_param->duplex == ON))
	{
		struct ibv_srq_init_attr_ex attr;
			memset(&attr, 0, sizeof(attr));
		attr.comp_mask = IBV_SRQ_INIT_ATTR_TYPE | IBV_SRQ_INIT_ATTR_PD;
		attr.attr.max_wr = user_param->rx_depth;
		attr.attr.max_sge = 1;
		attr.pd = ctx->pd;

		attr.srq_type = IBV_SRQT_BASIC;
		ctx->srq = ibv_create_srq_ex(ctx->context, &attr);
		if (!ctx->srq)  {
			fprintf(stderr, "Couldn't create SRQ\n");
			goto xrc_srq;
		}
	}

	if (user_param->use_srq && user_param->connection_type != DC &&
			!user_param->use_xrc && (user_param->tst == LAT ||
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
			goto xrcd;
		}
	}

	/*
	* QPs creation in RDMA CM flow will be done separately.
	* Unless, the function called with RDMA CM connection contexts,
	* need to verify the call with the existence of ctx->cm_id.
	*/
	if (!(user_param->work_rdma_cm == OFF || ctx->cm_id))
		return SUCCESS;

	for (i=0; i < user_param->num_of_qps; i++) {
		if (create_qp_main(ctx, user_param, i)) {
			fprintf(stderr, "Failed to create QP.\n");
			goto qps;
		}

		if (user_param->work_rdma_cm == OFF) {
			modify_qp_to_init(ctx, user_param, i);
		}
		#ifdef HAVE_DCS
		ctx->dci_stream_id[i] = 0;
		#endif
		qp_index++;
	}

	return SUCCESS;


qps:
	for(i = 0; i < qp_index; i++){
		ibv_destroy_qp(ctx->qp[i]);
	}

	if (user_param->use_srq && (user_param->tst == LAT ||
			user_param->machine == SERVER || user_param->duplex == ON))
		ibv_destroy_srq(ctx->srq);

xrcd: __attribute__((unused))
	#ifdef HAVE_XRCD
	if (user_param->use_xrc)
		ibv_close_xrcd(ctx->xrc_domain);

xrc_srq:
	if (user_param->use_xrc)
		ibv_destroy_srq(ctx->srq);
	#endif
// cppcheck-suppress unusedLabelConfiguration
cqs:
	ibv_destroy_cq(ctx->send_cq);

	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) || (user_param->connection_type == DC && !dct_only)){
		ibv_destroy_cq(ctx->recv_cq);
	}

mr:
	dereg_counter = (user_param->mr_per_qp) ? user_param->num_of_qps : 1;

	for (i = 0; i < dereg_counter; i++)
		ibv_dereg_mr(ctx->mr[i]);

mkey:
	#ifdef HAVE_AES_XTS
	if(user_param->aes_xts)
		for (i = 0; i < mkey_index; i++)
			mlx5dv_destroy_mkey(ctx->mkey[i]);

dek:
	if(user_param->aes_xts)
		for (i = 0; i < dek_index; i++)
			mlx5dv_dek_destroy(ctx->dek[i]);
	#endif

	ibv_dealloc_pd(ctx->pd);

comp_channel:
	if (user_param->use_event) {
		ibv_destroy_comp_channel(ctx->send_channel);
		ibv_destroy_comp_channel(ctx->recv_channel);
	}

	return FAILURE;
}

int modify_qp_to_init(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int qp_index)
{
	if (ctx_modify_qp_to_init(ctx->qp[qp_index], user_param,
		qp_index)) {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return FAILURE;
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
int create_reg_qp_main(struct pingpong_context *ctx,
				struct perftest_parameters *user_param,
				int i)
{
	if (user_param->use_xrc) {
		#ifdef HAVE_XRCD
		ctx->qp[i] = ctx_xrc_qp_create(ctx, user_param, i);
		#endif
	} else {
		ctx->qp[i] = ctx_qp_create(ctx, user_param, i);
	}

	if (ctx->qp[i] == NULL) {
		fprintf(stderr, "Unable to create QP.\n");
		return FAILURE;
	}
	#ifdef HAVE_IBV_WR_API
	if (!user_param->use_old_post_send) {
		ctx->qpx[i] = ibv_qp_to_qp_ex(ctx->qp[i]);
		#ifdef HAVE_MLX5DV
		if (user_param->connection_type == DC)
		{
			ctx->dv_qp[i] = mlx5dv_qp_ex_from_ibv_qp_ex(ctx->qpx[i]);
		}
		#ifdef HAVE_AES_XTS
		if (user_param->aes_xts){
			ctx->dv_qp[i] = mlx5dv_qp_ex_from_ibv_qp_ex(ctx->qpx[i]);
		}
		#endif
		#endif
	}
	#endif

	return SUCCESS;
}

int create_qp_main(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int i)
{
	int ret;
	ret = create_reg_qp_main(ctx, user_param, i);
	return ret;
}

struct ibv_qp* ctx_qp_create(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int qp_index)
{
	struct ibv_qp* qp = NULL;
	int dc_num_of_qps = user_param->num_of_qps / 2;

	int is_dc_server_side = 0;
	struct ibv_qp_init_attr attr;
	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	struct ibv_qp_cap *qp_cap = &attr.cap;

	#ifdef HAVE_IBV_WR_API
	enum ibv_wr_opcode opcode;
	struct ibv_qp_init_attr_ex attr_ex;
	memset(&attr_ex, 0, sizeof(struct ibv_qp_init_attr_ex));
	#ifdef HAVE_MLX5DV
	struct mlx5dv_qp_init_attr attr_dv;
	memset(&attr_dv, 0, sizeof(attr_dv));
	#endif
	#ifdef HAVE_SRD
	struct efadv_qp_init_attr efa_attr = {};
	#endif
	#endif
	#ifdef HAVE_HNSDV
	struct hnsdv_qp_init_attr hns_attr = {};
	#endif

	attr.send_cq = ctx->send_cq;
	attr.recv_cq = (user_param->verb == SEND || user_param->verb == WRITE_IMM) ? ctx->recv_cq : ctx->send_cq;

	is_dc_server_side = ((!(user_param->duplex || user_param->tst == LAT) &&
						  (user_param->machine == SERVER)) ||
						 ((user_param->duplex || user_param->tst == LAT) &&
						  (qp_index >= dc_num_of_qps)));

	attr.cap.max_inline_data = user_param->inline_size;
	if (!(user_param->connection_type == DC &&
			is_dc_server_side)) {
		attr.cap.max_send_wr  = user_param->tx_depth;
		attr.cap.max_send_sge = MAX_SEND_SGE;
	}

	if (user_param->use_srq &&
			(user_param->tst == LAT ||
			 user_param->machine == SERVER ||
			 user_param->duplex == ON)) {
		attr.srq = ctx->srq;
		if (user_param->connection_type != DC) {
			attr.cap.max_recv_wr  = 0;
			attr.cap.max_recv_sge = 0;
		}
	} else {
		attr.srq = NULL;
		if (user_param->connection_type != DC) {
			attr.cap.max_recv_wr  = user_param->rx_depth;
			attr.cap.max_recv_sge = MAX_RECV_SGE;
		}
	}

	switch (user_param->connection_type) {

		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
		#ifdef HAVE_IBV_WR_API
		case DC : attr.qp_type = IBV_QPT_DRIVER; break;
		#endif
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
			attr_ex.send_ops_flags |= IBV_QP_EX_WITH_ATOMIC_FETCH_AND_ADD;
		else if (opcode == IBV_WR_ATOMIC_CMP_AND_SWP)
			attr_ex.send_ops_flags |= IBV_QP_EX_WITH_ATOMIC_CMP_AND_SWP;
	}
	else {
		opcode = opcode_verbs_array[user_param->verb];
		if(0);
		else if (opcode == IBV_WR_SEND)
			attr_ex.send_ops_flags |= IBV_QP_EX_WITH_SEND;
		else if (opcode == IBV_WR_RDMA_WRITE)
			attr_ex.send_ops_flags |= IBV_QP_EX_WITH_RDMA_WRITE;
		else if (opcode == IBV_WR_RDMA_WRITE_WITH_IMM)
			attr_ex.send_ops_flags |= IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM;
		else if (opcode == IBV_WR_RDMA_READ)
			attr_ex.send_ops_flags |= IBV_QP_EX_WITH_RDMA_READ;
	}
	attr_ex.pd = ctx->pd;
	attr_ex.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS | IBV_QP_INIT_ATTR_PD;
	attr_ex.send_cq = attr.send_cq;
	attr_ex.recv_cq = attr.recv_cq;
	attr_ex.cap.max_send_wr = attr.cap.max_send_wr;
	attr_ex.cap.max_send_sge = attr.cap.max_send_sge;
	attr_ex.qp_type = attr.qp_type;
	attr_ex.srq = attr.srq;
	attr_ex.cap.max_inline_data = attr.cap.max_inline_data;
	attr_ex.cap.max_recv_wr  = attr.cap.max_recv_wr;
	attr_ex.cap.max_recv_sge = attr.cap.max_recv_sge;
	#endif

	if (user_param->work_rdma_cm) {
		#ifdef HAVE_IBV_WR_API
		if (!user_param->use_old_post_send)
		{
			if (rdma_create_qp_ex(ctx->cm_id, &attr_ex))
			{
				fprintf(stderr, "Couldn't create rdma new QP - %s\n", strerror(errno));
			}
			else
			{
				qp = ctx->cm_id->qp;
			}
		}
		else
		#endif
			if (rdma_create_qp(ctx->cm_id, ctx->pd, &attr))
			{
				fprintf(stderr, "Couldn't create rdma old QP - %s\n", strerror(errno));
			}
			else
			{
				qp = ctx->cm_id->qp;
			}

	} else if (user_param->connection_type == SRD) {
		#ifdef HAVE_SRD
		#ifdef HAVE_IBV_WR_API
		efa_attr.driver_qp_type = EFADV_QP_DRIVER_TYPE_SRD;
		qp = efadv_create_qp_ex(ctx->context, &attr_ex,
					&efa_attr, sizeof(efa_attr));
		#else
		qp = efadv_create_driver_qp(ctx->pd, &attr,
					    EFADV_QP_DRIVER_TYPE_SRD);
		#endif
		#endif
	} else {
		#ifdef HAVE_IBV_WR_API
		if (!user_param->use_old_post_send)
		{
			#ifdef HAVE_MLX5DV
			if (user_param->connection_type == DC)
			{
				attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_DC;

				if (is_dc_server_side)
				{
					attr_ex.srq = ctx->srq;
					attr_dv.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCT;
					attr_dv.dc_init_attr.dct_access_key = DC_KEY;
					attr_ex.comp_mask &= ~IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
				}
				else
				{
					attr_dv.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCI;
					attr_dv.create_flags |= MLX5DV_QP_CREATE_DISABLE_SCATTER_TO_CQE;
					attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;
					attr_ex.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
					#ifdef HAVE_DCS
					if (user_param->log_dci_streams) {
						attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_DCI_STREAMS;
						attr_dv.dc_init_attr.dci_streams.log_num_concurent = user_param->log_dci_streams;
					}
					#endif
				}
				qp = mlx5dv_create_qp(ctx->context, &attr_ex, &attr_dv);
			}
			#ifdef HAVE_AES_XTS
			else if (user_param->aes_xts) {
				attr_ex.cap.max_send_wr = user_param->tx_depth * 2;
				attr_ex.cap.max_inline_data = AES_XTS_INLINE;
				attr_dv.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS;
				attr_dv.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE;
				attr_dv.create_flags = MLX5DV_QP_CREATE_DISABLE_SCATTER_TO_CQE;
				qp = mlx5dv_create_qp(ctx->context, &attr_ex, &attr_dv);
			}
			#endif // HAVE_AES_XTS
			else
			#endif // HAVE_MLX5DV

			#ifdef HAVE_HNSDV
			if (user_param->congest_type) {
				hns_attr.comp_mask = HNSDV_QP_INIT_ATTR_MASK_QP_CONGEST_TYPE;
				hns_attr.congest_type = user_param->congest_type;
				qp = hnsdv_create_qp(ctx->context, &attr_ex, &hns_attr);
			}
			else
			#endif //HAVE_HNSDV
				qp = ibv_create_qp_ex(ctx->context, &attr_ex);
		}
		else
		#endif // HAVE_IBV_WR_API
			qp = ibv_create_qp(ctx->pd, &attr);
	}

	if (qp == NULL && errno == ENOMEM) {
		fprintf(stderr, "Requested QP size might be too big. Try reducing TX depth and/or inline size.\n");
		fprintf(stderr, "Current TX depth is %d and inline size is %d .\n", user_param->tx_depth, user_param->inline_size);
	}

	#ifdef HAVE_IBV_WR_API
	if (!user_param->use_old_post_send)
		qp_cap = &attr_ex.cap;
	#endif

	if (user_param->inline_size > qp_cap->max_inline_data) {
		printf("  Actual inline-size(%d) < requested inline-size(%d)\n",
			qp_cap->max_inline_data, user_param->inline_size);
		user_param->inline_size = qp_cap->max_inline_data;
	}

	return qp;
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param, int qp_index)
{
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;

	struct ibv_qp_attr attr;
	int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;
	int is_dc_server_side = 0;

	int ret = 0;

	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state        = IBV_QPS_INIT;
	attr.pkey_index      = user_param->pkey_index;

	if ((user_param->connection_type == DC || user_param->use_xrc) && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	is_dc_server_side = ((!(user_param->duplex || user_param->tst == LAT) &&
						 (user_param->machine == SERVER)) ||
						 ((user_param->duplex || user_param->tst == LAT) &&
						 (qp_index >= num_of_qps)));

	if (user_param->dualport==ON) {
		static int portindex=0;  /* for dual-port support */
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

	if (user_param->connection_type == RawEth) {
		flags = IBV_QP_STATE | IBV_QP_PORT;

	} else if (user_param->connection_type == UD || user_param->connection_type == SRD) {
		attr.qkey = DEFF_QKEY;
		flags |= IBV_QP_QKEY;
	} else if (!(user_param->connection_type == DC &&
			!is_dc_server_side)) {
		switch (user_param->verb) {
			case ATOMIC: attr.qp_access_flags = IBV_ACCESS_REMOTE_ATOMIC; break;
			case READ  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
			case WRITE_IMM:
			case WRITE :
				     attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
			case SEND  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
		}
		flags |= IBV_QP_ACCESS_FLAGS;
	}
	ret = ibv_modify_qp(qp, &attr, flags);

	if (ret) {
		fprintf(stderr, "Failed to modify QP to INIT, ret=%d\n",ret);
		return 1;
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_qp_to_rtr(struct ibv_qp *qp,
		struct ibv_qp_attr *attr,
		struct perftest_parameters *user_param,
		struct pingpong_dest *dest,
		struct pingpong_dest *my_dest,
		int qp_index)
{
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;
	int is_dc_server_side = 0;
	int flags = IBV_QP_STATE;
	int ooo_flags = 0;

	attr->qp_state = IBV_QPS_RTR;
	attr->ah_attr.src_path_bits = 0;

	/* in xrc with bidirectional,
	 * there are send qps and recv qps. the actual number of send/recv qps
	 * is num_of_qps / 2.
	 */
	if ((user_param->connection_type == DC || user_param->use_xrc) && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}
	is_dc_server_side = ((!(user_param->duplex || user_param->tst == LAT) &&
						 (user_param->machine == SERVER)) ||
						  ((user_param->duplex || user_param->tst == LAT) &&
						 (qp_index >= num_of_qps)));
	/* first half of qps are for ib_port and second half are for ib_port2
	 * in xrc with bidirectional, the first half of qps are xrc_send qps and
	 * the second half are xrc_recv qps. the first half of the send/recv qps
	 * are for ib_port1 and the second half are for ib_port2
	 */
	if (user_param->dualport == ON && (qp_index % num_of_qps >= num_of_qps_per_port))
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
			attr->ah_attr.grh.flow_label = user_param->flow_label;
		}
		if (user_param->connection_type != UD && user_param->connection_type != SRD) {
			if (user_param->connection_type == DC) {
				attr->path_mtu = user_param->curr_mtu;
				flags |= IBV_QP_AV | IBV_QP_PATH_MTU;
				if (is_dc_server_side)
				{
					attr->min_rnr_timer = MIN_RNR_TIMER;
					flags |= IBV_QP_MIN_RNR_TIMER;
				} //DCT
			}
			else {
				attr->path_mtu = user_param->curr_mtu;
				attr->dest_qp_num = dest->qpn;
				attr->rq_psn = dest->psn;

				flags |= (IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);

				if (user_param->connection_type == RC || user_param->connection_type == XRC) {

					attr->max_dest_rd_atomic = my_dest->out_reads;
					attr->min_rnr_timer = MIN_RNR_TIMER;
					flags |= (IBV_QP_MIN_RNR_TIMER | IBV_QP_MAX_DEST_RD_ATOMIC);
				}
			}
		}
	}
	else if (user_param->raw_qos) {
		attr->ah_attr.sl = user_param->sl;
		flags |= IBV_QP_AV;
	}

	#ifdef HAVE_OOO_ATTR
		ooo_flags |= IBV_QP_OOO_RW_DATA_PLACEMENT;
	#endif

	if (user_param->use_ooo)
		flags |= ooo_flags;
	return ibv_modify_qp(qp, attr, flags);
}

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_modify_qp_to_rts(struct ibv_qp *qp,
		struct ibv_qp_attr *attr,
		struct perftest_parameters *user_param,
		struct pingpong_dest *dest,
		struct pingpong_dest *my_dest)
{
	int flags = IBV_QP_STATE;

	attr->qp_state = IBV_QPS_RTS;

	if (user_param->connection_type != RawEth) {

		flags |= IBV_QP_SQ_PSN;
		attr->sq_psn = my_dest->psn;

		if (user_param->connection_type == DC ||
			user_param->connection_type == RC ||
			user_param->connection_type == XRC) {

			attr->timeout   = user_param->qp_timeout;
			attr->retry_cnt = 7;
			attr->rnr_retry = 7;
			attr->max_rd_atomic  = dest->out_reads;
			flags |= (IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
		}
	}

	#ifdef HAVE_PACKET_PACING
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
	struct ibv_qp_attr attr;
	int xrc_offset = 0;

	if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT)) {
		xrc_offset = user_param->num_of_qps / 2;
	}
	for (i=0; i < user_param->num_of_qps; i++) {


		memset(&attr, 0, sizeof attr);

		if (user_param->rate_limit_type == HW_RATE_LIMIT)
			attr.ah_attr.static_rate = user_param->valid_hw_rate_limit_index;

		#if defined (HAVE_PACKET_PACING)
		if (user_param->rate_limit_type == PP_RATE_LIMIT) {
			if (check_packet_pacing_support(ctx) == FAILURE) {
				fprintf(stderr, "Packet Pacing isn't supported.\n");
				return FAILURE;
			}
		}
		#endif

		if ((i >= xrc_offset) && (user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT))
			xrc_offset = -1*xrc_offset;


		if(ctx_modify_qp_to_rtr(ctx->qp[i], &attr, user_param, &dest[xrc_offset + i], &my_dest[i], i)) {
			fprintf(stderr, "Failed to modify QP %d to RTR\n",ctx->qp[i]->qp_num);
			return FAILURE;
		}
		if (user_param->connection_type == DC) {
			if ( ((!(user_param->duplex || user_param->tst == LAT) && (user_param->machine == SERVER) )
				|| ((user_param->duplex || user_param->tst == LAT) && (i >= user_param->num_of_qps/2)))) {
				continue;
			}
		}
		if (user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex) {
			if(ctx_modify_qp_to_rts(ctx->qp[i], &attr, user_param, &dest[xrc_offset + i], &my_dest[i])) {
				fprintf(stderr, "Failed to modify QP to RTS\n");
				return FAILURE;
			}
		}

		if (((user_param->connection_type == UD || user_param->connection_type == DC || user_param->connection_type == SRD) &&
				(user_param->tst == LAT || user_param->machine == CLIENT || user_param->duplex)) ||
				(user_param->connection_type == SRD && (user_param->verb == READ || user_param->verb == WRITE ||
									user_param->verb == WRITE_IMM))) {

			ctx->ah[i] = ibv_create_ah(ctx->pd,&(attr.ah_attr));

			if (!ctx->ah[i]) {
				fprintf(stderr, "Failed to create AH\n");
				return FAILURE;
			}
			user_param->ah_allocated = 1;
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
	ctx_set_send_reg_wqes(ctx,user_param,rem_dest);
}

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
	int use_enc = user_param->aes_xts;
	switch (user_param->connection_type) {
	case DC:
		switch (user_param->verb) {
			case SEND:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_inl_dc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_sge_dc;
				}
				break;
			case WRITE_IMM:
			case WRITE:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_inl_dc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_sge_dc;
				}
				break;
			case READ:
				ctx->new_post_send_work_request_func_pointer = &new_post_read_sge_dc;
				break;
			case ATOMIC:
				if (user_param->atomicType == FETCH_AND_ADD) {
					ctx->new_post_send_work_request_func_pointer = &new_post_atomic_fa_sge_dc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_atomic_cs_sge_dc;
				}
				break;
			default:
				fprintf(stderr, "The post send properties are not supported on DC. \n");
		}
		break;
	case RC:
		switch (user_param->verb) {
			case SEND:
				if (use_enc) {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_sge_enc_rc;
				}
				else if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_inl_rc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_sge_rc;
				}
				break;
			case WRITE_IMM:
			case WRITE:
				if(use_enc) {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_sge_enc_rc;
				}
				else if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_inl_rc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_sge_rc;
				}
				break;
			case READ:
				if(use_enc) {
					ctx->new_post_send_work_request_func_pointer = &new_post_read_sge_enc_rc;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_read_sge_rc;
				}
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
			case WRITE_IMM:
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
			case WRITE_IMM:
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
	case SRD:
		switch (user_param->verb) {
			case SEND:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_inl_srd;
				}
				else {
					ctx->new_post_send_work_request_func_pointer = &new_post_send_sge_srd;
				}
				break;
			case READ:
				ctx->new_post_send_work_request_func_pointer = &new_post_read_sge_srd;
				break;
			case WRITE_IMM:
			case WRITE:
				if (use_inl) {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_inl_srd;
				} else {
					ctx->new_post_send_work_request_func_pointer = &new_post_write_sge_srd;
				}
				break;
			default:
				fprintf(stderr, "The post send properties are not supported on SRD.\n");
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
	uint32_t remote_qkey;

	if((user_param->use_xrc || user_param->connection_type == DC) && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		xrc_offset = num_of_qps;
	}

	for (i = 0; i < num_of_qps ; i++) {
		if (user_param->connection_type == DC)
		{
			ctx->r_dctn[i] = rem_dest[xrc_offset + i].qpn;
		}
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

		if (user_param->verb == WRITE || user_param->verb == WRITE_IMM || user_param->verb == READ)
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
			if (user_param->use_null_mr) {
				ctx->sge_list[i*user_param->post_list + j].lkey = ctx->null_mr->lkey;
			}

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
			} else {
				ctx->wr[i*user_param->post_list + j].send_flags = 0;
			}

			if (user_param->verb == ATOMIC) {
				ctx->wr[i*user_param->post_list + j].opcode = opcode_atomic_array[user_param->atomicType];
			}
			else {
				ctx->wr[i*user_param->post_list + j].opcode = opcode_verbs_array[user_param->verb];
			}
			if (user_param->verb == WRITE || user_param->verb == WRITE_IMM || user_param->verb == READ) {

				ctx->wr[i*user_param->post_list + j].wr.rdma.rkey = rem_dest[xrc_offset + i].rkey;
				if (user_param->connection_type == SRD)
					ctx->rem_qpn[xrc_offset + i] = rem_dest[xrc_offset + i].qpn;
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
						ctx->rem_qpn[xrc_offset + i] = ctx->cma_master.nodes[i].remote_qpn;
						remote_qkey = ctx->cma_master.nodes[i].remote_qkey;
					} else {
						ctx->rem_qpn[xrc_offset + i] = rem_dest[xrc_offset + i].qpn;
						remote_qkey = DEF_QKEY;
					}
					ctx->wr[i*user_param->post_list + j].wr.ud.remote_qkey = remote_qkey;
					ctx->wr[i*user_param->post_list + j].wr.ud.remote_qpn = ctx->rem_qpn[xrc_offset + i];
				}
			}

			if ((user_param->verb == SEND || user_param->verb == WRITE || user_param->verb == WRITE_IMM) && user_param->size <= user_param->inline_size)
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
	int			size_per_qp = user_param->rx_depth / user_param->recv_post_list;
	int mtu = MTU_SIZE(user_param->curr_mtu);
	uint64_t length = user_param->use_srq == ON ? (((SIZE(user_param->connection_type ,user_param->size, 1) + mtu - 1 )/ mtu) * mtu) : SIZE(user_param->connection_type, user_param->size, 1);

	if((user_param->use_xrc || user_param->connection_type == DC) &&
				(user_param->duplex || user_param->tst == LAT)) {

		i = user_param->num_of_qps / 2;
		num_of_qps /= 2;
	}

	if (user_param->use_srq)
		size_per_qp /= user_param->num_of_qps;
	ctx->rposted = size_per_qp * user_param->recv_post_list;

	for (k = 0; i < user_param->num_of_qps; i++,k++) {
		if (!user_param->mr_per_qp) {
			ctx->recv_sge_list[i * user_param->recv_post_list].addr = (uintptr_t)ctx->buf[0] +
				(num_of_qps + k) * ctx->send_qp_buff_size;
		} else {
			ctx->recv_sge_list[i * user_param->recv_post_list].addr = (uintptr_t)ctx->buf[i];
		}

		if (user_param->connection_type == UD)
			ctx->recv_sge_list[i * user_param->recv_post_list].addr += (ctx->cache_line_size - UD_ADDITION);

		ctx->rx_buffer_addr[i] = ctx->recv_sge_list[i * user_param->recv_post_list].addr;

		for (j = 0; j < user_param->recv_post_list; j++) {
			ctx->recv_sge_list[i * user_param->recv_post_list + j].length = length;
			ctx->recv_sge_list[i * user_param->recv_post_list + j].lkey   = ctx->mr[i]->lkey;

			if (j > 0) {
				ctx->recv_sge_list[i * user_param->recv_post_list + j].addr = ctx->recv_sge_list[i * user_param->recv_post_list + j - 1].addr;

				if ((user_param->tst == BW || user_param->tst == LAT_BY_BW) && user_param->size <= (ctx->cycle_buffer / 2)) {
					increase_loc_addr(&ctx->recv_sge_list[i * user_param->recv_post_list + j],
							user_param->size,
							j,
							ctx->rx_buffer_addr[i],
							user_param->connection_type,ctx->cache_line_size,ctx->cycle_buffer);
				}
			}

			ctx->rwr[i * user_param->recv_post_list + j].sg_list = &ctx->recv_sge_list[i * user_param->recv_post_list + j];
			ctx->rwr[i * user_param->recv_post_list + j].num_sge = MAX_RECV_SGE;
			ctx->rwr[i * user_param->recv_post_list + j].wr_id   = i;

			if (j == (user_param->recv_post_list - 1))
				ctx->rwr[i * user_param->recv_post_list + j].next = NULL;
			else
				ctx->rwr[i * user_param->recv_post_list + j].next = &ctx->rwr[i * user_param->recv_post_list + j + 1];
		}

		for (j = 0; j < size_per_qp ; ++j) {

			if (user_param->use_srq) {

				if (ibv_post_srq_recv(ctx->srq,&ctx->rwr[i * user_param->recv_post_list], &bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv SRQ = %d: counter=%d\n",i,j);
					return 1;
				}

			} else {

				if (ibv_post_recv(ctx->qp[i],&ctx->rwr[i * user_param->recv_post_list],&bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv Qp = %d: counter=%d\n",i,j);
					return 1;
				}
			}

			if (user_param->recv_post_list == 1 && (user_param->tst == BW || user_param->tst == LAT_BY_BW) &&
					user_param->size <= (ctx->cycle_buffer / 2)) {
				increase_loc_addr(&ctx->recv_sge_list[i * user_param->recv_post_list],
						user_param->size,
						j,
						ctx->rx_buffer_addr[i],
						user_param->connection_type,ctx->cache_line_size,ctx->cycle_buffer);
			}
		}
		ctx->recv_sge_list[i * user_param->recv_post_list].addr = ctx->rx_buffer_addr[i];
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

	ALLOCATE(ctx->ctrl_buf,uint32_t,2*user_param->num_of_qps);
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
	int 		i = 0, sne;
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
	struct ibv_wc 		wc;
	struct ibv_wc 		*wc_for_cleaning = NULL;
	int 			num_of_qps = user_param->num_of_qps;
	int			return_value = 0;

	if(user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	warmupsession = (user_param->post_list == 1) ? user_param->tx_depth : user_param->post_list;
	ALLOCATE(wc_for_cleaning,struct ibv_wc,user_param->tx_depth);

#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
#endif

	/* Clean up the pipe */
	ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth,wc_for_cleaning);

	for (index=0 ; index < num_of_qps ; index++) {

		for (warmindex = 0 ;warmindex < warmupsession ;warmindex += user_param->post_list) {

			err = post_send_method(ctx, index, user_param);
			if (err) {
				fprintf(stderr,"Couldn't post send during warm up: qp %d scnt=%d \n",index,warmindex);
				return_value = FAILURE;
				goto cleaning;
			}
		}

		do {

			ne = ibv_poll_cq(ctx->send_cq,1,&wc);
			if (ne > 0) {

				//coverity[uninit_use]
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

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bw(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t           	totscnt = 0;
	uint64_t       	   	totccnt = 0;
	int                	i = 0;
	int			index;
	int			ne = 0;
	uint64_t	   	tot_iters;
	int			err = 0;
	struct ibv_wc 	   	*wc = NULL;
	int 			num_of_qps = user_param->num_of_qps;
	/* Rate Limiter*/
	int 			rate_limit_pps = 0;
	double 			gap_time = 0;	/* in usec */
	cycles_t 		gap_cycles = 0;	/* in cycles */
	cycles_t 		gap_deadline = 0;
	double 		number_of_bursts = 0;
	int 			burst_iter = 0;
	int 			is_sending_burst = 0;
	int 			cpu_mhz = 0;
	int 			return_value = 0;
	int			wc_id;
	int			send_flows_index = 0;
	uintptr_t		primary_send_addr = ctx->sge_list[0].addr;
	int			address_offset = 0;
	int			flows_burst_iter = 0;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

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
		number_of_bursts = (double)rate_limit_pps / (double)user_param->burst_size;
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
					(ctx->scnt[index] + user_param->post_list) <= (user_param->tx_depth + ctx->ccnt[index]) &&
					!((user_param->rate_limit_type == SW_RATE_LIMIT ) && is_sending_burst == 0)) {

				if (ctx->send_rcredit) {
					uint32_t swindow = ctx->scnt[index] + user_param->post_list - ctx->credit_buf[index];
					if (swindow >= user_param->rx_depth)
						break;
				}
				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)
					&& !(ctx->scnt[index] == (user_param->iters - 1) && user_param->test_type == ITERATIONS)) {

					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;
				}

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && user_param->state == END_STATE)
					break;

				err = post_send_method(ctx, index, user_param);
				if (err) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%lu \n",index,ctx->scnt[index]);
					return_value = FAILURE;
					goto cleaning;
				}

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
						increase_loc_addr(ctx->wr[index].sg_list,user_param->size, ctx->scnt[index],
								ctx->my_addr[index] + address_offset , 0, ctx->cache_line_size,
								ctx->cycle_buffer);

					if (user_param->verb != SEND) {
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
						ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
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
				/* Make sure all completions from previous event were polled before waiting for another */
				if (user_param->use_event && ne == 0) {
					if (ctx_notify_events(ctx->send_channel)) {
						fprintf(stderr, "Couldn't request CQ notification\n");
						return_value = FAILURE;
						goto cleaning;
					}
				}
				ne = ibv_poll_cq(ctx->send_cq, CTX_POLL_BATCH, wc);
				if (ne > 0) {
					for (i = 0; i < ne; i++) {
						wc_id = (int)wc[i].wr_id;

						if (wc[i].status != IBV_WC_SUCCESS) {
							NOTIFY_COMP_ERROR_SEND(wc[i],totscnt,totccnt);
							return_value = FAILURE;
							goto cleaning;
						}
						int fill = user_param->cq_mod;
						if (user_param->fill_count && ctx->ccnt[wc_id] + user_param->cq_mod > user_param->iters) {
							fill = user_param->iters - ctx->ccnt[wc_id];
						}
						ctx->ccnt[wc_id] += fill;
						totccnt += fill;

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
	uint64_t                *unused_recv_for_qp = NULL;
	uint64_t                *posted_per_qp = NULL;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr  	*bad_wr_recv = NULL;
	struct ibv_wc 		*swc = NULL;
	long 			*scredit_for_qp = NULL;
	int 			tot_scredit = 0;
	int 			firstRx = 1;
	int 			return_value = 0;
	int			wc_id;
	int			recv_flows_index = 0;
	uintptr_t		primary_recv_addr = ctx->recv_sge_list[0].addr;
	int			recv_flows_burst = 0;
	int			address_flows_offset =0;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);
	ALLOCATE(swc ,struct ibv_wc ,user_param->tx_depth);

	ALLOCATE(rcnt_for_qp,uint64_t,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	/* Number of receive WQEs available to be posted per QP.
	 * Start with zero as all receive buffers are pre-posted.
	 * Useful for recv_post_list mode. */
	ALLOCATE(unused_recv_for_qp, uint64_t, user_param->num_of_qps);
	memset(unused_recv_for_qp, 0, sizeof(uint64_t) * user_param->num_of_qps);

	ALLOCATE(scredit_for_qp,long,user_param->num_of_qps);
	memset(scredit_for_qp,0,sizeof(long)*user_param->num_of_qps);

	ALLOCATE(posted_per_qp, uint64_t, user_param->num_of_qps);
	for (i = 0; i < user_param->num_of_qps; i++)
		posted_per_qp[i] = ctx->rposted;

	tot_iters = (uint64_t)user_param->iters*user_param->num_of_qps;

	if (user_param->test_type == ITERATIONS) {
		check_alive_data.is_events = user_param->use_event;
		signal(SIGALRM, check_alive);
		alarm(60);
	}

	check_alive_data.g_total_iters = tot_iters;

	while (rcnt < tot_iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->recv_channel)) {
				fprintf(stderr ," Failed to notify events to CQ\n");
				return_value = FAILURE;
				goto cleaning;
			}
		}

		do {
			if (user_param->test_type == DURATION && user_param->state == END_STATE)
				break;

			ne = ibv_poll_cq(ctx->recv_cq,CTX_POLL_BATCH,wc);

			if (ne > 0) {
				if (firstRx) {
					set_on_first_rx_packet(user_param);
					firstRx = 0;
				}

				for (i = 0; i < ne; i++) {
					wc_id = (int)wc[i].wr_id;
					if (wc[i].status != IBV_WC_SUCCESS) {

						NOTIFY_COMP_ERROR_RECV(wc[i],rcnt_for_qp[wc_id]);
						return_value = FAILURE;
						goto cleaning;
					}
					rcnt_for_qp[wc_id]++;
					rcnt++;
					unused_recv_for_qp[wc_id]++;
					check_alive_data.current_totrcnt = rcnt;

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE) {
						if (user_param->report_per_port) {
							user_param->iters_per_port[user_param->port_by_qp[wc_id]]++;
						}
						user_param->iters++;
					}
					//coverity[uninit_use]
					if ((user_param->test_type==DURATION || posted_per_qp[wc_id] + user_param->recv_post_list <= user_param->iters) &&
							unused_recv_for_qp[wc_id] >= user_param->recv_post_list) {
						if (user_param->use_srq) {
							if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc_id * user_param->recv_post_list], &bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv SRQ. QP = %d: counter=%lu\n", wc_id,rcnt);
								return_value = FAILURE;
								goto cleaning;
							}

						} else {
							if (ibv_post_recv(ctx->qp[wc_id], &ctx->rwr[wc_id * user_param->recv_post_list], &bad_wr_recv)) {
								fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%lu\n",wc_id,rcnt_for_qp[wc_id]);
								return_value = 15;
								goto cleaning;
							}
						}
						unused_recv_for_qp[wc_id] -= user_param->recv_post_list;
						posted_per_qp[wc_id] += user_param->recv_post_list;

						if (user_param->flows != DEF_FLOWS) {
							if (++recv_flows_burst == user_param->flows_burst) {
								recv_flows_burst = 0;
								if (++recv_flows_index == user_param->flows)
									recv_flows_index = 0;
								address_flows_offset = recv_flows_index * ctx->cycle_buffer;
								ctx->recv_sge_list[0].addr = primary_recv_addr + address_flows_offset;
							}
						}
						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (ctx->cycle_buffer / 2) &&
								user_param->recv_post_list == 1) {
							increase_loc_addr(ctx->rwr[wc_id].sg_list,
									user_param->size,
									posted_per_qp[wc_id],
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
											fprintf(stderr, "Poll send CQ error status=%u qp %d credit=%lu scredit=%ld\n",
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
	free(unused_recv_for_qp);
	free(posted_per_qp);

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
	int 			index, ne;
	int 			err = 0;
	int			wc_id;
	uint64_t		*scnt_for_qp = NULL;
	struct ibv_wc 		*wc = NULL;
	int 			num_of_qps = user_param->num_of_qps;
	int 			return_value = 0;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);
	ALLOCATE(scnt_for_qp,uint64_t,user_param->num_of_qps);
	memset(scnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	duration_param=user_param;

	pthread_t print_thread;
	if (pthread_create(&print_thread, NULL, &handle_signal_print_thread,(void*)&user_param->duration) != 0){
		printf("Fail to create thread \n");
		free(wc);
		free(scnt_for_qp);
		return FAILURE;
	}

	user_param->iters = 0;
	user_param->last_iters = 0;

	/* Will be 0, in case of Duration (look at force_dependencies or in the exp above) */
	if (user_param->duplex && (user_param->use_xrc || user_param->connection_type == DC))
		num_of_qps /= 2;

	user_param->tposted[0] = get_cycles();

	/* main loop for posting */
	while (1) {
	/* main loop to run over all the qps and post each time n messages */
		for (index = 0 ; index < num_of_qps ; index++) {

			while ((ctx->scnt[index] - ctx->ccnt[index] + user_param->post_list) <= user_param->tx_depth) {
				if (ctx->send_rcredit) {
					uint32_t swindow = scnt_for_qp[index] + user_param->post_list - ctx->credit_buf[index];
					if (swindow >= user_param->rx_depth)
						break;
				}

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)) {
					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;
				}

				err = post_send_method(ctx, index, user_param);
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
					ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
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
					wc_id = (int)wc[i].wr_id;
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
	uint64_t                *unused_recv_for_qp = NULL;
	int                     *scredit_for_qp = NULL;
	int 			return_value = 0;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	ALLOCATE(wc ,struct ibv_wc ,CTX_POLL_BATCH);
	ALLOCATE(swc ,struct ibv_wc ,user_param->tx_depth);

	ALLOCATE(rcnt_for_qp,uint64_t,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	ALLOCATE(ccnt_for_qp,uint64_t,user_param->num_of_qps);
	memset(ccnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	/* Number of receive WQEs available to be posted per QP.
	 * Start with zero as all receive buffers are pre-posted.
	 * Useful for recv_post_list mode. */
	ALLOCATE(unused_recv_for_qp, uint64_t, user_param->num_of_qps);
	memset(unused_recv_for_qp, 0, sizeof(uint64_t) * user_param->num_of_qps);

	ALLOCATE(scredit_for_qp,int,user_param->num_of_qps);
	memset(scredit_for_qp,0,sizeof(int)*user_param->num_of_qps);

	duration_param=user_param;
	pthread_t print_thread;
	if (pthread_create(&print_thread, NULL, &handle_signal_print_thread, (void *)&user_param->duration) != 0)
	{
		printf("Fail to create thread \n");
		return_value = FAILURE;
		goto cleaning;
	}

	user_param->iters = 0;
	user_param->last_iters = 0;
	user_param->tposted[0] = get_cycles();

	while (1) {

		ne = ibv_poll_cq(ctx->recv_cq,CTX_POLL_BATCH,wc);

		if (ne > 0) {

			for (i = 0; i < ne; i++) {

				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr,"A completion with Error in run_infinitely_bw_server function");
					return_value = FAILURE;
					goto cleaning;
				}
				user_param->iters++;
				unused_recv_for_qp[wc[i].wr_id]++;
				if (unused_recv_for_qp[wc[i].wr_id] >= user_param->recv_post_list) {
					if (user_param->use_srq) {
						if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc[i].wr_id * user_param->recv_post_list],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv SRQ. QP = %d:\n",(int)wc[i].wr_id);
							return_value = FAILURE;
							goto cleaning;
						}

					} else {
						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id * user_param->recv_post_list],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d\n",(int)wc[i].wr_id);
							return_value = 15;
							goto cleaning;
						}
					}
					unused_recv_for_qp[wc[i].wr_id] -= user_param->recv_post_list;
				}

				if (!user_param->use_srq && ctx->send_rcredit) {
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
	free(unused_recv_for_qp);
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
	int			send_ne = 0;
	int			recv_ne = 0;
	int 			err = 0;
	uint64_t 		*rcnt_for_qp = NULL;
	uint64_t 		*unused_recv_for_qp = NULL;
	uint64_t		*posted_per_qp = NULL;
	uint64_t 		tot_iters = 0;
	uint64_t 		iters = 0;
	int 			tot_scredit = 0;
	int 			*scredit_for_qp = NULL;
	struct ibv_wc 		*wc = NULL;
	struct ibv_wc 		*wc_tx = NULL;
	struct ibv_recv_wr 	*bad_wr_recv = NULL;
	int 			num_of_qps = user_param->num_of_qps;
	/* This is to ensure SERVER will not start to send packets before CLIENT start the test. */
	int 			before_first_rx = ON;
	int 			return_value = 0;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	ALLOCATE(wc_tx,struct ibv_wc,CTX_POLL_BATCH);
	ALLOCATE(rcnt_for_qp,uint64_t,user_param->num_of_qps);
	ALLOCATE(scredit_for_qp,int,user_param->num_of_qps);
	ALLOCATE(wc,struct ibv_wc,user_param->rx_depth);

	memset(rcnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);
	memset(scredit_for_qp,0,sizeof(int)*user_param->num_of_qps);

	/* Number of receive WQEs available to be posted per QP.
	 * Start with zero as all receive buffers are pre-posted.
	 * Useful for recv_post_list mode. */
	ALLOCATE(unused_recv_for_qp, uint64_t, user_param->num_of_qps);
	memset(unused_recv_for_qp, 0, sizeof(uint64_t) * user_param->num_of_qps);

	ALLOCATE(posted_per_qp, uint64_t, user_param->num_of_qps);
	for (i = 0; i < user_param->num_of_qps; i++)
		posted_per_qp[i] = ctx->rposted;

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
					((ctx->scnt[index] + scredit_for_qp[index] - ctx->ccnt[index] + user_param->post_list) <= user_param->tx_depth)) {
				if (ctx->send_rcredit) {
					uint32_t swindow = ctx->scnt[index] + user_param->post_list - ctx->credit_buf[index];
					if (swindow >= user_param->rx_depth)
						break;
				}
				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)
					&& !(ctx->scnt[index] == (user_param->iters - 1) && user_param->test_type == ITERATIONS)) {
					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;
				}
				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && duration_param->state == END_STATE)
					break;

				err = post_send_method(ctx, index, user_param);

				if (err) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%lu \n",index,ctx->scnt[index]);
					return_value = FAILURE;
					goto cleaning;
				}

				if (user_param->post_list == 1 && user_param->size <= (ctx->cycle_buffer / 2)) {
					increase_loc_addr(ctx->wr[index].sg_list,user_param->size,ctx->scnt[index],
						ctx->my_addr[index],0,ctx->cache_line_size,ctx->cycle_buffer);
				}

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->post_list == 1 &&
					(ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 ||
						(user_param->test_type == ITERATIONS && ctx->scnt[index] == iters-1))) {

					ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
				}
			}
		}
		/* Make sure all completions from previous event were polled before waiting for another */
		if (user_param->use_event && recv_ne == 0 && send_ne == 0) {
			if (ctx_notify_send_recv_events(ctx)) {
				return_value = FAILURE;
				goto cleaning;
			}
		}

		recv_ne = ibv_poll_cq(ctx->recv_cq, user_param->rx_depth, wc);
		if (recv_ne > 0) {

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

			for (i = 0; i < recv_ne; i++) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_RECV(wc[i],totrcnt);
					return_value = FAILURE;
					goto cleaning;
				}

				rcnt_for_qp[wc[i].wr_id]++;
				unused_recv_for_qp[wc[i].wr_id]++;
				totrcnt++;
				check_alive_data.current_totrcnt = totrcnt;

				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE) {
					if (user_param->report_per_port) {
						user_param->iters_per_port[user_param->port_by_qp[(int)wc[i].wr_id]]++;
					}
					user_param->iters++;
				}

				if ((user_param->test_type==DURATION || posted_per_qp[wc[i].wr_id] + user_param->recv_post_list <= user_param->iters) &&
						unused_recv_for_qp[wc[i].wr_id] >= user_param->recv_post_list) {
					if (user_param->use_srq) {
						if (ibv_post_srq_recv(ctx->srq, &ctx->rwr[wc[i].wr_id * user_param->recv_post_list],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv SRQ. QP = %d: counter=%d\n",(int)wc[i].wr_id,(int)totrcnt);
							return_value = FAILURE;
							goto cleaning;
						}

					} else {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id * user_param->recv_post_list],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%lu\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
							return_value = 15;
							goto cleaning;
						}
					}
					unused_recv_for_qp[wc[i].wr_id] -= user_param->recv_post_list;
					//coverity[uninit_use]
					posted_per_qp[wc[i].wr_id] += user_param->recv_post_list;

					if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (ctx->cycle_buffer / 2) &&
							user_param->recv_post_list == 1) {
						increase_loc_addr(ctx->rwr[wc[i].wr_id].sg_list,
								user_param->size,
								posted_per_qp[wc[i].wr_id],
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

						while ((ctx->scnt[wc[i].wr_id] + scredit_for_qp[wc[i].wr_id]) >= (user_param->tx_depth + ctx->ccnt[wc[i].wr_id])) {
							sne = ibv_poll_cq(ctx->send_cq, 1, &credit_wc);
							if (sne > 0) {
								if (credit_wc.status != IBV_WC_SUCCESS) {
									fprintf(stderr, "Poll send CQ error status=%u qp %d credit=%lu scredit=%d\n",
											credit_wc.status,(int)credit_wc.wr_id,
											rcnt_for_qp[credit_wc.wr_id],scredit_for_qp[credit_wc.wr_id]);
									return_value = FAILURE;
									goto cleaning;
								}

								//coverity[uninit_use]
								if (credit_wc.opcode == IBV_WC_RDMA_WRITE) {
									scredit_for_qp[credit_wc.wr_id]--;
									tot_scredit--;
								} else  {
									totccnt += user_param->cq_mod;
									ctx->ccnt[(int)credit_wc.wr_id] += user_param->cq_mod;

									if (user_param->noPeak == OFF) {
										if ((user_param->test_type == ITERATIONS && (totccnt > tot_iters)))
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

		} else if (recv_ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", recv_ne);
			return_value = FAILURE;
			goto cleaning;
		}
		else {
			if (check_alive_data.to_exit) {
				user_param->check_alive_exited = 1;
				return_value = FAILURE;
				goto cleaning;
			}
		}

		send_ne = ibv_poll_cq(ctx->send_cq, CTX_POLL_BATCH, wc_tx);

		if (send_ne > 0) {
			for (i = 0; i < send_ne; i++) {
				if (wc_tx[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_SEND(wc_tx[i],totscnt,totccnt);
					return_value = FAILURE;
					goto cleaning;
				}

				if (wc_tx[i].opcode == IBV_WC_RDMA_WRITE && user_param->verb != WRITE_IMM) {
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

						if ((user_param->test_type == ITERATIONS && (totccnt > tot_iters)))
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

		} else if (send_ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", send_ne);
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
	free(posted_per_qp);
	free(unused_recv_for_qp);
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


	struct ibv_wc           wc;

	int 			cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
	int 			total_gap_cycles = user_param->latency_gap * cpu_mhz;
	cycles_t 		end_cycle, start_gap;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	ctx->wr[0].sg_list->length = user_param->size;
	ctx->wr[0].send_flags = IBV_SEND_SIGNALED;

	if (user_param->size <= user_param->inline_size) {
		ctx->wr[0].send_flags |= IBV_SEND_INLINE;
	}


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

			err = post_send_method(ctx, 0, user_param);

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
					//coverity[uninit_use_in_call]
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
int run_iter_lat_write_imm(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t                scnt = 0;
	uint64_t                ccnt = 0;
	uint64_t                rcnt = 0;
	int                     ne;
	int			err = 0;
	volatile char           *post_buf = NULL;

	int 			size_per_qp = (user_param->use_srq) ?
					user_param->rx_depth/user_param->num_of_qps : user_param->rx_depth;
	struct ibv_wc           wc;
	struct ibv_recv_wr 	*bad_wr_recv = NULL;


	int 			cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
	int 			total_gap_cycles = user_param->latency_gap * cpu_mhz;
	cycles_t 		end_cycle, start_gap;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	ctx->wr[0].sg_list->length = user_param->size;
	ctx->wr[0].send_flags = IBV_SEND_SIGNALED;

	if (user_param->size <= user_param->inline_size) {
		ctx->wr[0].send_flags |= IBV_SEND_INLINE;
	}

	post_buf = (char*)ctx->buf[0] + user_param->size - 1;

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

			/* Poll for a completion */
			do { ne = ibv_poll_cq(ctx->recv_cq, 1, &wc); } while (ne == 0);
			if (ne > 0) {
				if (wc.status != IBV_WC_SUCCESS) {
					//coverity[uninit_use_in_call]
					NOTIFY_COMP_ERROR_SEND(wc,scnt,ccnt);
					return 1;
				}

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
				}
			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return FAILURE;
			}
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

			err = post_send_method(ctx, 0, user_param);

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
					//coverity[uninit_use_in_call]
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
	struct 		ibv_wc wc;
	int 		cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
	int 		total_gap_cycles = user_param->latency_gap * cpu_mhz;
	cycles_t 	end_cycle, start_gap;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	ctx->wr[0].sg_list->length = user_param->size;
	ctx->wr[0].send_flags = IBV_SEND_SIGNALED;

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

		err = post_send_method(ctx, 0, user_param);

		if (err) {
			fprintf(stderr,"Couldn't post send: scnt=%lu\n",scnt);
			return 1;
		}

		if (user_param->test_type == DURATION && user_param->state == END_STATE)
			break;

		if (user_param->use_event) {
			if (ctx_notify_events(ctx->send_channel)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return 1;
			}
		}

		do {
			ne = ibv_poll_cq(ctx->send_cq, 1, &wc);
			if(ne > 0) {
				if (wc.status != IBV_WC_SUCCESS) {
					//coverity[uninit_use_in_call]
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

	int  			firstRx = 1;
	int 			size_per_qp = (user_param->use_srq) ?
					user_param->rx_depth/user_param->num_of_qps : user_param->rx_depth;
	int 			cpu_mhz = get_cpu_mhz(user_param->cpu_freq_f);
	int			total_gap_cycles = user_param->latency_gap * cpu_mhz;
	int			send_flows_index = 0;
	int			recv_flows_index = 0;
	cycles_t 		end_cycle, start_gap;
	uintptr_t		primary_send_addr = ctx->sge_list[0].addr;
	uintptr_t		primary_recv_addr = ctx->recv_sge_list[0].addr;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	if (user_param->connection_type != RawEth) {
		ctx->wr[0].sg_list->length = user_param->size;
		ctx->wr[0].send_flags = 0;
	}
	if (user_param->size <= user_param->inline_size) {
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
				if (ctx_notify_events(ctx->recv_channel)) {
					fprintf(stderr , " Failed to notify events to CQ\n");
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
						//coverity[uninit_use_in_call]
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
				ctx->wr[0].send_flags |= IBV_SEND_SIGNALED;
			}

			/* if we're in duration mode and the time is over, exit from this function */
			if (user_param->test_type == DURATION && user_param->state == END_STATE)
				break;

			/* send the packet that's in index 0 on the buffer */
			err = post_send_method(ctx, 0, user_param);

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
					if (ctx_notify_events(ctx->send_channel)) {
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
					//coverity[uninit_use_in_call]
					NOTIFY_COMP_ERROR_SEND(s_wc,scnt,scnt)
						return 1;
				}
				poll = 0;
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
	int ne;
	int err;
	uint64_t scnt = 0;
	uint64_t rcnt = 0;
	uint64_t ccnt = 0;
	struct ibv_wc		*wc = NULL;
	struct ibv_send_wr	*bad_wr;
	struct ibv_recv_wr      *bad_wr_recv = NULL;
	int wc_id;

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

	ALLOCATE(wc, struct ibv_wc, user_param->burst_size);

	/* main loop for polling */
	while (rcnt < user_param->iters) {

		ne = ibv_poll_cq(ctx->recv_cq, user_param->burst_size, wc);
		if (ne > 0) {
			for (i = 0; i < ne; i++) {
				wc_id = (int)wc[i].wr_id;
				if (wc[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_RECV(wc[i], rcnt);
					free(wc);
					return FAILURE;
				}
				rcnt++;
				if (rcnt%user_param->reply_every == 0 && scnt - ccnt < user_param->tx_depth) {
					err = ibv_post_send(ctx->qp[0], &ctx->wr[0], &bad_wr);
					if (err) {
						fprintf(stderr, "Couldn't post send: scnt=%lu\n", scnt);
						free(wc);
						return FAILURE;
					}
					scnt++;
				}

				if (ibv_post_recv(ctx->qp[wc_id], &ctx->rwr[wc_id], &bad_wr_recv)) {
					fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%lu\n", wc_id, rcnt);
					free(wc);
					return FAILURE;
				}
			}
		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			free(wc);
			return FAILURE;
		}
		ne = ibv_poll_cq(ctx->send_cq, CTX_POLL_BATCH, wc);
		if (ne > 0) {
			for (i = 0; i < ne; i++) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					NOTIFY_COMP_ERROR_SEND(wc[i], scnt, ccnt);
					free(wc);
					return FAILURE;
				}
				ccnt++;
			}

		} else if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			free(wc);
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

	#ifdef HAVE_IBV_WR_API
	if (user_param->connection_type != RawEth)
		ctx_post_send_work_request_func_pointer(ctx, user_param);
	#endif

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

			err = ibv_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr);

			if (err) {
				fprintf(stderr, "Couldn't post send: scnt=%lu\n", totscnt);
				free(wc);
				return FAILURE;
			}
			if (user_param->post_list == 1 && user_param->size <= (ctx->cycle_buffer / 2)) {
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
					wc_id = (int)wc[i].wr_id;
					user_param->tcompleted[totrcnt] = get_cycles();
					totrcnt++;
					if (wc[i].status != IBV_WC_SUCCESS) {
						NOTIFY_COMP_ERROR_SEND(wc[i], totscnt, totccnt);
						return_value = FAILURE;
						goto cleaning;
					}
					if (ibv_post_recv(ctx->qp[wc_id], &ctx->rwr[wc_id], &bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%lu\n", wc_id, totrcnt);
						return_value = FAILURE;
						goto cleaning;
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
					wc_id = (int)wc[i].wr_id;
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
	free(wc);
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

	//coverity[uninit_use]
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
void print_bw_infinite_mode()
{
	print_report_bw(duration_param,NULL);
	duration_param->last_iters = duration_param->iters;
	duration_param->tposted[0] = get_cycles();
}

/******************************************************************************
 *
 ******************************************************************************/
void *handle_signal_print_thread(void* duration)
{
	int* duration_p = (int*) duration;
	while(1){
		sleep(*duration_p);
		print_bw_infinite_mode();
	}

}

/******************************************************************************
 *
 ******************************************************************************/
#ifdef HAVE_PACKET_PACING
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

	struct ibv_flow			**flow_create_result;
	struct ibv_flow_attr		**flow_rules;
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


	ALLOCATE(flow_create_result, struct ibv_flow*, allocated_flows * user_param->num_of_qps);
	ALLOCATE(flow_rules, struct ibv_flow_attr*, allocated_flows * user_param->num_of_qps);

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
				flow_create_result[flow_index] =
					ibv_create_flow(ctx->qp[qp_index], flow_rules[(qp_index * allocated_flows) + flow_index]);
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
		if (ibv_destroy_flow(flow_create_result[flow_index])) {
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
	char *error_message;

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

        if (user_param->has_source_ip) {
                struct sockaddr_in *source_addr;
                source_addr = calloc(1, sizeof(*source_addr));
                source_addr->sin_family = AF_INET;
                source_addr->sin_addr.s_addr = inet_addr(user_param->source_ip);
                hints->ai_src_addr = (struct sockaddr *)(source_addr);
                hints->ai_src_len = sizeof(*source_addr);
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

	for (i = 0; i < user_param->num_of_qps; i++) {
		struct cma_node *cm_node = &ctx->cma_master.nodes[i];
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
