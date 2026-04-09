#ifndef PERFTEST_NEGOTIATION_H
#define PERFTEST_NEGOTIATION_H

#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "perftest_communication.h"

#define NEGOTIATION_VERSION 0

#define KV_PAIR_SEP     ";"
#define KV_SEP          "="
#define KV_HEX_PER_BYTE  2

typedef enum { INT, UINT64, NONE } CompareType;

/* Frozen wire format used when talking to peers running perftest <= 6.29.
 * Do NOT modify: new peers must produce the same bytes on the wire as old
 * peers, or cross-version negotiation will desync. */
struct perftest_parameters_negotiate_legacy {
	enum ctx_test_method test_method;
	int connection_type;
	VerbType verb;
	TestType tst;
	AtomicType atomicType;
	TestMethod test_type;
	enum ctx_report_fmt report_fmt;
	uint64_t size;
	uint64_t iters;
	int aes_xts;
	int num_of_qps;
	int duration;
	int use_rdma_cm;
	int use_write_with_imm;
	int no_enhanced_reorder;
	int sig_offload;
	int data_validation;
	struct ibv_device_attr attr;

#ifdef HAVE_MLX5DV
	uint64_t mlx5dv_comp_mask;
#endif
};

struct perftest_parameters_negotiate {
	enum ctx_test_method test_method;
	int connection_type;
	VerbType verb;
	TestType tst;
	AtomicType atomicType;
	TestMethod test_type;
	enum ctx_report_fmt report_fmt;
	uint64_t size;
	uint64_t iters;
	int aes_xts;
	int num_of_qps;
	int duration;
	int use_rdma_cm;
	int tx_depth;
	int use_write_with_imm;
	int no_enhanced_reorder;
	int sig_offload;
	int data_validation;
	int max_qp_rd_atom;

#ifdef HAVE_MLX5DV
	uint64_t mlx5dv_comp_mask;
#endif
};

typedef struct {
	int (*compare_func)(const char *, void *, void *, char **, struct perftest_parameters *, CompareType type);
	const char *name;
	void *local_value;
	void *remote_value;
	CompareType type;
	size_t value_size;
	bool cond;
	bool received;
	char **return_values;
} CompareFunction;

/* Negotiates parameters and capabilities between server and client.
 *
 * Peers older than 6.28 are silently skipped (return SUCCESS without exchange).
 * Peers <= 6.29 use the legacy fixed-struct wire format.
 * Peers >= 6.30 first exchange NEGOTIATION_VERSION; on match they exchange
 * a "name=hexbytes;..." KV string, on mismatch the negotiation is skipped.
 *
 * Returns SUCCESS / FAILURE. */
int negotiate_params(struct pingpong_context *ctx, struct perftest_comm *comm, struct perftest_parameters *user_param);

#endif /* PERFTEST_NEGOTIATION_H */
