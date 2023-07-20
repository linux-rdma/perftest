/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef IB_DEVICE_MEMORY_H
#define IB_DEVICE_MEMORY_H

#include "memory.h"
#include "config.h"


struct perftest_parameters;

bool ib_memory_supported();

struct memory_ctx *ib_memory_create(struct perftest_parameters *params);


#ifndef HAVE_IBV_DM

inline bool ib_memory_supported() {
	return false;
}

inline struct memory_ctx *ib_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* IB_DEVICE_MEMORY_H */
