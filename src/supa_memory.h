/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (c) 2026 Shanghai Biren Technology Co., Ltd. All rights reserved.
 */

#ifndef SUPA_MEMORY_H
#define SUPA_MEMORY_H

#include "memory.h"
#include "config.h"


struct perftest_parameters;

bool supa_memory_supported();

bool supa_memory_dmabuf_supported();

struct memory_ctx *supa_memory_create(struct perftest_parameters *params);


#ifndef HAVE_SUPA

inline bool supa_memory_supported() {
	return false;
}

inline bool supa_memory_dmabuf_supported() {
	return false;
}

inline struct memory_ctx *supa_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* SUPA_MEMORY_H */
