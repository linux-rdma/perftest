#ifndef MPI_LOADER_H
#define MPI_LOADER_H

#include <stddef.h>

/*
 * Runtime MPI loader via dlopen — no #include <mpi.h>.
 * Open MPI handles are opaque pointers resolved with dlsym.
 */
typedef void* mpi_comm_t;
typedef void* mpi_datatype_t;

typedef struct {
	void *handle;           /* dlopen handle for libmpi.so */
	int available;          /* 1 if MPI loaded and size > 1 */
	int rank;               /* MPI rank (0 = cluster worker) */
	int size;               /* total MPI ranks */

	/* Open MPI opaque objects (dlsym) */
	mpi_comm_t     comm_world;   /* ompi_mpi_comm_world */
	mpi_datatype_t type_byte;    /* ompi_mpi_byte */

	/* MPI entry points (dlsym) */
	int (*MPI_Init)(int*, char***);
	int (*MPI_Finalize)(void);
	int (*MPI_Comm_rank)(mpi_comm_t, int*);
	int (*MPI_Comm_size)(mpi_comm_t, int*);
	int (*MPI_Barrier)(mpi_comm_t);
	int (*MPI_Abort)(mpi_comm_t, int);
	int (*MPI_Gather)(void*, int, mpi_datatype_t,
			  void*, int, mpi_datatype_t,
			  int, mpi_comm_t);
	int (*MPI_Bcast)(void*, int, mpi_datatype_t, int, mpi_comm_t);
} mpi_context_t;

extern mpi_context_t g_mpi;

/*
 * Load MPI via dlopen. Standalone (no mpirun env): no-op, returns 0.
 * Under mpirun: any failure returns -1 (no silent fallback).
 * PERFTEST_FORCE_MPI=1 bypasses the standalone probe.
 */
int mpi_try_init(int *argc, char ***argv);

/* Barrier; no-op if MPI unavailable. */
int mpi_barrier(void);

/* Abort all ranks in MPI_COMM_WORLD; returns only if abort fails/unavailable. */
int mpi_abort(int error_code);

/* Gather elem_size bytes from each rank to root 0; no-op if unavailable. */
int mpi_gather_to_root(const void *send, void *recv, size_t elem_size);

/*
 * Verify MPI >= 3 and matching version on all ranks (collective).
 * Call from every rank after mpi_try_init(), before the first barrier.
 */
int mpi_check_version_compatibility(void);

/* Finalize MPI and close the dlopen handle. */
void mpi_finalize(void);

#endif /* MPI_LOADER_H */
