#ifndef CUDA_LOADER_H
#define CUDA_LOADER_H

#include "config.h"
#include CUDA_PATH

#ifdef __cplusplus
extern "C" {
#endif


#define CUDA_VER_2_0   2000 /* CUDA 2.0  */
#define CUDA_VER_2_2   2020 /* CUDA 2.2  */
#define CUDA_VER_3_2   3020 /* CUDA 3.2  */
#define CUDA_VER_4_0   4000 /* CUDA 4.0  */
#define CUDA_VER_7_0   7000 /* CUDA 7.0  */
#define CUDA_VER_11_3  11030 /* CUDA 11.3 */
#define CUDA_VER_11_7  11070 /* CUDA 11.7 */


typedef struct {
    void **func_ptr;
    const char *name;
    int  min_version;
} CudaSymbol;

/* Function pointers for CUDA Driver API */
extern CUresult (*p_cuInit)(unsigned int);
extern CUresult (*p_cuDeviceGetCount)(int *);
extern CUresult (*p_cuDeviceGet)(CUdevice *, int);
extern CUresult (*p_cuDeviceGetAttribute)(int *, CUdevice_attribute, CUdevice);
extern CUresult (*p_cuDeviceGetName)(char *, int, CUdevice);
extern CUresult (*p_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
extern CUresult (*p_cuDevicePrimaryCtxRetain)(CUcontext *, CUdevice);
extern CUresult (*p_cuCtxSetCurrent)(CUcontext);
extern CUresult (*p_cuCtxDestroy)(CUcontext);
extern CUresult (*p_cuDeviceGetByPCIBusId)(int *, const char *);
extern CUresult (*p_cuMemAllocHost)(void **, size_t);
extern CUresult (*p_cuMemAlloc)(CUdeviceptr *, size_t);
extern CUresult (*p_cuMemFreeHost)(void *);
extern CUresult (*p_cuMemFree)(CUdeviceptr);
extern CUresult (*p_cuMemcpy)(CUdeviceptr, CUdeviceptr, size_t);
extern CUresult (*p_cuMemcpyDtoD)(CUdeviceptr, CUdeviceptr, size_t);
#ifdef HAVE_CUDA_DMABUF
extern CUresult (*p_cuMemGetHandleForAddressRange)(void *, void *, size_t, CUmemRangeHandleType, unsigned int);
#endif
extern CUresult (*p_cuDriverGetVersion)(int* driverVersion);
extern CUresult (*p_cuCtxSynchronize) (void);
extern CUresult (*p_cuMemAllocManaged)(CUdeviceptr* dptr, size_t bytesize, unsigned int  flags);
#if CUDA_VER >= 12000
extern CUresult (*p_cuGetProcAddress)(const char* symbol, void** pfn, int  cudaVersion, uint64_t flags, CUdriverProcAddressQueryResult* symbolStatus);
#else
extern CUresult (*p_cuGetProcAddress)(const char* symbol, void** pfn, int  cudaVersion, uint64_t flags);
#endif

/* Loader functions for Driver API */
int load_cuda_library(void);
void unload_cuda_library(void);

/* CUDA Runtime API function pointers (loaded from libcudart.so) */
typedef int cudaError_t_dyn;
typedef int cudaMemcpyKind_dyn;

extern cudaError_t_dyn (*p_cudaSetDevice)(int);
extern cudaError_t_dyn (*p_cudaMalloc)(void**, size_t);
extern cudaError_t_dyn (*p_cudaMallocManaged)(void**, size_t, unsigned int);
extern cudaError_t_dyn (*p_cudaFree)(void*);
extern cudaError_t_dyn (*p_cudaMemset)(void*, int, size_t);
extern cudaError_t_dyn (*p_cudaMemcpy)(void*, const void*, size_t, int);
extern cudaError_t_dyn (*p_cudaDeviceSynchronize)(void);
extern cudaError_t_dyn (*p_cudaGetLastError)(void);
extern const char* (*p_cudaGetErrorString)(int);

/* Loader functions for Runtime API */
int load_cudart_library(void);
void unload_cudart_library(void);

/* Kernel Plugin function pointers (loaded from libperftest_kernels.so) */
typedef int (*validation_init_fn)(
    void *buffer_base,
    uint64_t markers_offset,
    uint64_t recv_slots_offset,
    uint64_t payload_size,
    uint32_t tx_depth,
    uint32_t num_qps,
    uint32_t chunks_per_qp,
    int validation_mode,
    int cuda_device_id,
    int debug_enabled
);

typedef int (*validation_start_fn)(
    void *params,
    int num_blocks,
    int threads_per_block
);

typedef int (*validation_stop_fn)(void);

typedef int (*validation_destroy_fn)(void);

typedef int (*validation_get_stats_fn)(
    uint64_t *chunks_validated,
    uint64_t *bytes_validated,
    uint64_t *errors_found,
    uint64_t *race_overwrites,
    uint64_t *dma_stale_retries
);

typedef int (*validation_get_error_fn)(
    uint32_t *qp_id,
    uint32_t *chunk_id,
    uint64_t *byte_offset,
    uint8_t *expected,
    uint8_t *actual
);

typedef const char* (*validation_strerror_fn)(int err);
typedef int (*validation_is_running_fn)(void);
typedef int (*validation_check_status_fn)(void);

/* GPU touch function pointer types */
typedef int (*touch_gpu_pages_fn)(
    uint8_t *addr,
    int buf_size,
    int is_infinite,
    volatile int **stop_flag
);

typedef int (*init_gpu_stop_flag_fn)(
    volatile int **stop_flag
);

/* Kernel plugin function pointers */
extern validation_init_fn p_validation_init;
extern validation_start_fn p_validation_start;
extern validation_stop_fn p_validation_stop;
extern validation_destroy_fn p_validation_destroy;
extern validation_get_stats_fn p_validation_get_stats;
extern validation_get_error_fn p_validation_get_error;

/* GPU touch function pointers */
extern touch_gpu_pages_fn p_touch_gpu_pages;
extern init_gpu_stop_flag_fn p_init_gpu_stop_flag;

/* Additional plugin functions */
extern validation_strerror_fn p_validation_strerror;
extern validation_is_running_fn p_validation_is_running;
extern validation_check_status_fn p_validation_check_status;

/* Loader functions for kernel plugin */
int load_kernel_plugin(void);
void unload_kernel_plugin(void);

#ifdef __cplusplus
}
#endif

#endif /* CUDA_LOADER_H */
