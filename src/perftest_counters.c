#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "perftest_parameters.h"

#define COUNTER_PATH "/sys/class/infiniband/%s/ports/%i/%s"
#define COUNTER_VALUE_MAX_LEN (21)

typedef unsigned long long counter_t;

struct counter_context {
	char *counter_list;
	unsigned num_counters;
	struct {
		int fd;
		char *name;
		counter_t prev_value;
		counter_t last_value;
	} counters[];
};


static int counters_read(struct counter_context *ctx)
{
	char read_buf[COUNTER_VALUE_MAX_LEN];

	int i;
	for (i = 0; i < ctx->num_counters; i++) {
		int read_fd = ctx->counters[i].fd;
		if (read(read_fd, &read_buf, COUNTER_VALUE_MAX_LEN) < 0) {
			return -1;
		}

		ctx->counters[i].prev_value = ctx->counters[i].last_value;
		ctx->counters[i].last_value = strtoll(read_buf, NULL, 10);
		(void) lseek(read_fd, 0, SEEK_SET);
	}

	return SUCCESS;
}

int counters_alloc(const char *counter_names,
		struct counter_context **ctx)
{
	/* Count the number of commas and allocate accordingly */
	unsigned i, num_counters = (unsigned)(strlen(counter_names) > 0);
	for (i = 0; i < strlen(counter_names); i++) {
		if (counter_names[i] == ',') {
			num_counters++;
		}
	}

	ALLOCATE(*ctx, struct counter_context, 3 * num_counters + 1);
	(*ctx)->counter_list = strdup(counter_names);
	(*ctx)->num_counters = num_counters;
	return SUCCESS;
}

int counters_open(struct counter_context *ctx,
		const char *dev_name, int port)
{
	/* Open the sysfs file for each counter */
	int i;
	char *given_path, *real_path, *next_counter;
	for (i = 0, next_counter = strtok(ctx->counter_list, ",");
		 i < ctx->num_counters;
		 i++, next_counter = strtok(0, ",")) {
		if (asprintf(&given_path, COUNTER_PATH, dev_name, port, next_counter) == -1) {
			goto counter_cleanup;
		}

		real_path = realpath(given_path, NULL);
		if (!real_path) {
			goto counter_cleanup;
		}

		free(given_path);
		if (strstr(real_path, COUNTER_PATH) != 0) {
			free(real_path);
			goto counter_cleanup;
		}

		if ((ctx->counters[i].fd = open(real_path, O_RDONLY)) < 0) {
			free(real_path);
			goto counter_cleanup;
		}

		ctx->counters[i].name = next_counter;
		ctx->counters[i].last_value = 0;
		free(real_path);
	}

	return counters_read(ctx);

counter_cleanup:
	ctx->num_counters = i;
	counters_close(ctx);
	return FAILURE;
}

void counters_print(struct counter_context *ctx)
{
	(void) counters_read(ctx);

	int i;
	for (i = 0; i < ctx->num_counters; i++) {
		printf("\t%s=%llu\n", ctx->counters[i].name,
				ctx->counters[i].last_value - ctx->counters[i].prev_value);
	}
	printf("\n");
}

void counters_close(struct counter_context *ctx)
{
	int i;
	for (i = 0; i < ctx->num_counters; i++) {
		close(ctx->counters[i].fd);
	}

	free(ctx->counter_list);
	free(ctx);
}
