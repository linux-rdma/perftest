/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <nrt/nrt.h>
#include "neuron_memory.h"
#include "perftest_parameters.h"

#define NRT_VISIBLE_CORES_STR_LEN 8
#define ACCEL_PAGE_SIZE (4 * 1024)


struct neuron_memory_ctx {
	struct memory_ctx base;
	int core_id;
	int max_tensors;
	nrt_tensor_t **tensors;
	int num_of_tensors;
	bool use_dmabuf;
};


int neuron_memory_init(struct memory_ctx *ctx) {
	struct neuron_memory_ctx *neuron_ctx = container_of(ctx, struct neuron_memory_ctx, base);
	NRT_STATUS result;
	char *env_var = getenv("NEURON_RT_VISIBLE_CORES");

	if (env_var != NULL) {
		printf("NEURON_RT_VISIBLE_CORES is set to %s, core id will be used relatively\n",env_var);
	} else {
		char nrt_visible_cores[NRT_VISIBLE_CORES_STR_LEN];

		printf("NEURON_RT_VISIBLE_CORES is not set, setting to %d\n",neuron_ctx->core_id);
		snprintf(nrt_visible_cores, sizeof(nrt_visible_cores), "%d", neuron_ctx->core_id);
		setenv("NEURON_RT_VISIBLE_CORES", nrt_visible_cores, 1);
		neuron_ctx->core_id = 0;
	}

	result = nrt_init(NRT_FRAMEWORK_TYPE_NO_FW, "", "");
	if (result != NRT_SUCCESS) {
		fprintf(stderr, "Couldn't initialize Neuron device\n");
		return FAILURE;
	}
	return SUCCESS;
}

int neuron_memory_destroy(struct memory_ctx *ctx) {
	struct neuron_memory_ctx *neuron_ctx = container_of(ctx, struct neuron_memory_ctx, base);
	int i;

	for (i = 0; i < neuron_ctx->num_of_tensors; i++) {
		nrt_tensor_free(&neuron_ctx->tensors[i]);
	}
	neuron_ctx->num_of_tensors = 0;
	nrt_close();

	free(neuron_ctx->tensors);
	free(neuron_ctx);
	return SUCCESS;
}

int neuron_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				  uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	struct neuron_memory_ctx *neuron_ctx = container_of(ctx, struct neuron_memory_ctx, base);
	void *d_A = NULL;
	NRT_STATUS result;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);
	int tensor_index = neuron_ctx->num_of_tensors;

	if (tensor_index >= neuron_ctx->max_tensors)
	{
		printf("Can't allocate Neuron memory, max tensors reached\n");
		return FAILURE;
	}

	result = nrt_tensor_allocate(NRT_TENSOR_PLACEMENT_DEVICE, neuron_ctx->core_id, buf_size, NULL, &neuron_ctx->tensors[tensor_index]);
	if (result != NRT_SUCCESS) {
		neuron_ctx->tensors[tensor_index] = NULL;
		printf("nrt_tensor_allocate_error =%d\n", (int)result);
		return FAILURE;
	}

	d_A = nrt_tensor_get_va(neuron_ctx->tensors[tensor_index]);
	if (d_A == NULL) {
		nrt_tensor_free(&neuron_ctx->tensors[tensor_index]);
		neuron_ctx->tensors[tensor_index] = NULL;
		printf("Failed to get va for the allocated tensor\n");
		return FAILURE;
	}

	if (neuron_ctx->use_dmabuf) {
		result = nrt_get_dmabuf_fd((uint64_t)d_A, (uint64_t)buf_size, dmabuf_fd);
		if (result != NRT_SUCCESS) {
			nrt_tensor_free(&neuron_ctx->tensors[tensor_index]);
			neuron_ctx->tensors[tensor_index] = NULL;
			*dmabuf_fd = 0;
			printf("Unable to retrieve dmabuf fd of Neuron device buffer\n");
			return FAILURE;
		}

		*dmabuf_offset = 0;
	}

	neuron_ctx->num_of_tensors++;
	*addr = d_A;
	*can_init = false;
	return SUCCESS;
}

int neuron_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	return SUCCESS;
}

bool neuron_memory_supported() {
	return true;
}

bool neuron_memory_dmabuf_supported() {
#ifdef HAVE_NEURON_DMABUF
	return true;
#else
	return false;
#endif
}

struct memory_ctx *neuron_memory_create(struct perftest_parameters *params) {
	struct neuron_memory_ctx *ctx;

	ALLOCATE(ctx, struct neuron_memory_ctx, 1);
	ctx->base.init = neuron_memory_init;
	ctx->base.destroy = neuron_memory_destroy;
	ctx->base.allocate_buffer = neuron_memory_allocate_buffer;
	ctx->base.free_buffer = neuron_memory_free_buffer;
	ctx->base.copy_host_to_buffer = memcpy;
	ctx->base.copy_buffer_to_host = memcpy;
	ctx->base.copy_buffer_to_buffer = memcpy;
	ctx->core_id = params->neuron_core_id;
	ctx->max_tensors = params->num_of_qps * 2;
	ALLOCATE(ctx->tensors, nrt_tensor_t* , ctx->max_tensors);
	ctx->num_of_tensors = 0;
	ctx->use_dmabuf = params->use_neuron_dmabuf;
	return &ctx->base;
}
