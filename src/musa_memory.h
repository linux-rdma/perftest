/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2026 Moore Threads Technology Co. Ltd. All rights reserved.
 */

#ifndef MUSA_MEMORY_H
#define MUSA_MEMORY_H

#include "config.h"
#include "memory.h"

struct perftest_parameters;

bool musa_memory_supported();
bool musa_memory_dmabuf_supported();
struct memory_ctx *musa_memory_create(struct perftest_parameters *params);

#ifndef HAVE_MUSA

inline bool musa_memory_supported()
{
	return false;
}

inline bool musa_memory_dmabuf_supported()
{
	return false;
}

inline struct memory_ctx *musa_memory_create(struct perftest_parameters *params)
{
	return NULL;
}

#endif

#endif /* MUSA_MEMORY_H */
