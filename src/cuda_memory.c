/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "cuda_memory.h"
#include "perftest_parameters.h"
#include CUDA_PATH

#define CUCHECK(stmt) \
	do { \
	CUresult result = (stmt); \
	ASSERT(CUDA_SUCCESS == result); \
} while (0)

#define ACCEL_PAGE_SIZE (64 * 1024)

static const char *cuda_mem_type_str[] = {
	"CUDA_MEM_DEVICE",
	"CUDA_MEM_MANAGED",
	"CUDA_MEM_HOSTALLOC",
	"CUDA_MEM_HOSTREGISTER",
	"CUDA_MEM_MALLOC",
	"CUDA_MEM_TYPES"
};

int touch_gpu_pages(uint8_t *addr, int buf_size, int is_infinitely, volatile int **stop_flag);
int init_gpu_stop_flag(volatile int **stop_flag);

struct cuda_memory_ctx {
	struct memory_ctx base;
	int mem_type;
	int gpu_touch;
	int device_id;
	char *device_bus_id;
	volatile int *stop_touch_gpu_kernel_flag; // used for stopping cuda gpu_touch kernel
	CUdevice cuDevice;
	CUcontext cuContext;
	bool use_dmabuf;
	bool use_data_direct;
	int driver_version;
};


static int init_gpu(struct cuda_memory_ctx *ctx)
{
	int cuda_device_id = ctx->device_id;
	int cuda_pci_bus_id;
	int cuda_pci_device_id;
	int index;
	CUdevice cu_device;

	printf("initializing CUDA\n");
	CUresult error = cuInit(0);
	if (error != CUDA_SUCCESS) {
		printf("cuInit(0) returned %d\n", error);
		return FAILURE;
	}

	int deviceCount = 0;
	error = cuDeviceGetCount(&deviceCount);
	if (error != CUDA_SUCCESS) {
		printf("cuDeviceGetCount() returned %d\n", error);
		return FAILURE;
	}
	/* This function call returns 0 if there are no CUDA capable devices. */
	if (deviceCount == 0) {
		printf("There are no available device(s) that support CUDA\n");
		return FAILURE;
	}
	if (cuda_device_id >= deviceCount) {
		fprintf(stderr, "No such device ID (%d) exists in system\n", cuda_device_id);
		return FAILURE;
	}

	printf("Listing all CUDA devices in system:\n");
	for (index = 0; index < deviceCount; index++) {
		CUCHECK(cuDeviceGet(&cu_device, index));
		cuDeviceGetAttribute(&cuda_pci_bus_id, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID , cu_device);
		cuDeviceGetAttribute(&cuda_pci_device_id, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID , cu_device);
		printf("CUDA device %d: PCIe address is %02X:%02X\n", index, (unsigned int)cuda_pci_bus_id, (unsigned int)cuda_pci_device_id);
	}

	printf("\nPicking device No. %d\n", cuda_device_id);

	CUCHECK(cuDeviceGet(&ctx->cuDevice, cuda_device_id));

	char name[128];
	CUCHECK(cuDeviceGetName(name, sizeof(name), cuda_device_id));
	printf("[pid = %d, dev = %d] device name = [%s]\n", getpid(), ctx->cuDevice, name);
	printf("creating CUDA Ctx\n");

	/* Create context */
	error = cuCtxCreate(&ctx->cuContext, CU_CTX_MAP_HOST, ctx->cuDevice);
	if (error != CUDA_SUCCESS) {
		printf("cuCtxCreate() error=%d\n", error);
		return FAILURE;
	}

	printf("making it the current CUDA Ctx\n");
	error = cuCtxSetCurrent(ctx->cuContext);
	if (error != CUDA_SUCCESS) {
		printf("cuCtxSetCurrent() error=%d\n", error);
		return FAILURE;
	}

	error = init_gpu_stop_flag(&ctx->stop_touch_gpu_kernel_flag);
	if (error != 0) {
		printf("init_gpu_stop_flag() error=%d\n", error);
		return FAILURE;
	}

	CUCHECK(cuDriverGetVersion(&ctx->driver_version));

	return SUCCESS;
}

static void free_gpu(struct cuda_memory_ctx *ctx)
{
	printf("destroying current CUDA Ctx\n");
	CUCHECK(cuCtxDestroy(ctx->cuContext));
}

int cuda_memory_init(struct memory_ctx *ctx) {
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);
	int return_value = 0;

	if (cuda_ctx->device_bus_id) {
		int err;

		printf("initializing CUDA\n");
		CUresult error = cuInit(0);
		if (error != CUDA_SUCCESS) {
			printf("cuInit(0) returned %d\n", error);
			return FAILURE;
		}

		printf("Finding PCIe BUS %s\n", cuda_ctx->device_bus_id);
		err = cuDeviceGetByPCIBusId(&cuda_ctx->device_id, cuda_ctx->device_bus_id);
		if (err != 0) {
			fprintf(stderr, "cuDeviceGetByPCIBusId failed with error: %d; Failed to get PCI Bus ID (%s)\n", err, cuda_ctx->device_bus_id);
			return FAILURE;
		}
		printf("Picking GPU number %d\n", cuda_ctx->device_id);
	}

	return_value = init_gpu(cuda_ctx);
	if (return_value) {
		fprintf(stderr, "Couldn't init GPU context: %d\n", return_value);
		return FAILURE;
	}

#ifdef HAVE_CUDA_DMABUF
	if (cuda_ctx->use_dmabuf) {
		int is_supported = 0;

		CUCHECK(cuDeviceGetAttribute(&is_supported, CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, cuda_ctx->cuDevice));
		if (!is_supported) {
			fprintf(stderr, "DMA-BUF is not supported on this GPU\n");
			return FAILURE;
		}
	}
#endif

	return SUCCESS;
}

int cuda_memory_destroy(struct memory_ctx *ctx) {
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

	free_gpu(cuda_ctx);
	free(cuda_ctx);
	return SUCCESS;
}

static int cuda_allocate_device_memory_buffer(struct cuda_memory_ctx *cuda_ctx, uint64_t size, int *dmabuf_fd,
		uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	int error;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

	// Check if discrete or integrated GPU (tegra), for allocating memory where adequate
	int cuda_device_integrated;
	cuDeviceGetAttribute(&cuda_device_integrated, CU_DEVICE_ATTRIBUTE_INTEGRATED, cuda_ctx->cuDevice);
	printf("CUDA device integrated: %X\n", (unsigned int)cuda_device_integrated);

	if (cuda_device_integrated == 1) {
		error = cuMemAllocHost(addr, buf_size);
		if (error != CUDA_SUCCESS) {
			printf("cuMemAllocHost error=%d\n", error);
			return FAILURE;
		}

		printf("allocated GPU buffer address at %p\n", addr);
		*can_init = false;
	} else {
		CUdeviceptr d_A;
		error = cuMemAlloc(&d_A, buf_size);
		if (error != CUDA_SUCCESS) {
			printf("cuMemAlloc error=%d\n", error);
			return FAILURE;
		}

		*addr = (void *)d_A;
		*can_init = false;

#ifdef HAVE_CUDA_DMABUF
		{
			if (cuda_ctx->use_dmabuf) {
				CUdeviceptr aligned_ptr;
				const size_t host_page_size = sysconf(_SC_PAGESIZE);
				uint64_t offset;
				size_t aligned_size;
				int cu_flags = 0;

				// Round down to host page size
				aligned_ptr = d_A & ~(host_page_size - 1);
				offset = d_A - aligned_ptr;
				aligned_size = (size + offset + host_page_size - 1) & ~(host_page_size - 1);

				printf("using DMA-BUF for GPU buffer address at %#llx aligned at %#llx with aligned size %zu\n", d_A, aligned_ptr, aligned_size);
				*dmabuf_fd = 0;
				CUmemRangeHandleType cuda_handle_type = CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD;

				if (cuda_ctx->use_data_direct) {
				#ifdef HAVE_DMABUF_MAPPING_TYPE_PCIE
				    cu_flags = CU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE;
					if (cuda_ctx->driver_version < 12*1000+8*10) {
						printf("CUDA driver version %d.%d does not support CU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE\n",
							  (cuda_ctx->driver_version / 1000), (cuda_ctx->driver_version % 1000) / 10);
						return FAILURE;
					}
				#else
					// this may happen with binaries built with a CUDA toolkit older than 12.8
					printf("support for CU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE is missing\n");
					return FAILURE;
				#endif
				}

				error = cuMemGetHandleForAddressRange((void *)dmabuf_fd, aligned_ptr, aligned_size, cuda_handle_type, cu_flags);
				if (error != CUDA_SUCCESS) {
					printf("cuMemGetHandleForAddressRange error=%d\n", error);
					return FAILURE;
				}

				*dmabuf_offset = offset;
			}
		}
#endif
	}

	return CUDA_SUCCESS;
}

int cuda_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	int error;
	CUdeviceptr d_ptr;

	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

	switch (cuda_ctx->mem_type) {
		case CUDA_MEM_DEVICE:
			error = cuda_allocate_device_memory_buffer(cuda_ctx, size, dmabuf_fd,
					dmabuf_offset, addr, can_init);
			if (error != CUDA_SUCCESS)
				return FAILURE;
			break;
		case CUDA_MEM_MANAGED:
			error = cuMemAllocManaged(&d_ptr, size, CU_MEM_ATTACH_GLOBAL);
			if (error != CUDA_SUCCESS) {
				printf("cuMemAllocManaged error=%d\n", error);
				return FAILURE;
			}

			*addr = (void *)d_ptr;
			*can_init = false;
			break;

		case CUDA_MEM_MALLOC:
			*can_init = false;
			// Fall through

			printf("Host allocation selected, calling memalign allocator for %ld bytes with %d page size\n", size, alignment);
			*addr = memalign(alignment, size);
			if (!*addr) {
				printf("memalign error=%d\n", errno);
				return FAILURE;
			}

			break;
		/*
		 * TODO: Add Implementation for HOSTALLOC and HOSTREGISTER
		 * buffer allocations
		 */
		case CUDA_MEM_HOSTALLOC:
		case CUDA_MEM_HOSTREGISTER:
		default:
			printf("invalid CUDA memory type\n");
			return FAILURE;
	}

	printf("allocated GPU buffer of a %lu address at %p for type %s\n", size, addr, cuda_mem_type_str[cuda_ctx->mem_type]);

	if (cuda_ctx->gpu_touch != GPU_NO_TOUCH) {
		printf("Starting GPU touching process\n");
		return touch_gpu_pages((uint8_t *)*addr, size, cuda_ctx->gpu_touch == GPU_TOUCH_INFINITE, &cuda_ctx->stop_touch_gpu_kernel_flag);
	}

	return SUCCESS;
}

int cuda_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);
	int cuda_device_integrated;
	cuDeviceGetAttribute(&cuda_device_integrated, CU_DEVICE_ATTRIBUTE_INTEGRATED, cuda_ctx->cuDevice);

	if (cuda_ctx->stop_touch_gpu_kernel_flag) {
		*cuda_ctx->stop_touch_gpu_kernel_flag = 1;
		printf("stopping CUDA gpu touch running kernel\n");
		cuCtxSynchronize();
		cuMemFree((CUdeviceptr)cuda_ctx->stop_touch_gpu_kernel_flag);
	}

	switch (cuda_ctx->mem_type) {
		case CUDA_MEM_DEVICE:
			if (cuda_device_integrated == 1) {
				printf("deallocating GPU buffer %p\n", addr);
				cuMemFreeHost(addr);
			} else {
				CUdeviceptr d_A = (CUdeviceptr)addr;
				printf("deallocating GPU buffer %016llx\n", d_A);
				cuMemFree(d_A);
			}
			break;
		case CUDA_MEM_MANAGED:
			CUCHECK(cuMemFree((CUdeviceptr)addr));
			break;
		case CUDA_MEM_MALLOC:
			free((void *) addr);
			break;
	}

	return SUCCESS;
}

void *cuda_memory_copy_host_buffer(void *dest, const void *src, size_t size) {
	cuMemcpy((CUdeviceptr)dest, (CUdeviceptr)src, size);
	return dest;
}

void *cuda_memory_copy_buffer_to_buffer(void *dest, const void *src, size_t size) {
	cuMemcpyDtoD((CUdeviceptr)dest, (CUdeviceptr)src, size);
	return dest;
}

bool cuda_memory_supported() {
	return true;
}

bool cuda_memory_dmabuf_supported() {
#ifdef HAVE_CUDA_DMABUF
	return true;
#else
	return false;
#endif
}


bool data_direct_supported() {
#ifdef HAVE_DATA_DIRECT
	return true;
#else
	return false;
#endif
}

struct memory_ctx *cuda_memory_create(struct perftest_parameters *params) {
	struct cuda_memory_ctx *ctx;

	ALLOCATE(ctx, struct cuda_memory_ctx, 1);
	ctx->base.init = cuda_memory_init;
	ctx->base.destroy = cuda_memory_destroy;
	ctx->base.allocate_buffer = cuda_memory_allocate_buffer;
	ctx->base.free_buffer = cuda_memory_free_buffer;
	ctx->base.copy_host_to_buffer = cuda_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_host = cuda_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_buffer = cuda_memory_copy_buffer_to_buffer;
	ctx->device_id = params->cuda_device_id;
	ctx->device_bus_id = params->cuda_device_bus_id;
	ctx->use_dmabuf = params->use_cuda_dmabuf;
	ctx->use_data_direct = params->use_data_direct;
	ctx->gpu_touch = params->gpu_touch;
	ctx->stop_touch_gpu_kernel_flag = NULL;
	ctx->mem_type = params->cuda_mem_type;

	return &ctx->base;
}
