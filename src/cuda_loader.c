#include "cuda_loader.h"
#include <dlfcn.h>
#include <stdio.h>

static void *cuda_handle = NULL;

/* Define the function pointers */
CUresult (*p_cuInit)(unsigned int) = NULL;
CUresult (*p_cuDeviceGetCount)(int *) = NULL;
CUresult (*p_cuDeviceGet)(CUdevice *, int) = NULL;
CUresult (*p_cuDeviceGetAttribute)(int *, CUdevice_attribute, CUdevice) = NULL;
CUresult (*p_cuDeviceGetName)(char *, int, CUdevice) = NULL;
#if CUDA_VERSION >= 13000
CUresult (*p_cuCtxCreate)(CUcontext *, CUctxCreateParams *, unsigned int, CUdevice) = NULL;
#else
CUresult (*p_cuCtxCreate)(CUcontext *, unsigned int, CUdevice) = NULL;
#endif
CUresult (*p_cuDevicePrimaryCtxRetain)(CUcontext *, CUdevice) = NULL;
CUresult (*p_cuCtxSetCurrent)(CUcontext) = NULL;
CUresult (*p_cuCtxDestroy)(CUcontext) = NULL;
CUresult (*p_cuDeviceGetByPCIBusId)(int *, const char *) = NULL;
CUresult (*p_cuMemAllocHost)(void **, size_t) = NULL;
CUresult (*p_cuMemAlloc)(CUdeviceptr *, size_t) = NULL;
CUresult (*p_cuMemFreeHost)(void *) = NULL;
CUresult (*p_cuMemFree)(CUdeviceptr) = NULL;
CUresult (*p_cuMemcpy)(CUdeviceptr, CUdeviceptr, size_t) = NULL;
CUresult (*p_cuMemcpyDtoD)(CUdeviceptr, CUdeviceptr, size_t) = NULL;
#ifdef HAVE_CUDA_DMABUF
CUresult (*p_cuMemGetHandleForAddressRange)(void *, void *, size_t, CUmemRangeHandleType, unsigned int) = NULL;
#endif
CUresult (*p_cuDriverGetVersion)(int* driverVersion) = NULL;
#if CUDA_VERSION >= 12000
CUresult (*p_cuGetProcAddress)(const char* symbol, void** pfn, int  cudaVersion, uint64_t flags, CUdriverProcAddressQueryResult* symbolStatus) = NULL;
#else
CUresult (*p_cuGetProcAddress)(const char* symbol, void** pfn, int  cudaVersion, uint64_t flags) = NULL;
#endif
CUresult (*p_cuMemAllocManaged)(CUdeviceptr* dptr, size_t bytesize, unsigned int  flags) = NULL;
CUresult (*p_cuCtxSynchronize) (void) = NULL;


int load_cuda_function(void **func_ptr, const char *func_name, int version) {
    #if CUDA_VERSION >= 12000
    CUresult res = p_cuGetProcAddress(func_name, func_ptr, version, 0, NULL);
    #else
    CUresult res = p_cuGetProcAddress(func_name, func_ptr, version, 0);
    #endif
    if (res != CUDA_SUCCESS)
    {
        fprintf(stderr, "load_cuda_function: Failed to get driver entry point '%s' (CUDA error %u)\n", func_name, res);
        return -1;
    }

    return 0;
}

int load_cuda_library(void) {

    cuda_handle = dlopen("libcuda.so", RTLD_LAZY);
    if (!cuda_handle) {
        fprintf(stderr, "dlerror: %s\n", dlerror());
        return -1;
    }


    p_cuGetProcAddress = dlsym(cuda_handle, "cuGetProcAddress");
    if (!p_cuGetProcAddress) {
        fprintf(stderr, "Failed to resolve cuGetProcAddress: %s\n", dlerror());
        unload_cuda_library();
        return -1;
    }


    static const CudaSymbol symbols[] = {
        { (void**)&p_cuInit,                      "cuInit",                       CUDA_VER_2_0  },
        { (void**)&p_cuDeviceGetCount,            "cuDeviceGetCount",             CUDA_VER_2_0  },
        { (void**)&p_cuDeviceGet,                 "cuDeviceGet",                  CUDA_VER_2_0  },
        { (void**)&p_cuDeviceGetAttribute,        "cuDeviceGetAttribute",         CUDA_VER_2_0  },
        { (void**)&p_cuDeviceGetName,             "cuDeviceGetName",              CUDA_VER_2_0  },
        { (void**)&p_cuCtxCreate,                 "cuCtxCreate",                  CUDA_VER_3_2  },
        { (void**)&p_cuDevicePrimaryCtxRetain,    "cuDevicePrimaryCtxRetain",     CUDA_VER_7_0  },
        { (void**)&p_cuCtxSetCurrent,             "cuCtxSetCurrent",              CUDA_VER_4_0  },
        { (void**)&p_cuCtxDestroy,                "cuCtxDestroy",                 CUDA_VER_4_0  },
        { (void**)&p_cuDeviceGetByPCIBusId,       "cuDeviceGetByPCIBusId",        CUDA_VER_11_3 },
        { (void**)&p_cuMemAllocHost,              "cuMemAllocHost",               CUDA_VER_3_2  },
        { (void**)&p_cuMemAlloc,                  "cuMemAlloc",                   CUDA_VER_3_2  },
        { (void**)&p_cuMemFreeHost,               "cuMemFreeHost",                CUDA_VER_3_2  },
        { (void**)&p_cuMemFree,                   "cuMemFree",                    CUDA_VER_3_2  },
        { (void**)&p_cuMemcpy,                    "cuMemcpy",                     CUDA_VER_11_3 },
        { (void**)&p_cuMemcpyDtoD,                "cuMemcpyDtoD",                 CUDA_VER_3_2  },
#ifdef HAVE_CUDA_DMABUF
        { (void**)&p_cuMemGetHandleForAddressRange, "cuMemGetHandleForAddressRange", CUDA_VER_11_7 },
#endif
        { (void**)&p_cuDriverGetVersion,          "cuDriverGetVersion",           CUDA_VER_2_2  },
        { (void**)&p_cuCtxSynchronize,            "cuCtxSynchronize",             CUDA_VER_11_3 },
        { (void**)&p_cuMemAllocManaged,           "cuMemAllocManaged",            CUDA_VER_11_3 }
    };

    for (size_t i = 0; i < sizeof(symbols)/sizeof(symbols[0]); ++i) {
        if (load_cuda_function(symbols[i].func_ptr, symbols[i].name, symbols[i].min_version) != 0) {
            unload_cuda_library();
            return -1;
        }
    }

    return 0;
}

void unload_cuda_library(void) {
    if (cuda_handle) {
        dlclose(cuda_handle);
        cuda_handle = NULL;
    }
}
