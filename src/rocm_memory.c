/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 * Copyright 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/utsname.h>
#include "rocm_memory.h"
#include <hip/hip_runtime_api.h>
#if defined HAVE_HIP_HIP_VERSION_H
#include <hip/hip_version.h>
#endif
#include "perftest_parameters.h"
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#define ROCM_CHECK(stmt)			\
	do {					\
	hipError_t result = (stmt);		\
	ASSERT(hipSuccess == result);		\
} while (0)

#define ACCEL_PAGE_SIZE (64 * 1024)


struct rocm_memory_ctx {
	struct memory_ctx base;
	int device_id;
	bool use_dmabuf;
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

#ifdef HAVE_ROCM_DMABUF
	if (rocm_ctx->use_dmabuf) {
		int dmabuf_supported = 0;
		const char kernel_opt1[] = "CONFIG_DMABUF_MOVE_NOTIFY=y";
		const char kernel_opt2[] = "CONFIG_PCI_P2PDMA=y";
		int found_opt1           = 0;
		int found_opt2           = 0;
		FILE *fp;
		struct utsname utsname;
		char kernel_conf_file[128];
		char buf[256];

		if (uname(&utsname) == -1) {
			printf("could not get kernel name");
			return FAILURE;
		}

		snprintf(kernel_conf_file, sizeof(kernel_conf_file),
						"/boot/config-%s", utsname.release);
		fp = fopen(kernel_conf_file, "r");
		if (fp == NULL) {
			printf("could not open kernel conf file %s error: %m",
					kernel_conf_file);
			return FAILURE;
		}

		while (fgets(buf, sizeof(buf), fp) != NULL) {
			if (strstr(buf, kernel_opt1) != NULL) {
				found_opt1 = 1;
			}
			if (strstr(buf, kernel_opt2) != NULL) {
				found_opt2 = 1;
			}
			if (found_opt1 && found_opt2) {
				dmabuf_supported = 1;
				break;
			}
		}
		fclose(fp);

		if (dmabuf_supported == 0) {
			return FAILURE;
		}
	}
#endif

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

#ifdef HAVE_ROCM_DMABUF
	struct rocm_memory_ctx *rocm_ctx = container_of(ctx, struct rocm_memory_ctx, base);

	if (rocm_ctx->use_dmabuf) {
		hipDeviceptr_t aligned_ptr;
		const size_t host_page_size = sysconf(_SC_PAGESIZE);
		uint64_t offset;
		size_t aligned_size;
		hsa_status_t status;

		// Round down to host page size
		aligned_ptr = (hipDeviceptr_t)((uintptr_t)d_A & ~(host_page_size - 1));
		offset = d_A - aligned_ptr;
		aligned_size = (size + offset + host_page_size - 1) & ~(host_page_size - 1);

		printf("using DMA-BUF for GPU buffer address at %p aligned at %p with aligned size %zu\n", d_A, aligned_ptr, aligned_size);
		*dmabuf_fd = 0;

		status = hsa_amd_portable_export_dmabuf(d_A, aligned_size, dmabuf_fd, &offset);
		if (status != HSA_STATUS_SUCCESS) {
			printf("failed to export dmabuf handle for addr %p / %zu", d_A,
					aligned_size);
			return FAILURE;
		}

		printf("dmabuf export addr %p %zu to dmabuf fd %d offset %lu\n",
				d_A, aligned_size, *dmabuf_fd, offset);

		*dmabuf_offset = offset;
	}
#endif

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

bool rocm_memory_dmabuf_supported() {
#ifdef HAVE_ROCM_DMABUF
	return true;
#else
	return false;
#endif
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
	ctx->use_dmabuf = params->use_rocm_dmabuf;

	return &ctx->base;
}
