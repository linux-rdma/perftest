/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "cuda_memory.h"
#include "perftest_parameters.h"
#include "cuda_loader.h"
#include "validation_common.h"

static int kernel_plugin_initialized = 0;
static void cuda_validation_destroy(struct memory_ctx *ctx);

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
	"CUDA_MEM_VMM",
	"CUDA_MEM_TYPES"
};

#define CUDA_ROUND_UP(x, n) (((x) + (n) - 1) & ~((n) - 1))

struct cuda_vmm_buffer;

struct cuda_memory_ctx {
	struct memory_ctx base;
	int mem_type;
	int gpu_touch;
	int device_id;
	char *device_bus_id;
	volatile int *stop_touch_gpu_kernel_flag;
	CUdevice cuDevice;
	CUcontext cuContext;
	bool use_dmabuf;
	bool use_pcie_mapping;
	int driver_version;
	int validation_active;
	int effective_mem_type;
	struct cuda_vmm_buffer *vmm_list_head;
};

#if CUDA_VERSION >= 11000
/*
 * The free path only gets the buffer address, but releasing a VMM allocation
 * also needs its handle and padded size, so track them in a small addr-keyed
 * list owned by the memory context.
 */
struct cuda_vmm_buffer {
	CUmemGenericAllocationHandle handle;
	void *addr;
	size_t padded_size;
	struct cuda_vmm_buffer *next;
};

static void cuda_vmm_buffer_push(struct cuda_memory_ctx *cuda_ctx, struct cuda_vmm_buffer *obj)
{
	obj->next = cuda_ctx->vmm_list_head;
	cuda_ctx->vmm_list_head = obj;
}

static struct cuda_vmm_buffer *cuda_vmm_buffer_pop_by_addr(struct cuda_memory_ctx *cuda_ctx, void *addr)
{
	struct cuda_vmm_buffer *prev = NULL;
	struct cuda_vmm_buffer *cur = cuda_ctx->vmm_list_head;

	while (cur != NULL) {
		if (cur->addr == addr) {
			if (prev)
				prev->next = cur->next;
			else
				cuda_ctx->vmm_list_head = cur->next;
			cur->next = NULL;
			return cur;
		}
		prev = cur;
		cur = cur->next;
	}
	return NULL;
}
#endif /* CUDA_VERSION >= 11000 */

static int init_gpu(struct cuda_memory_ctx *ctx)
{
	int cuda_device_id = ctx->device_id;
	int cuda_pci_bus_id;
	int cuda_pci_device_id;
	int index;
	CUdevice cu_device;

	printf("initializing CUDA\n");
	CUresult error = p_cuInit(0);
	if (error != CUDA_SUCCESS) {
		printf("cuInit(0) returned %d\n", error);
		return FAILURE;
	}

	int deviceCount = 0;
	error = p_cuDeviceGetCount(&deviceCount);
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
		CUCHECK(p_cuDeviceGet(&cu_device, index));
		p_cuDeviceGetAttribute(&cuda_pci_bus_id, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID , cu_device);
		p_cuDeviceGetAttribute(&cuda_pci_device_id, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID , cu_device);
		printf("CUDA device %d: PCIe address is %02X:%02X\n", index, (unsigned int)cuda_pci_bus_id, (unsigned int)cuda_pci_device_id);
	}

	printf("\nPicking device No. %d\n", cuda_device_id);

	CUCHECK(p_cuDeviceGet(&ctx->cuDevice, cuda_device_id));

	char name[128];
	CUCHECK(p_cuDeviceGetName(name, sizeof(name), cuda_device_id));
	printf("[pid = %d, dev = %d] device name = [%s]\n", getpid(), ctx->cuDevice, name);
	printf("creating CUDA Ctx\n");

	/* Create context */
	error = p_cuCtxCreate_v2(&ctx->cuContext, CU_CTX_MAP_HOST, ctx->cuDevice);

	if (error != CUDA_SUCCESS) {
		printf("cuCtxCreate_v2() error=%d\n", error);
		return FAILURE;
	}

	printf("making it the current CUDA Ctx\n");
	error = p_cuCtxSetCurrent(ctx->cuContext);
	if (error != CUDA_SUCCESS) {
		printf("cuCtxSetCurrent() error=%d\n", error);
		return FAILURE;
	}

	#ifdef HAVE_CUDART
	if (ctx->gpu_touch != GPU_NO_TOUCH) {
		if (load_kernel_plugin() != 0) {
			printf("Failed to load kernel plugin for GPU touch\n");
			return FAILURE;
		}
		if (!p_init_gpu_stop_flag) {
			printf("GPU touch not available in kernel plugin\n");
			return FAILURE;
		}
		error = p_init_gpu_stop_flag(&ctx->stop_touch_gpu_kernel_flag);
		if (error != 0) {
			printf("init_gpu_stop_flag() error=%d\n", error);
			return FAILURE;
		}
	}
	#endif

	CUCHECK(p_cuDriverGetVersion(&ctx->driver_version));

	return SUCCESS;
}

static void free_gpu(struct cuda_memory_ctx *ctx)
{
	printf("destroying current CUDA Ctx\n");
	CUCHECK(p_cuCtxDestroy(ctx->cuContext));
}

int cuda_memory_init(struct memory_ctx *ctx) {
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);
	int return_value = 0;

	if (load_cuda_library() != 0) {
        printf("Failed to load CUDA library dynamically\n");
        exit(1);
    }

	if (cuda_ctx->device_bus_id) {
		int err;

		printf("initializing CUDA\n");
		CUresult error = p_cuInit(0);
		if (error != CUDA_SUCCESS) {
			printf("cuInit(0) returned %d\n", error);
			return FAILURE;
		}

		printf("Finding PCIe BUS %s\n", cuda_ctx->device_bus_id);
		err = p_cuDeviceGetByPCIBusId(&cuda_ctx->device_id, cuda_ctx->device_bus_id);
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

		CUCHECK(p_cuDeviceGetAttribute(&is_supported, CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, cuda_ctx->cuDevice));
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

	if (cuda_ctx->validation_active) {
		cuda_validation_destroy(ctx);
	}

	if (cuda_ctx->cuContext) {
		free_gpu(cuda_ctx);
	}

	if (cuda_ctx) {
		free(cuda_ctx);
	}

	unload_kernel_plugin();
	unload_cudart_library();
	unload_cuda_library();
	kernel_plugin_initialized = 0;

	return SUCCESS;
}

#ifdef HAVE_CUDA_DMABUF
/*
 * Export a GPU buffer range as a DMA-BUF fd (shared by the device and VMM
 * paths). On failure returns FAILURE without releasing the buffer
 */
static int cuda_export_dmabuf(struct cuda_memory_ctx *cuda_ctx, CUdeviceptr ptr,
			      uint64_t size, int *dmabuf_fd, uint64_t *dmabuf_offset)
{
	const size_t host_page_size = sysconf(_SC_PAGESIZE);
	CUdeviceptr aligned_ptr = ptr & ~(host_page_size - 1);
	uint64_t offset = ptr - aligned_ptr;
	size_t aligned_size = (size + offset + host_page_size - 1) & ~(host_page_size - 1);
	int cu_flags = 0;
	CUresult error;

	*dmabuf_fd = 0;
	if (cuda_ctx->use_pcie_mapping) {
	#ifdef HAVE_DMABUF_MAPPING_TYPE_PCIE
		cu_flags = CU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE;
		if (cuda_ctx->driver_version < 12*1000+8*10) {
			printf("CUDA driver version %d.%d does not support CU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE\n",
				(cuda_ctx->driver_version / 1000), (cuda_ctx->driver_version % 1000) / 10);
			return FAILURE;
		}
	#else
		/* may happen with CUDA toolkit older than 12.8 */
		printf("support for CU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE is missing\n");
		return FAILURE;
	#endif
	}

	printf("using DMA-BUF for GPU buffer address at %#llx aligned at %#llx with aligned size %zu\n",
		ptr, aligned_ptr, aligned_size);
	error = p_cuMemGetHandleForAddressRange((void *)dmabuf_fd, (void *)aligned_ptr, aligned_size,
		CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, cu_flags);
	if (error != CUDA_SUCCESS) {
		printf("cuMemGetHandleForAddressRange error=%d\n", error);
		return FAILURE;
	}

	*dmabuf_offset = offset;
	return SUCCESS;
}
#endif /* HAVE_CUDA_DMABUF */

static int cuda_allocate_device_memory_buffer(struct cuda_memory_ctx *cuda_ctx, uint64_t size, int *dmabuf_fd,
		uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	int error;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

	/* Check if discrete or integrated GPU (tegra) */
	int cuda_device_integrated;
	p_cuDeviceGetAttribute(&cuda_device_integrated, CU_DEVICE_ATTRIBUTE_INTEGRATED, cuda_ctx->cuDevice);
	printf("CUDA device integrated: %X\n", (unsigned int)cuda_device_integrated);

	if (cuda_device_integrated == 1) {
		error = p_cuMemAllocHost(addr, buf_size);
		if (error != CUDA_SUCCESS) {
			printf("cuMemAllocHost error=%d\n", error);
			return FAILURE;
		}

		printf("allocated GPU buffer address at %p\n", addr);
		*can_init = false;
	} else {
		CUdeviceptr d_A;
		error = p_cuMemAlloc(&d_A, buf_size);
		if (error != CUDA_SUCCESS) {
			printf("cuMemAlloc error=%d\n", error);
			return FAILURE;
		}

		*addr = (void *)d_A;
		*can_init = false;

#ifdef HAVE_CUDA_DMABUF
		if (cuda_ctx->use_dmabuf) {
			if (cuda_export_dmabuf(cuda_ctx, d_A, size, dmabuf_fd, dmabuf_offset) != SUCCESS) {
				p_cuMemFree(d_A);
				*addr = NULL;
				return FAILURE;
			}
		}
#endif
	}

	return CUDA_SUCCESS;
}

#if CUDA_VERSION >= 11000
/*
 * Non-localized, GPUDirect-RDMA-capable VMM allocation: CU_MEM_LOCATION_TYPE_DEVICE
 * with allocFlags.gpuDirectRDMACapable = 1. Must stay non-localized (no locality
 * domain / memory node / localized mempool) so the buffer is RDMA-exportable. If
 * the platform can't honor it, cuMemCreate fails and the caller falls back.
 */
static int cuda_vmm_alloc(struct cuda_memory_ctx *cuda_ctx, uint64_t size,
			  CUdeviceptr *d_ptr, CUmemGenericAllocationHandle *handle_out,
			  size_t *padded_size_out)
{
	CUmemAllocationProp prop = {0};
	CUmemAccessDesc access_desc = {0};
	CUmemGenericAllocationHandle handle;
	CUdeviceptr ptr;
	size_t granularity = 0;
	size_t padded_size;
	CUresult error;

	prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
	prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
	prop.location.id = cuda_ctx->cuDevice;
	prop.allocFlags.gpuDirectRDMACapable = 1;

	error = p_cuMemGetAllocationGranularity(&granularity, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM);
	if (error != CUDA_SUCCESS) {
		printf("cuMemGetAllocationGranularity error=%d\n", error);
		return FAILURE;
	}
	padded_size = CUDA_ROUND_UP(size, granularity);

	error = p_cuMemCreate(&handle, padded_size, &prop, 0);
	if (error != CUDA_SUCCESS) {
		printf("cuMemCreate error=%d\n", error);
		return FAILURE;
	}

	error = p_cuMemAddressReserve(&ptr, padded_size, 0, 0, 0);
	if (error != CUDA_SUCCESS) {
		printf("cuMemAddressReserve error=%d\n", error);
		p_cuMemRelease(handle);
		return FAILURE;
	}

	error = p_cuMemMap(ptr, padded_size, 0, handle, 0);
	if (error != CUDA_SUCCESS) {
		printf("cuMemMap error=%d\n", error);
		p_cuMemAddressFree(ptr, padded_size);
		p_cuMemRelease(handle);
		return FAILURE;
	}

	access_desc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
	access_desc.location.id = cuda_ctx->cuDevice;
	access_desc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
	error = p_cuMemSetAccess(ptr, padded_size, &access_desc, 1);
	if (error != CUDA_SUCCESS) {
		printf("cuMemSetAccess error=%d\n", error);
		p_cuMemUnmap(ptr, padded_size);
		p_cuMemAddressFree(ptr, padded_size);
		p_cuMemRelease(handle);
		return FAILURE;
	}

	*d_ptr = ptr;
	*handle_out = handle;
	*padded_size_out = padded_size;
	return SUCCESS;
}

static int cuda_vmm_free(CUmemGenericAllocationHandle handle, CUdeviceptr d_ptr, size_t padded_size)
{
	p_cuMemUnmap(d_ptr, padded_size);
	p_cuMemRelease(handle);
	p_cuMemAddressFree(d_ptr, padded_size);
	return SUCCESS;
}
#endif /* CUDA_VERSION >= 11000 */

/* Whether the non-localized GDR VMM path can be used on this device/build. */
static bool vmm_runtime_available(struct cuda_memory_ctx *cuda_ctx)
{
#if CUDA_VERSION >= 11000
	int integrated = 0;
	int gdr_vmm = 0;

	p_cuDeviceGetAttribute(&integrated, CU_DEVICE_ATTRIBUTE_INTEGRATED, cuda_ctx->cuDevice);
	if (integrated)
		return false;

	p_cuDeviceGetAttribute(&gdr_vmm, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WITH_CUDA_VMM_SUPPORTED, cuda_ctx->cuDevice);
	return gdr_vmm > 0;
#else
	(void)cuda_ctx;
	return false;
#endif
}

/*
 * Resolve the effective allocation type. Explicit --cuda_mem_type is honored
 * exactly (no fallback). The internal AUTO default prefers the non-localized
 * VMM path and falls back to the legacy device path when VMM is unavailable.
 */
static int resolve_mem_type(struct cuda_memory_ctx *cuda_ctx)
{
	if (cuda_ctx->mem_type != CUDA_MEM_AUTO)
		return cuda_ctx->mem_type;

	return vmm_runtime_available(cuda_ctx) ? CUDA_MEM_VMM : CUDA_MEM_DEVICE;
}

int cuda_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	int error;
	int mem_type;
	CUdeviceptr d_ptr;

	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

	/* Resolve the effective allocation type once and cache it. */
	if (cuda_ctx->effective_mem_type < 0)
		cuda_ctx->effective_mem_type = resolve_mem_type(cuda_ctx);
	mem_type = cuda_ctx->effective_mem_type;

	switch (mem_type) {
		case CUDA_MEM_DEVICE:
			error = cuda_allocate_device_memory_buffer(cuda_ctx, size, dmabuf_fd,
					dmabuf_offset, addr, can_init);
			if (error != CUDA_SUCCESS)
				return FAILURE;
			break;
#if CUDA_VERSION >= 11000
		case CUDA_MEM_VMM: {
			CUmemGenericAllocationHandle handle;
			size_t padded_size = 0;
			struct cuda_vmm_buffer *vmm_buf;

			if (cuda_vmm_alloc(cuda_ctx, size, &d_ptr, &handle, &padded_size) != SUCCESS) {
				/* Explicit VMM request is honored exactly: no fallback. */
				if (cuda_ctx->mem_type != CUDA_MEM_AUTO) {
					fprintf(stderr, "Failed to allocate CUDA VMM buffer\n");
					return FAILURE;
				}
				/* AUTO default: fall back to the legacy cuMemAlloc device path. */
				printf("CUDA VMM allocation unavailable, falling back to cuMemAlloc device path\n");
				cuda_ctx->effective_mem_type = CUDA_MEM_DEVICE;
				mem_type = CUDA_MEM_DEVICE;
				error = cuda_allocate_device_memory_buffer(cuda_ctx, size, dmabuf_fd,
						dmabuf_offset, addr, can_init);
				if (error != CUDA_SUCCESS)
					return FAILURE;
				break;
			}

			*addr = (void *)d_ptr;
			*can_init = false;

#ifdef HAVE_CUDA_DMABUF
			/* Export DMA-BUF before creating the tracking node, so any failure
			 * here only needs to release the VMM allocation (nothing tracked). */
			if (cuda_ctx->use_dmabuf) {
				if (cuda_export_dmabuf(cuda_ctx, d_ptr, size, dmabuf_fd, dmabuf_offset) != SUCCESS) {
					cuda_vmm_free(handle, d_ptr, padded_size);
					*addr = NULL;
					return FAILURE;
				}
			}
#endif

			/* Track the allocation last, once it is fully handed off. */
			vmm_buf = calloc(1, sizeof(*vmm_buf));
			if (!vmm_buf) {
				printf("calloc for cuda_vmm_buffer failed\n");
				cuda_vmm_free(handle, d_ptr, padded_size);
				*addr = NULL;
				return FAILURE;
			}
			vmm_buf->handle = handle;
			vmm_buf->addr = (void *)d_ptr;
			vmm_buf->padded_size = padded_size;
			cuda_vmm_buffer_push(cuda_ctx, vmm_buf);
			break;
		}
#endif /* CUDA_VERSION >= 11000 */
		case CUDA_MEM_MANAGED:
			error = p_cuMemAllocManaged(&d_ptr, size, CU_MEM_ATTACH_GLOBAL);
			if (error != CUDA_SUCCESS) {
				printf("cuMemAllocManaged error=%d\n", error);
				return FAILURE;
			}

			*addr = (void *)d_ptr;
			*can_init = false;
			break;

		case CUDA_MEM_MALLOC:
			*can_init = false;
			/* Fall through */

			printf("Host allocation selected, calling memalign allocator for %lu bytes with %d page size\n", size, alignment);
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

	printf("allocated GPU buffer of a %lu address at %p for type %s\n", size, addr, cuda_mem_type_str[mem_type]);

	#ifdef HAVE_CUDART
	if (cuda_ctx->gpu_touch != GPU_NO_TOUCH) {
		if (!p_touch_gpu_pages) {
			printf("GPU touch not available in kernel plugin\n");
			return FAILURE;
		}
		printf("Starting GPU touching process\n");
		return p_touch_gpu_pages((uint8_t *)*addr, size,
		                         cuda_ctx->gpu_touch == GPU_TOUCH_INFINITE,
		                         &cuda_ctx->stop_touch_gpu_kernel_flag);
	}
	#endif

	return SUCCESS;
}

int cuda_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);
	int cuda_device_integrated;
	p_cuDeviceGetAttribute(&cuda_device_integrated, CU_DEVICE_ATTRIBUTE_INTEGRATED, cuda_ctx->cuDevice);

	if (cuda_ctx->stop_touch_gpu_kernel_flag) {
		*cuda_ctx->stop_touch_gpu_kernel_flag = 1;
		printf("stopping CUDA gpu touch running kernel\n");
		p_cuCtxSynchronize();
		p_cuMemFree((CUdeviceptr)cuda_ctx->stop_touch_gpu_kernel_flag);
		cuda_ctx->stop_touch_gpu_kernel_flag = NULL;
	}

	/* Dispatch on the resolved type so the AUTO->device fallback frees correctly. */
	int mem_type = (cuda_ctx->effective_mem_type >= 0) ? cuda_ctx->effective_mem_type : cuda_ctx->mem_type;

	switch (mem_type) {
		case CUDA_MEM_DEVICE:
			if (cuda_device_integrated == 1) {
				printf("deallocating GPU buffer %p\n", addr);
				p_cuMemFreeHost(addr);
			} else {
				CUdeviceptr d_A = (CUdeviceptr)addr;
				printf("deallocating GPU buffer %016llx\n", d_A);
				p_cuMemFree(d_A);
			}
			break;
		case CUDA_MEM_MANAGED:
			CUCHECK(p_cuMemFree((CUdeviceptr)addr));
			break;
		case CUDA_MEM_MALLOC:
			free((void *) addr);
			break;
#if CUDA_VERSION >= 11000
		case CUDA_MEM_VMM: {
			struct cuda_vmm_buffer *vmm_buf = cuda_vmm_buffer_pop_by_addr(cuda_ctx, addr);
			if (!vmm_buf) {
				fprintf(stderr, "VMM buffer %p not tracked\n", addr);
				return FAILURE;
			}
			cuda_vmm_free(vmm_buf->handle, (CUdeviceptr)vmm_buf->addr, vmm_buf->padded_size);
			free(vmm_buf);
			break;
		}
#endif
	}

	return SUCCESS;
}

void *cuda_memory_copy_host_buffer(void *dest, const void *src, size_t size) {
	p_cuMemcpy((CUdeviceptr)dest, (CUdeviceptr)src, size);
	return dest;
}

void *cuda_memory_copy_buffer_to_buffer(void *dest, const void *src, size_t size) {
	p_cuMemcpyDtoD((CUdeviceptr)dest, (CUdeviceptr)src, size);
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

bool cuda_gpu_touch_supported() {
#ifdef HAVE_CUDART
	return true;
#else
	return false;
#endif
}


static int ensure_kernel_plugin_loaded(void)
{
	if (kernel_plugin_initialized)
		return 0;

	if (load_cudart_library() != 0) {
		fprintf(stderr, "Failed to load CUDA runtime library\n");
		return -1;
	}

	if (load_kernel_plugin() != 0) {
		fprintf(stderr, "Failed to load validation kernel plugin (libperftest_kernels.so)\n");
		fprintf(stderr, "Data validation requires the kernel plugin to be installed.\n");
		return -1;
	}

	kernel_plugin_initialized = 1;
	return 0;
}

static int cuda_validation_init(struct memory_ctx *ctx,
				const struct validation_config *cfg)
{
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

	if (ensure_kernel_plugin_loaded() != 0)
		return -1;

	if (!p_validation_init) {
		fprintf(stderr, "Validation plugin not properly loaded\n");
		return -1;
	}

	int ret = p_validation_init(cfg->buffer_base,
	                                   cfg->markers_offset,
	                                   cfg->recv_slots_offset,
	                                   cfg->payload_size,
	                                   cfg->ops_per_chunk,
	                                   cfg->num_qps,
	                                   cfg->chunks_per_qp,
	                                   cfg->validation_mode,
	                                   cuda_ctx->device_id,
	                                   cfg->debug_enabled);
	if (ret != 0) {
		fprintf(stderr, "Failed to initialize validation context via plugin\n");
		return -1;
	}

	cuda_ctx->validation_active = 1;
	return 0;
}

static int cuda_validation_start(struct memory_ctx *ctx)
{
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

	if (!cuda_ctx->validation_active || !p_validation_start)
		return -1;

	int ret = p_validation_start(NULL, 0, 0);
	if (ret != 0) {
		fprintf(stderr, "Failed to launch validation kernel via plugin\n");
		return -1;
	}
	return 0;
}

static int cuda_validation_stop(struct memory_ctx *ctx,
                                struct data_validation_result *result)
{
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

	if (!cuda_ctx->validation_active || !result)
		return -1;

	if (!p_validation_stop || !p_validation_get_stats || !p_validation_get_error) {
		fprintf(stderr, "Validation plugin functions not available\n");
		return -1;
	}

	p_validation_stop();

	uint64_t chunks_validated = 0, bytes_validated = 0, errors_found = 0;
	uint64_t markers_scanned = 0, markers_hit = 0, skipped_steps = 0;
	uint64_t race_overwrites = 0, dma_stale_retries = 0, queue_full_drops = 0;
	uint64_t stale_work_skips = 0;
	p_validation_get_stats(&chunks_validated, &bytes_validated, &errors_found,
			       &markers_scanned, &markers_hit, &skipped_steps,
			       &race_overwrites, &dma_stale_retries,
			       &queue_full_drops, &stale_work_skips);

	result->chunks_validated = chunks_validated;
	result->bytes_validated = bytes_validated;
	result->errors_found = errors_found;
	result->passed = (errors_found == 0) ? 1 : 0;

	result->markers_scanned = markers_scanned;
	result->markers_hit = markers_hit;
	result->skipped_steps = skipped_steps;
	result->queue_full_drops = queue_full_drops;
	result->stale_work_skips = stale_work_skips;
	result->race_overwrites = race_overwrites;
	result->dma_stale_retries = dma_stale_retries;

	if (!result->passed) {
		uint32_t qp_id = 0, chunk_id = 0;
		uint64_t byte_offset = 0;
		uint8_t expected = 0, actual = 0;
		p_validation_get_error(&qp_id, &chunk_id, &byte_offset, &expected, &actual);
		result->error_qp_id = qp_id;
		result->error_chunk_id = chunk_id;
		result->error_byte_offset = byte_offset;
		result->error_expected = expected;
		result->error_actual = actual;
	}

	return 0;
}

static void cuda_validation_destroy(struct memory_ctx *ctx)
{
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

	if (cuda_ctx->validation_active) {
		if (p_validation_destroy) {
			p_validation_destroy();
		}
		cuda_ctx->validation_active = 0;
	}
}

struct memory_ctx *cuda_memory_create(struct perftest_parameters *params) {
	struct cuda_memory_ctx *ctx;

	ALLOCATE(ctx, struct cuda_memory_ctx, 1);
	memset(ctx, 0, sizeof(struct cuda_memory_ctx));
	ctx->base.init = cuda_memory_init;
	ctx->base.destroy = cuda_memory_destroy;
	ctx->base.allocate_buffer = cuda_memory_allocate_buffer;
	ctx->base.free_buffer = cuda_memory_free_buffer;
	ctx->base.copy_host_to_buffer = cuda_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_host = cuda_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_buffer = cuda_memory_copy_buffer_to_buffer;
	ctx->base.validation_init = cuda_validation_init;
	ctx->base.validation_start = cuda_validation_start;
	ctx->base.validation_stop = cuda_validation_stop;
	ctx->base.validation_destroy = cuda_validation_destroy;
	ctx->device_id = params->cuda_device_id;
	ctx->device_bus_id = params->cuda_device_bus_id;
	ctx->use_dmabuf = params->use_cuda_dmabuf;
	ctx->use_pcie_mapping = params->use_cuda_pcie_mapping;
	ctx->gpu_touch = params->gpu_touch;
	ctx->stop_touch_gpu_kernel_flag = NULL;
	ctx->mem_type = params->cuda_mem_type;
	ctx->validation_active = 0;
	ctx->effective_mem_type = -1;

	return &ctx->base;
}
