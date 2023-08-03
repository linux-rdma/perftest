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

#if HAVE_IBV_DM
struct ib_memory_ctx {
	struct memory_ctx	base;
	struct ibv_dm		*dm;
	struct ibv_mr		*mr;
	struct ibv_context	*ib_ctx;
	struct ibv_pd		*pd;
	void			*addr;
	void			*malloc_address;
};
#else

inline bool ib_memory_supported() {
	return false;
}

inline struct memory_ctx *ib_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* IB_DEVICE_MEMORY_H */
