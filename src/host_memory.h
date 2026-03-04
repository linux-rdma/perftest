/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef HOST_MEMORY_H
#define HOST_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include "memory.h"

struct perftest_parameters;
struct host_validation_ctx;

enum host_alloc_type {
	HOST_ALLOC_MALLOC,          /* Standard malloc/memalign */
	HOST_ALLOC_SHMGET,          /* Legacy shmget hugepages */
	HOST_ALLOC_MMAP_REGULAR,    /* mmap without huge pages */
	HOST_ALLOC_MMAP_HUGE_2MB,   /* mmap with 2MB huge pages */
	HOST_ALLOC_MMAP_HUGE_1GB    /* mmap with 1GB huge pages */
};

struct host_memory_ctx {
	struct memory_ctx base;
	int use_hugepages;              /* Legacy hugepages flag (--use_hugepages) */
	int use_huge_for_validation;    /* Auto huge pages for data validation */
	int debug;                      /* Print allocation diagnostics */
	enum host_alloc_type alloc_type; /* How buffer was allocated */
	uint64_t alloc_size;            /* Actual allocated size (for munmap) */
	int validation_flow_control;    /* Strict validation flow control enabled */
	struct host_validation_ctx *validation_ctx;  /* NULL if validation not active */
};

struct memory_ctx *host_memory_create(struct perftest_parameters *params);

#endif /* HOST_MEMORY_H */
