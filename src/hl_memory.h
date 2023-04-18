/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef HL_MEMORY_H
#define HL_MEMORY_H

#include "memory.h"
#include "config.h"


struct perftest_parameters;

bool hl_memory_supported();

struct memory_ctx *hl_memory_create(struct perftest_parameters *params);


#ifndef HAVE_HL

inline bool hl_memory_supported() {
	return false;
}

inline struct memory_ctx *hl_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* HL_MEMORY_H */
