/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#ifndef OPENCL_MEMORY_H
#define OPENCL_MEMORY_H

#include "memory.h"
#include "config.h"


struct perftest_parameters;

bool opencl_memory_supported();

struct memory_ctx *opencl_memory_create(struct perftest_parameters *params);


#ifndef HAVE_OPENCL

inline bool opencl_memory_supported() {
	return false;
}

inline struct memory_ctx *opencl_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* OPENCL_MEMORY_H */
