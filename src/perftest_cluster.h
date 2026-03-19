#ifndef PERFTEST_CLUSTER_H
#define PERFTEST_CLUSTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "perftest_parameters.h"

/*
 * Perftest cluster-mode glue: result structs, gather helpers, and barrier
 * protocol. Generic MPI loading lives in mpi_loader.{c,h}.
 */

/* "[Cluster]" diagnostics; macros append newline (no prefix/level in fmt). */
#define CLUSTER_TAG "[Cluster] "

#define cluster_errf(fmt, ...) \
	fprintf(stderr, CLUSTER_TAG "ERROR: " fmt "\n", ##__VA_ARGS__)

#define cluster_fatalf(fmt, ...) \
	fprintf(stderr, CLUSTER_TAG "Fatal: " fmt "\n", ##__VA_ARGS__)

#define cluster_infof(fmt, ...) \
	do { \
		printf(CLUSTER_TAG fmt "\n", ##__VA_ARGS__); \
		fflush(stdout); \
	} while (0)

struct data_validation_result;

/*
 * ABI version for gathered result structs (raw bytes between binaries).
 * Bump CLUSTER_ABI_VERSION on any layout change; collector validates at offset 0.
 */
#define CLUSTER_ABI_VERSION 1u

enum cluster_role {
	CLUSTER_ROLE_SERVER = 0,
	CLUSTER_ROLE_CLIENT = 1,
};

enum cluster_test_type {
	CLUSTER_TEST_ITERATIONS = 0,
	CLUSTER_TEST_DURATION   = 1,
};

struct cluster_bw_report {
	uint32_t abi_version;  /* == CLUSTER_ABI_VERSION; first field */
	struct bw_report_data bw_rep;
	int    role;           /* enum cluster_role */
	int    rank;
	int    report_fmt;     /* enum ctx_report_fmt: GBS=0, MBS=1 */

	int      dv_enabled;
	int      dv_passed;
	uint64_t dv_errors;
	uint64_t dv_bytes;
	uint64_t dv_chunks;
};

/* Per-worker latency result gathered to rank 0. Duration mode zeroes percentile fields. */
struct cluster_lat_report {
	uint32_t      abi_version; /* == CLUSTER_ABI_VERSION; first field */
	unsigned long size;
	uint64_t      iters;
	double        t_min;
	double        t_max;
	double        t_typical;
	double        t_avg;
	double        stdev;
	double        p99;
	double        p99_9;
	double        tps;
	int           test_type;   /* enum cluster_test_type */
	int           role;        /* enum cluster_role */
	int           rank;
};

_Static_assert(offsetof(struct cluster_bw_report, abi_version) == 0,
	       "abi_version must be the first field of cluster_bw_report");
_Static_assert(offsetof(struct cluster_lat_report, abi_version) == 0,
	       "abi_version must be the first field of cluster_lat_report");

void store_cluster_lat_report(const struct cluster_lat_report *rep);
const struct cluster_lat_report *get_cluster_lat_report(void);

/* mpi_try_init + version check; returns non-zero on fatal error. */
int cluster_init(int *argc, char ***argv);

/* Gather BW report to rank 0. bw/dv may be NULL. No-op if MPI unavailable. */
void cluster_report_bw(const struct perftest_parameters *user_param,
		       const struct bw_report_data *bw,
		       const struct data_validation_result *dv);

/* Gather stored latency report to rank 0. No-op if MPI unavailable. */
void cluster_report_lat(struct perftest_parameters *user_param);

void cluster_capture_lat_iter(unsigned long size, uint64_t iters,
			      double t_min, double t_max, double t_typical,
			      double t_avg, double stdev,
			      double p99, double p99_9);
void cluster_capture_lat_duration(unsigned long size, uint64_t iters,
				  double t_avg, double tps);

/*
 * Collective barrier phases (all ranks, fixed order, once per test cycle):
 * PRE_HANDSHAKE, RESOURCES, CONNECTED, TRAFFIC.
 */
typedef enum {
	CLUSTER_PHASE_PRE_HANDSHAKE = 0,
	CLUSTER_PHASE_RESOURCES,
	CLUSTER_PHASE_CONNECTED,
	CLUSTER_PHASE_TRAFFIC,
	CLUSTER_PHASE__COUNT
} cluster_phase_t;

#define CLUSTER_CLIENT_SETTLE_US    100000

void cluster_barrier(cluster_phase_t phase);
void cluster_barrier_pre_handshake(const struct perftest_parameters *user_param);

#endif /* PERFTEST_CLUSTER_H */
