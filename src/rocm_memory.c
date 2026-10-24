/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 * Copyright 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "rocm_memory.h"
#include "rocm_validation.h"
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

	/*
	 * No dma_buf pre-flight check here, deliberately.
	 *
	 * This used to scan /boot/config-* for CONFIG_DMABUF_MOVE_NOTIFY and
	 * CONFIG_PCI_P2PDMA and refuse to run if either was missing. Both
	 * halves were wrong. move_notify no longer exists upstream -- dma-buf
	 * always builds with it, and .move_notify became .invalidate_mappings
	 * -- so the probe started failing precisely because support became
	 * universal. And what it had gated was whether amdgpu_dma_buf_pin()
	 * kept AMDGPU_GEM_DOMAIN_VRAM or fell back to GTT: driver policy,
	 * versioned with the driver rather than with the running kernel, which
	 * on a DKMS install moves independently of it. Today that placement
	 * depends only on every attachment advertising peer2peer, which is not
	 * observable before attach either.
	 *
	 * PCI_P2PDMA fares no better. It cannot see whether the NIC's driver
	 * implements .reg_user_mr_dmabuf, so it passes on hardware that then
	 * fails to register anyway.
	 *
	 * Both questions the check was trying to answer are already answered,
	 * accurately, by the calls that do the work:
	 * hsa_amd_portable_export_dmabuf() in rocm_memory_allocate_buffer()
	 * reports whether ROCm can export, and ibv_reg_dmabuf_mr() reports
	 * whether this NIC can import. Let those speak for themselves.
	 */

	return SUCCESS;
}

int rocm_memory_destroy(struct memory_ctx *ctx) {
	struct rocm_memory_ctx *rocm_ctx = container_of(ctx, struct rocm_memory_ctx, base);

	free(rocm_ctx);
	return SUCCESS;
}

static void *rocm_memory_copy_host_to_buffer(void *dest, const void *src,
					     size_t size)
{
	hipError_t error = hipMemcpy(dest, src, size, hipMemcpyHostToDevice);

	if (error != hipSuccess)
		printf("hipMemcpy(HostToDevice) error=%d\n", error);
	return dest;
}

static void *rocm_memory_copy_buffer_to_host(void *dest, const void *src,
					     size_t size)
{
	hipError_t error = hipMemcpy(dest, src, size, hipMemcpyDeviceToHost);

	if (error != hipSuccess)
		printf("hipMemcpy(DeviceToHost) error=%d\n", error);
	return dest;
}

static void *rocm_memory_copy_buffer_to_buffer(void *dest, const void *src,
					       size_t size)
{
	hipError_t error = hipMemcpy(dest, src, size, hipMemcpyDeviceToDevice);

	if (error != hipSuccess)
		printf("hipMemcpy(DeviceToDevice) error=%d\n", error);
	return dest;
}

int rocm_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	void *d_A;
	hipError_t error;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

	/* Some RDMA devices cannot reach the GPU's VRAM even though the MR
	 * registers cleanly -- the failure surfaces later, as a gather error on
	 * the first work request. A host-mapped allocation is still addressable
	 * by the GPU but lives in system memory, which such a device can reach.
	 * Opt in with PERFTEST_ROCM_HOST_ALLOC=1. Note this measures host memory
	 * rather than VRAM, so it diagnoses reachability, it does not benchmark
	 * peer-to-peer. */
	if (getenv("PERFTEST_ROCM_HOST_ALLOC") != NULL) {
		error = hipHostMalloc(&d_A, buf_size,
				      hipHostMallocMapped | hipHostMallocPortable);
		if (error != hipSuccess) {
			printf("hipHostMalloc error=%d\n", error);
			return FAILURE;
		}
		printf("PERFTEST_ROCM_HOST_ALLOC: host-mapped GPU buffer\n");
	} else {
		error = hipMalloc(&d_A, buf_size);
		if (error != hipSuccess) {
			printf("hipMalloc error=%d\n", error);
			return FAILURE;
		}
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
			fprintf(stderr,
				"failed to export dmabuf handle for addr %p / %zu: "
				"hsa_status_t=%d\n", d_A, aligned_size, status);
			fprintf(stderr,
				"ROCm could not export this allocation as a dma_buf. "
				"The kernel may predate AMDKFD_IOC_EXPORT_DMABUF, or "
				"amdgpu may not be the exporter for this device.\n");
			return FAILURE;
		}

		printf("dmabuf export addr %p %zu to dmabuf fd %d offset %lu\n",
				d_A, aligned_size, *dmabuf_fd, offset);

		*dmabuf_offset = offset;
	}
#endif

	printf("allocated %lu bytes of GPU buffer at %p\n", (unsigned long)buf_size, d_A);
	*addr = d_A;
	/* Device memory: initialize_buffer_content() must not fill this with
	 * CPU stores. Where the allocation has no host mapping they fault, and
	 * the crash lands in perftest_rand() long after the MR was created. The
	 * buffer contents do not matter to a bandwidth test. */
	*can_init = false;
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
	ctx->base.copy_host_to_buffer = rocm_memory_copy_host_to_buffer;
	ctx->base.copy_buffer_to_host = rocm_memory_copy_buffer_to_host;
	ctx->base.copy_buffer_to_buffer = rocm_memory_copy_buffer_to_buffer;
	ctx->base.validation_init = rocm_validation_init;
	ctx->base.validation_start = rocm_validation_start;
	ctx->base.validation_stop = rocm_validation_stop;
	ctx->base.validation_destroy = rocm_validation_destroy;
	ctx->device_id = params->rocm_device_id;
	ctx->use_dmabuf = params->use_rocm_dmabuf;

	return &ctx->base;
}
