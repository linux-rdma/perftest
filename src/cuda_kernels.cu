/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * CUDA Kernel Plugin for perftest
 *
 * This file is compiled into libperftest_kernels.so and loaded
 * dynamically at runtime, allowing perftest to run on systems
 * without CUDA installed.
 *
 * All functions marked extern "C" are exported and loaded via dlsym().
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cuda_runtime.h>

#include "cuda_validation.h"

#define VDBG(fmt, ...) \
	do { if (g_ctx.debug_enabled) \
		printf(VALIDATION_LOG_PREFIX " " fmt, ##__VA_ARGS__); \
	} while (0)

/* --- Global validation context (single instance, avoids dlsym ABI issues) --- */
static struct ValidationContext g_ctx;
static int g_initialized = 0;

/* --- Error Handling --- */

extern "C" const char* validation_strerror(int err)
{
	switch (err) {
		case VALIDATION_OK:
			return "Success";
		case VALIDATION_ERR_NULL_PARAM:
			return "NULL parameter passed";
		case VALIDATION_ERR_CUDA_DEVICE:
			return "Failed to set CUDA device";
		case VALIDATION_ERR_ALLOC_QUEUE:
			return "Failed to allocate work queue";
		case VALIDATION_ERR_ALLOC_QP_STATE:
			return "Failed to allocate QP state";
		case VALIDATION_ERR_ALLOC_ERROR_INFO:
			return "Failed to allocate error info";
		case VALIDATION_ERR_ALLOC_STATS:
			return "Failed to allocate stats";
		case VALIDATION_ERR_ALLOC_FLAGS:
			return "Failed to allocate control flags";
		case VALIDATION_ERR_MEMSET:
			return "Failed to zero memory";
		case VALIDATION_ERR_ALREADY_RUNNING:
			return "Kernel already running";
		case VALIDATION_ERR_INIT_FAILED:
			return "Context initialization failed";
		case VALIDATION_ERR_LAUNCH:
			return "Kernel launch failed";
		case VALIDATION_ERR_SYNC:
			return "Device synchronization failed";
		case VALIDATION_ERR_NOT_RUNNING:
			return "Kernel not running";
		default:
			return "Unknown error";
	}
}

/* --- Lock-free work queue (single-producer monitor, multi-consumer validators) --- */

/* Push a work item (called only by monitor block, no contention on head). */
__device__ bool work_queue_push(struct ValidationWorkQueue *queue,
                                uint32_t qp_id,
                                uint16_t chunk_id,
                                uint8_t pattern)
{
	uint32_t my_slot_index = atomicAdd((uint32_t*)&queue->head, 1);

	uint32_t tail = *(volatile uint32_t*)&queue->tail;
	if ((my_slot_index - tail) >= VALIDATION_QUEUE_SIZE)
		return false;

	uint32_t slot = my_slot_index & (VALIDATION_QUEUE_SIZE - 1);

	queue->items[slot].qp_id = qp_id;
	queue->items[slot].chunk_id = chunk_id;
	queue->items[slot].expected_pattern = pattern;

	__threadfence();
	queue->items[slot].valid = 1;
	__threadfence();

	return true;
}

/* Non-blocking pop (returns false on empty or CAS contention). */
__device__ bool work_queue_try_pop(struct ValidationWorkQueue *queue,
                                   struct ValidationWorkItem *item_out)
{
	uint32_t tail = *(volatile uint32_t*)&queue->tail;
	uint32_t head = *(volatile uint32_t*)&queue->head;

	if (tail >= head) {
		item_out->valid = 0;
		return false;
	}

	uint32_t claimed = atomicCAS((uint32_t*)&queue->tail, tail, tail + 1);
	if (claimed != tail) {
		item_out->valid = 0;
		return false;
	}

	uint32_t slot = tail & (VALIDATION_QUEUE_SIZE - 1);

	/* Spin until producer marks slot valid */
	while (*(volatile uint8_t*)&queue->items[slot].valid == 0)
		__threadfence();

	item_out->qp_id = queue->items[slot].qp_id;
	item_out->chunk_id = queue->items[slot].chunk_id;
	item_out->expected_pattern = queue->items[slot].expected_pattern;
	item_out->valid = 1;

	queue->items[slot].valid = 0;
	return true;
}

/* --- Monitor block: scan tail markers, push work items to queue --- */

__device__ void monitor_scan_tails(
	volatile uint64_t *tail_markers,
	uint32_t num_qps,
	uint32_t chunks_per_qp,
	struct QpValidationState *qp_state,
	struct ValidationWorkQueue *work_queue,
	volatile uint32_t *validation_failed,
	volatile uint32_t *should_stop,
	struct ValidationStats *stats)
{
	uint32_t tid = threadIdx.x;
	uint32_t num_threads = blockDim.x;
	uint32_t total_tails = num_qps * chunks_per_qp;

	while (!(*validation_failed) && !(*should_stop)) {
		for (uint32_t i = tid; i < total_tails; i += num_threads) {

			/* Atomic read-and-reset: eliminates race between read and clear */
			uint64_t tail_value = atomicExch((unsigned long long*)&tail_markers[i], 0ULL);

			/* Force L2→DRAM writeback for GPUDirect RDMA coherency */
			__threadfence_system();

			if (tail_value != 0) {
				uint32_t qp_id = i / chunks_per_qp;
				uint32_t chunk_id = i % chunks_per_qp;

				atomicAdd((unsigned long long*)&stats->monitor_detections, 1);

				/* atomicAdd returns OLD value — unique sequence per concurrent detection */
				uint32_t chunks_so_far = atomicAdd(&qp_state[qp_id].chunks_validated, 1);
				uint8_t expected_pattern = validation_cycle_to_pattern(chunks_so_far / chunks_per_qp);

				bool pushed = work_queue_push(work_queue, qp_id, (uint16_t)chunk_id, expected_pattern);
				(void)pushed;
			}
		}
	}
}

/* --- Validator block logic: pop work items, validate data --- */

/*
 * Validate a chunk using uint4 (16-byte) vectorized loads for peak bandwidth.
 * Handles unaligned head/tail with scalar fallback.
 * Returns number of mismatches found by this thread.
 */
__device__ uint32_t validate_chunk_portion(
	const uint8_t *data,
	uint64_t size,
	uint8_t expected,
	uint64_t *error_offset,
	uint8_t *actual_byte)
{
	const uint32_t tid = threadIdx.x;
	const uint32_t num_threads = blockDim.x;

	/* Build 16-byte pattern from single byte */
	const uint32_t pattern32 = expected | (expected << 8) |
	                           (expected << 16) | (expected << 24);
	const uint4 pattern128 = make_uint4(pattern32, pattern32, pattern32, pattern32);

	/* Aligned boundaries */
	const uintptr_t data_addr = (uintptr_t)data;
	const uintptr_t aligned_start_addr = (data_addr + 15) & ~15ULL;
	const uintptr_t aligned_end_addr = (data_addr + size) & ~15ULL;

	const uint64_t head_size = (aligned_start_addr > data_addr) ?
	                           (aligned_start_addr - data_addr) : 0;
	const uint64_t tail_offset = aligned_end_addr - data_addr;
	const uint64_t tail_size = size - tail_offset;
	const uint4 *aligned_data = (const uint4*)aligned_start_addr;
	const uint64_t num_uint4s = (aligned_end_addr > aligned_start_addr) ?
	                            ((aligned_end_addr - aligned_start_addr) / 16) : 0;

	uint32_t my_errors = 0;
	uint64_t my_first_error = UINT64_MAX;
	uint8_t my_error_byte = 0;

	/* Unaligned head (0-15 bytes) */
	for (uint64_t i = tid; i < head_size; i += num_threads) {
		uint8_t val = data[i];
		if (val != expected) {
			my_errors++;
			if (i < my_first_error) {
				my_first_error = i;
				my_error_byte = val;
			}
		}
	}

	/* Aligned region — vectorized uint4 loads (hot path) */
	for (uint64_t idx = tid; idx < num_uint4s; idx += num_threads) {
		uint4 loaded = __ldg(&aligned_data[idx]);

		if ((loaded.x != pattern128.x) | (loaded.y != pattern128.y) |
		    (loaded.z != pattern128.z) | (loaded.w != pattern128.w)) {
			uint64_t base_offset = head_size + idx * 16;
			const uint8_t *bytes = (const uint8_t*)&loaded;
			const uint8_t *exp = (const uint8_t*)&pattern128;

			#pragma unroll
			for (int b = 0; b < 16; b++) {
				if (bytes[b] != exp[b]) {
					my_errors++;
					uint64_t offset = base_offset + b;
					if (offset < my_first_error) {
						my_first_error = offset;
						my_error_byte = bytes[b];
					}
				}
			}
		}
	}

	/* Unaligned tail (0-15 bytes) */
	for (uint64_t i = tid; i < tail_size; i += num_threads) {
		uint64_t byte_idx = tail_offset + i;
		uint8_t val = data[byte_idx];
		if (val != expected) {
			my_errors++;
			if (byte_idx < my_first_error) {
				my_first_error = byte_idx;
				my_error_byte = val;
			}
		}
	}

	*error_offset = my_first_error;
	*actual_byte = my_error_byte;
	return my_errors;
}

/* Pop work items, validate data, classify mismatches. */
__device__ void validator_process_work(
	const uint8_t *recv_slots_base,
	uint64_t payload_size,
	uint32_t tx_depth,
	uint32_t num_qps,
	uint32_t chunks_per_qp,
	struct ValidationWorkQueue *work_queue,
	volatile uint32_t *validation_failed,
	volatile uint32_t *should_stop,
	struct ValidationErrorInfo *error_info,
	struct ValidationStats *stats)
{
	(void)num_qps;

	__shared__ struct ValidationWorkItem work;
	__shared__ uint32_t total_errors;
	__shared__ uint64_t first_error_offset;
	__shared__ uint8_t first_error_actual;
	__shared__ int have_work;
	__shared__ int should_exit;

	uint32_t tid = threadIdx.x;
	const uint64_t chunk_size = (uint64_t)tx_depth * payload_size;
	const uint64_t qp_stride = chunks_per_qp * chunk_size;

	while (true) {
		/* Thread 0 reads volatile flags into shared memory so ALL threads
		 * see the same value — prevents divergent __syncthreads() deadlock. */
		if (tid == 0) {
			should_exit = (*validation_failed || *should_stop) ? 1 : 0;
			have_work = 0;
			if (!should_exit) {
				have_work = work_queue_try_pop(work_queue, &work) ? 1 : 0;
				if (have_work) {
					total_errors = 0;
					first_error_offset = UINT64_MAX;
				}
			}
		}
		__syncthreads();

		if (should_exit)
			break;
		if (!have_work) {
			for (volatile int i = 0; i < 1; i++) {}
			continue;
		}

		const uint8_t *chunk_data = recv_slots_base +
		                            work.qp_id * qp_stride +
		                            work.chunk_id * chunk_size;

		/* Ensure RDMA PCIe writes are visible before validation */
		__threadfence_system();

		uint64_t my_error_offset;
		uint8_t my_error_actual;
		uint32_t my_errors = validate_chunk_portion(
			chunk_data, chunk_size, work.expected_pattern,
			&my_error_offset, &my_error_actual);

		/* Warp-level reduction: reduces 256 potential atomics to max 8 */
		const uint32_t FULL_WARP_MASK = 0xFFFFFFFF;
		const uint32_t lane = tid & 31;
		uint32_t warp_has_errors = __any_sync(FULL_WARP_MASK, my_errors > 0);

		uint32_t warp_error_count = 0;
		uint64_t warp_first_error_offset = UINT64_MAX;
		uint8_t warp_first_error_actual = 0;

		if (warp_has_errors) {
			warp_error_count = my_errors;
			#pragma unroll
			for (int offset = 16; offset > 0; offset /= 2)
				warp_error_count += __shfl_xor_sync(FULL_WARP_MASK, warp_error_count, offset);

			warp_first_error_offset = my_error_offset;
			warp_first_error_actual = my_error_actual;
			#pragma unroll
			for (int offset = 16; offset > 0; offset /= 2) {
				uint64_t other_offset = __shfl_xor_sync(FULL_WARP_MASK, warp_first_error_offset, offset);
				uint8_t other_actual = __shfl_xor_sync(FULL_WARP_MASK, warp_first_error_actual, offset);
				if (other_offset < warp_first_error_offset) {
					warp_first_error_offset = other_offset;
					warp_first_error_actual = other_actual;
				}
			}
		}

		if (lane == 0 && warp_has_errors) {
			atomicAdd(&total_errors, warp_error_count);
			if (warp_first_error_offset < first_error_offset) {
				first_error_offset = warp_first_error_offset;
				first_error_actual = warp_first_error_actual;
			}
		}
		__syncthreads();

		/* Thread 0: classify mismatch and update stats */
		if (tid == 0) {
			if (total_errors > 0) {
				uint64_t err_off = first_error_offset;
				uint8_t err_actual = first_error_actual;
				uint8_t expected = work.expected_pattern;

				/* DMA tail retry */
				int retry_matched = 0;
				uint64_t tail_bytes = payload_size * 2;
				if (tail_bytes < 256)
					tail_bytes = 256;
				if (err_off >= chunk_size - tail_bytes) {
					__threadfence_system();
					for (volatile int d = 0; d < 32; d++) {}
					uint8_t retry_val = __ldg(&chunk_data[err_off]);
					if (retry_val == expected)
						retry_matched = 1;
				}

				/* Forward-byte snapshot for epoch guard */
				uint8_t fwd[4] = {expected, expected, expected, expected};
				int fwd_valid = 0;
				if (!retry_matched && (err_off + 4) < chunk_size) {
					fwd[0] = __ldg(&chunk_data[err_off + 1]);
					fwd[1] = __ldg(&chunk_data[err_off + 2]);
					fwd[2] = __ldg(&chunk_data[err_off + 3]);
					fwd[3] = __ldg(&chunk_data[err_off + 4]);
					fwd_valid = 1;
				}

				enum validation_mismatch_type mtype =
					classify_validation_mismatch(
						err_off, chunk_size,
						payload_size, expected,
						retry_matched, fwd, fwd_valid);

				if (mtype == VALIDATION_MISMATCH_STALE) {
					atomicAdd((unsigned long long *)&stats->dma_stale_retries, 1);
				} else if (mtype == VALIDATION_MISMATCH_RACE) {
					atomicAdd((unsigned long long *)&stats->race_overwrites, 1);
				} else {
					error_info->qp_id = work.qp_id;
					error_info->chunk_id = work.chunk_id;
					error_info->byte_offset = err_off;
					error_info->expected = expected;
					error_info->actual = err_actual;
					atomicExch((uint32_t *)validation_failed, 1);
					atomicAdd((unsigned long long *)&stats->errors_found, total_errors);
				}
			}
			atomicAdd((unsigned long long*)&stats->chunks_validated, 1);
			atomicAdd((unsigned long long*)&stats->bytes_validated, chunk_size);
		}
		__syncthreads();
	}
}

/* --- Main kernel entry point (block 0 = monitor, blocks 1..N = validators) --- */
__global__ void validation_kernel(
	volatile uint64_t *tail_markers,
	const uint8_t *recv_slots_base,
	uint64_t payload_size,
	uint32_t tx_depth,
	uint32_t num_qps,
	uint32_t chunks_per_qp,
	struct QpValidationState *qp_state,
	struct ValidationWorkQueue *work_queue,
	volatile uint32_t *validation_failed,
	volatile uint32_t *should_stop,
	struct ValidationErrorInfo *error_info,
	struct ValidationStats *stats)
{
	/* Block 0 is the monitor, blocks 1..N are validators */
	if (blockIdx.x == 0) {
		monitor_scan_tails(tail_markers, num_qps, chunks_per_qp,
		                   qp_state, work_queue,
		                   validation_failed, should_stop, stats);
	} else {
		validator_process_work(recv_slots_base, payload_size, tx_depth,
		                       num_qps, chunks_per_qp, work_queue,
		                       validation_failed, should_stop,
		                       error_info, stats);
	}
}

/* --- Exported Plugin API (loaded via dlsym) --- */

/* Free all GPU allocations in g_ctx (NULL-safe, idempotent). */
static void free_validation_allocations(void)
{
	if (g_ctx.d_work_queue)        cudaFree(g_ctx.d_work_queue);
	if (g_ctx.d_qp_state)          cudaFree(g_ctx.d_qp_state);
	if (g_ctx.d_error_info)        cudaFree(g_ctx.d_error_info);
	if (g_ctx.d_stats)             cudaFree(g_ctx.d_stats);
	if (g_ctx.d_validation_failed) cudaFree((void*)g_ctx.d_validation_failed);
	if (g_ctx.d_should_stop)       cudaFree((void*)g_ctx.d_should_stop);
	/* d_tail_markers points into main buffer — don't free */
}

/* Zero all runtime state (GPU buffers + host copies) for a fresh run. */
static void reset_validation_state(void)
{
	cudaMemset(g_ctx.d_work_queue, 0, sizeof(struct ValidationWorkQueue));
	cudaMemset(g_ctx.d_qp_state, 0, g_ctx.params.num_qps * sizeof(struct QpValidationState));
	cudaMemset(g_ctx.d_error_info, 0, sizeof(struct ValidationErrorInfo));
	cudaMemset(g_ctx.d_stats, 0, sizeof(struct ValidationStats));
	*(uint32_t*)g_ctx.d_validation_failed = 0;
	*(uint32_t*)g_ctx.d_should_stop = 0;
	memset(&g_ctx.h_error_info, 0, sizeof(struct ValidationErrorInfo));
	memset(&g_ctx.h_stats, 0, sizeof(struct ValidationStats));
}

static int calculate_validator_blocks(uint32_t num_qps)
{
	int blocks = num_qps * 16;
	if (blocks < 32) blocks = 32;
	if (blocks > VALIDATION_MAX_VALIDATOR_BLOCKS) blocks = VALIDATION_MAX_VALIDATOR_BLOCKS;
	return blocks;
}

/* Initialize the validation context and allocate GPU memory. */
extern "C" int validation_init(
	void *buffer_base,
	uint64_t markers_offset,
	uint64_t recv_slots_offset,
	uint64_t payload_size,
	uint32_t tx_depth,
	uint32_t num_qps,
	uint32_t chunks_per_qp,
	int validation_mode,
	int cuda_device_id,
	int debug_enabled)
{
	cudaError_t err;
	(void)validation_mode;

	if (buffer_base == NULL) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error: %s\n", validation_strerror(VALIDATION_ERR_NULL_PARAM));
		return -1;
	}

	err = cudaSetDevice(cuda_device_id);
	if (err != cudaSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error setting CUDA device %d: %s\n",
		        cuda_device_id, cudaGetErrorString(err));
		return -1;
	}
	if (debug_enabled) printf(VALIDATION_LOG_PREFIX " Using CUDA device %d\n", cuda_device_id);

	if (chunks_per_qp == 0 || chunks_per_qp > VALIDATION_MAX_CHUNKS_PER_QP) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error: chunks_per_qp=%u out of range [1, %d]\n",
			chunks_per_qp, VALIDATION_MAX_CHUNKS_PER_QP);
		return -1;
	}

	memset(&g_ctx, 0, sizeof(g_ctx));
	g_ctx.cuda_device_id = cuda_device_id;
	g_ctx.debug_enabled = debug_enabled;
	g_ctx.chunks_per_qp = chunks_per_qp;

	g_ctx.params.tail_markers_addr = (uint64_t)buffer_base + markers_offset;
	g_ctx.params.recv_slots_addr = (uint64_t)buffer_base + recv_slots_offset;
	g_ctx.params.patterns_addr = (uint64_t)buffer_base;
	g_ctx.params.payload_size = payload_size;
	g_ctx.params.tx_depth = tx_depth;
	g_ctx.params.num_qps = num_qps;
	g_ctx.d_tail_markers = (volatile uint64_t*)g_ctx.params.tail_markers_addr;

	int num_validators = calculate_validator_blocks(num_qps);
	g_ctx.params.num_validator_blocks = num_validators;
	g_ctx.num_blocks = 1 + num_validators;

	VDBG("Creating context: num_qps=%u, tx_depth=%u, payload=%lu, chunks_per_qp=%u\n",
	     num_qps, tx_depth, payload_size, chunks_per_qp);
	VDBG("Blocks: 1 monitor + %d validators = %d total\n",
	     num_validators, g_ctx.num_blocks);

	/* Allocate GPU memory — all pointers start as NULL (memset above),
	 * so the goto-fail cleanup can safely free any partial allocations. */
	err = cudaMalloc((void**)&g_ctx.d_work_queue, sizeof(struct ValidationWorkQueue));
	if (err != cudaSuccess) goto alloc_fail;

	err = cudaMalloc((void**)&g_ctx.d_qp_state,
			 num_qps * sizeof(struct QpValidationState));
	if (err != cudaSuccess) goto alloc_fail;

	err = cudaMallocManaged((void**)&g_ctx.d_error_info, sizeof(struct ValidationErrorInfo));
	if (err != cudaSuccess) goto alloc_fail;

	err = cudaMallocManaged((void**)&g_ctx.d_stats, sizeof(struct ValidationStats));
	if (err != cudaSuccess) goto alloc_fail;

	err = cudaMallocManaged((void**)&g_ctx.d_validation_failed, sizeof(uint32_t));
	if (err != cudaSuccess) goto alloc_fail;

	err = cudaMallocManaged((void**)&g_ctx.d_should_stop, sizeof(uint32_t));
	if (err != cudaSuccess) goto alloc_fail;

	reset_validation_state();

	g_initialized = 1;
	g_ctx.kernel_running = 0;

	VDBG("Context created successfully\n");
	return 0;

alloc_fail:
	fprintf(stderr, VALIDATION_LOG_PREFIX " CUDA allocation failed: %s\n",
		cudaGetErrorString(err));
	free_validation_allocations();
	memset(&g_ctx, 0, sizeof(g_ctx));
	return -1;
}

/* Start the validation kernel. Params/num_blocks/threads_per_block are unused (kept for ABI). */
extern "C" int validation_start(void *params, int num_blocks, int threads_per_block)
{
	(void)params;
	(void)num_blocks;
	(void)threads_per_block;

	if (!g_initialized) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error: Context not initialized\n");
		return -1;
	}
	if (g_ctx.kernel_running) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error: %s\n", validation_strerror(VALIDATION_ERR_ALREADY_RUNNING));
		return -1;
	}

	cudaError_t err = cudaSetDevice(g_ctx.cuda_device_id);
	if (err != cudaSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error setting CUDA device: %s\n",
		        cudaGetErrorString(err));
		return -1;
	}

	reset_validation_state();

	uint8_t *recv_slots_base = (uint8_t*)g_ctx.params.recv_slots_addr;

	VDBG("Launching kernel with %d blocks (%d threads each)\n",
	     g_ctx.num_blocks, VALIDATION_THREADS_PER_BLOCK);
	VDBG("tail_markers: %p, recv_slots: %p\n",
	     (void*)g_ctx.d_tail_markers, recv_slots_base);
	VDBG("payload_size: %lu, tx_depth: %u, num_qps: %u\n",
	     g_ctx.params.payload_size, g_ctx.params.tx_depth, g_ctx.params.num_qps);

	validation_kernel<<<g_ctx.num_blocks, VALIDATION_THREADS_PER_BLOCK>>>(
		g_ctx.d_tail_markers,
		recv_slots_base,
		g_ctx.params.payload_size,
		g_ctx.params.tx_depth,
		g_ctx.params.num_qps,
		g_ctx.chunks_per_qp,
		g_ctx.d_qp_state,
		g_ctx.d_work_queue,
		g_ctx.d_validation_failed,
		g_ctx.d_should_stop,
		g_ctx.d_error_info,
		g_ctx.d_stats);

	err = cudaGetLastError();
	if (err != cudaSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error launching kernel: %s\n",
		        cudaGetErrorString(err));
		return -1;
	}

	g_ctx.kernel_running = 1;
	VDBG("Kernel launched successfully\n");
	return 0;
}

extern "C" int validation_stop(void)
{
	if (!g_initialized || !g_ctx.kernel_running)
		return 0;

	VDBG("Stopping kernel...\n");
	*(volatile uint32_t*)g_ctx.d_should_stop = 1;

	cudaError_t err = cudaDeviceSynchronize();
	if (err != cudaSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error waiting for kernel: %s\n",
		        cudaGetErrorString(err));
		return -1;
	}

	g_ctx.kernel_running = 0;

	VDBG("Kernel stopped\n");
	VDBG("Final stats: chunks=%lu, bytes=%lu, errors=%lu, "
	     "detections=%lu, races=%lu, retries=%lu\n",
	     g_ctx.d_stats->chunks_validated,
	     g_ctx.d_stats->bytes_validated,
	     g_ctx.d_stats->errors_found,
	     g_ctx.d_stats->monitor_detections,
	     g_ctx.d_stats->race_overwrites,
	     g_ctx.d_stats->dma_stale_retries);

	return 0;
}

/* Read stats from managed memory (NULL-safe for optional outputs). */
extern "C" int validation_get_stats(uint64_t *chunks_validated,
				    uint64_t *bytes_validated,
				    uint64_t *errors_found,
				    uint64_t *race_overwrites,
				    uint64_t *dma_stale_retries)
{
	if (!g_initialized) return -1;
	if (chunks_validated) *chunks_validated = g_ctx.d_stats->chunks_validated;
	if (bytes_validated) *bytes_validated = g_ctx.d_stats->bytes_validated;
	if (errors_found) *errors_found = g_ctx.d_stats->errors_found;
	if (race_overwrites) *race_overwrites = g_ctx.d_stats->race_overwrites;
	if (dma_stale_retries) *dma_stale_retries = g_ctx.d_stats->dma_stale_retries;
	return 0;
}

/* Read error details from managed memory. */
extern "C" int validation_get_error(uint32_t *qp_id, uint32_t *chunk_id,
				    uint64_t *byte_offset,
				    uint8_t *expected, uint8_t *actual)
{
	if (!g_initialized) return -1;
	if (qp_id) *qp_id = g_ctx.d_error_info->qp_id;
	if (chunk_id) *chunk_id = g_ctx.d_error_info->chunk_id;
	if (byte_offset) *byte_offset = g_ctx.d_error_info->byte_offset;
	if (expected) *expected = g_ctx.d_error_info->expected;
	if (actual) *actual = g_ctx.d_error_info->actual;
	return 0;
}

extern "C" int validation_destroy(void)
{
	int debug = g_ctx.debug_enabled;

	if (!g_initialized) return 0;

	if (g_ctx.kernel_running)
		validation_stop();

	free_validation_allocations();
	memset(&g_ctx, 0, sizeof(g_ctx));
	g_initialized = 0;

	if (debug)
		printf(VALIDATION_LOG_PREFIX " Context destroyed, all memory freed\n");
	return 0;
}

extern "C" int validation_is_running(void)
{
	return g_initialized && g_ctx.kernel_running;
}

/* Returns 0 = OK, 1 = failed, -1 = not initialized. */
extern "C" int validation_check_status(void)
{
	if (!g_initialized) return -1;
	uint32_t failed = 0;
	cudaMemcpy(&failed, (void*)g_ctx.d_validation_failed, sizeof(uint32_t), cudaMemcpyDeviceToHost);
	return failed ? 1 : 0;
}

/* --- GPU page touch (--gpu_touch feature, ensures pages are resident for RDMA) --- */

#define GPU_TOUCH_STEP 4096

__global__ void cuda_touch_pages(volatile uint8_t *c, int size,
                                 volatile int *stop_flag, int is_infinite)
{
	do {
		for (int iter = 0; iter < size; iter += GPU_TOUCH_STEP)
			c[iter] = 0;
	} while (is_infinite && !*stop_flag);
}

extern "C" int touch_gpu_pages(uint8_t *addr, int buf_size,
                               int is_infinite, volatile int **stop_flag)
{
	cuda_touch_pages<<<1, 1>>>(addr, buf_size, *stop_flag, is_infinite);
	cudaError_t err = cudaGetLastError();
	if (err != cudaSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " GPU touch kernel launch error: %s\n",
		        cudaGetErrorString(err));
		return -1;
	}
	return 0;
}

extern "C" int init_gpu_stop_flag(volatile int **stop_flag)
{
	cudaError_t ret = cudaMallocManaged((void **)stop_flag, sizeof(int));
	if (ret != cudaSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Failed to allocate stop flag: %s\n",
		        cudaGetErrorString(ret));
		return -1;
	}
	**stop_flag = 0;
	return 0;
}
