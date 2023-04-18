/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "host_memory.h"
#include "perftest_parameters.h"


struct host_memory_ctx {
	struct memory_ctx base;
	int use_hugepages;
};


#define HUGEPAGE_ALIGN  (2*1024*1024)
#define SHMAT_ADDR (void *)(0x0UL)
#define SHMAT_FLAGS (0)
#define SHMAT_INVALID_PTR ((void *)-1)

#if !defined(__FreeBSD__)
int alloc_hugepage_region(int alignment, uint64_t size, void **addr)
{
	int huge_shmid;
	uint64_t buf_size;
	uint64_t buf_alignment = (((alignment + HUGEPAGE_ALIGN -1) / HUGEPAGE_ALIGN) * HUGEPAGE_ALIGN);
	buf_size = (((size + buf_alignment -1 ) / buf_alignment ) * buf_alignment);

	/* create hugepage shared region */
	huge_shmid = shmget(IPC_PRIVATE, buf_size, SHM_HUGETLB | IPC_CREAT | SHM_R | SHM_W);
	if (huge_shmid < 0) {
		fprintf(stderr, "Failed to allocate hugepages. Please configure hugepages\n");
		return FAILURE;
	}

	/* attach shared memory */
	*addr = (void *)shmat(huge_shmid, SHMAT_ADDR, SHMAT_FLAGS);
	if (*addr == SHMAT_INVALID_PTR) {
		fprintf(stderr, "Failed to attach shared memory region\n");
		return FAILURE;
	}

	/* Mark shmem for removal */
	if (shmctl(huge_shmid, IPC_RMID, 0) != 0) {
		fprintf(stderr, "Failed to mark shm for removal\n");
		return FAILURE;
	}

	return SUCCESS;
}
#endif

int host_memory_init(struct memory_ctx *ctx) {
	return SUCCESS;
}

int host_memory_destroy(struct memory_ctx *ctx) {
	struct host_memory_ctx *host_ctx = container_of(ctx, struct host_memory_ctx, base);

	free(host_ctx);
	return SUCCESS;
}

int host_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset, void **addr, bool *can_init) {
#if defined(__FreeBSD__)
	posix_memalign(addr, alignment, size);
#else
	struct host_memory_ctx *host_ctx = container_of(ctx, struct host_memory_ctx, base);
	if (host_ctx->use_hugepages) {
		if (alloc_hugepage_region(alignment, size, addr) != 0){
			fprintf(stderr, "Failed to allocate hugepage region.\n");
			return FAILURE;
		}
	} else {
		*addr = memalign(alignment, size);
	}
#endif
	if (!*addr) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return FAILURE;
	}

	memset(*addr, 0, size);
	*can_init = true;
	return SUCCESS;
}

int host_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	struct host_memory_ctx *host_ctx = container_of(ctx, struct host_memory_ctx, base);

	if (host_ctx->use_hugepages) {
		shmdt(addr);
	} else {
		free(addr);
	}
	return SUCCESS;
}

struct memory_ctx *host_memory_create(struct perftest_parameters *params) {
	struct host_memory_ctx *ctx;

	ALLOCATE(ctx, struct host_memory_ctx, 1);
	ctx->base.init = host_memory_init;
	ctx->base.destroy = host_memory_destroy;
	ctx->base.allocate_buffer = host_memory_allocate_buffer;
	ctx->base.free_buffer = host_memory_free_buffer;
	ctx->base.copy_host_to_buffer = memcpy;
	ctx->base.copy_buffer_to_host = memcpy;
	ctx->base.copy_buffer_to_buffer = memcpy;
	ctx->use_hugepages = params->use_hugepages;
	return &ctx->base;
}
