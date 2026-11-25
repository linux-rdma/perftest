/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
#include <stdlib.h>

#include "perftest_cpu_util.h"

static void parse_sample(const char *line, struct cpu_util_sample *sample)
{
	FILE *file = tmpfile();

	if (!file || fputs(line, file) == EOF) {
		perror("CPU sample fixture");
		exit(EXIT_FAILURE);
	}
	rewind(file);
	read_cpu_util_sample(file, sample);
	fclose(file);
}

int main(void)
{
	const struct {
		const char *name;
		const char *start;
		const char *end;
		float expected;
	} cases[] = {
		{ "busy fields, idle, iowait and guest",
		  "cpu  100 200 300 400 500 600 700 800 50 100\n",
		  "cpu  110 210 320 450 520 610 710 810 55 105\n", 50 },
		{ "system time without user activity",
		  "cpu 0 0 10 100 20 0 0 0\n",
		  "cpu 0 0 30 110 30 0 0 0\n", 50 },
		{ "four-field sample",
		  "cpu 100 10 20 100\n", "cpu 110 10 30 120\n", 50 },
		{ "decreasing iowait",
		  "cpu 10 0 0 20 50\n", "cpu 20 0 0 30 49\n", 50 },
		{ "no elapsed ticks",
		  "cpu 1 2 3 4 5 6 7 8\n", "cpu 1 2 3 4 5 6 7 8\n", 0 },
		{ "decreasing idle",
		  "cpu 10 0 0 20\n", "cpu 20 0 0 19\n", 0 },
		{ "incomplete sample",
		  "cpu 1 2 3\n", "cpu 20 0 0 30\n", 0 },
		{ "per-CPU line",
		  "cpu0 10 0 0 20\n", "cpu 20 0 0 30\n", 0 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct cpu_util_data data = { .enable = 1 };
		float actual;

		parse_sample(cases[i].start, &data.samples[0]);
		parse_sample(cases[i].end, &data.samples[1]);
		actual = calc_cpu_util(&data);
		if (actual != cases[i].expected) {
			fprintf(stderr, "%s: expected %.2f%%, got %.2f%%\n",
				cases[i].name, cases[i].expected, actual);
			return EXIT_FAILURE;
		}
		printf("PASS %s: %.2f%%\n", cases[i].name, actual);
	}
	return EXIT_SUCCESS;
}
