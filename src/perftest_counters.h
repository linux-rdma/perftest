#ifndef PERFTEST_COUNTERS_H
#define PERFTEST_COUNTERS_H

struct counter_context;

/*
 * Allocate context for performance counters.
 */
int counters_alloc(const char *counter_names,
		struct counter_context **ctx);

/*
 * Open a handle to the counters (and sample once).
 */
int counters_open(struct counter_context *ctx,
		const char *dev_name, int port);

/*
 * Sample and output the values to STDOUT.
 */
void counters_print(struct counter_context *ctx);

/*
 * Close the handle to the counters.
 */
void counters_close(struct counter_context *ctx);

#endif
