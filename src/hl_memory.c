/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "hl_memory.h"
#include "perftest_parameters.h"
#include "synapse_api.h"
#include "hlthunk.h"

#define ACCEL_PAGE_SIZE 4096


struct hl_memory_ctx {
	struct memory_ctx base;
	char *device_bus_id;
	synDeviceId device_id;
	int device_fd;
};


int hl_memory_init(struct memory_ctx *ctx) {
	struct hl_memory_ctx *hl_ctx = container_of(ctx, struct hl_memory_ctx, base);
	synStatus status;
	synDeviceInfo device_info;

	status = synInitialize();
	if (status != synSuccess) {
		fprintf(stderr, "Failed to initialize synapse, status %d\n", status);
		return FAILURE;
	}

	status = synDeviceAcquire(&hl_ctx->device_id, hl_ctx->device_bus_id);
	if (status != synSuccess) {
		fprintf(stderr, "Failed to acquire device, status %d\n", status);
		return FAILURE;
	}

	status = synDeviceGetInfo(hl_ctx->device_id, &device_info);
	if (status != synSuccess) {
		fprintf(stderr, "Failed to get device info, status %d\n", status);
		return FAILURE;
	}

	hl_ctx->device_fd = device_info.fd;
	return SUCCESS;
}

int hl_memory_destroy(struct memory_ctx *ctx) {
	struct hl_memory_ctx *hl_ctx = container_of(ctx, struct hl_memory_ctx, base);

	synDeviceRelease(hl_ctx->device_id);
	synDestroy();

	free(hl_ctx);
	return SUCCESS;
}

int hl_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
			      uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	struct hl_memory_ctx *hl_ctx = container_of(ctx, struct hl_memory_ctx, base);
	int fd;
	uint64_t buffer_addr;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);
	synStatus status = synDeviceMalloc(hl_ctx->device_id, buf_size, 0, 0, &buffer_addr);

	if (status != synSuccess) {
		fprintf(stderr, "Failed to allocate HL memory on device %d of size %lu\n",
			hl_ctx->device_id, (unsigned long)buf_size);
		return FAILURE;
	}

	fd = hlthunk_device_memory_export_dmabuf_fd(hl_ctx->device_fd, buffer_addr, buf_size, 0);
	if (fd < 0) {
		fprintf(stderr, "Failed to export dmabuf. sz[%lu] ptr[%p] err[%d]\n",
			(unsigned long)buf_size, (void*)buffer_addr, fd);
		return FAILURE;
	}

	printf("Allocated %lu bytes of accelerator buffer at %p on fd %d\n",
	       (unsigned long)buf_size, (void*)buffer_addr, fd);
	*dmabuf_fd = fd;
	*dmabuf_offset = 0;
	*addr = (void*)buffer_addr;
	*can_init = false;
	return SUCCESS;
}

int hl_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	struct hl_memory_ctx *hl_ctx = container_of(ctx, struct hl_memory_ctx, base);

	synDeviceFree(hl_ctx->device_id, (uint64_t)addr, 0);
	return SUCCESS;
}

bool hl_memory_supported() {
	return true;
}

struct memory_ctx *hl_memory_create(struct perftest_parameters *params) {
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
