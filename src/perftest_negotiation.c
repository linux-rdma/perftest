#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <infiniband/verbs.h>

#include "perftest_negotiation.h"

#ifdef HAVE_MLX5DV
	#include <infiniband/mlx5dv.h>
#endif

/******************************************************************************
 * Chunked transport: send/receive an arbitrarily sized byte stream as a sequence
 * of chunks of up to KV_BUF_SIZE bytes each. The total length is swapped up front.
 ******************************************************************************/
static int write_data_chunked(struct perftest_comm *comm, void *data, size_t size, int is_rdma)
{
	const char *src = data;
	size_t sent = 0;

	while (sent < size) {
		size_t chunk = size - sent;
		if (chunk > KV_BUF_SIZE)
			chunk = KV_BUF_SIZE;

		if (is_rdma ? rdma_write_data((void *)(src + sent), comm, chunk)
			    : ethernet_write_data(comm, (char *)(src + sent), chunk))
			return FAILURE;

		sent += chunk;
	}

	return SUCCESS;
}

static int read_data_chunked(struct perftest_comm *comm, void *data, size_t size, int is_rdma)
{
	char *dst = data;
	size_t received = 0;

	while (received < size) {
		size_t chunk = size - received;
		if (chunk > KV_BUF_SIZE)
			chunk = KV_BUF_SIZE;

		if (is_rdma ? rdma_read_data(dst + received, comm, chunk)
			    : ethernet_read_data(comm, dst + received, chunk))
			return FAILURE;

		received += chunk;
	}

	return SUCCESS;
}

/* Bidirectional chunked exchange */
static int ctx_xchg_data_chunked(struct perftest_comm *comm, void *my_data, size_t my_size,
								 void **out_rem_data, size_t *out_rem_size)
{
	int is_rdma = comm->rdma_params->use_rdma_cm || comm->rdma_params->work_rdma_cm;
	uint32_t my_len = htonl((uint32_t)my_size);
	uint32_t rem_len = 0;
	size_t rem_size;
	char *rem_buf;

	/* Swap total lengths first so each side can size its receive buffer. */
	if (ctx_xchg_data(comm, &my_len, &rem_len, sizeof(my_len))) {
		fprintf(stderr, " Unable to exchange chunked payload size\n");
		return FAILURE;
	}
	rem_size = ntohl(rem_len);

	rem_buf = malloc(rem_size + 1);
	if (!rem_buf) {
		fprintf(stderr, " Failed to allocate %zu bytes for peer payload\n", rem_size + 1);
		return FAILURE;
	}

	if (comm->rdma_params->servername) {
		if (write_data_chunked(comm, my_data, my_size, is_rdma) ||
		    read_data_chunked(comm, rem_buf, rem_size, is_rdma)) {
			fprintf(stderr, " Unable to exchange chunked payload\n");
			free(rem_buf);
			return FAILURE;
		}
	} else {
		if (read_data_chunked(comm, rem_buf, rem_size, is_rdma) ||
		    write_data_chunked(comm, my_data, my_size, is_rdma)) {
			fprintf(stderr, " Unable to exchange chunked payload\n");
			free(rem_buf);
			return FAILURE;
		}
	}

	rem_buf[rem_size] = '\0';
	*out_rem_data = rem_buf;
	*out_rem_size = rem_size;
	return SUCCESS;
}

/******************************************************************************
 * Legacy path
 ******************************************************************************/
static void mark_received(CompareFunction *cfs, int num_cfs, const char *name)
{
	int i;
	for (i = 0; i < num_cfs; i++) {
		if (strcmp(cfs[i].name, name) == 0) {
			cfs[i].received = true;
			return;
		}
	}
}

static int negotiate_params_legacy(struct perftest_comm *comm, struct perftest_parameters_negotiate *local_params,
								   struct perftest_parameters_negotiate *remote_params, CompareFunction *cfs, int num_cfs)
{
	struct perftest_parameters_negotiate_legacy local_legacy;
	struct perftest_parameters_negotiate_legacy remote_legacy;
	memset(&local_legacy, 0, sizeof(local_legacy));
	memset(&remote_legacy, 0, sizeof(remote_legacy));

	/* Pack local_params into the legacy wire format. */
	local_legacy.test_method = local_params->test_method;
	local_legacy.connection_type = local_params->connection_type;
	local_legacy.verb = local_params->verb;
	local_legacy.tst = local_params->tst;
	local_legacy.atomicType = local_params->atomicType;
	local_legacy.test_type = local_params->test_type;
	local_legacy.report_fmt = local_params->report_fmt;
	local_legacy.size = local_params->size;
	local_legacy.iters = local_params->iters;
	local_legacy.aes_xts = local_params->aes_xts;
	local_legacy.num_of_qps = local_params->num_of_qps;
	local_legacy.duration = local_params->duration;
	local_legacy.use_rdma_cm = local_params->use_rdma_cm;
	local_legacy.use_write_with_imm = local_params->use_write_with_imm;
	local_legacy.no_enhanced_reorder = local_params->no_enhanced_reorder;
	local_legacy.sig_offload = local_params->sig_offload;
	local_legacy.data_validation = local_params->data_validation;
	local_legacy.attr.max_qp_rd_atom = ntoh_int(local_params->max_qp_rd_atom);

	#ifdef HAVE_MLX5DV
	local_legacy.mlx5dv_comp_mask = local_params->mlx5dv_comp_mask;
	#endif

	if (ctx_xchg_data(comm, &local_legacy, &remote_legacy, sizeof(local_legacy))) {
		fprintf(stderr, " Failed to exchange negotiation parameters between server and client\n");
		return FAILURE;
	}

	/* Unpack the peer's legacy reply into remote_params, and mark each received field. */
	remote_params->test_method = remote_legacy.test_method;
	mark_received(cfs, num_cfs, "test_method");

	remote_params->connection_type = remote_legacy.connection_type;
	mark_received(cfs, num_cfs, "connection_type");

	remote_params->verb = remote_legacy.verb;
	mark_received(cfs, num_cfs, "verb");

	remote_params->tst = remote_legacy.tst;
	mark_received(cfs, num_cfs, "tst");

	remote_params->atomicType = remote_legacy.atomicType;
	mark_received(cfs, num_cfs, "atomicType");

	remote_params->test_type = remote_legacy.test_type;
	mark_received(cfs, num_cfs, "test_type");

	remote_params->report_fmt = remote_legacy.report_fmt;
	mark_received(cfs, num_cfs, "report_fmt");

	remote_params->size = remote_legacy.size;
	mark_received(cfs, num_cfs, "size");

	remote_params->iters = remote_legacy.iters;
	mark_received(cfs, num_cfs, "iters");

	remote_params->aes_xts = remote_legacy.aes_xts;
	mark_received(cfs, num_cfs, "aes_xts");

	remote_params->num_of_qps = remote_legacy.num_of_qps;
	mark_received(cfs, num_cfs, "num_of_qps");

	remote_params->duration = remote_legacy.duration;
	mark_received(cfs, num_cfs, "duration");

	remote_params->use_rdma_cm = remote_legacy.use_rdma_cm;
	mark_received(cfs, num_cfs, "use_rdma_cm");

	remote_params->use_write_with_imm = remote_legacy.use_write_with_imm;
	mark_received(cfs, num_cfs, "use_write_with_imm");

	remote_params->no_enhanced_reorder = remote_legacy.no_enhanced_reorder;
	mark_received(cfs, num_cfs, "no_enhanced_reorder");

	remote_params->sig_offload = remote_legacy.sig_offload;
	mark_received(cfs, num_cfs, "sig_offload");

	remote_params->data_validation = remote_legacy.data_validation;
	mark_received(cfs, num_cfs, "data_validation");

	remote_params->max_qp_rd_atom = hton_int(remote_legacy.attr.max_qp_rd_atom);
	mark_received(cfs, num_cfs, "max_qp_rd_atom");

	#ifdef HAVE_MLX5DV
	remote_params->mlx5dv_comp_mask = remote_legacy.mlx5dv_comp_mask;
	mark_received(cfs, num_cfs, "mlx5dv_comp_mask");
	#endif

	return SUCCESS;
}

/******************************************************************************
 * Modern path: serialize the table as "name=hexbytes;..." and exchange via
 * the chunked transport. Unknown keys are silently ignored; missing keys
 * leave the compare entry with received=false (the compare loop skips it).
 ******************************************************************************/
static int negotiate_params_kv_string(struct perftest_comm *comm, CompareFunction *compare_functions, int num_compare)
{
	char *local_str = NULL;
	size_t local_size = 0;
	size_t off = 0;
	char *remote_str = NULL;
	size_t remote_size = 0;
	int i;
	size_t j;
	char *saveptr;
	char *token;

	/* Compute the exact buffer size up front so we allocate once. */
	for (i = 0; i < num_compare; i++)
		local_size += strlen(compare_functions[i].name) + strlen(KV_SEP) +
			      compare_functions[i].value_size * KV_HEX_PER_BYTE + strlen(KV_PAIR_SEP);
	local_size += 1;

	local_str = malloc(local_size);
	if (!local_str) {
		fprintf(stderr, " Failed to allocate KV negotiation string\n");
		return FAILURE;
	}

	for (i = 0; i < num_compare; i++) {
		CompareFunction *cf = &compare_functions[i];
		unsigned char *bytes = (unsigned char *)cf->local_value;

		off += snprintf(local_str + off, local_size - off, "%s" KV_SEP, cf->name);
		for (j = 0; j < cf->value_size; j++)
			off += snprintf(local_str + off, local_size - off, "%02x", bytes[j]);
		off += snprintf(local_str + off, local_size - off, KV_PAIR_SEP);
	}

	if (ctx_xchg_data_chunked(comm, local_str, off, (void **)&remote_str, &remote_size)) {
		fprintf(stderr, " Failed to exchange KV negotiation string\n");
		free(local_str);
		return FAILURE;
	}
	free(local_str);

	token = strtok_r(remote_str, KV_PAIR_SEP, &saveptr);
	while (token != NULL) {
		char *eq = strchr(token, KV_SEP[0]);

		if (eq) {
			*eq = '\0';
			const char *key = token;
			const char *hex = eq + 1;

			for (i = 0; i < num_compare; i++) {
				CompareFunction *cf = &compare_functions[i];

				if (strcmp(cf->name, key) != 0)
					continue;

				unsigned char *bytes = (unsigned char *)cf->remote_value;
				for (j = 0; j < cf->value_size; j++) {
					if (sscanf(hex + KV_HEX_PER_BYTE * j, "%2hhx", &bytes[j]) != 1)
						break;
				}
				if (j == cf->value_size)
					cf->received = true;
				break;
			}
		}
		token = strtok_r(NULL, KV_PAIR_SEP, &saveptr);
	}

	free(remote_str);
	return SUCCESS;
}

/******************************************************************************
 * Compare Functions
 ******************************************************************************/
static int compare(const char *name, void *local_value, void *remote_value, char **return_values,
				   struct perftest_parameters *user_param, CompareType type)
{
	uint64_t local_value_int, remote_value_int;
	const char *type_str = (type >= INT && type <= NONE) ? ((char *[]){"INT", "UINT64", "NONE"})[type] : "UNKNOWN";

	switch (type) {
	case UINT64:
		local_value_int = ntoh_64(*(uint64_t *)local_value);
		remote_value_int = ntoh_64(*(uint64_t *)remote_value);
		break;
	case INT:
		local_value_int = ntoh_int(*(int *)local_value);
		remote_value_int = ntoh_int(*(int *)remote_value);
		break;
	default:
		fprintf(stderr, " Unexpected compare type %s for %s\n", type_str, name);
		return FAILURE;
	}

	if (local_value_int == remote_value_int)
		return SUCCESS;

	if (return_values) {
		fprintf(stderr, " %s mismatch: local=%s, remote=%s\n", name, return_values[local_value_int],
				return_values[remote_value_int]);
	} else {
		fprintf(stderr, " %s mismatch: local=%lu, remote=%lu\n", name, local_value_int, remote_value_int);
	}

	return FAILURE;
}

static int compare_max_qp_rd_atom(const char *name, void *local_value, void *remote_value, char **return_values,
								  struct perftest_parameters *user_param, CompareType type)
{
	int remote_max_qp_rd_atom = ntoh_int(*(int *)remote_value);

	if (user_param->out_reads > remote_max_qp_rd_atom) {
		printf(" out_reads is greater than remote max_qp_rd_atom, using remote max_qp_rd_atom\n");
		user_param->out_reads = remote_max_qp_rd_atom;
	}

	return SUCCESS;
}

#ifdef HAVE_MLX5DV
static int compare_mlx5dv(const char *name, void *local_value, void *remote_value, char **return_values,
						  struct perftest_parameters *user_param, CompareType type)
{
	#ifdef HAVE_OOO_RECV_WRS
	uint64_t local_value_uint64 = ntoh_64(*(uint64_t *)local_value);
	uint64_t remote_value_uint64 = ntoh_64(*(uint64_t *)remote_value);

	if (user_param->no_enhanced_reorder != ON && (local_value_uint64 & MLX5DV_CONTEXT_MASK_OOO_RECV_WRS) !=
													 (remote_value_uint64 & MLX5DV_CONTEXT_MASK_OOO_RECV_WRS)) {
		user_param->no_enhanced_reorder = ON;
		printf(" OOO_RECV_WRS mismatch, disabling enhanced reorder\n");
	}
	#endif
	return SUCCESS;
}
#endif

/******************************************************************************
 * Public entry point.
 ******************************************************************************/
int negotiate_params(struct pingpong_context *ctx, struct perftest_comm *comm, struct perftest_parameters *user_param)
{
	if (atof(user_param->rem_version) < 6.28)
		return SUCCESS;

	struct ibv_device_attr dev_attr;
	if (ibv_query_device(ctx->context, &dev_attr)) {
		fprintf(stderr, " Failed to query device attributes\n");
		return FAILURE;
	}

	/* Fill local parameters */
	struct perftest_parameters_negotiate local_params = {
		.test_method = hton_int(user_param->test_method),
		.connection_type = hton_int(user_param->connection_type),
		.verb = hton_int(user_param->verb),
		.tst = hton_int(user_param->tst),
		.atomicType = hton_int(user_param->atomicType),
		.test_type = hton_int(user_param->test_type),
		.report_fmt = hton_int(user_param->report_fmt),
		.size = hton_64(user_param->size),
		.iters = hton_64(user_param->iters),
		.aes_xts = hton_int((int)user_param->aes_xts),
		.num_of_qps = hton_int(user_param->num_of_qps),
		.duration = hton_int(user_param->duration),
		.use_rdma_cm = hton_int(user_param->use_rdma_cm),
		.use_write_with_imm = hton_int(user_param->use_write_with_imm),
		.no_enhanced_reorder = hton_int(user_param->no_enhanced_reorder),
		.sig_offload = hton_int(user_param->sig_offload),
		.data_validation = hton_int(user_param->data_validation),
		.max_qp_rd_atom = hton_int(dev_attr.max_qp_rd_atom),
		.tx_depth = hton_int(user_param->tx_depth),
	};

	#ifdef HAVE_MLX5DV
	if (dev_attr.vendor_id == MLNX_VENDOR_ID) {
		struct mlx5dv_context ctx_dv = {};
		#ifdef HAVE_OOO_RECV_WRS
		ctx_dv.comp_mask = MLX5DV_CONTEXT_MASK_OOO_RECV_WRS;
		#endif

		/* Devices that cannot report DV capabilities (mlx4) have none.
		 * Leaving the mask at 0 is accurate, and keeps both peers in sync. */
		if (!mlx5dv_query_device(ctx->context, &ctx_dv))
			local_params.mlx5dv_comp_mask = hton_64(ctx_dv.comp_mask);
	}
	#endif

	struct perftest_parameters_negotiate remote_params;
	memset(&remote_params, 0, sizeof(remote_params));

	#define COMPARE(func, name, type, cond, values)                                                                        \
		{func, #name, &local_params.name, &remote_params.name, type, sizeof(local_params.name), cond, false, values}

	CompareFunction compare_functions[] = {
		//  COMPARE(compare_func, name, type, cond, values) -> { compare_func, name, local_value, remote_value, type,value_size, cond, received, return_values}
		COMPARE(compare, test_method, INT, true, ((char *[]){"RUN_REGULAR", "RUN_ALL", "RUN_INFINITELY"})),
		COMPARE(compare, connection_type, INT, true, ((char *[]){"RC", "UC", "UD", "RawEth", "XRC", "DC", "SRD"})),
		COMPARE(compare, verb, INT, true, ((char *[]){"SEND", "SEND_IMM", "WRITE", "WRITE_IMM", "READ", "ATOMIC"})),
		COMPARE(compare, tst, INT, true, ((char *[]){"LAT", "BW", "LAT_BY_BW", "FS_RATE"})),
		COMPARE(compare, atomicType, INT, true, ((char *[]){"CMP_AND_SWAP", "FETCH_AND_ADD"})),
		COMPARE(compare, test_type, INT, true, ((char *[]){"ITERATIONS", "DURATION"})),
		COMPARE(compare, report_fmt, INT, true, ((char *[]){"GBS", "MBS"})),
		COMPARE(compare, size, UINT64, true, NULL),
		COMPARE(compare, iters, UINT64, true, NULL),
		COMPARE(compare, aes_xts, INT, true, ((char *[]){"OFF", "ON"})),
		COMPARE(compare, num_of_qps, INT, true, NULL),
		COMPARE(compare, duration, INT, true, NULL),
		COMPARE(compare, use_rdma_cm, INT, true, ((char *[]){"OFF", "ON"})),
		COMPARE(compare, use_write_with_imm, INT, true, ((char *[]){"OFF", "ON"})),
		COMPARE(compare, no_enhanced_reorder, INT, true, ((char *[]){"OFF", "ON"})),
		COMPARE(compare, sig_offload, INT, true, ((char *[]){"OFF", "ON"})),
		COMPARE(compare, data_validation, INT, true, ((char *[]){"OFF", "ON"})),
		COMPARE(compare, tx_depth, INT, true, NULL),
		COMPARE(compare_max_qp_rd_atom, max_qp_rd_atom, INT, user_param->connection_type != DC, NULL),

		#ifdef HAVE_MLX5DV
		COMPARE(compare_mlx5dv, mlx5dv_comp_mask, UINT64, dev_attr.vendor_id == MLNX_VENDOR_ID, NULL),
		#endif
	};
	#undef COMPARE

	int num_compare = GET_ARRAY_SIZE(compare_functions);
	int i;

	if (atof(user_param->rem_version) <= 6.29) {
		if (negotiate_params_legacy(comm, &local_params, &remote_params, compare_functions, num_compare))
			return FAILURE;
	} else {
		int remote_version;
		int local_version = hton_int(NEGOTIATION_VERSION);
		if (ctx_xchg_data(comm, &local_version, &remote_version, sizeof(int))) {
			fprintf(stderr, " Failed to exchange negotiation version between server and clients\n");
			return FAILURE;
		}

		remote_version = ntoh_int(remote_version);
		if (remote_version != NEGOTIATION_VERSION) {
			fprintf(stderr, " Negotiation version mismatch: local=%d, remote=%d, skipping negotiation\n",
					NEGOTIATION_VERSION, remote_version);
			return SUCCESS;
		}

		if (negotiate_params_kv_string(comm, compare_functions, num_compare))
			return FAILURE;
	}

	/* Compare all parameters */
	for (i = 0; i < num_compare; i++) {
		CompareFunction *cf = &compare_functions[i];

		if (!cf->received || !cf->cond)
			continue;

		if (cf->compare_func(cf->name, cf->local_value, cf->remote_value, cf->return_values,
							 user_param, cf->type) == FAILURE)
			return FAILURE;
	}

	return SUCCESS;
}
