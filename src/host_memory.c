/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "host_memory.h"
#include "host_validation.h"
#include "perftest_parameters.h"

#if !defined(__FreeBSD__)
#include <sys/mman.h>
#endif

#define SHMAT_ADDR (void *)(0x0UL)
#define SHMAT_FLAGS (0)
#define SHMAT_INVALID_PTR ((void *)-1)

#define HUGEPAGE_SIZE_2MB  (2ULL * 1024 * 1024)
#define HUGEPAGE_SIZE_1GB  (1ULL * 1024 * 1024 * 1024)

/* MAP_HUGE_* flags for specifying huge page size (Linux 3.8+) */
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB    (21 << 26)  /* 2^21 = 2MB */
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB    (30 << 26)  /* 2^30 = 1GB */
#endif

#if !defined(__FreeBSD__)
int alloc_hugepage_region(int alignment, uint64_t size, void **addr)
{
	int huge_shmid;
	uint64_t buf_size = (size + HUGEPAGE_SIZE_2MB - 1) & ~(HUGEPAGE_SIZE_2MB - 1);

	huge_shmid = shmget(IPC_PRIVATE, buf_size, SHM_HUGETLB | IPC_CREAT | SHM_R | SHM_W);
	if (huge_shmid < 0) {
		fprintf(stderr, "Failed to allocate hugepages. Please configure hugepages\n");
		return FAILURE;
	}

	*addr = (void *)shmat(huge_shmid, SHMAT_ADDR, SHMAT_FLAGS);
	if (*addr == SHMAT_INVALID_PTR) {
		fprintf(stderr, "Failed to attach shared memory region\n");
		return FAILURE;
	}

	/* Mark for removal so shmem is freed when process detaches */
	if (shmctl(huge_shmid, IPC_RMID, 0) != 0) {
		fprintf(stderr, "Failed to mark shm for removal\n");
		return FAILURE;
	}

	return SUCCESS;
}

/* Allocate via mmap, trying 1GB -> 2MB -> regular pages */
static int alloc_huge_pages_with_fallback(uint64_t size, void **addr,
					   enum host_alloc_type *alloc_type,
					   uint64_t *actual_size, int debug)
{
	static const struct {
		uint64_t             page_size;   /* 0 = regular pages */
		int                  extra_flags;
		enum host_alloc_type type;
		const char          *name;
	} opts[] = {
		{ HUGEPAGE_SIZE_1GB, MAP_HUGETLB | MAP_HUGE_1GB, HOST_ALLOC_MMAP_HUGE_1GB, "1GB"     },
		{ HUGEPAGE_SIZE_2MB, MAP_HUGETLB | MAP_HUGE_2MB, HOST_ALLOC_MMAP_HUGE_2MB, "2MB"     },
		{ 0,                 0,                           HOST_ALLOC_MMAP_REGULAR,  "regular" },
	};
	int i;

	for (i = 0; i < 3; i++) {
		uint64_t aligned = opts[i].page_size
			? (size + opts[i].page_size - 1) & ~(opts[i].page_size - 1)
			: size;
		void *ptr = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | opts[i].extra_flags, -1, 0);
		if (ptr != MAP_FAILED) {
			*addr = ptr;
			*alloc_type = opts[i].type;
			*actual_size = aligned;
			if (debug)
				printf("[HOST_MEMORY] Allocated %lu bytes using %s pages\n",
				       (unsigned long)size, opts[i].name);
			return SUCCESS;
		}
		if (debug)
			printf("[HOST_MEMORY] %s pages not available, trying next...\n",
			       opts[i].name);
	}

	fprintf(stderr, "[HOST_MEMORY] Failed to allocate %lu bytes (errno=%d: %s)\n",
		(unsigned long)size, errno, strerror(errno));
	return FAILURE;
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
	struct host_memory_ctx *host_ctx = container_of(ctx, struct host_memory_ctx, base);

#if defined(__FreeBSD__)
	posix_memalign(addr, alignment, size);
	host_ctx->alloc_type = HOST_ALLOC_MALLOC;
	host_ctx->alloc_size = size;
#else
	/* Priority: data_validation -> auto huge pages,
	 * --use_hugepages -> legacy shmget, otherwise -> memalign */
	if (host_ctx->use_huge_for_validation) {
		if (alloc_huge_pages_with_fallback(size, addr, &host_ctx->alloc_type,
						    &host_ctx->alloc_size, host_ctx->debug) != SUCCESS) {
			fprintf(stderr, "Failed to allocate memory for data validation.\n");
			return FAILURE;
		}
	} else if (host_ctx->use_hugepages) {
		if (alloc_hugepage_region(alignment, size, addr) != SUCCESS) {
			fprintf(stderr, "Failed to allocate hugepage region.\n");
			return FAILURE;
		}
		host_ctx->alloc_type = HOST_ALLOC_SHMGET;
		host_ctx->alloc_size = size;
	} else {
		*addr = memalign(alignment, size);
		host_ctx->alloc_type = HOST_ALLOC_MALLOC;
		host_ctx->alloc_size = size;
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

	switch (host_ctx->alloc_type) {
	case HOST_ALLOC_SHMGET:
		shmdt(addr);
		break;
	case HOST_ALLOC_MMAP_REGULAR:
	case HOST_ALLOC_MMAP_HUGE_2MB:
	case HOST_ALLOC_MMAP_HUGE_1GB:
#if !defined(__FreeBSD__)
		munmap(addr, host_ctx->alloc_size);
#endif
		break;
	case HOST_ALLOC_MALLOC:
	default:
		free(addr);
		break;
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
	ctx->base.validation_init = host_validation_init;
	ctx->base.validation_start = host_validation_start;
	ctx->base.validation_stop = host_validation_stop;
	ctx->base.validation_destroy = host_validation_destroy;
	ctx->use_hugepages = params->use_hugepages;
	ctx->use_huge_for_validation = params->data_validation;
	ctx->debug = params->data_validation_debug;
	ctx->alloc_type = HOST_ALLOC_MALLOC;
	ctx->alloc_size = 0;
	ctx->validation_ctx = NULL;
	return &ctx->base;
}
