#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "mpi_loader.h"

mpi_context_t g_mpi = {0};

/* True if mpirun/PMIx env is present (PERFTEST_FORCE_MPI=1 overrides). */
static int launched_under_mpirun(void)
{
	static const char * const sentinels[] = {
		"OMPI_COMM_WORLD_SIZE",
		"OMPI_COMM_WORLD_RANK",
		"PMIX_RANK",
		"PMI_RANK",
		NULL,
	};
	int i;
	const char *force = getenv("PERFTEST_FORCE_MPI");

	if (force && strcmp(force, "1") == 0)
		return 1;

	for (i = 0; sentinels[i]; ++i)
		if (getenv(sentinels[i]))
			return 1;
	return 0;
}

/* dlsym Open MPI's comm_world and byte datatype; returns 0 or -1. */
static int resolve_mpi_constants(void)
{
	g_mpi.comm_world = (mpi_comm_t)dlsym(g_mpi.handle, "ompi_mpi_comm_world");
	if (!g_mpi.comm_world) {
		fprintf(stderr, "[Cluster] ERROR: Cannot resolve ompi_mpi_comm_world. "
			"Is this Open MPI?\n");
		return -1;
	}

	g_mpi.type_byte = (mpi_datatype_t)dlsym(g_mpi.handle, "ompi_mpi_byte");
	if (!g_mpi.type_byte) {
		fprintf(stderr, "[Cluster] ERROR: Cannot resolve ompi_mpi_byte. "
			"Is this Open MPI?\n");
		return -1;
	}

	return 0;
}

/* dlsym MPI entry points from libmpi.so; returns 0 or -1. */
static int resolve_mpi_functions(void)
{
	static const struct {
		const char *name;
		void      **slot;
	} mpi_syms[] = {
		{ "MPI_Init",      (void **)&g_mpi.MPI_Init      },
		{ "MPI_Finalize",  (void **)&g_mpi.MPI_Finalize  },
		{ "MPI_Comm_rank", (void **)&g_mpi.MPI_Comm_rank },
		{ "MPI_Comm_size", (void **)&g_mpi.MPI_Comm_size },
		{ "MPI_Barrier",   (void **)&g_mpi.MPI_Barrier   },
		{ "MPI_Gather",    (void **)&g_mpi.MPI_Gather    },
		{ "MPI_Bcast",     (void **)&g_mpi.MPI_Bcast     },
	};
	size_t i;

	for (i = 0; i < sizeof(mpi_syms) / sizeof(mpi_syms[0]); ++i) {
		*mpi_syms[i].slot = dlsym(g_mpi.handle, mpi_syms[i].name);
		if (!*mpi_syms[i].slot) {
			fprintf(stderr, "[Cluster] ERROR: Cannot resolve %s "
				"from libmpi.so\n", mpi_syms[i].name);
			return -1;
		}
	}

	return 0;
}

int mpi_try_init(int *argc, char ***argv)
{
	struct sigaction saved_alarm, saved_usr1;

	if (!launched_under_mpirun()) {
		g_mpi.available = 0;
		g_mpi.rank = 0;
		g_mpi.size = 1;
		return 0;
	}

	/* Save perftest handlers; MPI_Init may overwrite them. */
	sigaction(SIGALRM, NULL, &saved_alarm);
	sigaction(SIGUSR1, NULL, &saved_usr1);

	/* Under mpirun, all failures below are fatal (see mpi_loader.h). */

	g_mpi.handle = dlopen("libmpi.so", RTLD_NOW | RTLD_GLOBAL);
	if (!g_mpi.handle) {
		fprintf(stderr, "[Cluster] ERROR: dlopen(\"libmpi.so\") failed "
			"under mpirun: %s\n", dlerror());
		g_mpi.available = 0;
		return -1;
	}

	if (resolve_mpi_functions() != 0) {
		dlclose(g_mpi.handle);
		g_mpi.handle = NULL;
		g_mpi.available = 0;
		return -1;
	}

	if (resolve_mpi_constants() != 0) {
		dlclose(g_mpi.handle);
		g_mpi.handle = NULL;
		g_mpi.available = 0;
		return -1;
	}

	if (g_mpi.MPI_Init(argc, argv) != 0) {
		fprintf(stderr, "[Cluster] ERROR: MPI_Init failed\n");
		dlclose(g_mpi.handle);
		g_mpi.handle = NULL;
		g_mpi.available = 0;
		return -1;
	}
	if (g_mpi.MPI_Comm_rank(g_mpi.comm_world, &g_mpi.rank) != 0) {
		fprintf(stderr, "[Cluster] ERROR: MPI_Comm_rank failed\n");
		g_mpi.MPI_Finalize();
		dlclose(g_mpi.handle);
		g_mpi.handle = NULL;
		g_mpi.available = 0;
		return -1;
	}
	if (g_mpi.MPI_Comm_size(g_mpi.comm_world, &g_mpi.size) != 0) {
		fprintf(stderr, "[Cluster] ERROR: MPI_Comm_size failed\n");
		g_mpi.MPI_Finalize();
		dlclose(g_mpi.handle);
		g_mpi.handle = NULL;
		g_mpi.available = 0;
		return -1;
	}

	g_mpi.available = (g_mpi.size > 1);

	/* Restore handlers MPI_Init may have overwritten. */
	sigaction(SIGALRM, &saved_alarm, NULL);
	sigaction(SIGUSR1, &saved_usr1, NULL);

	return 0;
}

int mpi_barrier(void)
{
	if (g_mpi.available) {
		return g_mpi.MPI_Barrier(g_mpi.comm_world);
	}
	return 0;
}

int mpi_gather_to_root(const void *send, void *recv, size_t elem_size)
{
	if (!g_mpi.available)
		return 0;

	return g_mpi.MPI_Gather((void *)send, (int)elem_size, g_mpi.type_byte,
				recv, (int)elem_size, g_mpi.type_byte,
				0, g_mpi.comm_world);
}

int mpi_check_version_compatibility(void)
{
	if (!g_mpi.available)
		return 0;

	int major = 0, minor = 0;
	int (*MPI_Get_version)(int*, int*) =
		dlsym(g_mpi.handle, "MPI_Get_version");
	if (!MPI_Get_version) {
		fprintf(stderr, "[Cluster] WARNING: MPI_Get_version symbol not "
			"found; skipping MPI version compatibility check on "
			"rank %d.\n", g_mpi.rank);
		return 0;
	}

	if (MPI_Get_version(&major, &minor) != 0) {
		fprintf(stderr, "[Cluster] ERROR: MPI_Get_version failed on "
			"rank %d\n", g_mpi.rank);
		return -1;
	}

	/* Require MPI 3.0+ on every rank. */
	if (major < 3) {
		fprintf(stderr, "[Cluster] ERROR: MPI version %d.%d detected on "
			"rank %d. Open MPI 3.0+ is required for cluster mode.\n",
			major, minor, g_mpi.rank);
		return -1;
	}

	/* Rank 0 broadcasts (major, minor); all ranks must match. */
	int root_version[2] = { major, minor };
	if (g_mpi.MPI_Bcast(root_version, (int)sizeof(root_version),
	                    g_mpi.type_byte, 0, g_mpi.comm_world) != 0) {
		fprintf(stderr, "[Cluster] ERROR: MPI_Bcast failed on rank %d "
			"during version compatibility check; treating as a "
			"fatal mixed-MPI-installation symptom.\n", g_mpi.rank);
		return -1;
	}

	if (root_version[0] != major || root_version[1] != minor) {
		fprintf(stderr, "[Cluster] ERROR: MPI version mismatch on rank %d: "
			"rank 0 reports %d.%d, this rank reports %d.%d. "
			"All hosts must run the same Open MPI release.\n",
			g_mpi.rank, root_version[0], root_version[1],
			major, minor);
		return -1;
	}

	return 0;
}

void mpi_finalize(void)
{
	if (g_mpi.available && g_mpi.MPI_Finalize) {
		g_mpi.MPI_Finalize();
	}
	if (g_mpi.handle) {
		dlclose(g_mpi.handle);
		g_mpi.handle = NULL;
	}
}
