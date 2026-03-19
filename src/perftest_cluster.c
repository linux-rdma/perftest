/*
 * perftest_cluster.c — implementation; see perftest_cluster.h for the API
 * and the ABI/barrier-protocol contract.
 */

#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "perftest_cluster.h"
#include "mpi_loader.h"
#include "memory.h"

/* Last computed latency report — filled by cluster_capture_lat_*,
 * consumed by cluster_report_lat(). */
static struct cluster_lat_report g_cluster_lat_rep;

void store_cluster_lat_report(const struct cluster_lat_report *rep)
{
	g_cluster_lat_rep = *rep;
}

const struct cluster_lat_report *get_cluster_lat_report(void)
{
	return &g_cluster_lat_rep;
}

int cluster_init(int *argc, char ***argv)
{
	if (mpi_try_init(argc, argv)) {
		cluster_fatalf("MPI initialization failed");
		return -1;
	}
	if (mpi_check_version_compatibility()) {
		mpi_finalize();
		return -1;
	}
	return 0;
}

void cluster_report_bw(const struct perftest_parameters *user_param,
		       const struct bw_report_data *bw,
		       const struct data_validation_result *dv)
{
	struct cluster_bw_report my_report;

	if (!g_mpi.available)
		return;

	memset(&my_report, 0, sizeof(my_report));
	my_report.abi_version = CLUSTER_ABI_VERSION;
	if (bw)
		my_report.bw_rep = *bw;
	my_report.role = (user_param->machine == CLIENT) ?
		CLUSTER_ROLE_CLIENT : CLUSTER_ROLE_SERVER;
	my_report.rank = g_mpi.rank;
	my_report.report_fmt = user_param->report_fmt;

	if (dv) {
		my_report.dv_enabled = 1;
		my_report.dv_passed  = dv->passed;
		my_report.dv_errors  = dv->errors_found;
		my_report.dv_bytes   = dv->bytes_validated;
		my_report.dv_chunks  = dv->chunks_validated;
	}

	/* Non-root sender: recv is NULL, gathered to rank 0. */
	mpi_gather_to_root(&my_report, NULL, sizeof(my_report));
}

void cluster_report_lat(struct perftest_parameters *user_param)
{
	struct cluster_lat_report my_report;

	if (!g_mpi.available)
		return;

	my_report = *get_cluster_lat_report();

	my_report.abi_version = CLUSTER_ABI_VERSION;
	my_report.role = (user_param->machine == CLIENT) ?
		CLUSTER_ROLE_CLIENT : CLUSTER_ROLE_SERVER;
	my_report.rank = g_mpi.rank;

	/* Non-root sender: recv is NULL, gathered to rank 0. */
	mpi_gather_to_root(&my_report, NULL, sizeof(my_report));
}

void cluster_capture_lat_iter(unsigned long size, uint64_t iters,
			      double t_min, double t_max, double t_typical,
			      double t_avg, double stdev,
			      double p99, double p99_9)
{
	struct cluster_lat_report rep;

	if (!g_mpi.available)
		return;

	memset(&rep, 0, sizeof(rep));
	rep.abi_version = CLUSTER_ABI_VERSION;
	rep.size        = size;
	rep.iters       = iters;
	rep.t_min       = t_min;
	rep.t_max       = t_max;
	rep.t_typical   = t_typical;
	rep.t_avg       = t_avg;
	rep.stdev       = stdev;
	rep.p99         = p99;
	rep.p99_9       = p99_9;
	rep.tps         = 0.0;
	rep.test_type   = CLUSTER_TEST_ITERATIONS;
	store_cluster_lat_report(&rep);
}

void cluster_capture_lat_duration(unsigned long size, uint64_t iters,
				  double t_avg, double tps)
{
	struct cluster_lat_report rep;

	if (!g_mpi.available)
		return;

	memset(&rep, 0, sizeof(rep));
	rep.abi_version = CLUSTER_ABI_VERSION;
	rep.size        = size;
	rep.iters       = iters;
	rep.t_avg       = t_avg;
	rep.tps         = tps;
	rep.test_type   = CLUSTER_TEST_DURATION;
	store_cluster_lat_report(&rep);
}

void cluster_barrier(cluster_phase_t phase)
{
#ifndef NDEBUG
	/* Verify the documented phase order within each test cycle. The cycle
	 * restarts whenever phase 0 is seen, so repeated cycles are fine. */
	static int expected = CLUSTER_PHASE_PRE_HANDSHAKE;

	if (phase == CLUSTER_PHASE_PRE_HANDSHAKE)
		expected = CLUSTER_PHASE_PRE_HANDSHAKE;
	assert(phase == expected && "cluster barrier phases issued out of order");
	expected = (phase + 1) % CLUSTER_PHASE__COUNT;
#else
	(void)phase;
#endif
	mpi_barrier();
}

void cluster_barrier_pre_handshake(const struct perftest_parameters *user_param)
{
	cluster_barrier(CLUSTER_PHASE_PRE_HANDSHAKE);

	/* Give the server a moment to reach its listen/accept before the
	 * client dials in (avoids a connect race under mpirun co-location). */
	if (user_param->machine == CLIENT && g_mpi.available)
		usleep(CLUSTER_CLIENT_SETTLE_US);
}
