/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef CUDA_MEMORY_H
#define CUDA_MEMORY_H

#include "memory.h"
#include "config.h"


struct perftest_parameters;

bool cuda_memory_supported();

bool cuda_memory_dmabuf_supported();

bool data_direct_supported();

struct memory_ctx *cuda_memory_create(struct perftest_parameters *params);


#ifndef HAVE_CUDA

inline bool cuda_memory_supported() {
	return false;
}

inline bool cuda_memory_dmabuf_supported() {
	return false;
}

inline bool data_direct_supported() {
	return false;
}

inline struct memory_ctx *cuda_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* CUDA_MEMORY_H */
