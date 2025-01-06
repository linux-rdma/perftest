/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "perftest_parameters.h"
#include "perftest_resources.h"
#define CL_TARGET_OPENCL_VERSION 220
#include <CL/cl.h>
#define NUM_PLATFORMS 5
#define NUM_DEVICES 5

__attribute__ ((unused)) static const char *opencl_mem_types_str[] = {
	"OPENCL_MEM_SVM",
};

// Allocating this context on heap so it can be passed to other thread
struct buffer_ctx {
	pthread_t thread;
	const void * addr;
	bool gpu_touch_infinite;
	bool gpu_touch_stop;
	size_t size;
	cl_command_queue command_queue;
};

struct opencl_memory_ctx {
	struct buffer_ctx *buffer_ctx;
	struct memory_ctx base;
	int platform_ix;
	int device_ix;
	cl_context cl_context;
	cl_command_queue command_queue;
	int gpu_touch;
};

static int init_gpu(struct opencl_memory_ctx *ctx)
{
	cl_platform_id platforms_id[NUM_PLATFORMS];
	cl_device_id devices_id[NUM_PLATFORMS];
	cl_uint num_devices;
	cl_uint num_platforms;
	cl_device_svm_capabilities caps;
	cl_int error;

	error = clGetPlatformIDs(NUM_PLATFORMS, platforms_id, &num_platforms);
	if (error) {
		printf("clGetPlatformIDs returned %d\n", error);
		return FAILURE;
	}

	if (num_platforms < ctx->platform_ix)
	{
		printf("platform_id (%d) is not in the range of num_platforms (%d)\n",
				ctx->platform_ix, num_platforms);
		return FAILURE;
	}

	error = clGetDeviceIDs(platforms_id[ctx->platform_ix], CL_DEVICE_TYPE_ALL, NUM_DEVICES, devices_id, &num_devices);
	if (error) {
		printf("clGetDeviceIDs returned %d\n", error);
		return FAILURE;
	}

	if (num_devices < ctx->device_ix)
	{
		printf("device_id (%d) is not in the range of num_devices (%d)\n",
				ctx->device_ix, num_devices);
		return FAILURE;
	}

	error = clGetDeviceInfo(devices_id[ctx->device_ix], CL_DEVICE_SVM_CAPABILITIES, sizeof(cl_device_svm_capabilities), &caps, 0);
	if (error) {
		printf("clGetDeviceInfo returned %d\n", error);
		return FAILURE;
	}

	if (!(caps & CL_DEVICE_SVM_FINE_GRAIN_SYSTEM))
	{
		printf("SVM needed caps are not supported\n");
		return FAILURE;
	}

	ctx->cl_context = clCreateContext(NULL, 1, &devices_id[ctx->device_ix], NULL, NULL, &error);
	if (error) {
		printf("clCreateContext returned %d\n", error);
		return FAILURE;
	}

	ctx->command_queue = clCreateCommandQueueWithProperties(ctx->cl_context, devices_id[ctx->device_ix], NULL, &error);
	if (error) {
		printf("clCreateCommandQueueWithProperties failed with ret=%d\n", error);
		clReleaseContext(ctx->cl_context);
		return FAILURE;
	}

	return SUCCESS;
}

static void free_gpu(struct opencl_memory_ctx *ctx)
{
	printf("destroying current OpenCL ctx\n");
	clReleaseCommandQueue(ctx->command_queue);
	clReleaseContext(ctx->cl_context);
}

int opencl_memory_init(struct memory_ctx *ctx)
{
	struct opencl_memory_ctx *opencl_ctx = container_of(ctx, struct opencl_memory_ctx, base);
	int return_value = 0;

	return_value = init_gpu(opencl_ctx);
	if (return_value) {
		fprintf(stderr, "Couldn't init GPU context: %d\n", return_value);
		return FAILURE;
	}

	return SUCCESS;
}

int opencl_memory_destroy(struct memory_ctx *ctx) {
	struct opencl_memory_ctx *opencl_ctx = container_of(ctx, struct opencl_memory_ctx, base);

	free_gpu(opencl_ctx);
	free(opencl_ctx);
	return SUCCESS;
}

void *touch_gpu_pages(void *ctx_param) {
	struct buffer_ctx *ctx = (struct buffer_ctx *)ctx_param;
	int ret;
	do {
		ret = clEnqueueSVMMigrateMem(ctx->command_queue, 1, &ctx->addr, &ctx->size, 0, 0, NULL, NULL);
		if (ret) {
			printf("clEnqueueSVMMigrateMem failed with ret=%d\n", ret);
			break;
		}

		ret = clFlush(ctx->command_queue);
		if (ret) {
			printf("clFlush with ret=%d\n", ret);
			break;
		}

		ret = clFinish(ctx->command_queue);
		if (ret) {
			printf("clFinish with ret=%d\n", ret);
			break;
		}
	} while (ctx->gpu_touch_infinite && !ctx->gpu_touch_stop);
	return NULL;
}

int opencl_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd, uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	struct opencl_memory_ctx *opencl_ctx = container_of(ctx, struct opencl_memory_ctx, base);

	*addr = clSVMAlloc(opencl_ctx->cl_context, CL_MEM_READ_WRITE, size, MAX(alignment, sysconf(_SC_PAGESIZE)));
	if (!*addr)
	{
		printf("clSVMAlloc failed\n");
		return -1;
	}

	opencl_ctx->buffer_ctx = NULL;

	if (opencl_ctx->gpu_touch != GPU_NO_TOUCH) {
		opencl_ctx->buffer_ctx = malloc(sizeof(struct buffer_ctx));
		if (!opencl_ctx->buffer_ctx) {
			printf("Failed to allocate context for gpu_touch\n");
			return -ENOMEM;
		}

		opencl_ctx->buffer_ctx->addr = *addr;
		opencl_ctx->buffer_ctx->gpu_touch_infinite = opencl_ctx->gpu_touch == GPU_TOUCH_INFINITE;
		opencl_ctx->buffer_ctx->gpu_touch_stop = 0;
		opencl_ctx->buffer_ctx->size = size;
		opencl_ctx->buffer_ctx->command_queue = opencl_ctx->command_queue;
		*can_init = false;

		return pthread_create(&opencl_ctx->buffer_ctx->thread, NULL, touch_gpu_pages, opencl_ctx->buffer_ctx);
	}

	return 0;
}

int opencl_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	struct opencl_memory_ctx *opencl_ctx = container_of(ctx, struct opencl_memory_ctx, base);
	if (opencl_ctx->buffer_ctx) {
		opencl_ctx->buffer_ctx->gpu_touch_stop = 1;
		if (pthread_join(opencl_ctx->buffer_ctx->thread, NULL)) {
			free(opencl_ctx->buffer_ctx);
			printf("Error stopping gpu_touch thread\n");
			return -1;
		}
		free(opencl_ctx->buffer_ctx);
	}
	clSVMFree(opencl_ctx->cl_context, addr);
	return 0;
}

bool opencl_memory_supported() {
	return true;
}

struct memory_ctx *opencl_memory_create(struct perftest_parameters *params) {
	struct opencl_memory_ctx *ctx;

	ALLOCATE(ctx, struct opencl_memory_ctx, 1);
	ctx->base.init = opencl_memory_init;
	ctx->base.destroy = opencl_memory_destroy;
	ctx->base.allocate_buffer = opencl_memory_allocate_buffer;
	ctx->base.free_buffer = opencl_memory_free_buffer;
	ctx->base.copy_host_to_buffer = memcpy;
	ctx->base.copy_buffer_to_host = memcpy;
	ctx->base.copy_buffer_to_buffer = memcpy;
	ctx->device_ix = params->opencl_device_id;
	ctx->platform_ix = params->opencl_platform_id;
	ctx->gpu_touch = params->gpu_touch;

	return &ctx->base;
}
