/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (c) 2026 Shanghai Biren Technology Co., Ltd. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "supa_memory.h"
#include "perftest_parameters.h"
#include SUPA_PATH

#define SUCHECK(stmt) \
	do { \
	SUresult result = (stmt); \
	ASSERT(SUPA_SUCCESS == result); \
} while (0)

#define ACCEL_PAGE_SIZE (64 * 1024)

struct supa_memory_ctx {
	struct memory_ctx base;
	int device_id;
	char *device_bus_id;
	SUdevice su_device;
	SUcontext su_context;
	bool use_dmabuf;
};


static int init_gpu(struct supa_memory_ctx *ctx)
{
	int supa_device_id = ctx->device_id;
	int supa_pci_bus_id;
	int supa_pci_device_id;
	int index;
	SUdevice su_device;

	printf("initializing SUPA\n");
	SUresult error = suInit(0);
	if (error != SUPA_SUCCESS) {
		printf("suInit(0) returned %d\n", error);
		return FAILURE;
	}

	int deviceCount = 0;
	error = suDeviceGetCount(&deviceCount);
	if (error != SUPA_SUCCESS) {
		printf("suDeviceGetCount() returned %d\n", error);
		return FAILURE;
	}
	/* This function call returns 0 if there are no SUPA capable devices. */
	if (deviceCount == 0) {
		printf("There are no available device(s) that support SUPA\n");
		return FAILURE;
	}
	if (supa_device_id >= deviceCount) {
		fprintf(stderr, "No such device ID (%d) exists in system\n", supa_device_id);
		return FAILURE;
	}

	printf("Listing all SUPA devices in system:\n");
	for (index = 0; index < deviceCount; index++) {
		SUCHECK(suDeviceGet(&su_device, index));
		suDeviceGetAttribute(&supa_pci_bus_id, SU_DEVICE_ATTRIBUTE_PCI_BUS_ID , su_device);
		suDeviceGetAttribute(&supa_pci_device_id, SU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID , su_device);
		printf("SUPA device %d: PCIe address is %02X:%02X\n", index, (unsigned int)supa_pci_bus_id, (unsigned int)supa_pci_device_id);
	}

	printf("\nPicking device No. %d\n", supa_device_id);

	SUCHECK(suDeviceGet(&ctx->su_device, supa_device_id));

	char name[128];
	SUCHECK(suDeviceGetName(name, sizeof(name), supa_device_id));
	printf("[pid = %d, dev = %d] device name = [%s]\n", getpid(), ctx->su_device, name);
	printf("creating SUPA Ctx\n");

	/* Create context */
	error = suCtxCreate(&ctx->su_context, SU_CTX_MAP_HOST, ctx->su_device);
	if (error != SUPA_SUCCESS) {
		printf("suCtxCreate() error=%d\n", error);
		return FAILURE;
	}

	printf("making it the current SUPA Ctx\n");
	error = suCtxSetCurrent(ctx->su_context);
	if (error != SUPA_SUCCESS) {
		printf("suCtxSetCurrent() error=%d\n", error);
		return FAILURE;
	}

	return SUCCESS;
}

static void free_gpu(struct supa_memory_ctx *ctx)
{
	printf("destroying current SUPA Ctx\n");
	SUCHECK(suCtxDestroy(ctx->su_context));
}

int supa_memory_init(struct memory_ctx *ctx) {
	struct supa_memory_ctx *supa_ctx = container_of(ctx, struct supa_memory_ctx, base);
	int return_value = 0;

	if (supa_ctx->device_bus_id) {
		int err;

		printf("initializing SUPA\n");
		SUresult error = suInit(0);
		if (error != SUPA_SUCCESS) {
			printf("suInit(0) returned %d\n", error);
			return FAILURE;
		}

		printf("Finding PCIe BUS %s\n", supa_ctx->device_bus_id);
		err = suDeviceGetByPCIBusId(&supa_ctx->device_id, supa_ctx->device_bus_id);
		if (err != 0) {
			fprintf(stderr, "suDeviceGetByPCIBusId failed with error: %d; Failed to get PCI Bus ID (%s)\n", err, supa_ctx->device_bus_id);
			return FAILURE;
		}
		printf("Picking GPU number %d\n", supa_ctx->device_id);
	}

	return_value = init_gpu(supa_ctx);
	if (return_value) {
		fprintf(stderr, "Couldn't init GPU context: %d\n", return_value);
		return FAILURE;
	}

#ifdef HAVE_SUPA_DMABUF
	if (supa_ctx->use_dmabuf) {
		int is_supported = 0;

		SUCHECK(suDeviceGetAttribute(&is_supported, SU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, supa_ctx->su_device));
		if (!is_supported) {
			fprintf(stderr, "DMA-BUF is not supported on this GPU\n");
			return FAILURE;
		}
	}
#endif

	return SUCCESS;
}

int supa_memory_destroy(struct memory_ctx *ctx) {
	struct supa_memory_ctx *supa_ctx = container_of(ctx, struct supa_memory_ctx, base);

	free_gpu(supa_ctx);
	free(supa_ctx);
	return SUCCESS;
}

int supa_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset,  void **addr, bool *can_init) {
	SUresult error;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

	struct supa_memory_ctx *supa_ctx = container_of(ctx, struct supa_memory_ctx, base);
	int supa_device_integrated;
	suDeviceGetAttribute(&supa_device_integrated, SU_DEVICE_ATTRIBUTE_INTEGRATED, supa_ctx->su_device);

	if (supa_device_integrated == 1) {
		error = suMemAllocHost(addr, buf_size);
		if (error != SUPA_SUCCESS) {
			printf("suMemAllocHost error=%d\n", error);
			return FAILURE;
		}

		printf("allocated GPU buffer address at %p\n", addr);
		*can_init = false;
	} else {
		SUdeviceptr d_A;
		error = suMemAlloc(&d_A, buf_size);
		if (error != SUPA_SUCCESS) {
			printf("suMemAlloc error=%d\n", error);
			return FAILURE;
		}

		printf("allocated GPU buffer address at %p\n", (void *)d_A);
		*addr = (void *)d_A;
		*can_init = false;

#ifdef HAVE_SUPA_DMABUF
		{
			if (supa_ctx->use_dmabuf) {
				SUdeviceptr aligned_ptr;
				const size_t host_page_size = sysconf(_SC_PAGESIZE);
				uint64_t offset;
				size_t aligned_size;

				// Round down to host page size
				aligned_ptr = (SUdeviceptr)((uint64_t)d_A & ~(host_page_size - 1));
				offset = d_A - aligned_ptr;
				aligned_size = (size + offset + host_page_size - 1) & ~(host_page_size - 1);

				printf("using DMA-BUF for GPU buffer address at %p aligned at %p with aligned size %zu\n", (void *)d_A, (void *)aligned_ptr, aligned_size);
				*dmabuf_fd = 0;
				error = suMemGetHandleForAddressRange((void *)dmabuf_fd, aligned_ptr, aligned_size, SU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
				if (error != SUPA_SUCCESS) {
					printf("suMemGetHandleForAddressRange error=%d\n", error);
					return FAILURE;
				}

				*dmabuf_offset = offset;
			}
		}
#endif
	}

	return SUCCESS;
}

int supa_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	struct supa_memory_ctx *supa_ctx = container_of(ctx, struct supa_memory_ctx, base);
	int supa_device_integrated;
	suDeviceGetAttribute(&supa_device_integrated, SU_DEVICE_ATTRIBUTE_INTEGRATED, supa_ctx->su_device);

	if (supa_device_integrated == 1) {
		printf("deallocating GPU buffer %p\n", addr);
		suMemFreeHost(addr);
	} else {
		SUdeviceptr d_A = (SUdeviceptr)addr;
		printf("deallocating GPU buffer %p\n", (void *)d_A);
		suMemFree(d_A);
	}

	return SUCCESS;
}

void *supa_memory_copy_host_buffer(void *dest, const void *src, size_t size) {
	suMemcpy((SUdeviceptr)dest, (SUdeviceptr)src, size);
	return dest;
}

void *supa_memory_copy_buffer_to_buffer(void *dest, const void *src, size_t size) {
	suMemcpyDtoD((SUdeviceptr)dest, (SUdeviceptr)src, size);
	return dest;
}

bool supa_memory_supported() {
	return true;
}

bool supa_memory_dmabuf_supported() {
#ifdef HAVE_SUPA_DMABUF
	return true;
#else
	return false;
#endif
}

struct memory_ctx *supa_memory_create(struct perftest_parameters *params) {
	struct supa_memory_ctx *ctx;

	ALLOCATE(ctx, struct supa_memory_ctx, 1);
	ctx->base.init = supa_memory_init;
	ctx->base.destroy = supa_memory_destroy;
	ctx->base.allocate_buffer = supa_memory_allocate_buffer;
	ctx->base.free_buffer = supa_memory_free_buffer;
	ctx->base.copy_host_to_buffer = supa_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_host = supa_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_buffer = supa_memory_copy_buffer_to_buffer;
	ctx->device_id = params->supa_device_id;
	ctx->device_bus_id = params->supa_device_bus_id;
	ctx->use_dmabuf = params->use_supa_dmabuf;

	return &ctx->base;
}
