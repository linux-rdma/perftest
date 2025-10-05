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

// Function pointers for CUDA Driver API
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

// Loader functions
int load_cuda_library(void);
void unload_cuda_library(void);

#ifdef __cplusplus
}
#endif

#endif // CUDA_LOADER_H