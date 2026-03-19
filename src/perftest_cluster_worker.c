/*
 * MPI rank 0 for cluster mode: runs barrier protocol, gathers worker results,
 * writes JSON for the orchestrator. Uses mpi_loader (dlopen) like ib_* binaries.
 *
 * CLI: perftest_cluster_worker --result-kind {bw|lat} --num-workers N --output-file PATH
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpi_loader.h"
#include "perftest_parameters.h"
#include "perftest_cluster.h"

enum result_kind {
	RESULT_KIND_BW  = 0,
	RESULT_KIND_LAT = 1,
};

struct cli_args {
	enum result_kind kind;
	int              num_workers;
	const char      *output_file;
};

static void print_phase(const char *msg)
{
	cluster_infof("%s", msg);
}

static void usage(FILE *out)
{
	fputs(
		"Usage: perftest_cluster_worker --result-kind {bw|lat} "
		"--num-workers N --output-file PATH\n"
		"\n"
		"Internal MPI rank 0 process for perftest cluster mode.\n"
		"Launched by mpirun via the orchestrator; not intended for\n"
		"direct invocation.\n",
		out);
}

static int parse_args(int argc, char *argv[], struct cli_args *out)
{
	static struct option longopts[] = {
		{"result-kind", required_argument, NULL, 'k'},
		{"num-workers", required_argument, NULL, 'n'},
		{"output-file", required_argument, NULL, 'o'},
		{"help",        no_argument,       NULL, 'h'},
		{0, 0, 0, 0}
	};
	int kind_set = 0, workers_set = 0, output_set = 0;
	int c;

	memset(out, 0, sizeof(*out));
	out->num_workers = -1;

	while ((c = getopt_long(argc, argv, "k:n:o:h", longopts, NULL)) != -1) {
		switch (c) {
		case 'k':
			if (strcmp(optarg, "bw") == 0) {
				out->kind = RESULT_KIND_BW;
			} else if (strcmp(optarg, "lat") == 0) {
				out->kind = RESULT_KIND_LAT;
			} else {
				cluster_errf("invalid --result-kind: '%s' "
					"(expected 'bw' or 'lat')", optarg);
				return -1;
			}
			kind_set = 1;
			break;
		case 'n': {
			char *endptr;
			long val;

			errno = 0;
			val = strtol(optarg, &endptr, 10);
			if (errno != 0 || endptr == optarg || *endptr != '\0'
			    || val <= 0 || val > INT_MAX) {
				cluster_errf("--num-workers must be a positive "
					"integer, got '%s'", optarg);
				return -1;
			}
			out->num_workers = (int)val;
			workers_set = 1;
			break;
		}
		case 'o':
			out->output_file = optarg;
			output_set = 1;
			break;
		case 'h':
			usage(stdout);
			exit(0);
		default:
			usage(stderr);
			return -1;
		}
	}

	if (!kind_set || !workers_set || !output_set) {
		cluster_errf("--result-kind, --num-workers, and "
			"--output-file are all required");
		usage(stderr);
		return -1;
	}
	return 0;
}

/* Map ctx_report_fmt to unit string; default MB/s. */
static const char *bw_unit_string(int report_fmt)
{
	if (report_fmt == 0)
		return "Gb/s";
	return "MB/s";
}

/*
 * BW JSON: one entry per CLIENT rank. Duplex (-b) sums client + paired server
 * measurements (matches standalone perftest bidirectional total).
 */
static void emit_json_bw(FILE *fp, const struct cluster_bw_report *recvbuf,
                         int total_size)
{
	unsigned long size = 0;
	int report_fmt = 1;
	int first_client = 1;
	int num_workers = total_size - 1;
	int half = num_workers / 2;
	int i;

	for (i = 1; i < total_size; i++) {
		if (recvbuf[i].role == CLUSTER_ROLE_CLIENT) {
			size = recvbuf[i].bw_rep.size;
			report_fmt = recvbuf[i].report_fmt;
			break;
		}
	}

	fprintf(fp,
		"{\"result_kind\": \"bw\", \"size\": %lu, "
		"\"bw_unit\": \"%s\", \"ranks\": [",
		size, bw_unit_string(report_fmt));

	for (i = 1; i < total_size; i++) {
		const struct cluster_bw_report *r = &recvbuf[i];
		double bw_avg, msgrate_avg;
		int peer_idx;

		if (r->role != CLUSTER_ROLE_CLIENT)
			continue;

		bw_avg = r->bw_rep.bw_avg;
		msgrate_avg = r->bw_rep.msgRate_avg;

		peer_idx = i - half;
		if (half > 0 && peer_idx >= 1 && peer_idx < i) {
			const struct cluster_bw_report *peer = &recvbuf[peer_idx];

			if (peer->role == CLUSTER_ROLE_SERVER &&
			    peer->bw_rep.bw_avg > 0.0) {
				bw_avg += peer->bw_rep.bw_avg;
				msgrate_avg += peer->bw_rep.msgRate_avg;
			}
		}

		if (!first_client)
			fputs(", ", fp);
		first_client = 0;
		fprintf(fp,
			"{\"rank\": %d, \"bw\": %.2f, \"msgrate\": %.6f}",
			r->rank, bw_avg, msgrate_avg);
	}

	fputs("]", fp);

	/* Data validation: aggregate + per-rank entries for validating workers. */
	{
		int dv_enabled = 0;
		int dv_passed = 1;
		uint64_t dv_errors = 0;
		int first_dv = 1;

		for (i = 1; i < total_size; i++) {
			if (!recvbuf[i].dv_enabled)
				continue;
			dv_enabled = 1;
			if (!recvbuf[i].dv_passed)
				dv_passed = 0;
			dv_errors += recvbuf[i].dv_errors;
		}

		fprintf(fp,
			", \"data_validation\": {\"enabled\": %d, "
			"\"passed\": %d, \"errors\": %" PRIu64 ", \"workers\": [",
			dv_enabled, dv_enabled ? dv_passed : 1, dv_errors);

		for (i = 1; i < total_size; i++) {
			const struct cluster_bw_report *r = &recvbuf[i];
			if (!r->dv_enabled)
				continue;
			if (!first_dv)
				fputs(", ", fp);
			first_dv = 0;
			fprintf(fp,
				"{\"rank\": %d, \"role\": %d, \"passed\": %d, "
				"\"errors\": %" PRIu64 ", \"bytes\": %" PRIu64 ", "
				"\"chunks\": %" PRIu64 "}",
				r->rank, r->role, r->dv_passed,
				(uint64_t)r->dv_errors, (uint64_t)r->dv_bytes,
				(uint64_t)r->dv_chunks);
		}

		fputs("]}", fp);
	}

	fputs("}", fp);
}

/* LAT JSON: all workers; orchestrator filters by role at render time. */
static void emit_json_lat(FILE *fp, const struct cluster_lat_report *recvbuf,
                          int total_size)
{
	int first = 1;
	int i;

	fputs("{\"result_kind\": \"lat\", \"workers\": [", fp);

	for (i = 1; i < total_size; i++) {
		const struct cluster_lat_report *r = &recvbuf[i];
		if (!first)
			fputs(", ", fp);
		first = 0;
		fprintf(fp,
			"{\"rank\": %d, \"role\": %d, \"size\": %lu, "
			"\"iters\": %" PRIu64 ", \"test_type\": %d, "
			"\"t_avg\": %.3f, \"t_min\": %.3f, \"t_max\": %.3f, "
			"\"t_typical\": %.3f, \"stdev\": %.3f, "
			"\"p99\": %.3f, \"p99_9\": %.3f, \"tps\": %.2f}",
			r->rank, r->role, r->size,
			(uint64_t)r->iters, r->test_type,
			r->t_avg, r->t_min, r->t_max, r->t_typical,
			r->stdev, r->p99, r->p99_9, r->tps);
	}

	fputs("]}", fp);
}

/* Validate CLUSTER_ABI_VERSION on gathered worker slots [1..N). */
static int check_gathered_abi(const void *recvbuf, size_t elem_size,
			      int total_size, const char *kind)
{
	int i;

	for (i = 1; i < total_size; i++) {
		const unsigned char *elem =
			(const unsigned char *)recvbuf + (size_t)i * elem_size;
		uint32_t ver;

		memcpy(&ver, elem, sizeof(ver));
		if (ver != CLUSTER_ABI_VERSION) {
			cluster_errf("%s result from rank %d has ABI version %u, "
				"expected %u - rebuild ALL perftest binaries "
				"(ib_* and perftest_cluster_worker) together",
				kind, i, ver, CLUSTER_ABI_VERSION);
			return -1;
		}
	}
	return 0;
}

/* Close result file; check ferror and fclose return. */
static int close_output_file(FILE *fp, const char *path)
{
	int write_failed = ferror(fp);

	if (fclose(fp) != 0 || write_failed) {
		cluster_errf("Error writing result file '%s': %s",
			path, strerror(errno));
		return -1;
	}
	return 0;
}

static int gather_and_emit_bw(const struct cli_args *args, int total_size)
{
	struct cluster_bw_report  sendbuf;
	struct cluster_bw_report *recvbuf;
	FILE *fp;

	memset(&sendbuf, 0, sizeof(sendbuf));
	sendbuf.abi_version = CLUSTER_ABI_VERSION;

	recvbuf = calloc((size_t)total_size, sizeof(*recvbuf));
	if (!recvbuf) {
		cluster_errf("out of memory allocating BW recvbuf");
		return -1;
	}

	if (mpi_gather_to_root(&sendbuf, recvbuf, sizeof(sendbuf)) != 0) {
		cluster_errf("MPI_Gather (BW) failed");
		free(recvbuf);
		return -1;
	}

	if (check_gathered_abi(recvbuf, sizeof(*recvbuf), total_size, "BW") != 0) {
		free(recvbuf);
		return -1;
	}

	fp = fopen(args->output_file, "w");
	if (!fp) {
		cluster_errf("Could not open output file '%s' for writing",
			args->output_file);
		free(recvbuf);
		return -1;
	}
	emit_json_bw(fp, recvbuf, total_size);
	if (close_output_file(fp, args->output_file) != 0) {
		free(recvbuf);
		return -1;
	}
	free(recvbuf);
	return 0;
}

static int gather_and_emit_lat(const struct cli_args *args, int total_size)
{
	struct cluster_lat_report  sendbuf;
	struct cluster_lat_report *recvbuf;
	FILE *fp;

	memset(&sendbuf, 0, sizeof(sendbuf));
	sendbuf.abi_version = CLUSTER_ABI_VERSION;

	recvbuf = calloc((size_t)total_size, sizeof(*recvbuf));
	if (!recvbuf) {
		cluster_errf("out of memory allocating LAT recvbuf");
		return -1;
	}

	if (mpi_gather_to_root(&sendbuf, recvbuf, sizeof(sendbuf)) != 0) {
		cluster_errf("MPI_Gather (LAT) failed");
		free(recvbuf);
		return -1;
	}

	if (check_gathered_abi(recvbuf, sizeof(*recvbuf), total_size, "LAT") != 0) {
		free(recvbuf);
		return -1;
	}

	fp = fopen(args->output_file, "w");
	if (!fp) {
		cluster_errf("Could not open output file '%s' for writing",
			args->output_file);
		free(recvbuf);
		return -1;
	}
	emit_json_lat(fp, recvbuf, total_size);
	if (close_output_file(fp, args->output_file) != 0) {
		free(recvbuf);
		return -1;
	}
	free(recvbuf);
	return 0;
}

int main(int argc, char *argv[])
{
	struct cli_args args;
	int total_size;
	int rc;

	if (parse_args(argc, argv, &args) != 0)
		return 1;

	if (mpi_try_init(&argc, &argv) != 0) {
		cluster_errf("MPI initialization failed");
		return 1;
	}

	if (!g_mpi.available) {
		cluster_errf("perftest_cluster_worker requires running under mpirun "
			"with size > 1; libmpi.so not loaded or only 1 rank.");
		mpi_finalize();
		return 1;
	}

	if (g_mpi.rank != 0) {
		cluster_errf("perftest_cluster_worker must run as rank 0, got rank %d",
			g_mpi.rank);
		mpi_finalize();
		return 1;
	}

	total_size = args.num_workers + 1;
	if (g_mpi.size != total_size) {
		cluster_errf("expected %d total MPI ranks "
			"(%d workers + 1 orchestrator), got %d",
			total_size, args.num_workers, g_mpi.size);
		mpi_finalize();
		return 1;
	}

	if (mpi_check_version_compatibility() != 0) {
		mpi_finalize();
		return 1;
	}

	/* Barrier sequence must match ib_* workers (see cluster_phase_t). */

	print_phase("Connecting...");
	cluster_barrier(CLUSTER_PHASE_PRE_HANDSHAKE);

	cluster_barrier(CLUSTER_PHASE_RESOURCES);
	print_phase("Resources ready");

	cluster_barrier(CLUSTER_PHASE_CONNECTED);

	print_phase("Running traffic...");
	cluster_barrier(CLUSTER_PHASE_TRAFFIC);

	print_phase("Collecting results...");

	if (args.kind == RESULT_KIND_BW)
		rc = gather_and_emit_bw(&args, total_size);
	else
		rc = gather_and_emit_lat(&args, total_size);

	mpi_finalize();
	return rc != 0 ? 1 : 0;
}
