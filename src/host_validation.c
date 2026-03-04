#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>

#include "host_validation.h"
#include "host_memory.h"


/* SIMD headers based on detected support */
#ifdef HAVE_AVX512
#include <immintrin.h>
#elif defined(HAVE_AVX2)
#include <immintrin.h>
#endif

#ifdef HAVE_SSE42
#include <nmmintrin.h>
#endif

#ifdef HAVE_NEON
#include <arm_neon.h>
#endif

/* Cross-platform CPU yield/pause hint for spin-wait loops */
#if defined(__x86_64__) || defined(__i386__)
  #define cpu_relax() _mm_pause()
#elif defined(__aarch64__)
  #define cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__arm__)
  #define cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#else
  /* Fallback: compiler barrier */
  #define cpu_relax() __asm__ __volatile__("" ::: "memory")
#endif

#define VALIDATOR_SPIN_CHECK_INTERVAL    1024    /* Iterations before rechecking stop flag */
#define VALIDATION_REPOLL_THRESHOLD_BYTES (64 * 1024)  /* Max chunk_bytes for depth-first re-poll */

/* 512 bytes ahead ≈ 8 cache lines; covers DRAM latency for DMA-cold data */
#define VALIDATION_PREFETCH_BYTES  512

static struct host_validation_ctx g_host_ctx;

#define HDBG(fmt, ...) \
	do { if (g_host_ctx.debug_enabled) \
		printf(VALIDATION_LOG_PREFIX " " fmt, ##__VA_ARGS__); \
	} while (0)

#define NUMA_MAX_CPUS  1024  /* Max logical CPUs for NUMA pinning */

/* Forward declaration — defined after SIMD validation functions */
static int get_numa_node_cpus(int numa_node, int *cpu_list, int max_cpus);

/* --- Helper functions --- */

/* Convert 1-based marker epoch to expected data pattern. */
static inline int marker_to_expected_pattern(uint64_t marker_val, uint8_t *expected_out)
{
	if (marker_val == 0 || expected_out == NULL)
		return -1;

	*expected_out = validation_cycle_to_pattern(marker_val - 1);
	return 0;
}

/* NUMA-aware validator count: cap at 50% of local-node (or system) cores. */
static int get_num_validators(uint32_t num_qps, uint32_t chunks_per_qp,
			      int validation_mode, int numa_node)
{
	uint32_t total_markers = num_qps * chunks_per_qp;

	int per_qp = (validation_mode == VALIDATION_MODE_READ) ? 3 : 2;
	int num_validators = (int)num_qps * per_qp;

	if (num_validators < HOST_VALIDATION_DEFAULT_VALIDATORS)
		num_validators = HOST_VALIDATION_DEFAULT_VALIDATORS;

	/* Cap at 50% of available cores (NUMA-local when possible) */
	{
		int available_cores = 0;

		if (numa_node >= 0) {
			int cpu_list[NUMA_MAX_CPUS];

			available_cores = get_numa_node_cpus(numa_node,
							     cpu_list,
							     NUMA_MAX_CPUS);
		}

		if (available_cores <= 0) {
			long nproc = sysconf(_SC_NPROCESSORS_ONLN);

			if (nproc > 0)
				available_cores = (int)nproc;
		}
		if (available_cores > 0) {
			int half_cores = available_cores / 2;

			if (num_validators > half_cores)
				num_validators = half_cores;
		}
	}

	if (num_validators > HOST_VALIDATION_MAX_VALIDATORS)
		num_validators = HOST_VALIDATION_MAX_VALIDATORS;
	if ((uint32_t)num_validators > total_markers)
		num_validators = (int)total_markers;
	if (num_validators < 1)
		num_validators = 1;

	return num_validators;
}

/* --- SIMD validation functions --- */

#ifdef HAVE_AVX512
/* AVX-512: 4× unrolled, 256 bytes/iter. Returns mismatch offset or -1. */
static int64_t validate_avx512(const uint8_t *data, uint8_t pattern,
			       uint64_t size, uint8_t *actual_byte)
{
	const __m512i pat = _mm512_set1_epi8((char)pattern);
	uint64_t i = 0;

	for (; i + 256 <= size; i += 256) {
		__builtin_prefetch(data + i + VALIDATION_PREFETCH_BYTES, 0, 0);

		__m512i v0 = _mm512_loadu_si512((const __m512i *)(data + i));
		__m512i v1 = _mm512_loadu_si512((const __m512i *)(data + i + 64));
		__m512i v2 = _mm512_loadu_si512((const __m512i *)(data + i + 128));
		__m512i v3 = _mm512_loadu_si512((const __m512i *)(data + i + 192));

		__mmask64 m0 = _mm512_cmpeq_epi8_mask(v0, pat);
		__mmask64 m1 = _mm512_cmpeq_epi8_mask(v1, pat);
		__mmask64 m2 = _mm512_cmpeq_epi8_mask(v2, pat);
		__mmask64 m3 = _mm512_cmpeq_epi8_mask(v3, pat);

		if ((m0 & m1 & m2 & m3) != 0xFFFFFFFFFFFFFFFFULL) {
			if (m0 != 0xFFFFFFFFFFFFFFFFULL) {
				int pos = __builtin_ctzll(~m0);
				if (actual_byte)
					*actual_byte = data[i + pos];
				return (int64_t)(i + pos);
			}
			if (m1 != 0xFFFFFFFFFFFFFFFFULL) {
				int pos = __builtin_ctzll(~m1);
				if (actual_byte)
					*actual_byte = data[i + 64 + pos];
				return (int64_t)(i + 64 + pos);
			}
			if (m2 != 0xFFFFFFFFFFFFFFFFULL) {
				int pos = __builtin_ctzll(~m2);
				if (actual_byte)
					*actual_byte = data[i + 128 + pos];
				return (int64_t)(i + 128 + pos);
			}
			{
				int pos = __builtin_ctzll(~m3);
				if (actual_byte)
					*actual_byte = data[i + 192 + pos];
				return (int64_t)(i + 192 + pos);
			}
		}
	}

	for (; i + 64 <= size; i += 64) {
		__m512i data_vec = _mm512_loadu_si512((const __m512i *)(data + i));
		__mmask64 mask = _mm512_cmpeq_epi8_mask(data_vec, pat);

		if (mask != 0xFFFFFFFFFFFFFFFFULL) {
			int pos = __builtin_ctzll(~mask);
			if (actual_byte)
				*actual_byte = data[i + pos];
			return (int64_t)(i + pos);
		}
	}

	for (; i < size; i++) {
		if (data[i] != pattern) {
			if (actual_byte)
				*actual_byte = data[i];
			return (int64_t)i;
		}
	}

	return -1;
}
#endif /* HAVE_AVX512 */

#ifdef HAVE_AVX2
/* AVX2: 4× unrolled, 128 bytes/iter. Returns mismatch offset or -1. */
static int64_t validate_avx2(const uint8_t *data, uint8_t pattern,
			     uint64_t size, uint8_t *actual_byte)
{
	const __m256i pat = _mm256_set1_epi8((char)pattern);
	uint64_t i = 0;

	for (; i + 128 <= size; i += 128) {
		__builtin_prefetch(data + i + VALIDATION_PREFETCH_BYTES, 0, 0);

		__m256i v0 = _mm256_loadu_si256((const __m256i *)(data + i));
		__m256i v1 = _mm256_loadu_si256((const __m256i *)(data + i + 32));
		__m256i v2 = _mm256_loadu_si256((const __m256i *)(data + i + 64));
		__m256i v3 = _mm256_loadu_si256((const __m256i *)(data + i + 96));

		int m0 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, pat));
		int m1 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, pat));
		int m2 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v2, pat));
		int m3 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v3, pat));

		if ((m0 & m1 & m2 & m3) != (int)0xFFFFFFFF) {
			if (m0 != (int)0xFFFFFFFF) {
				int pos = __builtin_ctz(~m0);
				if (actual_byte)
					*actual_byte = data[i + pos];
				return (int64_t)(i + pos);
			}
			if (m1 != (int)0xFFFFFFFF) {
				int pos = __builtin_ctz(~m1);
				if (actual_byte)
					*actual_byte = data[i + 32 + pos];
				return (int64_t)(i + 32 + pos);
			}
			if (m2 != (int)0xFFFFFFFF) {
				int pos = __builtin_ctz(~m2);
				if (actual_byte)
					*actual_byte = data[i + 64 + pos];
				return (int64_t)(i + 64 + pos);
			}
			{
				int pos = __builtin_ctz(~m3);
				if (actual_byte)
					*actual_byte = data[i + 96 + pos];
				return (int64_t)(i + 96 + pos);
			}
		}
	}

	for (; i + 32 <= size; i += 32) {
		__m256i data_vec = _mm256_loadu_si256((const __m256i *)(data + i));
		int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(data_vec, pat));

		if (mask != (int)0xFFFFFFFF) {
			int pos = __builtin_ctz(~mask);
			if (actual_byte)
				*actual_byte = data[i + pos];
			return (int64_t)(i + pos);
		}
	}

	for (; i < size; i++) {
		if (data[i] != pattern) {
			if (actual_byte)
				*actual_byte = data[i];
			return (int64_t)i;
		}
	}

	return -1;
}
#endif /* HAVE_AVX2 */

#ifdef HAVE_SSE42
/* SSE4.2: 4× unrolled, 64 bytes/iter. Returns mismatch offset or -1. */
static int64_t validate_sse42(const uint8_t *data, uint8_t pattern,
			      uint64_t size, uint8_t *actual_byte)
{
	const __m128i pat = _mm_set1_epi8((char)pattern);
	uint64_t i = 0;

	for (; i + 64 <= size; i += 64) {
		__builtin_prefetch(data + i + VALIDATION_PREFETCH_BYTES, 0, 0);

		__m128i v0 = _mm_loadu_si128((const __m128i *)(data + i));
		__m128i v1 = _mm_loadu_si128((const __m128i *)(data + i + 16));
		__m128i v2 = _mm_loadu_si128((const __m128i *)(data + i + 32));
		__m128i v3 = _mm_loadu_si128((const __m128i *)(data + i + 48));

		int m0 = _mm_movemask_epi8(_mm_cmpeq_epi8(v0, pat));
		int m1 = _mm_movemask_epi8(_mm_cmpeq_epi8(v1, pat));
		int m2 = _mm_movemask_epi8(_mm_cmpeq_epi8(v2, pat));
		int m3 = _mm_movemask_epi8(_mm_cmpeq_epi8(v3, pat));

		if ((m0 & m1 & m2 & m3) != 0xFFFF) {
			if (m0 != 0xFFFF) {
				int pos = __builtin_ctz(~m0 & 0xFFFF);
				if (actual_byte)
					*actual_byte = data[i + pos];
				return (int64_t)(i + pos);
			}
			if (m1 != 0xFFFF) {
				int pos = __builtin_ctz(~m1 & 0xFFFF);
				if (actual_byte)
					*actual_byte = data[i + 16 + pos];
				return (int64_t)(i + 16 + pos);
			}
			if (m2 != 0xFFFF) {
				int pos = __builtin_ctz(~m2 & 0xFFFF);
				if (actual_byte)
					*actual_byte = data[i + 32 + pos];
				return (int64_t)(i + 32 + pos);
			}
			{
				int pos = __builtin_ctz(~m3 & 0xFFFF);
				if (actual_byte)
					*actual_byte = data[i + 48 + pos];
				return (int64_t)(i + 48 + pos);
			}
		}
	}

	for (; i + 16 <= size; i += 16) {
		__m128i data_vec = _mm_loadu_si128((const __m128i *)(data + i));
		int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(data_vec, pat));

		if (mask != 0xFFFF) {
			int pos = __builtin_ctz(~mask & 0xFFFF);
			if (actual_byte)
				*actual_byte = data[i + pos];
			return (int64_t)(i + pos);
		}
	}

	for (; i < size; i++) {
		if (data[i] != pattern) {
			if (actual_byte)
				*actual_byte = data[i];
			return (int64_t)i;
		}
	}

	return -1;
}
#endif /* HAVE_SSE42 */

#ifdef HAVE_NEON
/* NEON: 4× unrolled, 64 bytes/iter. Returns mismatch offset or -1. */
static int64_t validate_neon(const uint8_t *data, uint8_t pattern,
			     uint64_t size, uint8_t *actual_byte)
{
	const uint8x16_t pat = vdupq_n_u8(pattern);
	uint64_t i = 0;

	for (; i + 64 <= size; i += 64) {
		__builtin_prefetch(data + i + VALIDATION_PREFETCH_BYTES, 0, 0);

		uint8x16_t v0 = vld1q_u8(data + i);
		uint8x16_t v1 = vld1q_u8(data + i + 16);
		uint8x16_t v2 = vld1q_u8(data + i + 32);
		uint8x16_t v3 = vld1q_u8(data + i + 48);

		uint8x16_t c0 = vceqq_u8(v0, pat);
		uint8x16_t c1 = vceqq_u8(v1, pat);
		uint8x16_t c2 = vceqq_u8(v2, pat);
		uint8x16_t c3 = vceqq_u8(v3, pat);

		uint8x16_t all = vandq_u8(vandq_u8(c0, c1),
					  vandq_u8(c2, c3));
		uint64x2_t all64 = vreinterpretq_u64_u8(all);

		if (vgetq_lane_u64(all64, 0) != 0xFFFFFFFFFFFFFFFFULL ||
		    vgetq_lane_u64(all64, 1) != 0xFFFFFFFFFFFFFFFFULL) {
			for (int j = 0; j < 64; j++) {
				if (data[i + j] != pattern) {
					if (actual_byte)
						*actual_byte = data[i + j];
					return (int64_t)(i + j);
				}
			}
		}
	}

	for (; i + 16 <= size; i += 16) {
		uint8x16_t data_vec = vld1q_u8(data + i);
		uint8x16_t cmp = vceqq_u8(data_vec, pat);
		uint64x2_t cmp64 = vreinterpretq_u64_u8(cmp);

		if (vgetq_lane_u64(cmp64, 0) != 0xFFFFFFFFFFFFFFFFULL ||
		    vgetq_lane_u64(cmp64, 1) != 0xFFFFFFFFFFFFFFFFULL) {
			for (int j = 0; j < 16; j++) {
				if (data[i + j] != pattern) {
					if (actual_byte)
						*actual_byte = data[i + j];
					return (int64_t)(i + j);
				}
			}
		}
	}

	for (; i < size; i++) {
		if (data[i] != pattern) {
			if (actual_byte)
				*actual_byte = data[i];
			return (int64_t)i;
		}
	}

	return -1;
}
#endif /* HAVE_NEON */

/* Compile-time SIMD dispatch — direct call, no function pointer overhead.
 * At least one SIMD ISA is required; builds without SIMD reject
 * --data_validation for host memory at parameter parsing time. */
#ifdef HAVE_AVX512
  #define VALIDATE_CHUNK_FN validate_avx512
#elif defined(HAVE_AVX2)
  #define VALIDATE_CHUNK_FN validate_avx2
#elif defined(HAVE_SSE42)
  #define VALIDATE_CHUNK_FN validate_sse42
#elif defined(HAVE_NEON)
  #define VALIDATE_CHUNK_FN validate_neon
#else
  #define VALIDATE_CHUNK_FN(...) ((int64_t-1))
#endif

/* Read IB device NUMA node from sysfs. Returns node ID or -1. */
static int get_ib_device_numa_node(const char *ib_devname)
{
#ifdef __linux__
	char path[256];
	FILE *f;
	int numa_node = -1;

	if (!ib_devname || !ib_devname[0])
		return -1;

	snprintf(path, sizeof(path),
		 "/sys/class/infiniband/%s/device/numa_node", ib_devname);

	f = fopen(path, "r");
	if (!f)
		return -1;

	if (fscanf(f, "%d", &numa_node) != 1)
		numa_node = -1;
	fclose(f);

	return (numa_node >= 0) ? numa_node : -1;
#else
	(void)ib_devname;
	return -1;
#endif
}

int host_validation_get_numa_node(const char *ib_devname)
{
	return get_ib_device_numa_node(ib_devname);
}

/* Parse /sys/.../cpulist for a NUMA node. Returns CPU count, 0 on failure. */
static int get_numa_node_cpus(int numa_node, int *cpu_list, int max_cpus)
{
#ifdef __linux__
	char path[256];
	char buf[4096];
	FILE *f;
	int count = 0;

	if (numa_node < 0 || !cpu_list || max_cpus <= 0)
		return 0;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/node/node%d/cpulist", numa_node);

	f = fopen(path, "r");
	if (!f)
		return 0;

	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);

	const char *p = buf;

	while (*p && count < max_cpus) {
		char *end;
		long start, stop;

		while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (!*p)
			break;

		start = strtol(p, &end, 10);
		if (end == p)
			break;
		p = end;

		if (*p == '-') {
			p++;
			stop = strtol(p, &end, 10);
			if (end == p)
				break;
			p = end;
		} else {
			stop = start;
		}

		for (long cpu = start; cpu <= stop && count < max_cpus; cpu++)
			cpu_list[count++] = (int)cpu;
	}

	return count;
#else
	(void)numa_node;
	(void)cpu_list;
	(void)max_cpus;
	return 0;
#endif
}

/* Pick a NUMA-local CPU for a validator (even cores first, then HT siblings).
 * Returns 0 on success, -1 on error, -2 if pinning should be skipped. */
static int choose_validator_cpu(int validator_id, int numa_node, int *cpu_out)
{
	int cpu_list[NUMA_MAX_CPUS];
	int num_cpus = 0;

	if (!cpu_out)
		return -1;

	if (numa_node >= 0)
		num_cpus = get_numa_node_cpus(numa_node, cpu_list, NUMA_MAX_CPUS);

	if (num_cpus > 0) {
		int even_slots = (num_cpus + 1) / 2;

		if (num_cpus <= 1)
			return -2;

		int target_idx;

		if (validator_id < even_slots) {
			target_idx = validator_id * 2;
		} else {
			int odd_idx = validator_id - even_slots;
			target_idx = odd_idx * 2 + 1;
		}

		target_idx = target_idx % num_cpus;
		*cpu_out = cpu_list[target_idx];
		return 0;
	}

	/* Fallback: all system CPUs, skip 0-1 */
	{
		long nproc;
		int total_cpus;
		int usable_cpus, even_slots, target_cpu;

		nproc = sysconf(_SC_NPROCESSORS_ONLN);
		total_cpus = (nproc > 0) ? (int)nproc : 0;

		if (total_cpus <= 2)
			return -2;

		usable_cpus = total_cpus - 2;  /* Skip CPUs 0-1 */
		even_slots = usable_cpus / 2;

		if (validator_id < even_slots) {
			target_cpu = 2 + (validator_id * 2);
		} else {
			int odd_idx = validator_id - even_slots;

			target_cpu = 3 + (odd_idx * 2);
			if (target_cpu >= total_cpus)
				target_cpu = 2 + ((target_cpu - 2) % usable_cpus);
		}

		*cpu_out = target_cpu;
		return 0;
	}
}

/* Validate one marker transition. Returns 1 if validated, 0 on error. */
static inline int validate_one_marker(struct host_validator_ctx *vctx,
				      struct marker_scan_entry *entry,
				      uint64_t marker_val,
				      uint64_t chunk_bytes)
{
	struct host_validation_ctx *ctx = vctx->parent;

	atomic_thread_fence(memory_order_acquire);

	uint8_t expected;
	if (marker_to_expected_pattern(marker_val, &expected) != 0) {
		entry->last_seen = marker_val;
		return 0;
	}

	uint64_t delta = marker_val - entry->last_seen;
	if (delta > 1)
		vctx->stats.skipped_steps += delta - 1;

	// cppcheck-suppress variableScope
	uint8_t actual_byte = 0;
	int64_t mismatch = VALIDATE_CHUNK_FN(entry->chunk_data, expected,
					     chunk_bytes, &actual_byte);


	// cppcheck-suppress knownConditionTrueFalse
	// cppcheck-suppress unmatchedSuppression
	if (mismatch >= 0) {
		int retry_matched = 0;
		uint8_t fwd_snap[4] = {expected, expected, expected, expected};
		int fwd_valid = 0;
		uint64_t tail_bytes = (uint64_t)ctx->payload_size * 2;
		if (tail_bytes < 256)
			tail_bytes = 256;

		/* DMA tail retry */
		if ((uint64_t)mismatch >= chunk_bytes - tail_bytes) {
			cpu_relax();
			cpu_relax();

			// cppcheck-suppress variableScope
			uint8_t retry_actual = 0;
			int64_t retry = VALIDATE_CHUNK_FN(
				entry->chunk_data + mismatch, expected,
				chunk_bytes - (uint64_t)mismatch,
				&retry_actual);

			// cppcheck-suppress knownConditionTrueFalse
			// cppcheck-suppress unmatchedSuppression
			if (retry < 0) {
				retry_matched = 1;
			} else {
				mismatch = mismatch + retry;
				actual_byte = retry_actual;
			}
		}

		/* Forward-byte snapshot */
		if (!retry_matched && (uint64_t)(mismatch + 4) < chunk_bytes) {
			const uint8_t *fwd = entry->chunk_data + mismatch + 1;
			fwd_snap[0] = fwd[0];
			fwd_snap[1] = fwd[1];
			fwd_snap[2] = fwd[2];
			fwd_snap[3] = fwd[3];
			fwd_valid = 1;
		}

		enum validation_mismatch_type mtype =
			classify_validation_mismatch(
				(uint64_t)mismatch, chunk_bytes,
				ctx->payload_size, expected,
				retry_matched, fwd_snap, fwd_valid);

		if (mtype == VALIDATION_MISMATCH_STALE) {
			vctx->stats.dma_stale_retries++;
	} else if (mtype == VALIDATION_MISMATCH_RACE) {
		vctx->stats.race_overwrites++;
	} else {
			uint64_t error_offset = mismatch % ctx->payload_size;

			int expected_no_error = 0;
			if (atomic_compare_exchange_strong(
				&ctx->error.has_error,
				&expected_no_error, 1)) {
				ctx->error.qp_id = entry->qp_id;
				ctx->error.chunk_id = entry->chunk_id;
				ctx->error.byte_offset = error_offset;
				ctx->error.expected = expected;
				ctx->error.actual = actual_byte;
			}
			atomic_fetch_add(&ctx->stats.errors_found, 1);
		}
	}

	vctx->stats.work_items_processed += ctx->ops_per_chunk;
	vctx->stats.bytes_validated += chunk_bytes;
	entry->last_seen = marker_val;

	return 1;
}

/* --- Sharded inline validator thread --- */

static void *validator_thread_fn(void *arg)
{
	struct host_validator_ctx *vctx = (struct host_validator_ctx *)arg;
	struct host_validation_ctx *ctx = vctx->parent;

	uint64_t chunk_bytes = (uint64_t)ctx->ops_per_chunk * ctx->payload_size;

	/* Re-poll in WRITE mode only: drains bursty markers depth-first */
	int repoll = (ctx->validation_mode == VALIDATION_MODE_WRITE)
		     && (chunk_bytes <= VALIDATION_REPOLL_THRESHOLD_BYTES);

	int should_run = 1;
	uint32_t spin_count = 0;

	struct marker_scan_entry *my_markers = vctx->own_markers;
	uint32_t my_marker_count = vctx->own_marker_count;

	atomic_store(&vctx->running, 1);
	atomic_fetch_add(&ctx->control.validators_running, 1);

#ifdef __linux__
	{
		int target_cpu;
		int pin_ret = choose_validator_cpu(vctx->id,
						   ctx->numa_node,
						   &target_cpu);

		if (pin_ret == 0) {
			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(target_cpu, &cpuset);
			if (pthread_setaffinity_np(pthread_self(),
						   sizeof(cpu_set_t),
						   &cpuset) != 0 && ctx->debug_enabled) {
				fprintf(stderr, VALIDATION_LOG_PREFIX
					" Validator[%d] failed to pin to CPU %d: %s\n",
					vctx->id, target_cpu, strerror(errno));
			}
		} else if (pin_ret == -2) {
			HDBG("Validator[%d] skipping CPU pinning "
			     "(too few CPUs on NUMA %d)\n",
			     vctx->id, ctx->numa_node);
		}
	}
#endif

	/* ========== MAIN LOOP: Poll own markers, validate inline ========== */
	while (should_run) {
		int found_work = 0;

		vctx->stats.scan_cycles++;

		for (uint32_t m = 0; m < my_marker_count; m++) {
			struct marker_scan_entry *entry = &my_markers[m];

		if (m + 1 < my_marker_count)
				__builtin_prefetch((const void *)my_markers[m + 1].marker_ptr, 0, 0);

		uint64_t marker_val = *entry->marker_ptr;
			if (marker_val != entry->last_seen && marker_val != 0) {
			do {
				found_work = 1;
				vctx->stats.markers_hit++;

			if (!validate_one_marker(vctx, entry,
						 marker_val,
						 chunk_bytes))
					break;

			marker_val = *entry->marker_ptr;
			} while (repoll
				 && marker_val != entry->last_seen
				 && marker_val != 0);
			}

			vctx->stats.markers_scanned++;
		}

		if (!found_work) {
			cpu_relax();
			if (++spin_count >= VALIDATOR_SPIN_CHECK_INTERVAL) {
				spin_count = 0;
				should_run = !atomic_load(&ctx->control.stop_flag);
			}
		} else {
			spin_count = 0;
		}
	}

	/* Final drain */
	for (uint32_t m = 0; m < my_marker_count; m++) {
		struct marker_scan_entry *entry = &my_markers[m];
		uint64_t marker_val = *entry->marker_ptr;

		if (marker_val == entry->last_seen || marker_val == 0)
			continue;

		vctx->stats.markers_hit++;
		validate_one_marker(vctx, entry, marker_val,
				    chunk_bytes);
	}

	atomic_store(&vctx->running, 0);
	atomic_fetch_sub(&ctx->control.validators_running, 1);

	return NULL;
}

/* --- Internal helpers for init / destroy --- */

/* Free all validator threads and marker arrays (NULL-safe, idempotent). */
static void free_validators(void)
{
	int i;

	if (!g_host_ctx.validators)
		return;

	for (i = 0; i < g_host_ctx.num_validators; i++) {
		if (g_host_ctx.validators[i].thread) {
			pthread_join(g_host_ctx.validators[i].thread, NULL);
			g_host_ctx.validators[i].thread = 0;
		}
		free(g_host_ctx.validators[i].own_markers);
		g_host_ctx.validators[i].own_markers = NULL;
	}
	free(g_host_ctx.validators);
	g_host_ctx.validators = NULL;
}

/* Debug-only: print per-validator marker/QP distribution. */
static void debug_print_marker_distribution(uint32_t num_qps,
					    const uint32_t *marker_counts)
{
	int i;
	uint32_t nv = (uint32_t)g_host_ctx.num_validators;
	uint32_t tm = g_host_ctx.total_markers;
	uint32_t base_count = tm / nv;
	uint32_t remainder  = tm % nv;

	printf(VALIDATION_LOG_PREFIX " Marker distribution "
	       "(QP-balanced hybrid, base=%u, remainder=%u):\n",
	       base_count, remainder);

	for (i = 0; i < g_host_ctx.num_validators; i++) {
		uint32_t qp_count = 0;
		uint32_t mask_words = (num_qps + 31) / 32;
		uint32_t *qp_mask = calloc(mask_words, sizeof(uint32_t));

		if (qp_mask) {
			for (uint32_t s = 0; s < marker_counts[i]; s++) {
				uint32_t q = g_host_ctx.validators[i].own_markers[s].qp_id;
				qp_mask[q / 32] |= (1u << (q % 32));
			}
			for (uint32_t w = 0; w < mask_words; w++)
				for (uint32_t b = 0; b < 32; b++)
					if (qp_mask[w] & (1u << b))
						qp_count++;
			free(qp_mask);
		}
		printf("  Validator[%d]: %u markers across %u QPs\n",
		       i, g_host_ctx.validators[i].own_marker_count, qp_count);
	}
}

/*
 * QP-balanced hybrid marker distribution.
 *
 * Walk markers in chunk-major, QP-interleaved order so each validator
 * gets markers from all (or most) QPs — balances load regardless of
 * per-QP traffic skew.  After assignment, sort each validator's
 * markers by memory address for cache-line reuse.
 *
 * Returns 0 on success, -1 on allocation failure (cleans up on error).
 */
static int distribute_markers(uint32_t num_qps, uint32_t chunks_per_qp,
			      uint32_t ops_per_chunk, uint64_t payload_size)
{
	int i;
	uint32_t qp, chunk, idx;
	uint32_t nv = (uint32_t)g_host_ctx.num_validators;
	uint32_t tm = g_host_ctx.total_markers;
	uint32_t base_count = tm / nv;
	uint32_t remainder  = tm % nv;

	/* Phase 1: Build QP-interleaved logical order */
	uint32_t *logical_order = (uint32_t *)calloc(tm, sizeof(uint32_t));
	if (!logical_order) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Failed to allocate logical order array\n");
		return -1;
	}
	{
		uint32_t pos = 0;
		for (chunk = 0; chunk < chunks_per_qp; chunk++)
			for (qp = 0; qp < num_qps; qp++)
				logical_order[pos++] = qp * chunks_per_qp + chunk;
	}

	/* Phase 2: Count markers per validator */
	uint32_t *marker_counts = (uint32_t *)calloc(nv, sizeof(uint32_t));
	if (!marker_counts) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Failed to allocate marker counts\n");
		free(logical_order);
		return -1;
	}
	for (i = 0; i < g_host_ctx.num_validators; i++)
		marker_counts[i] = base_count + ((uint32_t)i < remainder ? 1 : 0);

	/* Phase 3: Allocate per-validator marker arrays */
	for (i = 0; i < g_host_ctx.num_validators; i++) {
		g_host_ctx.validators[i].own_marker_count = marker_counts[i];
		g_host_ctx.validators[i].own_markers = (struct marker_scan_entry *)calloc(
			marker_counts[i], sizeof(struct marker_scan_entry));
		if (!g_host_ctx.validators[i].own_markers) {
			fprintf(stderr, VALIDATION_LOG_PREFIX " Failed to allocate "
				"marker array for validator %d\n", i);
			free(marker_counts);
			free(logical_order);
			return -1;
		}
	}

	/* Phase 4: Assign + sort markers per validator */
	uint32_t logical_pos = 0;
	for (i = 0; i < g_host_ctx.num_validators; i++) {
		uint32_t count = marker_counts[i];
		uint32_t *vindices = logical_order + logical_pos;
		logical_pos += count;

		/* Insertion sort by marker index (array is tiny) */
		for (uint32_t a = 1; a < count; a++) {
			uint32_t key = vindices[a];
			int b = (int)a - 1;
			while (b >= 0 && vindices[b] > key) {
				vindices[b + 1] = vindices[b];
				b--;
			}
			vindices[b + 1] = key;
		}

		/* Populate marker scan entries in sorted order */
		for (uint32_t slot = 0; slot < count; slot++) {
			idx = vindices[slot];
			qp = idx / chunks_per_qp;
			chunk = idx % chunks_per_qp;

			struct marker_scan_entry *entry =
				&g_host_ctx.validators[i].own_markers[slot];

			entry->marker_ptr = &g_host_ctx.tail_markers[idx];
			entry->last_seen = 0;
			entry->qp_id = qp;
			entry->chunk_id = chunk;
			entry->chunk_data = g_host_ctx.recv_slots +
				(uint64_t)(qp * g_host_ctx.slots_per_qp +
					   chunk * ops_per_chunk)
				* payload_size;
		}
	}

	if (g_host_ctx.debug_enabled)
		debug_print_marker_distribution(num_qps, marker_counts);

	free(logical_order);
	free(marker_counts);
	return 0;
}

/* --- Public API (implements memory_ctx interface) --- */

int host_validation_init(struct memory_ctx *ctx,
			 const struct validation_config *cfg)
{
	struct host_memory_ctx *host_ctx = container_of(ctx, struct host_memory_ctx, base);
	int i;

	void *buffer_base = cfg->buffer_base;
	uint64_t markers_offset = cfg->markers_offset;
	uint64_t recv_slots_offset = cfg->recv_slots_offset;
	uint64_t payload_size = cfg->payload_size;
	uint32_t ops_per_chunk = cfg->ops_per_chunk;
	uint32_t num_qps = cfg->num_qps;
	uint32_t chunks_per_qp = cfg->chunks_per_qp;
	int validation_mode = cfg->validation_mode;
	int debug_enabled = cfg->debug_enabled;

	if (g_host_ctx.initialized) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Already initialized\n");
		return -1;
	}

	if (!buffer_base) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " NULL buffer_base\n");
		return -1;
	}

	if (chunks_per_qp < 1 || chunks_per_qp > VALIDATION_MAX_CHUNKS_PER_QP) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " chunks_per_qp=%u out of range [1,%d]\n",
			chunks_per_qp, VALIDATION_MAX_CHUNKS_PER_QP);
		return -1;
	}

	memset(&g_host_ctx, 0, sizeof(g_host_ctx));

	g_host_ctx.buffer_base = buffer_base;
	g_host_ctx.markers_offset = markers_offset;
	g_host_ctx.recv_slots_offset = recv_slots_offset;
	g_host_ctx.payload_size = payload_size;
	g_host_ctx.ops_per_chunk = ops_per_chunk;
	g_host_ctx.num_qps = num_qps;
	g_host_ctx.chunks_per_qp = chunks_per_qp;
	g_host_ctx.slots_per_qp = ops_per_chunk * chunks_per_qp;
	g_host_ctx.validation_mode = validation_mode;
	g_host_ctx.debug_enabled = debug_enabled;
	g_host_ctx.numa_node = cfg->numa_node;

	g_host_ctx.tail_markers = (volatile uint64_t *)((uint8_t *)buffer_base + markers_offset);
	g_host_ctx.recv_slots = (uint8_t *)buffer_base + recv_slots_offset;
	g_host_ctx.total_markers = num_qps * chunks_per_qp;

	g_host_ctx.num_validators = get_num_validators(num_qps, chunks_per_qp,
						       validation_mode,
						       g_host_ctx.numa_node);

	HDBG("Init: buffer_base=%p, markers_offset=%lu, recv_slots_offset=%lu, "
	     "payload=%lu, ops_per_chunk=%u, num_qps=%u, num_validators=%d, "
	     "chunks_per_qp=%u, total_markers=%u, numa_node=%d\n",
	     buffer_base, (unsigned long)markers_offset,
	     (unsigned long)recv_slots_offset, (unsigned long)payload_size,
	     ops_per_chunk, num_qps, g_host_ctx.num_validators,
	     chunks_per_qp, g_host_ctx.total_markers, g_host_ctx.numa_node);
	HDBG("Architecture: Sharded Inline Validation (no monitor, no queues)\n");
	HDBG("Whole-chunk validation: %lu bytes per chunk (%u ops x %lu payload)\n",
	     (unsigned long)((uint64_t)ops_per_chunk * payload_size),
	     ops_per_chunk, (unsigned long)payload_size);

	{
		size_t alloc_size = g_host_ctx.num_validators *
				    sizeof(struct host_validator_ctx);
		g_host_ctx.validators = (struct host_validator_ctx *)
			aligned_alloc(128, alloc_size);
		if (!g_host_ctx.validators) {
			fprintf(stderr, VALIDATION_LOG_PREFIX " Failed to allocate "
				"validator contexts\n");
			return -1;
		}
		memset(g_host_ctx.validators, 0, alloc_size);
	}

	for (i = 0; i < g_host_ctx.num_validators; i++) {
		g_host_ctx.validators[i].id = i;
		g_host_ctx.validators[i].parent = &g_host_ctx;
		atomic_store(&g_host_ctx.validators[i].running, 0);
	}

	if (distribute_markers(num_qps, chunks_per_qp, ops_per_chunk,
			       payload_size) != 0) {
		free_validators();
		return -1;
	}

	atomic_store(&g_host_ctx.stats.errors_found, 0);
	atomic_store(&g_host_ctx.error.has_error, 0);
	atomic_store(&g_host_ctx.control.stop_flag, 0);
	atomic_store(&g_host_ctx.control.validators_running, 0);

	host_ctx->validation_ctx = &g_host_ctx;
	g_host_ctx.initialized = 1;

	HDBG("Initialization complete with %d sharded validators\n",
	     g_host_ctx.num_validators);

	return 0;
}

int host_validation_start(struct memory_ctx *ctx)
{
	int i;
	(void)ctx;

	if (!g_host_ctx.initialized) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Not initialized\n");
		return -1;
	}

	if (atomic_load(&g_host_ctx.control.validators_running) > 0) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Already running\n");
		return -1;
	}

	atomic_store(&g_host_ctx.control.stop_flag, 0);

	for (i = 0; i < g_host_ctx.num_validators; i++) {
		if (pthread_create(&g_host_ctx.validators[i].thread, NULL,
		                   validator_thread_fn, &g_host_ctx.validators[i]) != 0) {
			fprintf(stderr, VALIDATION_LOG_PREFIX " Failed to create "
				"validator thread %d\n", i);
			atomic_store(&g_host_ctx.control.stop_flag, 1);
			for (int j = 0; j < i; j++)
				pthread_join(g_host_ctx.validators[j].thread, NULL);
			return -1;
		}
	}

	while (atomic_load(&g_host_ctx.control.validators_running) <
	       g_host_ctx.num_validators) {
		cpu_relax();
	}

	return 0;
}

int host_validation_stop(struct memory_ctx *ctx,
                         struct data_validation_result *result)
{
	int i;
	(void)ctx;

	if (!g_host_ctx.initialized) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " Not initialized\n");
		return -1;
	}

	if (!result) {
		fprintf(stderr, VALIDATION_LOG_PREFIX " NULL result pointer\n");
		return -1;
	}

	atomic_store(&g_host_ctx.control.stop_flag, 1);

	for (i = 0; i < g_host_ctx.num_validators; i++) {
		if (g_host_ctx.validators[i].thread) {
			pthread_join(g_host_ctx.validators[i].thread, NULL);
			g_host_ctx.validators[i].thread = 0;
		}
	}

	uint64_t total_chunks = 0;
	uint64_t total_bytes = 0;
	uint64_t total_markers_scanned = 0;
	uint64_t total_markers_hit = 0;
	uint64_t total_skipped_steps = 0;
	uint64_t total_race_overwrites = 0;
	uint64_t total_dma_stale_retries = 0;

	for (i = 0; i < g_host_ctx.num_validators; i++) {
		total_chunks += g_host_ctx.validators[i].stats.work_items_processed;
		total_bytes += g_host_ctx.validators[i].stats.bytes_validated;
		total_markers_scanned += g_host_ctx.validators[i].stats.markers_scanned;
		total_markers_hit += g_host_ctx.validators[i].stats.markers_hit;
		total_skipped_steps += g_host_ctx.validators[i].stats.skipped_steps;
		total_race_overwrites += g_host_ctx.validators[i].stats.race_overwrites;
		total_dma_stale_retries += g_host_ctx.validators[i].stats.dma_stale_retries;
	}

	result->chunks_validated = total_chunks;
	result->bytes_validated = total_bytes;
	result->errors_found = (uint64_t)atomic_load(&g_host_ctx.stats.errors_found);
	result->passed = (result->errors_found == 0) ? 1 : 0;

	result->markers_scanned = total_markers_scanned;
	result->markers_hit = total_markers_hit;
	result->skipped_steps = total_skipped_steps;
	result->race_overwrites = total_race_overwrites;
	result->dma_stale_retries = total_dma_stale_retries;

	if (!result->passed) {
		result->error_qp_id = g_host_ctx.error.qp_id;
		result->error_chunk_id = g_host_ctx.error.chunk_id;
		result->error_byte_offset = g_host_ctx.error.byte_offset;
		result->error_expected = g_host_ctx.error.expected;
		result->error_actual = g_host_ctx.error.actual;
	}

	if (g_host_ctx.debug_enabled) {
		printf("\n--- Per-Validator Breakdown ---\n");
		for (i = 0; i < g_host_ctx.num_validators; i++) {
			struct host_validator_stats *vs =
				&g_host_ctx.validators[i].stats;
			double share = 0.0;
			if (total_chunks > 0)
				share = 100.0 * vs->work_items_processed /
					total_chunks;

			printf("  Validator[%d]: %lu slots (%.1f%%), "
			       "%u markers owned, "
			       "%lu hits / %lu scans",
			       i,
			       (unsigned long)vs->work_items_processed,
			       share,
			       g_host_ctx.validators[i].own_marker_count,
			       (unsigned long)vs->markers_hit,
			       (unsigned long)vs->markers_scanned);
			if (vs->race_overwrites || vs->dma_stale_retries)
				printf(", races=%lu, retries=%lu",
				       (unsigned long)vs->race_overwrites,
				       (unsigned long)vs->dma_stale_retries);
			printf("\n");
		}
		printf("---\n");
	}

	return 0;
}

void host_validation_destroy(struct memory_ctx *ctx)
{
	struct host_memory_ctx *host_ctx = container_of(ctx, struct host_memory_ctx, base);

	if (!g_host_ctx.initialized)
		return;

	atomic_store(&g_host_ctx.control.stop_flag, 1);
	free_validators();

	host_ctx->validation_ctx = NULL;
	g_host_ctx.initialized = 0;
	HDBG("Destroyed\n");
}
