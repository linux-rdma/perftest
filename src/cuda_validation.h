/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * GPU-side data validation for perftest
 *
 * This header defines data structures for validating RDMA data
 * written to GPU memory using CUDA kernels.
 */

#ifndef CUDA_VALIDATION_H
#define CUDA_VALIDATION_H

#include <stdint.h>
#include "validation_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Validation error codes
 */
enum ValidationError {
	VALIDATION_OK = 0,                /* Success */
	VALIDATION_ERR_NULL_PARAM,        /* NULL parameter passed */
	VALIDATION_ERR_CUDA_DEVICE,       /* Failed to set CUDA device */
	VALIDATION_ERR_ALLOC_QUEUE,       /* Failed to allocate work queue */
	VALIDATION_ERR_ALLOC_QP_STATE,    /* Failed to allocate QP state */
	VALIDATION_ERR_ALLOC_ERROR_INFO,  /* Failed to allocate error info */
	VALIDATION_ERR_ALLOC_STATS,       /* Failed to allocate stats */
	VALIDATION_ERR_ALLOC_FLAGS,       /* Failed to allocate control flags */
	VALIDATION_ERR_MEMSET,            /* Failed to zero memory */
	VALIDATION_ERR_ALREADY_RUNNING,   /* Kernel already running */
	VALIDATION_ERR_INIT_FAILED,       /* Context initialization failed */
	VALIDATION_ERR_LAUNCH,            /* Kernel launch failed */
	VALIDATION_ERR_SYNC,              /* Device synchronization failed */
	VALIDATION_ERR_NOT_RUNNING        /* Kernel not running */
};

/*
 * CUDA-specific configuration constants
 * (Pattern values and VALIDATION_MAX_CHUNKS_PER_QP come from validation_common.h)
 */
#define VALIDATION_QUEUE_SIZE     1048576   /* Must be power of 2 */
#define VALIDATION_THREADS_PER_BLOCK  1024
#define VALIDATION_MAX_VALIDATOR_BLOCKS 128

/*
 * Work item passed from monitor to validator blocks
 * Size: 8 bytes (aligned for atomic operations)
 */
struct ValidationWorkItem {
	uint32_t qp_id;           /* QP index (0 to num_qps-1) */
	uint16_t chunk_id;        /* Chunk within QP (0 to chunks_per_qp-1) */
	uint8_t  expected_pattern; /* Expected byte value (0x01, 0x02, 0x03) */
	uint8_t  valid;           /* 1 if this is a valid work item, 0 if empty */
};

/*
 * Lock-free ring buffer for work queue
 * Monitor block pushes, validator blocks pop
 * Head and tail on separate 128-byte cache lines to avoid false sharing
 */
struct ValidationWorkQueue {
	struct ValidationWorkItem items[VALIDATION_QUEUE_SIZE];
	volatile uint32_t head;      /* Next write position (monitor increments) */
	uint8_t  head_padding[124];  /* Pad to 128-byte GPU cache line */
	volatile uint32_t tail;      /* Next read position (validators increment) */
	uint8_t  tail_padding[124];  /* Pad to 128-byte GPU cache line */
};

/*
 * Per-QP validation state
 * Tracks how many chunks have been validated for pattern calculation
 */
struct QpValidationState {
	uint32_t chunks_validated; /* Total chunks validated for this QP */
	uint32_t padding;          /* Align to 8 bytes */
};

/*
 * Error information when validation fails
 */
struct ValidationErrorInfo {
	uint32_t qp_id;            /* QP where error occurred */
	uint32_t chunk_id;         /* Chunk index (0 to chunks_per_qp-1) */
	uint64_t byte_offset;      /* Offset within chunk where mismatch found */
	uint8_t  expected;         /* Expected byte value */
	uint8_t  actual;           /* Actual byte value found */
	uint8_t  padding[6];       /* Align to 8 bytes */
};

/*
 * Statistics collected during validation
 */
struct ValidationStats {
	uint64_t chunks_validated;    /* Total chunks validated (by validator) */
	uint64_t bytes_validated;     /* Total bytes validated */
	uint64_t errors_found;        /* Total mismatches (if not stopping on first) */
	uint64_t monitor_detections;  /* Total tail detections (by monitor) - for debugging */
	uint64_t race_overwrites;     /* DMA overwrites detected by epoch guard */
	uint64_t dma_stale_retries;   /* Tail mismatches resolved by DMA flush retry */
};

/*
 * GPU kernel parameters (copied to device constant memory)
 */
struct ValidationParams {
	uint64_t tail_markers_addr;    /* GPU address of tail_markers array */
	uint64_t recv_slots_addr;      /* GPU address of receive slots base */
	uint64_t patterns_addr;        /* GPU address of pattern buffers */
	uint64_t payload_size;         /* Size of each slot in bytes */
	uint32_t tx_depth;             /* Number of slots per chunk */
	uint32_t num_qps;              /* Number of queue pairs */
	uint32_t num_validator_blocks; /* Number of validator blocks (1..64) */
	uint32_t padding;
};

/*
 * Main validation context - holds all GPU memory pointers
 * Allocated on host, contains device pointers
 */
struct ValidationContext {
	/* Device memory pointers */
	volatile uint64_t *d_tail_markers;    /* [num_qps * chunks_per_qp] */
	struct ValidationWorkQueue *d_work_queue;
	struct QpValidationState *d_qp_state; /* [num_qps] */
	struct ValidationErrorInfo *d_error_info;
	struct ValidationStats *d_stats;
	volatile uint32_t *d_validation_failed; /* Atomic flag */
	volatile uint32_t *d_should_stop;       /* Signal kernel to exit */

	/* Host-side copies for reading results */
	struct ValidationErrorInfo h_error_info;
	struct ValidationStats h_stats;

	/* Configuration */
	struct ValidationParams params;

	/* Kernel state */
	int kernel_running;
	int num_blocks;  /* Total blocks: 1 monitor + N validators */
	int cuda_device_id;  /* GPU device ID for multi-GPU systems */
	int debug_enabled;   /* Enable verbose debug output */
	uint32_t chunks_per_qp;  /* Ring depth per QP (dynamic, from derive_validation_chunks_per_qp) */
};

/*
 * Host-callable API is defined via function-pointer typedefs in
 * cuda_loader.h and implemented in the kernel plugin (libperftest_kernels.so).
 * This header only contains data structure definitions.
 */

#ifdef __cplusplus
}
#endif

#endif /* CUDA_VALIDATION_H */
