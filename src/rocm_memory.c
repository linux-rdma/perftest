/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "rocm_memory.h"
#include <hip/hip_runtime_api.h>
#if defined HAVE_HIP_HIP_VERSION_H
#include <hip/hip_version.h>
#endif
#include "perftest_parameters.h"

#define ROCM_CHECK(stmt)			\
	do {					\
	hipError_t result = (stmt);		\
	ASSERT(hipSuccess == result);		\
} while (0)

#define ACCEL_PAGE_SIZE (64 * 1024)


struct rocm_memory_ctx {
	struct memory_ctx base;
	int device_id;
};


static int init_rocm(int device_id) {
	int deviceCount = 0;
	hipError_t error = hipGetDeviceCount(&deviceCount);

	if (error != hipSuccess) {
		printf("hipDeviceGetCount() returned %d\n", error);
		return FAILURE;
	}

	if (device_id >= deviceCount) {
		printf("Requested ROCm device %d but found only %d device(s)\n",
               device_id, deviceCount);
		return FAILURE;
	}

	ROCM_CHECK(hipSetDevice(device_id));

	hipDeviceProp_t prop = {0};
	ROCM_CHECK(hipGetDeviceProperties(&prop, device_id));

	/* Need 256 bytes to silence compiler warning */
	char archName[256];
#if HIP_VERSION >= 60000000
	snprintf(archName, 256, "%s", prop.gcnArchName);
#else
	snprintf(archName, 256, "%d", prop.gcnArch);
#endif

	printf("Using ROCm Device with ID: %d, Name: %s, PCI Bus ID: 0x%x, GCN Arch: %s\n",
	       device_id, prop.name, prop.pciBusID, archName);

	return SUCCESS;
}

int rocm_memory_init(struct memory_ctx *ctx) {
	struct rocm_memory_ctx *rocm_ctx = container_of(ctx, struct rocm_memory_ctx, base);

	if (init_rocm(rocm_ctx->device_id)) {
		fprintf(stderr, "Couldn't initialize ROCm device\n");
		return FAILURE;
	}
	return SUCCESS;
}

int rocm_memory_destroy(struct memory_ctx *ctx) {
	struct rocm_memory_ctx *rocm_ctx = container_of(ctx, struct rocm_memory_ctx, base);

	free(rocm_ctx);
	return SUCCESS;
}

int rocm_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	void *d_A;
	hipError_t error;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

	error = hipMalloc(&d_A, buf_size);
	if (error != hipSuccess) {
		printf("hipMalloc error=%d\n", error);
		return FAILURE;
	}

	printf("allocated %lu bytes of GPU buffer at %p\n", (unsigned long)buf_size, d_A);
	*addr = d_A;
	*can_init = true;
	return SUCCESS;
}

int rocm_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	printf("deallocating GPU buffer %p\n", addr);
	hipFree(addr);
	return SUCCESS;
}

bool rocm_memory_supported() {
	return true;
}

struct memory_ctx *rocm_memory_create(struct perftest_parameters *params) {
	struct rocm_memory_ctx *ctx;

	ALLOCATE(ctx, struct rocm_memory_ctx, 1);
	ctx->base.init = rocm_memory_init;
	ctx->base.destroy = rocm_memory_destroy;
	ctx->base.allocate_buffer = rocm_memory_allocate_buffer;
	ctx->base.free_buffer = rocm_memory_free_buffer;
	ctx->base.copy_host_to_buffer = memcpy;
	ctx->base.copy_buffer_to_host = memcpy;
	ctx->base.copy_buffer_to_buffer = memcpy;
	ctx->device_id = params->rocm_device_id;

	return &ctx->base;
}
