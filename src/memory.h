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
};

#endif /* MEMORY_H */
