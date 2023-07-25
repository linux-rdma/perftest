/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <infiniband/mlx5dv.h>
#include "ib_device_memory.h"
#include "perftest_parameters.h"


#define align_down_pow2(_n, _alignment) \
    ( (_n) & ~((_alignment) - 1) )

#define align_up_pow2(_n, _alignment) \
    align_down_pow2((_n) + (_alignment) - 1, _alignment)



static int ib_memory_init(struct memory_ctx *ctx) {
	struct ib_memory_ctx *ib_ctx = container_of(ctx, struct ib_memory_ctx, base);
	ib_ctx->pd = ibv_alloc_pd(ib_ctx->ib_ctx);
	if (ib_ctx->pd == NULL) {
		return FAILURE;
	}
	return SUCCESS;
}

static int ib_memory_destroy(struct memory_ctx *ctx) {
	struct ib_memory_ctx *ib_ctx = container_of(ctx, struct ib_memory_ctx, base);
	ibv_dealloc_pd(ib_ctx->pd);
	return SUCCESS;
}

static void* get_dm_mapped_address(struct ibv_dm *dm) {
	struct mlx5dv_dm dvdm = {};
	struct mlx5dv_obj dv = {};
	int ret = 0;
	dv.dm.in = dm;
	dv.dm.out = &dvdm;
	ret = mlx5dv_init_obj(&dv, MLX5DV_OBJ_DM);
	if (ret != 0) {
		return NULL;
	}
	return dvdm.buf;
}

static int ib_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
									 uint64_t *dmabuf_offset,  void **addr, bool *can_init) {
	struct ib_memory_ctx *ib_ctx = container_of(ctx, struct ib_memory_ctx, base);
	struct ibv_alloc_dm_attr dm_attr = {
		.length = align_up_pow2(size, alignment),
		.comp_mask = 0
	};

	ib_ctx->dm = ibv_alloc_dm(ib_ctx->ib_ctx, &dm_attr);
	if (ib_ctx->dm == NULL) {
		return FAILURE;
	}

	ib_ctx->addr = get_dm_mapped_address(ib_ctx->dm);

	if(ib_ctx->addr == NULL) {
		return FAILURE;
	}

	*addr = ib_ctx->addr;
	*can_init = false;
	memset(*addr, 0, dm_attr.length);
	return SUCCESS;
}

static int ib_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	struct ib_memory_ctx *ib_ctx = container_of(ctx, struct ib_memory_ctx, base);
	int ret = ibv_free_dm(ib_ctx->dm) == 0 ? SUCCESS : FAILURE;

	munmap(addr, size);
	return ret;
}

bool ib_memory_supported() {
	return true;
}

struct memory_ctx *ib_memory_create(struct perftest_parameters *params) {
	struct ib_memory_ctx *ctx;
	ALLOCATE(ctx, struct ib_memory_ctx, 1);
	ctx->base.init = ib_memory_init;
	ctx->base.destroy = ib_memory_destroy;
	ctx->base.allocate_buffer = ib_memory_allocate_buffer;
	ctx->base.free_buffer = ib_memory_free_buffer;
	ctx->base.copy_host_to_buffer = memcpy;
	ctx->base.copy_buffer_to_host = memcpy;
	ctx->base.copy_buffer_to_buffer = memcpy;
	ctx->ib_ctx = params->ib_ctx;

	return &ctx->base;
}
