/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include "hl_memory.h"
#include "perftest_parameters.h"
#include "hlthunk.h"

#define ACCEL_PAGE_SIZE (4096)
#define INVALID_FD (-1)

#define LIST_SIZE (64)

struct KeyValuePair {
    uint64_t key;
    uint64_t value;
    bool is_occupied;
};

struct hl_memory_ctx {
    struct memory_ctx base;
    char *device_bus_id;
    int device_fd;
    struct KeyValuePair mem_handle_table[LIST_SIZE];
    pthread_mutex_t mem_handle_table_lock;
};

static int hl_set_memory_handle(struct hl_memory_ctx *const hl_ctx, const uint64_t addr, const uint64_t memory_handle) 
{
    size_t i = 0;

    for (i = 0; i < LIST_SIZE; i++) {
	if (hl_ctx->mem_handle_table[i].is_occupied)
	    continue;
	hl_ctx->mem_handle_table[i].key = addr;
	hl_ctx->mem_handle_table[i].value = memory_handle;
	hl_ctx->mem_handle_table[i].is_occupied = true;
	return SUCCESS; // successfully inserted
    }
    return FAILURE; // list is full
}

static int
hl_delete_memory_handle(struct hl_memory_ctx *const hl_ctx, const uint64_t addr, uint64_t *const memory_handle) {
    size_t i = 0;

    for (i = 0; i < LIST_SIZE; i++) {
	if (hl_ctx->mem_handle_table[i].is_occupied && hl_ctx->mem_handle_table[i].key == addr) {
	    hl_ctx->mem_handle_table[i].is_occupied = false;
	    *memory_handle = hl_ctx->mem_handle_table[i].value;
	    return SUCCESS; // key removed
	}
    }
    return FAILURE; // key not found
}

int hl_memory_init(struct memory_ctx *ctx) 
{
    struct hl_memory_ctx *const hl_ctx = container_of(ctx, struct hl_memory_ctx, base);

    hl_ctx->device_fd = hlthunk_open(HLTHUNK_DEVICE_DONT_CARE, hl_ctx->device_bus_id);
    if (hl_ctx->device_fd < 0)
	return FAILURE;

    memset(hl_ctx->mem_handle_table, 0, sizeof(hl_ctx->mem_handle_table));

    if (pthread_mutex_init(&hl_ctx->mem_handle_table_lock, NULL) != 0) {
	(void) hlthunk_close(hl_ctx->device_fd);
	return FAILURE;
    }

    return SUCCESS;
}

int hl_memory_destroy(struct memory_ctx *ctx) 
{
    struct hl_memory_ctx *const hl_ctx = container_of(ctx, struct hl_memory_ctx, base);

    (void) pthread_mutex_destroy(&hl_ctx->mem_handle_table_lock);
    (void) hlthunk_close(hl_ctx->device_fd);

    free(hl_ctx);
    return SUCCESS;
}

int hl_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
			      uint64_t *dmabuf_offset, void **addr, bool *can_init) {
    struct hl_memory_ctx *const hl_ctx = container_of(ctx, struct hl_memory_ctx, base);
    const uint64_t page_size = 0;
    const uint64_t NO_OFFSET = 0;
    const bool NOT_SHARED = false;
    int fd = INVALID_FD;
    uint64_t buffer_addr = 0;
    const size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

    const uint64_t memory_handle = hlthunk_device_memory_alloc(hl_ctx->device_fd, buf_size, page_size,
							       HL_MEM_CONTIGUOUS, NOT_SHARED);
    if (memory_handle == 0) {
	fprintf(stderr, "Failed to allocate %lu bytes of device memory\n", (unsigned long) buf_size);
	return FAILURE;
    }
    buffer_addr = hlthunk_device_memory_map(hl_ctx->device_fd, memory_handle, 0);
    if (buffer_addr == 0) {
	fprintf(stderr, "Failed to map device memory allocation\n");
	return FAILURE;
    }
    if (pthread_mutex_lock(&hl_ctx->mem_handle_table_lock) != 0) {
	fprintf(stderr, "Failed to lock mutex while allocating memory\n");
	return FAILURE;
    }
    if (hl_set_memory_handle(hl_ctx, buffer_addr, memory_handle) != SUCCESS) {
	(void) pthread_mutex_unlock(&hl_ctx->mem_handle_table_lock);
	return FAILURE;
    }
    if (pthread_mutex_unlock(&hl_ctx->mem_handle_table_lock) != 0) {
	fprintf(stderr, "Failed to unlock mutex\n");
	return FAILURE;
    }

    fd = hlthunk_device_mapped_memory_export_dmabuf_fd(hl_ctx->device_fd, buffer_addr, buf_size, NO_OFFSET,
						       O_RDWR | O_CLOEXEC);
    if (fd < 0) {
	fprintf(stderr, "Failed to export dmabuf. sz[%lu] ptr[%p] err[%d]\n",
		(unsigned long) buf_size, (void *) buffer_addr, fd);
	return FAILURE;
    }

    fprintf(stderr, "Allocated %lu bytes of accelerator buffer at %p on fd %d\n",
	    (unsigned long) buf_size, (void *) buffer_addr, fd);
    *dmabuf_fd = fd;
    *dmabuf_offset = NO_OFFSET;
    *addr = (void *) buffer_addr;
    *can_init = false;
    return SUCCESS;
}

int hl_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) 
{
    struct hl_memory_ctx *hl_ctx = container_of(ctx, struct hl_memory_ctx, base);
    uint64_t memory_handle = INVALID_FD;
    int rc = hlthunk_memory_unmap(hl_ctx->device_fd, (uint64_t) addr);

    if (rc) {
	fprintf(stderr, "Failed to unmap host memory\n");
	return rc;
    }
    if (pthread_mutex_lock(&hl_ctx->mem_handle_table_lock) != 0) {
	fprintf(stderr, "Failed to lock mutex while deallocating memory\n");
	return FAILURE;
    }
    if (hl_delete_memory_handle(hl_ctx, (uint64_t) addr, &memory_handle) != SUCCESS) {
	fprintf(stderr, "Failed to remove memory handle\n");
	(void) pthread_mutex_unlock(&hl_ctx->mem_handle_table_lock);
	return FAILURE;
    }

    rc = hlthunk_device_memory_free(hl_ctx->device_fd, memory_handle);
    pthread_mutex_unlock(&hl_ctx->mem_handle_table_lock);
    return (rc == 0) ? SUCCESS : FAILURE;
}

bool hl_memory_supported(void) 
{
    return true;
}

struct memory_ctx *hl_memory_create(struct perftest_parameters *params) 
{
    struct hl_memory_ctx *ctx;

    ALLOCATE(ctx, struct hl_memory_ctx, 1);
    ctx->base.init = hl_memory_init;
    ctx->base.destroy = hl_memory_destroy;
    ctx->base.allocate_buffer = hl_memory_allocate_buffer;
    ctx->base.free_buffer = hl_memory_free_buffer;
    ctx->base.copy_host_to_buffer = memcpy;
    ctx->base.copy_buffer_to_host = memcpy;
    ctx->base.copy_buffer_to_buffer = memcpy;
    ctx->device_bus_id = params->hl_device_bus_id;
    return &ctx->base;
}
