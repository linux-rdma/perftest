/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Host memory data validation for perftest
 *
 * Sharded Inline Validation architecture:
 * - No separate monitor thread; no inter-thread queues
 * - Each validator thread owns markers from all QPs (QP-balanced hybrid)
 * - Validators poll their own markers and validate data immediately
 * - Whole-chunk validation (contiguous SIMD scan) for maximum throughput
 */

#ifndef HOST_VALIDATION_H
#define HOST_VALIDATION_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include "memory.h"
#include "perftest_parameters.h"
#include "validation_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-specific configuration constants
 */
#define HOST_VALIDATION_MAX_VALIDATORS  32      /* Maximum validator threads */
#define HOST_VALIDATION_DEFAULT_VALIDATORS 4    /* Default if auto-detect fails */

/*
 * All shared constants (patterns, max chunks, modes) come directly from
 * validation_common.h — use the VALIDATION_* names throughout.
 */

/*
 * Pre-computed marker scan entry for optimized hot path.
 * Each validator owns an array of these (validator-local, no sharing).
 * `last_seen` is stored inline so it lives in the validator's own cache lines.
 */
struct marker_scan_entry {
	volatile uint64_t *marker_ptr;   /* Direct pointer to tail_markers[idx] */
	uint64_t last_seen;              /* Last seen marker value (validator-local) */
	uint8_t *chunk_data;             /* Pre-computed: recv_slots + offset (constant per marker) */
	uint32_t qp_id;                  /* QP index */
	uint32_t chunk_id;               /* Chunk index within QP */
};

/*
 * Validation statistics (atomic for thread safety).
 * Per-chunk/byte counts and skipped-step counts are tracked in per-validator
 * non-atomic stats and aggregated at stop time — no need for atomics on the
 * hot path.  Only error_found needs an atomic since it's a rare cross-thread
 * signal (first-error capture via CAS).
 */
struct host_validation_stats {
	atomic_uint_fast64_t errors_found;
};

/*
 * Error information structure
 */
struct host_validation_error {
	atomic_int has_error;       /* Set to 1 on first error */
	uint32_t qp_id;
	uint32_t chunk_id;
	uint64_t byte_offset;
	uint8_t expected;
	uint8_t actual;
};

/*
 * Thread control flags
 */
struct host_thread_control {
	atomic_int stop_flag;           /* Signal threads to stop */
	atomic_int validators_running;  /* Count of running validator threads */
};

/*
 * Per-validator performance statistics (not atomic - only accessed by owning thread)
 */
struct host_validator_stats {
	uint64_t work_items_processed;   /* Total slots validated (in ops_per_chunk units) */
	uint64_t bytes_validated;        /* Total bytes validated */
	uint64_t markers_scanned;        /* Total marker polls */
	uint64_t markers_hit;            /* Found new data */
	uint64_t scan_cycles;            /* Full scan loop iterations */
	uint64_t skipped_steps;          /* Marker advanced by >1 between scans */
	uint64_t race_overwrites;        /* DMA overwrites detected by epoch guard */
	uint64_t dma_stale_retries;      /* Tail mismatches resolved by DMA flush retry */
};

/*
 * Per-validator thread context
 *
 * CACHE LINE ALIGNED: Compiler-enforced 128-byte alignment prevents false
 * sharing between adjacent validators in the array.  Use aligned_alloc()
 * when allocating an array of these.
 *
 * Sharded ownership: each validator exclusively owns markers from all QPs (hybrid).
 * It polls its own markers and validates data inline (no queue handoff).
 */
struct host_validator_ctx {
	int id;                              /* Validator ID (0 to num_validators-1) */
	struct host_validation_ctx *parent;  /* Back-pointer to main context */
	pthread_t thread;                    /* Thread handle */
	atomic_int running;                  /* Is this validator running? */
	struct host_validator_stats stats;   /* Per-thread performance stats */

	/* Sharded marker ownership */
	struct marker_scan_entry *own_markers;  /* Array of markers this validator polls */
	uint32_t own_marker_count;              /* Number of markers owned */
} __attribute__((aligned(128)));

/*
 * Main validation context
 */
struct host_validation_ctx {
	/* Memory layout parameters */
	void *buffer_base;
	uint64_t markers_offset;      /* Tail markers offset */
	uint64_t recv_slots_offset;   /* Where validated data resides */
	uint64_t payload_size;
	uint32_t ops_per_chunk;       /* Operations per validation chunk (was tx_depth in API) */
	uint32_t num_qps;
	uint32_t chunks_per_qp;       /* Dynamic buffer depth (passed from perftest_parameters) */
	uint32_t slots_per_qp;       /* Pre-computed: ops_per_chunk * chunks_per_qp */
	int validation_mode;          /* WRITE or READ */
	int debug_enabled;
	int numa_node;                /* NUMA node of IB device, -1 = any */

	/* Derived pointers (computed at init) */
	volatile uint64_t *tail_markers;
	uint8_t *recv_slots;

	/* Thread management */
	struct host_thread_control control;

	/* Validator thread pool (sharded: each validator owns its markers) */
	int num_validators;           /* Number of validator threads */
	struct host_validator_ctx *validators;  /* Array of validator contexts */

	/* Total marker count (num_qps * chunks_per_qp) */
	uint32_t total_markers;

	/* Statistics and error tracking */
	struct host_validation_stats stats;
	struct host_validation_error error;

	/* Initialization status */
	int initialized;
};

/*
 * Discover the NUMA node of an InfiniBand device via sysfs.
 * Returns the NUMA node ID (>= 0), or -1 if unknown / not Linux.
 */
int host_validation_get_numa_node(const char *ib_devname);

/*
 * Host validation API (implements memory_ctx interface)
 */
int host_validation_init(struct memory_ctx *ctx,
			 const struct validation_config *cfg);

int host_validation_start(struct memory_ctx *ctx);

int host_validation_stop(struct memory_ctx *ctx,
                         struct data_validation_result *result);

void host_validation_destroy(struct memory_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* HOST_VALIDATION_H */
