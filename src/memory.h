/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Get pointer to containing type object by a pointer to its member field */
#define container_of(ptr, type, member) ({                      \
                const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
                (type *)( (char *)__mptr - offsetof(type,member) );})


/*
 * Data validation result - memory type agnostic.
 * Used to report validation status, error details, and diagnostic counters.
 * Both host and CUDA backends populate what they can; unused fields are 0.
 */
struct data_validation_result {
	int passed;                /* 1 = passed, 0 = failed */
	uint64_t bytes_validated;
	uint64_t chunks_validated;

	/* Error info (only valid if passed == 0) */
	uint64_t errors_found;     /* Total REAL mismatch count */
	uint32_t error_qp_id;
	uint32_t error_chunk_id;
	uint64_t error_byte_offset;
	uint8_t  error_expected;
	uint8_t  error_actual;

	/* Diagnostic counters (populated by both backends where applicable) */
	uint64_t markers_scanned;      /* Total marker polls */
	uint64_t markers_hit;          /* Markers that had new data */
	uint64_t skipped_steps;        /* Marker jumps > 1 (missed cycles) */
	uint64_t race_overwrites;      /* Epoch guard: DMA overwrite suppressions */
	uint64_t dma_stale_retries;    /* Tail retry: stale DMA resolutions */
};

/* Forward declaration for validation config */
struct validation_config;

/* Base context for memory management to be extended by concrete implementations */
struct memory_ctx {
	int (*init)(struct memory_ctx *ctx);
	int (*destroy)(struct memory_ctx *ctx);
	int (*allocate_buffer)(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
			       uint64_t *dmabuf_offset, void **addr, bool *can_init);
	int (*free_buffer)(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size);
	void *(*copy_host_to_buffer)(void *dest, const void *src, size_t size);
	void *(*copy_buffer_to_host)(void *dest, const void *src, size_t size);
	void *(*copy_buffer_to_buffer)(void *dest, const void *src, size_t size);
	/* Data validation interface (optional - NULL if not supported by memory type) */
	int (*validation_init)(struct memory_ctx *ctx,
		const struct validation_config *cfg);
	int (*validation_start)(struct memory_ctx *ctx);
	int (*validation_stop)(struct memory_ctx *ctx, struct data_validation_result *result);
	void (*validation_destroy)(struct memory_ctx *ctx);
};

#endif /* MEMORY_H */
