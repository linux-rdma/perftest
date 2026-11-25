/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
#include <string.h>

#include "perftest_cpu_util.h"

void read_cpu_util_sample(FILE *file, struct cpu_util_sample *sample)
{
	char line[256];
	unsigned long long user = 0, nice = 0, system = 0, idle = 0;
	unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;

	*sample = (struct cpu_util_sample){0};
	if (!file || !fgets(line, sizeof(line), file) ||
	    strncmp(line, "cpu", 3) || (line[3] != ' ' && line[3] != '\t'))
		return;

	if (sscanf(line + 3, "%llu %llu %llu %llu %llu %llu %llu %llu",
		   &user, &nice, &system, &idle, &iowait, &irq, &softirq,
		   &steal) < 4)
		return;

	/* Linux already includes guest and guest_nice in user and nice. */
	sample->busy = user + nice + system + irq + softirq + steal;
	sample->idle = idle;
	sample->iowait = iowait;
	sample->valid = true;
}

float calc_cpu_util(const struct cpu_util_data *data)
{
	const struct cpu_util_sample *start = &data->samples[0];
	const struct cpu_util_sample *end = &data->samples[1];
	unsigned long long busy, idle, iowait = 0;
	double total;

	if (!data->enable || !start->valid || !end->valid ||
	    end->busy < start->busy || end->idle < start->idle)
		return 0;

	busy = end->busy - start->busy;
	idle = end->idle - start->idle;
	/* Linux may account less I/O wait in the later sample. */
	if (end->iowait >= start->iowait)
		iowait = end->iowait - start->iowait;
	total = (double)busy + idle + iowait;

	return total ? 100.0 * busy / total : 0;
}
