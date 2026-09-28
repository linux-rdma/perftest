/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#ifndef SYSTEM_DMABUF_MEMORY_H
#define SYSTEM_DMABUF_MEMORY_H

#include "memory.h"
#include "config.h"

struct perftest_parameters;

#ifdef HAVE_SYSTEM_DMABUF
bool system_dmabuf_memory_supported();
struct memory_ctx *system_dmabuf_memory_create(struct perftest_parameters *params);
#else
static inline bool system_dmabuf_memory_supported()
{
	return false;
}

static inline struct memory_ctx *system_dmabuf_memory_create(struct perftest_parameters *params)
{
	return NULL;
}
#endif

#endif /* SYSTEM_DMABUF_MEMORY_H */
