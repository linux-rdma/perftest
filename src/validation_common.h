/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Shared validation constants and types for perftest
 *
 * Single source of truth for pattern values, buffer depth limits,
 * and error classification shared between host (CPU) and GPU (CUDA)
 * validation paths.
 *
 * This header is pure C — no CUDA, no pthreads, no SIMD dependencies.
 * Safe to include from .c, .cu, and .h files.
 */

#ifndef VALIDATION_COMMON_H
#define VALIDATION_COMMON_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Number of distinct fill patterns that rotate across ring-buffer cycles.
 * Sender fills chunk with pattern[cycle % count], validator checks match.
 */
#define VALIDATION_PATTERNS_COUNT     3

/*
 * Fill-pattern byte values.
 * Chosen to be non-zero (distinguish from zeroed memory) and small
 * enough to be visible in hex dumps.
 */
#define VALIDATION_PATTERN_0          0x01
#define VALIDATION_PATTERN_1          0x02
#define VALIDATION_PATTERN_2          0x03

/*
 * Maximum number of validation chunks (ring-buffer slots) per QP.
 * Actual depth is dynamic — see derive_validation_chunks_per_qp().
 * Both host and CUDA paths must respect this upper bound for
 * buffer sizing and array allocation.
 */
#define VALIDATION_MAX_CHUNKS_PER_QP  24

/*
 * Unified log prefix for debug/diagnostic output from both host and CUDA
 * validation backends.  Keeps grep/parsing consistent across memory types.
 * User-facing result lines (PASSED/FAILED) in perftest_resources.c use a
 * separate format and are not affected by this prefix.
 */
#define VALIDATION_LOG_PREFIX  "[DATA_VALIDATION]"

/*
 * Mismatch classification returned by the epoch guard / DMA retry logic.
 * Shared so that both backends report the same category to the common
 * result struct and log format.
 */
enum validation_mismatch_type {
	VALIDATION_MISMATCH_NONE  = 0,  /* No mismatch (all bytes match) */
	VALIDATION_MISMATCH_REAL  = 1,  /* Genuine data corruption */
	VALIDATION_MISMATCH_RACE  = 2,  /* DMA overwrite detected by epoch guard */
	VALIDATION_MISMATCH_STALE = 3   /* DMA tail flush resolved by retry */
};

/*
 * Portable inline qualifier: works in both C (.c) and CUDA (.cu) files.
 */
#ifndef VALIDATION_INLINE
#ifdef __CUDACC__
#define VALIDATION_INLINE __host__ __device__ static inline
#else
#define VALIDATION_INLINE static inline
#endif
#endif

/*
 * Map a 0-based cycle number to the expected fill-pattern byte.
 * Patterns are consecutive: 0x01, 0x02, 0x03 — no lookup table needed.
 * Used by both host (cycle = marker_val - 1) and CUDA (cycle =
 * chunks_validated / chunks_per_qp).
 */
VALIDATION_INLINE uint8_t validation_cycle_to_pattern(uint64_t cycle)
{
	return (uint8_t)(VALIDATION_PATTERN_0 +
			 (cycle % VALIDATION_PATTERNS_COUNT));
}

/* -----------------------------------------------------------------------
 * Chunk-size / ring-depth derivation (shared by host and CUDA paths)
 * ----------------------------------------------------------------------- */

/* Tuning constants for validation chunk sizing */
#define VALIDATION_MIN_CHUNK_BYTES  (64ULL * 1024)         /* 64 KB */
#define VALIDATION_MAX_CHUNK_BYTES  (8ULL * 1024 * 1024)   /* 8 MB */

/*
 * Derive validation_chunk_size (ops per marker) from payload and tx_depth.
 * Returns the number of operations between marker firings.
 * Adjusts when chunk_bytes falls outside
 * [VALIDATION_MIN_CHUNK_BYTES, VALIDATION_MAX_CHUNK_BYTES].
 */
VALIDATION_INLINE uint32_t
derive_validation_chunk_size(uint64_t payload, uint32_t tx_depth)
{
	uint64_t raw_chunk_bytes = payload * tx_depth;
	uint32_t vcs = tx_depth;

	if (raw_chunk_bytes < VALIDATION_MIN_CHUNK_BYTES && payload > 0) {
		uint32_t needed = (uint32_t)
			((VALIDATION_MIN_CHUNK_BYTES + payload - 1) / payload);
		vcs = ((needed + tx_depth - 1) / tx_depth) * tx_depth;
	} else if (raw_chunk_bytes > VALIDATION_MAX_CHUNK_BYTES && payload > 0) {
		uint32_t target = (uint32_t)(VALIDATION_MAX_CHUNK_BYTES / payload);
		if (target >= tx_depth)
			vcs = (target / tx_depth) * tx_depth;
		else
			vcs = target > 0 ? target : 1;
	}
	return vcs;
}

/*
 * Derive chunks_per_qp (ring depth) from chunk_bytes, QP count, and
 * whether this is a READ test (is_read != 0).
 *
 * Smaller chunks → sender wraps faster → deeper ring needed.
 * Capped at VALIDATION_MAX_CHUNKS_PER_QP.
 */
VALIDATION_INLINE int
derive_validation_chunks_per_qp(uint64_t chunk_bytes,
				int num_qps, int is_read)
{
	int base;

	if (chunk_bytes < 128 * 1024)
		base = 12;
	else if (chunk_bytes < 512 * 1024)
		base = 10;
	else if (chunk_bytes < 2 * 1024 * 1024)
		base = 8;
	else if (chunk_bytes < 8 * 1024 * 1024)
		base = 6;
	else
		base = 4;

	if (num_qps <= 2)
		base *= 2;
	else if (num_qps <= 4)
		base = (base * 3) / 2;

	if (is_read)
		base = (base * 3) / 2;

	if (base > VALIDATION_MAX_CHUNKS_PER_QP)
		base = VALIDATION_MAX_CHUNKS_PER_QP;

	return base;
}

/* -----------------------------------------------------------------------
 * Buffer layout computation (shared by host and CUDA paths)
 * ----------------------------------------------------------------------- */

#define VALIDATION_CACHE_LINE_ALIGN(x)  (((x) + 63) & ~63ULL)

struct validation_buffer_layout {
	uint64_t total_size;
	uint64_t markers_offset;
	uint64_t recv_slots_offset;
	uint64_t atomic_returns_offset;
	uint64_t markers_size;
	uint64_t recv_slots_size;
	uint64_t atomic_returns_size;
	uint32_t slots_per_qp;
};

/*
 * Checked multiplication: returns 0 on success, -1 on overflow.
 */
VALIDATION_INLINE int validation_safe_mul_u64(uint64_t a, uint64_t b,
					      uint64_t *result)
{
	if (a != 0 && b > UINT64_MAX / a)
		return -1;
	*result = a * b;
	return 0;
}

/*
 * Compute the validation buffer layout.
 * Returns 0 on success, -1 on overflow or invalid parameters.
 * Layout: [3 pattern buffers] [markers] [recv_slots] [atomic_returns]
 */
VALIDATION_INLINE int
compute_validation_layout(uint64_t payload_size,
			  uint32_t tx_depth,
			  uint32_t num_qps,
			  uint32_t chunks_per_qp,
			  int is_read_mode,
			  struct validation_buffer_layout *out)
{
	if (!out || payload_size == 0 || tx_depth == 0 ||
	    num_qps == 0 || chunks_per_qp == 0)
		return -1;

	memset(out, 0, sizeof(*out));

	uint32_t total_chunks = num_qps * chunks_per_qp;
	out->slots_per_qp = tx_depth * chunks_per_qp;

	out->markers_size = (uint64_t)total_chunks * sizeof(uint64_t);

	uint64_t chunk_bytes, qp_bytes;
	if (validation_safe_mul_u64(payload_size, tx_depth, &chunk_bytes) != 0)
		return -1;
	if (validation_safe_mul_u64(chunk_bytes, chunks_per_qp, &qp_bytes) != 0)
		return -1;
	if (validation_safe_mul_u64(qp_bytes, num_qps, &out->recv_slots_size) != 0)
		return -1;

	out->atomic_returns_size = is_read_mode
		? (uint64_t)total_chunks * sizeof(uint64_t)
		: (uint64_t)num_qps * sizeof(uint64_t);

	out->markers_offset = VALIDATION_CACHE_LINE_ALIGN(payload_size * 3);
	out->recv_slots_offset = VALIDATION_CACHE_LINE_ALIGN(
		out->markers_offset + out->markers_size);
	out->atomic_returns_offset = VALIDATION_CACHE_LINE_ALIGN(
		out->recv_slots_offset + out->recv_slots_size);
	out->total_size = out->atomic_returns_offset + out->atomic_returns_size;

	return 0;
}

/*
 * Classify a byte mismatch as real corruption, DMA race overwrite, or
 * DMA stale (tail flush resolved).
 *
 * This is the shared decision function.  The caller is responsible for
 * performing the platform-specific memory reads (CPU direct / __ldg)
 * and passing the results here.
 *
 * @param error_offset   Byte offset of the first mismatch within the chunk
 * @param chunk_size     Total chunk size in bytes
 * @param payload_size   Single message payload size in bytes
 * @param expected       Expected pattern byte
 * @param retry_matched  1 if the DMA tail retry re-read matched expected
 *                       (caller only sets this when error is in tail zone)
 * @param fwd            4 forward bytes read from [error+1 .. error+4]
 *                       (only valid when fwd_valid == 1)
 * @param fwd_valid      1 if forward bytes could be read (error+4 < chunk_size)
 *
 * @return  VALIDATION_MISMATCH_STALE if retry resolved,
 *          VALIDATION_MISMATCH_RACE  if epoch guard detects overwrite,
 *          VALIDATION_MISMATCH_REAL  otherwise (genuine corruption).
 */
VALIDATION_INLINE enum validation_mismatch_type
classify_validation_mismatch(uint64_t error_offset,
			     uint64_t chunk_size,
			     uint64_t payload_size,
			     uint8_t  expected,
			     int      retry_matched,
			     const uint8_t fwd[4],
			     int      fwd_valid)
{
	/*
	 * Step 1: DMA tail retry.
	 * If the error is in the tail zone and the retry re-read now
	 * matches, it was a PCIe flush latency artefact.
	 */
	uint64_t tail_bytes = payload_size * 2;
	if (tail_bytes < 256)
		tail_bytes = 256;

	if (error_offset >= chunk_size - tail_bytes && retry_matched)
		return VALIDATION_MISMATCH_STALE;

	/*
	 * Step 2: Epoch guard.
	 * If 4 forward bytes are all != expected, a new DMA cycle has
	 * started overwriting this chunk — race, not corruption.
	 */
	if (fwd_valid &&
	    fwd[0] != expected && fwd[1] != expected &&
	    fwd[2] != expected && fwd[3] != expected)
		return VALIDATION_MISMATCH_RACE;

	return VALIDATION_MISMATCH_REAL;
}

/*
 * Configuration bundle for validation_init (shared by host and CUDA backends).
 * Groups related parameters to prevent argument-order errors.
 */
struct validation_config {
	void *buffer_base;
	uint64_t markers_offset;
	uint64_t recv_slots_offset;
	uint64_t payload_size;
	uint32_t ops_per_chunk;
	uint32_t num_qps;
	uint32_t chunks_per_qp;
	int validation_mode;
	int debug_enabled;
	int numa_node;
};

#ifdef __cplusplus
}
#endif

#endif /* VALIDATION_COMMON_H */
