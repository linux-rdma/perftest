/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2026 Moore Threads Technology Co. Ltd. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "musa_loader.h"
#include "musa_memory.h"
#include "perftest_parameters.h"

#define MUCHECK(stmt) \
	do { \
		MUresult result = (stmt); \
		ASSERT(MUSA_SUCCESS == result); \
	} while (0)

#define ACCEL_PAGE_SIZE (64 * 1024)

struct musa_memory_ctx {
	struct memory_ctx base;
	int device_id;
	char *device_bus_id;
	MUdevice mu_device;
	MUcontext mu_context;
	bool use_dmabuf;
	bool use_pcie_mapping;
};

static int init_gpu(struct musa_memory_ctx *ctx)
{
	int device_count = 0;
	int index;
	MUdevice mu_device;
	MUresult error;

	printf("initializing MUSA\n");
	error = p_muInit(0);
	if (error != MUSA_SUCCESS) {
		printf("muInit(0) returned %d\n", error);
		return FAILURE;
	}

	error = p_muDeviceGetCount(&device_count);
	if (error != MUSA_SUCCESS) {
		printf("muDeviceGetCount() returned %d\n", error);
		return FAILURE;
	}

	if (device_count == 0) {
		printf("There are no available device(s) that support MUSA\n");
		return FAILURE;
	}

	if (ctx->device_id >= device_count) {
		fprintf(stderr, "No such device ID (%d) exists in system\n", ctx->device_id);
		return FAILURE;
	}

	printf("Listing all MUSA devices in system:\n");
	for (index = 0; index < device_count; index++) {
		int pci_bus_id = 0;
		int pci_device_id = 0;

		MUCHECK(p_muDeviceGet(&mu_device, index));
		p_muDeviceGetAttribute(&pci_bus_id, MU_DEVICE_ATTRIBUTE_PCI_BUS_ID, mu_device);
		p_muDeviceGetAttribute(&pci_device_id, MU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, mu_device);
		printf("MUSA device %d: PCIe address is %02X:%02X\n",
		       index, (unsigned int)pci_bus_id, (unsigned int)pci_device_id);
	}

	printf("\nPicking device No. %d\n", ctx->device_id);
	MUCHECK(p_muDeviceGet(&ctx->mu_device, ctx->device_id));

	{
		char name[128];

		MUCHECK(p_muDeviceGetName(name, sizeof(name), ctx->device_id));
		printf("[pid = %d, dev = %d] device name = [%s]\n", getpid(), ctx->mu_device, name);
	}

	printf("creating MUSA Ctx\n");
	error = p_muCtxCreate(&ctx->mu_context, MU_CTX_MAP_HOST, ctx->mu_device);
	if (error != MUSA_SUCCESS) {
		printf("muCtxCreate() error=%d\n", error);
		return FAILURE;
	}

	printf("making it the current MUSA Ctx\n");
	error = p_muCtxSetCurrent(ctx->mu_context);
	if (error != MUSA_SUCCESS) {
		printf("muCtxSetCurrent() error=%d\n", error);
		return FAILURE;
	}

	return SUCCESS;
}

static void free_gpu(struct musa_memory_ctx *ctx)
{
	if (!ctx->mu_context)
		return;

	printf("destroying current MUSA Ctx\n");
	MUCHECK(p_muCtxDestroy(ctx->mu_context));
	ctx->mu_context = 0;
}

static int musa_memory_init(struct memory_ctx *ctx)
{
	struct musa_memory_ctx *musa_ctx = container_of(ctx, struct musa_memory_ctx, base);
	int return_value;

	if (load_musa_library()) {
		printf("Failed to load MUSA library dynamically\n");
		return FAILURE;
	}

	if (musa_ctx->device_bus_id) {
		MUresult error;

		printf("initializing MUSA\n");
		error = p_muInit(0);
		if (error != MUSA_SUCCESS) {
			printf("muInit(0) returned %d\n", error);
			return FAILURE;
		}

		printf("Finding PCIe BUS %s\n", musa_ctx->device_bus_id);
		error = p_muDeviceGetByPCIBusId(&musa_ctx->device_id, musa_ctx->device_bus_id);
		if (error != MUSA_SUCCESS) {
			fprintf(stderr, "muDeviceGetByPCIBusId failed with error: %d; Failed to get PCI Bus ID (%s)\n",
				error, musa_ctx->device_bus_id);
			return FAILURE;
		}
		printf("Picking GPU number %d\n", musa_ctx->device_id);
	}

	return_value = init_gpu(musa_ctx);
	if (return_value) {
		fprintf(stderr, "Couldn't init GPU context: %d\n", return_value);
		return FAILURE;
	}

#ifdef HAVE_MUSA_DMABUF
	if (musa_ctx->use_dmabuf) {
		int is_supported = 0;

		MUCHECK(p_muDeviceGetAttribute(&is_supported,
					       MU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED,
					       musa_ctx->mu_device));
		if (!is_supported) {
			fprintf(stderr, "DMA-BUF is not supported on this GPU\n");
			return FAILURE;
		}
	}
#endif

	return SUCCESS;
}

static int musa_memory_destroy(struct memory_ctx *ctx)
{
	struct musa_memory_ctx *musa_ctx = container_of(ctx, struct musa_memory_ctx, base);

	free_gpu(musa_ctx);
	free(musa_ctx);
	unload_musa_library();

	return SUCCESS;
}

static int musa_allocate_device_memory_buffer(struct musa_memory_ctx *musa_ctx, uint64_t size,
					      int *dmabuf_fd, uint64_t *dmabuf_offset,
					      void **addr, bool *can_init)
{
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);
	int integrated = 0;
	MUresult error;

	p_muDeviceGetAttribute(&integrated, MU_DEVICE_ATTRIBUTE_INTEGRATED, musa_ctx->mu_device);
	printf("MUSA device integrated: %X\n", (unsigned int)integrated);

	if (integrated == 1) {
		error = p_muMemAllocHost(addr, buf_size);
		if (error != MUSA_SUCCESS) {
			printf("muMemAllocHost error=%d\n", error);
			return FAILURE;
		}

		printf("allocated GPU buffer address at %p\n", *addr);
		*can_init = false;
		return SUCCESS;
	}

	{
		MUdeviceptr dptr;

		error = p_muMemAlloc(&dptr, buf_size);
		if (error != MUSA_SUCCESS) {
			printf("muMemAlloc error=%d\n", error);
			return FAILURE;
		}

		*addr = (void *)dptr;
		*can_init = false;

#ifdef HAVE_MUSA_DMABUF
		if (musa_ctx->use_dmabuf) {
			const size_t host_page_size = sysconf(_SC_PAGESIZE);
			MUdeviceptr aligned_ptr = dptr & ~(host_page_size - 1);
			uint64_t offset = dptr - aligned_ptr;
			size_t aligned_size = (size + offset + host_page_size - 1) & ~(host_page_size - 1);
			unsigned long long flags = 0;

			printf("using DMA-BUF for GPU buffer address at %#llx aligned at %#llx with aligned size %zu\n",
			       (unsigned long long)dptr, (unsigned long long)aligned_ptr, aligned_size);

			if (musa_ctx->use_pcie_mapping) {
#ifdef HAVE_MUSA_DMABUF_MAPPING_TYPE_PCIE
				flags = MU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE;
#else
				printf("support for MU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE is missing\n");
				return FAILURE;
#endif
			}

			error = p_muMemGetHandleForAddressRange((void *)dmabuf_fd, aligned_ptr,
								aligned_size,
								MU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,
								flags);
			if (error != MUSA_SUCCESS) {
				printf("muMemGetHandleForAddressRange error=%d\n", error);
				return FAILURE;
			}

			*dmabuf_offset = offset;
		}
#endif
	}

	return SUCCESS;
}

static int musa_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size,
				       int *dmabuf_fd, uint64_t *dmabuf_offset,
				       void **addr, bool *can_init)
{
	struct musa_memory_ctx *musa_ctx = container_of(ctx, struct musa_memory_ctx, base);
	int error;

	error = musa_allocate_device_memory_buffer(musa_ctx, size, dmabuf_fd,
						  dmabuf_offset, addr, can_init);
	if (error != SUCCESS)
		return FAILURE;

	printf("allocated MUSA buffer of %lu bytes at %p\n", size, *addr);
	return SUCCESS;
}

static int musa_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size)
{
	struct musa_memory_ctx *musa_ctx = container_of(ctx, struct musa_memory_ctx, base);
	int integrated = 0;

	p_muDeviceGetAttribute(&integrated, MU_DEVICE_ATTRIBUTE_INTEGRATED, musa_ctx->mu_device);

	if (integrated == 1) {
		printf("deallocating GPU buffer %p\n", addr);
		p_muMemFreeHost(addr);
	} else {
		MUdeviceptr dptr = (MUdeviceptr)addr;

		printf("deallocating GPU buffer %016llx\n", (unsigned long long)dptr);
		p_muMemFree(dptr);
	}

	return SUCCESS;
}

static void *musa_memory_copy_host_buffer(void *dest, const void *src, size_t size)
{
	p_muMemcpy((MUdeviceptr)dest, (MUdeviceptr)src, size);
	return dest;
}

static void *musa_memory_copy_buffer_to_buffer(void *dest, const void *src, size_t size)
{
	p_muMemcpyDtoD((MUdeviceptr)dest, (MUdeviceptr)src, size);
	return dest;
}

bool musa_memory_supported()
{
	return true;
}

bool musa_memory_dmabuf_supported()
{
#ifdef HAVE_MUSA_DMABUF
	return true;
#else
	return false;
#endif
}

struct memory_ctx *musa_memory_create(struct perftest_parameters *params)
{
	struct musa_memory_ctx *ctx;

	ALLOCATE(ctx, struct musa_memory_ctx, 1);
	memset(ctx, 0, sizeof(struct musa_memory_ctx));

	ctx->base.init = musa_memory_init;
	ctx->base.destroy = musa_memory_destroy;
	ctx->base.allocate_buffer = musa_memory_allocate_buffer;
	ctx->base.free_buffer = musa_memory_free_buffer;
	ctx->base.copy_host_to_buffer = musa_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_host = musa_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_buffer = musa_memory_copy_buffer_to_buffer;
	ctx->device_id = params->musa_device_id;
	ctx->device_bus_id = params->musa_device_bus_id;
	ctx->use_dmabuf = params->use_musa_dmabuf;
	ctx->use_pcie_mapping = params->use_musa_pcie_mapping;

	return &ctx->base;
}
