/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef MLU_MEMORY_H
#define MLU_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include "memory.h"
#include "config.h"


struct perftest_parameters;

bool mlu_memory_supported();

struct memory_ctx *mlu_memory_create(struct perftest_parameters *params);


#ifndef HAVE_MLU

inline bool mlu_memory_supported() {
	return false;
}

inline struct memory_ctx *mlu_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* MLU_MEMORY_H */
