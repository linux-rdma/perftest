/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include "system_dmabuf_memory.h"
#include "perftest_parameters.h"

#define DMA_HEAP_PATH "/dev/dma_heap/system"

struct system_dmabuf_memory_ctx {
	struct memory_ctx	base;
	enum verbosity_level	output;
};

static int system_dmabuf_memory_init(struct memory_ctx *ctx)
{
	return SUCCESS;
}

static int system_dmabuf_memory_destroy(struct memory_ctx *ctx)
{
	struct system_dmabuf_memory_ctx *sdc =
		container_of(ctx, struct system_dmabuf_memory_ctx, base);

	free(sdc);
	return SUCCESS;
}

static int system_dmabuf_memory_allocate_buffer(struct memory_ctx *ctx,
						int alignment, uint64_t size,
						int *dmabuf_fd,
						uint64_t *dmabuf_offset,
						void **addr, bool *can_init)
{
	struct system_dmabuf_memory_ctx *sdc =
		container_of(ctx, struct system_dmabuf_memory_ctx, base);
	struct dma_heap_allocation_data alloc = {
		.len      = size,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	int heap_fd;
	void *mmap_addr;

	heap_fd = open(DMA_HEAP_PATH, O_RDONLY | O_CLOEXEC);
	if (heap_fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n",
			DMA_HEAP_PATH, strerror(errno));
		return FAILURE;
	}

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		int saved_errno = errno;

		close(heap_fd);
		fprintf(stderr, "DMA heap alloc (%lu bytes) failed: %s\n",
			(unsigned long)size, strerror(saved_errno));
		return FAILURE;
	}

	/*
	 * Do not retain the dma heap char device fd because CRIU can't
	 * checkpoint it. DMA_HEAP_IOCTL_ALLOC installs an independent dma buf
	 * fd in alloc.fd so closing the allocator fd can't release the buffer
	 */
	close(heap_fd);

	mmap_addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, alloc.fd, 0);
	if (mmap_addr == MAP_FAILED) {
		fprintf(stderr, "mmap of dma-buf failed: %s\n", strerror(errno));
		close(alloc.fd);
		return FAILURE;
	}

	*dmabuf_fd = alloc.fd;
	*dmabuf_offset = 0;
	*addr = mmap_addr;
	*can_init = true;

	if (sdc->output == FULL_VERBOSITY)
		printf("System dma-buf: %lu bytes, fd=%d addr=%p\n",
		       (unsigned long)size, alloc.fd, mmap_addr);
	return SUCCESS;
}

static int system_dmabuf_memory_free_buffer(struct memory_ctx *ctx,
					    int dmabuf_fd, void *addr,
					    uint64_t size)
{
	if (munmap(addr, size)) {
		fprintf(stderr, "munmap of dma-buf failed: %s\n", strerror(errno));
		return FAILURE;
	}

	return SUCCESS;
}

bool system_dmabuf_memory_supported()
{
	return true;
}

struct memory_ctx *system_dmabuf_memory_create(struct perftest_parameters *params)
{
	struct system_dmabuf_memory_ctx *ctx;

	ALLOCATE(ctx, struct system_dmabuf_memory_ctx, 1);
	ctx->base.init = system_dmabuf_memory_init;
	ctx->base.destroy = system_dmabuf_memory_destroy;
	ctx->base.allocate_buffer = system_dmabuf_memory_allocate_buffer;
	ctx->base.free_buffer = system_dmabuf_memory_free_buffer;
	ctx->base.copy_host_to_buffer = memcpy;
	ctx->base.copy_buffer_to_host = memcpy;
	ctx->base.copy_buffer_to_buffer = memcpy;
	ctx->output = params->output;

	return &ctx->base;
}
