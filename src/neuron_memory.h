/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef NEURON_MEMORY_H
#define NEURON_MEMORY_H

#include "memory.h"
#include "config.h"


struct perftest_parameters;

bool neuron_memory_supported();

bool neuron_memory_dmabuf_supported();

struct memory_ctx *neuron_memory_create(struct perftest_parameters *params);


#ifndef HAVE_NEURON

inline bool neuron_memory_supported() {
	return false;
}

inline bool neuron_memory_dmabuf_supported() {
	return false;
}

inline struct memory_ctx *neuron_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* NEURON_MEMORY_H */
