/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * HIP validation kernels for perftest ROCm memory.
 *
 * A port of src/cuda_kernels.cu, kept deliberately close to it: same
 * monitor/validator split, same lock-free work queue, same shared structures
 * from cuda_validation.h, same exported entry points (prefixed rocm_ because
 * these are linked into libperftest.a rather than dlopen'd from a plugin
 * .so, and must not collide when both backends are enabled).
 *
 * Divergences from the CUDA original are only where CUDA semantics do not
 * exist on AMD, and each is marked PORT: below. The important one is warp
 * width -- the CUDA code assumes 32 lanes, AMD wavefronts are 32 or 64 --
 * which is a correctness issue, not a tuning one.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <hip/hip_runtime.h>

#include "cuda_validation.h"

/*
 * PORT: HIP's warp intrinsics take no mask operand -- an AMD wavefront is
 * always fully converged at these points -- so the CUDA *_sync forms are
 * mapped onto the HIP ones and the mask argument is discarded.
 */
#define PORT_any_sync(mask, pred)           __any(pred)
#define PORT_shfl_xor_sync(mask, var, lane) __shfl_xor(var, lane)

/*
 * PORT: a real backoff in the poll loops.
 *
 * The CUDA original spins tightly -- its idle hint is an empty
 * `for (volatile int i = 0; i < 1; i++) {}`, which costs a few cycles and
 * never yields. That is fine on hardware that preempts waves. It is not fine
 * everywhere: a device that runs a wave until it blocks will never retire a
 * tight poll loop, so the kernel cannot observe should_stop and the host
 * hangs in hipDeviceSynchronize() forever. s_sleep yields the wave for a
 * short interval and costs nothing on real silicon.
 */
#define PORT_POLL_BACKOFF() __builtin_amdgcn_s_sleep(1)

/*
 * PORT: scoped atomics in place of the CUDA original's volatile idiom.
 *
 * The CUDA code polls through `*(volatile T *)p` and orders with
 * __threadfence[_system](). volatile is not a synchronisation primitive: it
 * constrains the compiler for one access and says nothing about which agents
 * the access must be coherent with. It works on NVIDIA because the emitted
 * loads happen to bypass cache, not because the language promises it.
 *
 * HIP can state the scope exactly, and on AMD the scope is what selects which
 * cache levels are bypassed and invalidated -- so being explicit is both more
 * honest and cheaper than the uniform __threadfence_system() it replaces.
 * Two scopes are in play:
 *
 *   SYSTEM  the tail markers and receive slots, written by the NIC's DMA, and
 *           should_stop and validation_failed, shared with the host.
 *   AGENT   the work queue, which only this kernel's own blocks touch.
 *
 * The queue handoff is a release/acquire pair on the slot's valid flag: the
 * producer's release publishes the item fields written before it, and the
 * consumer's acquire makes them visible. That ordering is what the bare
 * __threadfence() calls around the flag were standing in for.
 */
#define PORT_LOAD_SYSTEM(p) \
	__hip_atomic_load((p), __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM)
#define PORT_ACQUIRE_SYSTEM(p) \
	__hip_atomic_load((p), __ATOMIC_ACQUIRE, __HIP_MEMORY_SCOPE_SYSTEM)
#define PORT_LOAD_AGENT(p) \
	__hip_atomic_load((p), __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_AGENT)
#define PORT_ACQUIRE_AGENT(p) \
	__hip_atomic_load((p), __ATOMIC_ACQUIRE, __HIP_MEMORY_SCOPE_AGENT)
#define PORT_RELEASE_AGENT(p, v) \
	__hip_atomic_store((p), (v), __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_AGENT)

#define VDBG(fmt, ...) \
	do { if (g_ctx.debug_enabled) \
		printf(VALIDATION_LOG_PREFIX " " fmt, ##__VA_ARGS__); \
	} while (0)

/* --- Global validation context (single instance, avoids dlsym ABI issues) --- */
static struct ValidationContext g_ctx;
static int g_initialized = 0;

/* --- Error Handling --- */

extern "C" const char* rocm_kernel_validation_strerror(int err)
{
	switch (err) {
		case VALIDATION_OK:
			return "Success";
		case VALIDATION_ERR_NULL_PARAM:
			return "NULL parameter passed";
		case VALIDATION_ERR_CUDA_DEVICE:
			return "Failed to set GPU device";
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

/* --- Lock-free work queue (monitor threads produce, validator blocks consume) --- */

/* Push a work item without advancing head on queue-full failure. */
__device__ bool work_queue_push(struct ValidationWorkQueue *queue,
                                uint32_t qp_id,
                                uint16_t chunk_id,
                                uint8_t pattern,
                                uint64_t marker_epoch)
{
	uint32_t my_slot_index;

	while (true) {
		uint32_t head = PORT_LOAD_AGENT(&queue->head);
		uint32_t tail = PORT_LOAD_AGENT(&queue->tail);

		if ((head - tail) >= VALIDATION_QUEUE_SIZE)
			return false;

		uint32_t claimed = atomicCAS((uint32_t*)&queue->head, head, head + 1);
		if (claimed == head) {
			my_slot_index = head;
			break;
		}
	}

	uint32_t slot = my_slot_index & (VALIDATION_QUEUE_SIZE - 1);

	queue->items[slot].qp_id = qp_id;
	queue->items[slot].marker_epoch = marker_epoch;
	queue->items[slot].chunk_id = chunk_id;
	queue->items[slot].expected_pattern = pattern;

	/* Release: publishes the item fields written above to the consumer. */
	PORT_RELEASE_AGENT(&queue->items[slot].valid, (uint8_t)1);

	return true;
}

/* Non-blocking pop (returns false on empty or CAS contention). */
__device__ bool work_queue_try_pop(struct ValidationWorkQueue *queue,
                                   struct ValidationWorkItem *item_out,
                                   volatile uint32_t *should_stop)
{
	uint32_t tail = PORT_LOAD_AGENT(&queue->tail);
	uint32_t head = PORT_LOAD_AGENT(&queue->head);

	if (tail == head) {
		item_out->valid = 0;
		return false;
	}

	uint32_t claimed = atomicCAS((uint32_t*)&queue->tail, tail, tail + 1);
	if (claimed != tail) {
		item_out->valid = 0;
		return false;
	}

	uint32_t slot = tail & (VALIDATION_QUEUE_SIZE - 1);

	/*
	 * Spin until the producer marks the slot valid. The CAS above already
	 * claimed this slot, so a producer is committed to filling it -- but
	 * this is a poll loop like any other and must honour the rules at the
	 * top of this file: back off so the wave yields, and bail on
	 * should_stop so a wedged producer cannot strand the whole block. The
	 * caller runs this under __syncthreads(), so spinning here forever
	 * deadlocks every thread in the block, not just this one.
	 */
	while (PORT_ACQUIRE_AGENT(&queue->items[slot].valid) == 0) {
		if (PORT_LOAD_SYSTEM(should_stop)) {
			item_out->valid = 0;
			return false;
		}
		PORT_POLL_BACKOFF();
	}

	item_out->qp_id = queue->items[slot].qp_id;
	item_out->marker_epoch = queue->items[slot].marker_epoch;
	item_out->chunk_id = queue->items[slot].chunk_id;
	item_out->expected_pattern = queue->items[slot].expected_pattern;
	item_out->valid = 1;

	queue->items[slot].valid = 0;
	return true;
}

/* --- Monitor block: scan tail markers, push work items to queue --- */

__device__ void monitor_scan_tails(
	volatile uint64_t *tail_markers,
	uint64_t *marker_last_seen,
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
	uint32_t scan_passes = 0;

	while (!PORT_LOAD_SYSTEM(validation_failed) &&
	       !PORT_LOAD_SYSTEM(should_stop)) {
		PORT_POLL_BACKOFF();

		if (tid == 0 && (++scan_passes & 0x3FF) == 0)
			atomicAdd((unsigned long long*)&stats->markers_scanned,
			          (unsigned long long)total_tails * 1024ULL);

		for (uint32_t i = tid; i < total_tails; i += num_threads) {
			/*
			 * Acquire at system scope: seeing this marker is what
			 * makes the payload the NIC DMA'd beneath it visible.
			 * The CUDA original expressed that as a volatile read
			 * followed by __threadfence_system(); the acquire is
			 * the same ordering, attached to the load that needs it.
			 */
			uint64_t marker_value = PORT_ACQUIRE_SYSTEM(&tail_markers[i]);
			uint64_t last_seen = marker_last_seen[i];

			if (marker_value == 0 || marker_value <= last_seen)
				continue;

			uint32_t qp_id = i / chunks_per_qp;
			uint32_t chunk_id = i % chunks_per_qp;

			atomicAdd((unsigned long long*)&stats->monitor_detections, 1);

			uint8_t expected_pattern = validation_cycle_to_pattern(marker_value - 1);
			bool pushed = work_queue_push(work_queue, qp_id, (uint16_t)chunk_id,
			                              expected_pattern, marker_value);
			if (pushed) {
				atomicAdd((unsigned long long*)&stats->markers_hit, 1);
				if (marker_value - last_seen > 1)
					atomicAdd((unsigned long long*)&stats->skipped_steps,
					          (unsigned long long)(marker_value - last_seen - 1));
				marker_last_seen[i] = marker_value;
				atomicAdd(&qp_state[qp_id].chunks_validated, 1);
			} else {
				atomicAdd((unsigned long long*)&stats->queue_full_drops, 1);
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
	volatile uint64_t *tail_markers,
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
	__shared__ int stale_work;

	uint32_t tid = threadIdx.x;
	const uint64_t chunk_size = (uint64_t)tx_depth * payload_size;
	const uint64_t qp_stride = chunks_per_qp * chunk_size;

	while (true) {
		/* Thread 0 reads volatile flags into shared memory so ALL threads
		 * see the same value — prevents divergent __syncthreads() deadlock. */
		if (tid == 0) {
			should_exit = (PORT_LOAD_SYSTEM(validation_failed) ||
				       PORT_LOAD_SYSTEM(should_stop)) ? 1 : 0;
			have_work = 0;
			if (!should_exit) {
				have_work = work_queue_try_pop(work_queue, &work,
				                               should_stop) ? 1 : 0;
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
			PORT_POLL_BACKOFF();
			continue;
		}

		const uint8_t *chunk_data = recv_slots_base +
		                            work.qp_id * qp_stride +
		                            work.chunk_id * chunk_size;
		const uint32_t marker_idx = work.qp_id * chunks_per_qp + work.chunk_id;

		/* Thread 0 snapshots the marker so all threads take the same branch. */
		if (tid == 0) {
			uint64_t marker_before =
				PORT_ACQUIRE_SYSTEM(&tail_markers[marker_idx]);
			stale_work = (marker_before > work.marker_epoch) ? 1 : 0;
			if (stale_work)
				atomicAdd((unsigned long long*)&stats->stale_work_skips, 1);
		}
		__syncthreads();

		if (stale_work)
			continue;

		/*
		 * No fence here: thread 0's acquire on marker_before above
		 * already ordered the NIC's payload writes ahead of this
		 * block's reads, and the __syncthreads() propagated that to
		 * every thread.
		 */

		uint64_t my_error_offset;
		uint8_t my_error_actual;
		uint32_t my_errors = validate_chunk_portion(
			chunk_data, chunk_size, work.expected_pattern,
			&my_error_offset, &my_error_actual);

		/* Warp-level reduction: reduces 256 potential atomics to max 8 */
		const uint32_t FULL_WARP_MASK = 0xFFFFFFFF;
		/*
		 * PORT: the CUDA original uses a literal 32-lane warp
		 * (tid & 31, offset starting at 16). An AMD wavefront is 64
		 * lanes on CDNA and 32 on RDNA, so a literal 32 would reduce
		 * only within half a wave and let two lanes see lane == 0 --
		 * double-counting the atomics below. warpSize is a built-in,
		 * not a constant, so the unrolls are dropped with it.
		 */
		const uint32_t lane = tid % warpSize;
		uint32_t warp_has_errors = PORT_any_sync(FULL_WARP_MASK, my_errors > 0);

		uint32_t warp_error_count = 0;
		uint64_t warp_first_error_offset = UINT64_MAX;
		uint8_t warp_first_error_actual = 0;

		if (warp_has_errors) {
			warp_error_count = my_errors;
			for (int offset = warpSize / 2; offset > 0; offset /= 2)
				warp_error_count += PORT_shfl_xor_sync(FULL_WARP_MASK, warp_error_count, offset);

			warp_first_error_offset = my_error_offset;
			warp_first_error_actual = my_error_actual;
			for (int offset = warpSize / 2; offset > 0; offset /= 2) {
				uint64_t other_offset = PORT_shfl_xor_sync(FULL_WARP_MASK, warp_first_error_offset, offset);
				uint8_t other_actual = PORT_shfl_xor_sync(FULL_WARP_MASK, warp_first_error_actual, offset);
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
					/*
					 * PORT: the CUDA original fences, spins on
					 * an empty loop, then re-reads with __ldg.
					 * __ldg is a read-only-cache hint, which is
					 * the one load least likely to observe a
					 * DMA that landed in the meantime -- the
					 * opposite of what a retry wants. Back off
					 * as the other poll loops do and re-read at
					 * system scope, which is both the fence and
					 * the uncached fetch.
					 */
					PORT_POLL_BACKOFF();
					uint8_t retry_val =
						PORT_LOAD_SYSTEM(&chunk_data[err_off]);
					if (retry_val == expected)
						retry_matched = 1;
				}

				/*
				 * Forward-byte snapshot for the epoch guard.
				 * PORT: system scope rather than __ldg. These
				 * five bytes are what classify_validation_mismatch()
				 * uses to tell an in-flight overwrite from real
				 * corruption, so reading them from a stale cache
				 * biases towards reporting corruption that did not
				 * happen -- the one error a validator must not make.
				 */
				uint8_t actual_snap = err_actual;
				uint8_t fwd[4] = {expected, expected, expected, expected};
				int fwd_valid = 0;
				if (!retry_matched && (err_off + 4) < chunk_size) {
					actual_snap = PORT_LOAD_SYSTEM(&chunk_data[err_off]);
					fwd[0] = PORT_LOAD_SYSTEM(&chunk_data[err_off + 1]);
					fwd[1] = PORT_LOAD_SYSTEM(&chunk_data[err_off + 2]);
					fwd[2] = PORT_LOAD_SYSTEM(&chunk_data[err_off + 3]);
					fwd[3] = PORT_LOAD_SYSTEM(&chunk_data[err_off + 4]);
					fwd_valid = 1;
				}

				enum validation_mismatch_type mtype =
					classify_validation_mismatch(
						err_off, chunk_size,
						payload_size, expected,
						actual_snap, retry_matched, fwd,
						fwd_valid);

				uint64_t marker_after =
					PORT_LOAD_SYSTEM(&tail_markers[marker_idx]);
				if (marker_after > work.marker_epoch) {
					atomicAdd((unsigned long long *)&stats->stale_work_skips, 1);
					atomicAdd((unsigned long long *)&stats->race_overwrites, 1);
				} else if (mtype == VALIDATION_MISMATCH_STALE) {
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
	uint64_t *marker_last_seen,
	volatile uint32_t *validation_failed,
	volatile uint32_t *should_stop,
	struct ValidationErrorInfo *error_info,
	struct ValidationStats *stats)
{
	/* Block 0 is the monitor, blocks 1..N are validators */
	if (blockIdx.x == 0) {
		monitor_scan_tails(tail_markers, marker_last_seen,
		                   num_qps, chunks_per_qp,
		                   qp_state, work_queue,
		                   validation_failed, should_stop, stats);
	} else {
		validator_process_work(tail_markers, recv_slots_base, payload_size, tx_depth,
		                       num_qps, chunks_per_qp, work_queue,
		                       validation_failed, should_stop,
		                       error_info, stats);
	}
}

/* --- Exported Plugin API (loaded via dlsym) --- */

/*
 * Free all GPU allocations in g_ctx (NULL-safe, idempotent).
 *
 * hipFree's result is discarded deliberately, and the casts say so: this runs
 * on the teardown path, where every caller goes on to zero g_ctx and return a
 * failure that is already more specific than "a free failed". hipcc marks the
 * HIP entry points nodiscard, which nvcc does not, so the CUDA original gets
 * away with the bare calls.
 */
static void free_validation_allocations(void)
{
	if (g_ctx.d_work_queue)        (void)hipFree(g_ctx.d_work_queue);
	if (g_ctx.d_marker_last_seen)  (void)hipFree(g_ctx.d_marker_last_seen);
	if (g_ctx.d_qp_state)          (void)hipFree(g_ctx.d_qp_state);
	if (g_ctx.d_error_info)        (void)hipFree(g_ctx.d_error_info);
	if (g_ctx.d_stats)             (void)hipFree(g_ctx.d_stats);
	if (g_ctx.d_validation_failed) (void)hipFree((void*)g_ctx.d_validation_failed);
	if (g_ctx.d_should_stop)       (void)hipFree((void*)g_ctx.d_should_stop);
	/* d_tail_markers points into main buffer — don't free */
}

/*
 * Zero all runtime state (GPU buffers + host copies) for a fresh run.
 *
 * Unlike the frees above these results do matter: a hipMemset that silently
 * failed would leave a previous run's markers, stats or error record in place
 * and the validator would report them as this run's.
 *
 * Returns VALIDATION_OK or VALIDATION_ERR_MEMSET.
 */
static int reset_validation_state(void)
{
	hipError_t err;

	err = hipMemset(g_ctx.d_work_queue, 0, sizeof(struct ValidationWorkQueue));
	if (err == hipSuccess)
		err = hipMemset(g_ctx.d_marker_last_seen, 0,
				g_ctx.params.num_qps * g_ctx.chunks_per_qp *
				sizeof(uint64_t));
	if (err == hipSuccess)
		err = hipMemset(g_ctx.d_qp_state, 0,
				g_ctx.params.num_qps *
				sizeof(struct QpValidationState));
	if (err == hipSuccess)
		err = hipMemset(g_ctx.d_error_info, 0,
				sizeof(struct ValidationErrorInfo));
	if (err == hipSuccess)
		err = hipMemset(g_ctx.d_stats, 0, sizeof(struct ValidationStats));

	if (err != hipSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX
			" Failed to zero validation state: %s\n",
			hipGetErrorString(err));
		return VALIDATION_ERR_MEMSET;
	}

	*(uint32_t*)g_ctx.d_validation_failed = 0;
	*(uint32_t*)g_ctx.d_should_stop = 0;
	memset(&g_ctx.h_error_info, 0, sizeof(struct ValidationErrorInfo));
	memset(&g_ctx.h_stats, 0, sizeof(struct ValidationStats));
	return VALIDATION_OK;
}

static int calculate_validator_blocks(uint32_t num_qps)
{
	int blocks = num_qps * 16;
	if (blocks < 32) blocks = 32;
	if (blocks > VALIDATION_MAX_VALIDATOR_BLOCKS) blocks = VALIDATION_MAX_VALIDATOR_BLOCKS;
	return blocks;
}

/* Initialize the validation context and allocate GPU memory. */
extern "C" int rocm_kernel_validation_init(
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
	hipError_t err;
	(void)validation_mode;

	if (buffer_base == NULL) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error: %s\n", rocm_kernel_validation_strerror(VALIDATION_ERR_NULL_PARAM));
		return -1;
	}

	err = hipSetDevice(cuda_device_id);
	if (err != hipSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error setting GPU device %d: %s\n",
		        cuda_device_id, hipGetErrorString(err));
		return -1;
	}
	if (debug_enabled) printf(VALIDATION_LOG_PREFIX " Using GPU device %d\n", cuda_device_id);

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
	err = hipMalloc((void**)&g_ctx.d_work_queue, sizeof(struct ValidationWorkQueue));
	if (err != hipSuccess) goto alloc_fail;

	err = hipMalloc((void**)&g_ctx.d_marker_last_seen,
			 num_qps * chunks_per_qp * sizeof(uint64_t));
	if (err != hipSuccess) goto alloc_fail;

	err = hipMalloc((void**)&g_ctx.d_qp_state,
			 num_qps * sizeof(struct QpValidationState));
	if (err != hipSuccess) goto alloc_fail;

	err = hipMallocManaged((void**)&g_ctx.d_error_info, sizeof(struct ValidationErrorInfo));
	if (err != hipSuccess) goto alloc_fail;

	err = hipMallocManaged((void**)&g_ctx.d_stats, sizeof(struct ValidationStats));
	if (err != hipSuccess) goto alloc_fail;

	err = hipMallocManaged((void**)&g_ctx.d_validation_failed, sizeof(uint32_t));
	if (err != hipSuccess) goto alloc_fail;

	err = hipMallocManaged((void**)&g_ctx.d_should_stop, sizeof(uint32_t));
	if (err != hipSuccess) goto alloc_fail;

	if (reset_validation_state() != VALIDATION_OK) {
		free_validation_allocations();
		memset(&g_ctx, 0, sizeof(g_ctx));
		return VALIDATION_ERR_MEMSET;
	}

	g_initialized = 1;
	g_ctx.kernel_running = 0;

	VDBG("Context created successfully\n");
	return 0;

alloc_fail:
	fprintf(stderr, VALIDATION_LOG_PREFIX " GPU allocation failed: %s\n",
		hipGetErrorString(err));
	free_validation_allocations();
	memset(&g_ctx, 0, sizeof(g_ctx));
	return -1;
}

/* Start the validation kernel. Params/num_blocks/threads_per_block are unused (kept for ABI). */
extern "C" int rocm_kernel_validation_start(void *params, int num_blocks, int threads_per_block)
{
	(void)params;

	/*
	 * PORT: the CUDA original accepts num_blocks and threads_per_block and
	 * then ignores them, always launching calculate_validator_blocks()+1
	 * blocks of VALIDATION_THREADS_PER_BLOCK. Honour them when non-zero.
	 *
	 * The geometry is the one thing about this design that does not carry
	 * across devices unchanged: the default is 33 blocks x 1024 threads,
	 * which a discrete GPU retires trivially and a functional simulator
	 * does not retire at all. Nothing about the algorithm changes with a
	 * smaller launch -- the monitor still scans, the validators still
	 * drain the queue -- so making it settable keeps one implementation
	 * for both.
	 */
	const int blocks = (num_blocks > 0) ? num_blocks : g_ctx.num_blocks;
	const int threads = (threads_per_block > 0) ? threads_per_block
						    : VALIDATION_THREADS_PER_BLOCK;

	if (!g_initialized) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error: Context not initialized\n");
		return -1;
	}
	if (g_ctx.kernel_running) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error: %s\n", rocm_kernel_validation_strerror(VALIDATION_ERR_ALREADY_RUNNING));
		return -1;
	}

	hipError_t err = hipSetDevice(g_ctx.cuda_device_id);
	if (err != hipSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error setting GPU device: %s\n",
		        hipGetErrorString(err));
		return -1;
	}

	if (reset_validation_state() != VALIDATION_OK)
		return VALIDATION_ERR_MEMSET;

	uint8_t *recv_slots_base = (uint8_t*)g_ctx.params.recv_slots_addr;

	VDBG("Launching kernel with %d blocks (%d threads each)\n",
	     blocks, threads);
	VDBG("tail_markers: %p, recv_slots: %p\n",
	     (void*)g_ctx.d_tail_markers, recv_slots_base);
	VDBG("payload_size: %lu, tx_depth: %u, num_qps: %u\n",
	     g_ctx.params.payload_size, g_ctx.params.tx_depth, g_ctx.params.num_qps);

	validation_kernel<<<blocks, threads>>>(
		g_ctx.d_tail_markers,
		recv_slots_base,
		g_ctx.params.payload_size,
		g_ctx.params.tx_depth,
		g_ctx.params.num_qps,
		g_ctx.chunks_per_qp,
		g_ctx.d_qp_state,
		g_ctx.d_work_queue,
		g_ctx.d_marker_last_seen,
		g_ctx.d_validation_failed,
		g_ctx.d_should_stop,
		g_ctx.d_error_info,
		g_ctx.d_stats);

	err = hipGetLastError();
	if (err != hipSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error launching kernel: %s\n",
		        hipGetErrorString(err));
		return -1;
	}

	g_ctx.kernel_running = 1;
	VDBG("Kernel launched successfully\n");
	return 0;
}

extern "C" int rocm_kernel_validation_stop(void)
{
	if (!g_initialized || !g_ctx.kernel_running)
		return 0;

	VDBG("Stopping kernel...\n");
	*(volatile uint32_t*)g_ctx.d_should_stop = 1;

	hipError_t err = hipDeviceSynchronize();
	if (err != hipSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Error waiting for kernel: %s\n",
		        hipGetErrorString(err));
		return -1;
	}

	g_ctx.kernel_running = 0;

	VDBG("Kernel stopped\n");
	VDBG("Final stats: chunks=%lu, bytes=%lu, errors=%lu, "
	     "detections=%lu, skips=%lu, queue_full=%lu, stale_work=%lu, "
	     "races=%lu, retries=%lu\n",
	     g_ctx.d_stats->chunks_validated,
	     g_ctx.d_stats->bytes_validated,
	     g_ctx.d_stats->errors_found,
	     g_ctx.d_stats->monitor_detections,
	     g_ctx.d_stats->skipped_steps,
	     g_ctx.d_stats->queue_full_drops,
	     g_ctx.d_stats->stale_work_skips,
	     g_ctx.d_stats->race_overwrites,
	     g_ctx.d_stats->dma_stale_retries);

	return 0;
}

/* Read stats from managed memory (NULL-safe for optional outputs). */
extern "C" int rocm_kernel_validation_get_stats(uint64_t *chunks_validated,
				    uint64_t *bytes_validated,
				    uint64_t *errors_found,
				    uint64_t *markers_scanned,
				    uint64_t *markers_hit,
				    uint64_t *skipped_steps,
				    uint64_t *race_overwrites,
				    uint64_t *dma_stale_retries,
				    uint64_t *queue_full_drops,
				    uint64_t *stale_work_skips)
{
	if (!g_initialized) return -1;
	if (chunks_validated) *chunks_validated = g_ctx.d_stats->chunks_validated;
	if (bytes_validated) *bytes_validated = g_ctx.d_stats->bytes_validated;
	if (errors_found) *errors_found = g_ctx.d_stats->errors_found;
	if (markers_scanned) *markers_scanned = g_ctx.d_stats->markers_scanned;
	if (markers_hit) *markers_hit = g_ctx.d_stats->markers_hit;
	if (skipped_steps) *skipped_steps = g_ctx.d_stats->skipped_steps;
	if (race_overwrites) *race_overwrites = g_ctx.d_stats->race_overwrites;
	if (dma_stale_retries) *dma_stale_retries = g_ctx.d_stats->dma_stale_retries;
	if (queue_full_drops) *queue_full_drops = g_ctx.d_stats->queue_full_drops;
	if (stale_work_skips) *stale_work_skips = g_ctx.d_stats->stale_work_skips;
	return 0;
}

/* Read error details from managed memory. */
extern "C" int rocm_kernel_validation_get_error(uint32_t *qp_id, uint32_t *chunk_id,
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

extern "C" int rocm_kernel_validation_destroy(void)
{
	int debug = g_ctx.debug_enabled;

	if (!g_initialized) return 0;

	if (g_ctx.kernel_running)
		rocm_kernel_validation_stop();

	free_validation_allocations();
	memset(&g_ctx, 0, sizeof(g_ctx));
	g_initialized = 0;

	if (debug)
		printf(VALIDATION_LOG_PREFIX " Context destroyed, all memory freed\n");
	return 0;
}

extern "C" int rocm_kernel_validation_is_running(void)
{
	return g_initialized && g_ctx.kernel_running;
}

/* Returns 0 = OK, 1 = failed, -1 = not initialized. */
extern "C" int rocm_kernel_validation_check_status(void)
{
	if (!g_initialized) return -1;
	uint32_t failed = 0;
	/*
	 * A discarded result here would be reported as "not failed", which is
	 * the wrong direction to guess in: we would be asserting the data is
	 * good on the strength of a copy that did not happen.
	 */
	hipError_t err = hipMemcpy(&failed, (void*)g_ctx.d_validation_failed,
				   sizeof(uint32_t), hipMemcpyDeviceToHost);
	if (err != hipSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX
			" Failed to read validation status: %s\n",
			hipGetErrorString(err));
		return -1;
	}
	return failed ? 1 : 0;
}

/* --- GPU page touch (--gpu_touch feature, ensures pages are resident for RDMA) --- */

#define GPU_TOUCH_STEP 4096

__global__ void rocm_touch_pages_kernel(volatile uint8_t *c, int size,
                                 volatile int *stop_flag, int is_infinite)
{
	do {
		for (int iter = 0; iter < size; iter += GPU_TOUCH_STEP)
			c[iter] = 0;
	} while (is_infinite && !*stop_flag);
}

extern "C" int rocm_touch_gpu_pages(uint8_t *addr, int buf_size,
                               int is_infinite, volatile int **stop_flag)
{
	rocm_touch_pages_kernel<<<1, 1>>>(addr, buf_size, *stop_flag, is_infinite);
	hipError_t err = hipGetLastError();
	if (err != hipSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " GPU touch kernel launch error: %s\n",
		        hipGetErrorString(err));
		return -1;
	}
	return 0;
}

extern "C" int rocm_init_gpu_stop_flag(volatile int **stop_flag)
{
	hipError_t ret = hipMallocManaged((void **)stop_flag, sizeof(int));
	if (ret != hipSuccess) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Failed to allocate stop flag: %s\n",
		        hipGetErrorString(ret));
		return -1;
	}
	**stop_flag = 0;
	return 0;
}

/*
 * Has no CUDA counterpart: the CUDA plugin answers this question by whether
 * libperftest_kernels.so dlopen'd at all. These kernels are linked in
 * unconditionally, so rocm_validation.c needs a runtime probe instead.
 */
extern "C" int rocm_kernels_available(void)
{
	int count = 0;

	return (hipGetDeviceCount(&count) == hipSuccess && count > 0) ? 1 : 0;
}
