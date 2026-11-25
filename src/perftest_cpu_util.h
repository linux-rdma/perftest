/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
#ifndef PERFTEST_CPU_UTIL_H
#define PERFTEST_CPU_UTIL_H

#include <stdbool.h>
#include <stdio.h>

struct cpu_util_sample {
	unsigned long long busy;
	unsigned long long idle;
	unsigned long long iowait;
	bool valid;
};

struct cpu_util_data {
	int enable;
	struct cpu_util_sample samples[2];
};

void read_cpu_util_sample(FILE *file, struct cpu_util_sample *sample);
float calc_cpu_util(const struct cpu_util_data *data);

#endif
