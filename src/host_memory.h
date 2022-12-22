/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef HOST_MEMORY_H
#define HOST_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include "memory.h"


struct perftest_parameters;

struct memory_ctx *host_memory_create(struct perftest_parameters *params);

#endif /* HOST_MEMORY_H */
