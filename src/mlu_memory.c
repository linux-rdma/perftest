/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <cn_api.h>
#include "mlu_memory.h"
#include "perftest_parameters.h"

static inline const char *getErrorName(CNresult error)
{
    const char *str;
    cnGetErrorName(error, &str);
    return str;
}

static inline const char *getErrorString(CNresult error)
{
    const char *str;
    cnGetErrorString(error, &str);
    return str;
}

#define ERROR_CHECK(ret)                                                                           \
	do {                                                                                           \
		CNresult r__ = (ret);                                                                      \
		if (r__ != CN_SUCCESS) {                                                                   \
			printf(                                                                                \
					"error occur, func: %s, line: %d, ret:%d, cn_error_code:%s, cn_error_string:%s\n", \
					__func__, __LINE__, r__, getErrorName(r__), getErrorString(r__));                  \
			exit(0);                                                                               \
		}                                                                                          \
	} while (0)

#define ACCEL_PAGE_SIZE (64 * 1024)


struct mlu_memory_ctx {
	struct memory_ctx base;
	int device_id;
	CNdev cnDevice;
	CNcontext cnContext;

};


static int init_mlu(struct mlu_memory_ctx *ctx) 
{
	int mlu_device_id = ctx->device_id;
	int mlu_pci_bus_id;
	int mlu_pci_device_id;
	int index;
	CNdev cn_device;

	printf("initializing MLU\n");
	CNresult error = cnInit(0);
	if (error != CN_SUCCESS) {
		printf("cnInit(0) returned %d\n", error);
		return FAILURE;
	}

    int deviceCount = 0;
    error = cnDeviceGetCount(&deviceCount);
	if (error != CN_SUCCESS) {
		printf("cnDeviceGetCount() returned %d\n", error);
		return FAILURE;
	}

    if (deviceCount == 0) {
		printf("There are no available device(s) that support MLU\n");
		return FAILURE;
    }
	if (mlu_device_id >= deviceCount) {
		fprintf(stderr, "No such device ID (%d) exists in system\n", mlu_device_id);
		return FAILURE;
	}

	printf("Listing all MLU devices in system:\n");
	for (index = 0; index < deviceCount; index++) {
		ERROR_CHECK(cnDeviceGet(&cn_device, index));
		cnDeviceGetAttribute(&mlu_pci_bus_id, CN_DEVICE_ATTRIBUTE_PCI_BUS_ID , cn_device);
		cnDeviceGetAttribute(&mlu_pci_device_id, CN_DEVICE_ATTRIBUTE_PCI_DEVICE_ID , cn_device);
		printf("MLU device %d: PCIe address is %02X:%02X\n", index, (unsigned int)mlu_pci_bus_id, (unsigned int)mlu_pci_device_id);
	}

	printf("\nPicking device No. %d\n", mlu_device_id);

    ERROR_CHECK(cnDeviceGet(&ctx->cnDevice, mlu_device_id));

	char name[128];
	ERROR_CHECK(cnDeviceGetName(name, sizeof(name), mlu_device_id));
	printf("[pid = %d, dev = %ld] device name = [%s]\n", getpid(), ctx->cnDevice, name);
	printf("creating MLU Ctx\n");

	/* Create context */
	error = cnCtxCreate(&ctx->cnContext, 0, ctx->cnDevice);
	if (error != CN_SUCCESS) {
		printf("cnCtxCreate() error=%d\n", error);
		return FAILURE;
	}

	printf("making it the current MLU Ctx\n");
	error = cnCtxSetCurrent(ctx->cnContext);
	if (error != CN_SUCCESS) {
		printf("cnCtxSetCurrent() error=%d\n", error);
		return FAILURE;
	}

	return SUCCESS;
}

static void free_mlu(struct mlu_memory_ctx *ctx)
{
	printf("destroying current MLU Ctx\n");
	ERROR_CHECK(cnCtxDestroy(ctx->cnContext));
}

int mlu_memory_init(struct memory_ctx *ctx) {
	struct mlu_memory_ctx *mlu_ctx = container_of(ctx, struct mlu_memory_ctx, base);
	int return_value = 0;

	return_value = init_mlu(mlu_ctx);

	if (return_value) {
		fprintf(stderr, "Couldn't initialize mlu device : %d\n", return_value);
		return FAILURE;
	}

	return SUCCESS;
}

int mlu_memory_destroy(struct memory_ctx *ctx) {
	struct mlu_memory_ctx *mlu_ctx = container_of(ctx, struct mlu_memory_ctx, base);

	free_mlu(mlu_ctx);
	free(mlu_ctx);
	return SUCCESS;
}

int mlu_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	CNresult error;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

	CNaddr mlu_addr;
	printf("cnMalloc() of a %lu bytes MLU buffer\n", size);

	error = cnMalloc(&mlu_addr, buf_size);
	if (error != CN_SUCCESS) {
		printf("cnMalloc error=%d\n", error);
		return FAILURE;
	}

	printf("allocated %lu bytes of MLU buffer at %ld\n", (unsigned long)buf_size, mlu_addr);
	*addr = (void *)mlu_addr;
	*can_init = false;
	return SUCCESS;
}

int mlu_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size)
{
	CNaddr mlu_addr = (CNaddr)addr;
	printf("deallocating MLU buffer %016lx\n", mlu_addr);
	cnFree(mlu_addr);
	return SUCCESS;
}

void *mlu_memory_copy_host_buffer(void *dest, const void *src, size_t size) {
	cnMemcpy((CNaddr) dest, (CNaddr) src, size);
	return dest;
}

void *mlu_memory_copy_buffer_to_buffer(void *dest, const void *src, size_t size) {
	cnMemcpyDtoD((CNaddr) dest, (CNaddr) src, size);
	return dest;
}

bool mlu_memory_supported() {
	return true;
}

struct memory_ctx *mlu_memory_create(struct perftest_parameters *params) {
	struct mlu_memory_ctx *ctx;

	ALLOCATE(ctx, struct mlu_memory_ctx, 1);
	ctx->base.init = mlu_memory_init;
	ctx->base.destroy = mlu_memory_destroy;
	ctx->base.allocate_buffer = mlu_memory_allocate_buffer;
	ctx->base.free_buffer = mlu_memory_free_buffer;
	ctx->base.copy_host_to_buffer = mlu_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_host = mlu_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_buffer = mlu_memory_copy_buffer_to_buffer;
	ctx->device_id = params->mlu_device_id;

	return &ctx->base;
}
