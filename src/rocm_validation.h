/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Data validation for ROCm device memory.
 *
 * Upstream perftest validates host memory (src/host_validation.c, SIMD
 * polling threads) and CUDA memory (src/cuda_memory.c driving kernels from
 * cuda_kernels.cu).  There is no ROCm equivalent, so --data_validation is
 * refused for --use_rocm at parameter-parse time.
 *
 * This backend mirrors the CUDA one.  src/rocm_kernels.hip is a port of
 * cuda_kernels.cu: a persistent kernel runs for the duration of the transfer,
 * a monitor block scanning tail markers and validator blocks draining a
 * lock-free queue, so each cycle is checked as it lands.  That is the default
 * and reports the same counters the CUDA backend reports.
 *
 * Where it cannot run -- a build without hipcc, a device the kernel will not
 * launch on, or PERFTEST_ROCM_VALIDATION=staged -- the fallback is a host
 * compare: the whole validation region is copied device->host once, after the
 * run, and every chunk's final contents are checked against the pattern its
 * marker names.
 *
 * The fallback is a strictly weaker check.  It validates the final state of
 * each chunk rather than every cycle, so bytes_validated is much lower than
 * the live path reports for the same run, and transient corruption that a
 * later cycle overwrites is not detected.  Both paths catch what matters most
 * here -- address-translation and offset faults, where a transfer landing in
 * the wrong place writes the wrong bytes everywhere and a bandwidth test
 * cannot see it.
 *
 * Which path ran is printed at stop.  Do not compare bytes_validated across
 * them.
 */

#ifndef ROCM_VALIDATION_H
#define ROCM_VALIDATION_H

#include "memory.h"
#include "validation_common.h"

#ifdef __cplusplus
extern "C" {
#endif

int rocm_validation_init(struct memory_ctx *ctx,
			 const struct validation_config *cfg);
int rocm_validation_start(struct memory_ctx *ctx);
int rocm_validation_stop(struct memory_ctx *ctx,
			 struct data_validation_result *result);
void rocm_validation_destroy(struct memory_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ROCM_VALIDATION_H */
