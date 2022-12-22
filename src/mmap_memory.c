/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include "mmap_memory.h"
#include "perftest_parameters.h"


struct mmap_memory_ctx {
	struct memory_ctx base;
	char *file;
	unsigned long offset;
};


static int init_mmap(void **addr, size_t size, const char *fname, unsigned long offset) {
	int fd = open(fname, O_RDWR);

	if (fd < 0) {
		printf("Unable to open '%s': %s\n", fname, strerror(errno));
		return FAILURE;
	}

	*addr = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, offset);
	close(fd);

	if (*addr == MAP_FAILED) {
		printf("Unable to mmap '%s': %s\n", fname, strerror(errno));
		return FAILURE;
	}

	printf("allocated mmap buffer of size %zu at %p\n", size, *addr);

	return SUCCESS;
}

int mmap_memory_init(struct memory_ctx *ctx) {
	return SUCCESS;
}

int mmap_memory_destroy(struct memory_ctx *ctx) {
	struct mmap_memory_ctx *mmap_ctx = container_of(ctx, struct mmap_memory_ctx, base);

	free(mmap_ctx);
	return SUCCESS;
}

int mmap_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset, void **addr, bool *can_init) {
	struct mmap_memory_ctx *mmap_ctx = container_of(ctx, struct mmap_memory_ctx, base);

	if (init_mmap(addr, size, mmap_ctx->file, mmap_ctx->offset))
	{
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return FAILURE;
	}
	*can_init = true;
	return SUCCESS;
}

int mmap_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	munmap(addr, size);
	return SUCCESS;
}

struct memory_ctx *mmap_memory_create(struct perftest_parameters *params) {
	struct mmap_memory_ctx *ctx;

	ALLOCATE(ctx, struct mmap_memory_ctx, 1);
	ctx->base.init = mmap_memory_init;
	ctx->base.destroy = mmap_memory_destroy;
	ctx->base.allocate_buffer = mmap_memory_allocate_buffer;
	ctx->base.free_buffer = mmap_memory_free_buffer;
	ctx->base.copy_host_to_buffer = memcpy;
	ctx->base.copy_buffer_to_host = memcpy;
	ctx->base.copy_buffer_to_buffer = memcpy;
	ctx->file = params->mmap_file;
	ctx->offset = params->mmap_offset;
	return &ctx->base;
}
