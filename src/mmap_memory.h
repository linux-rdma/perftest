/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef MMAP_MEMORY_H
#define MMAP_MEMORY_H

#include "memory.h"


struct perftest_parameters;

struct memory_ctx *mmap_memory_create(struct perftest_parameters *params);

#endif /* MMAP_MEMORY_H */
