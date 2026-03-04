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
CUresult (*p_cuCtxCreate)(CUcontext *, unsigned int, CUdevice) = NULL;
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
#if CUDA_VER >= 12000
CUresult (*p_cuGetProcAddress)(const char* symbol, void** pfn, int  cudaVersion, uint64_t flags, CUdriverProcAddressQueryResult* symbolStatus) = NULL;
#else
CUresult (*p_cuGetProcAddress)(const char* symbol, void** pfn, int  cudaVersion, uint64_t flags) = NULL;
#endif
CUresult (*p_cuMemAllocManaged)(CUdeviceptr* dptr, size_t bytesize, unsigned int  flags) = NULL;
CUresult (*p_cuCtxSynchronize) (void) = NULL;

int load_cuda_function(void **func_ptr, const char *func_name, int version) {
    #if CUDA_VER >= 12000
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

/* CUDA Runtime API dynamic loading */
static void *cudart_handle = NULL;

cudaError_t_dyn (*p_cudaSetDevice)(int) = NULL;
cudaError_t_dyn (*p_cudaMalloc)(void**, size_t) = NULL;
cudaError_t_dyn (*p_cudaMallocManaged)(void**, size_t, unsigned int) = NULL;
cudaError_t_dyn (*p_cudaFree)(void*) = NULL;
cudaError_t_dyn (*p_cudaMemset)(void*, int, size_t) = NULL;
cudaError_t_dyn (*p_cudaMemcpy)(void*, const void*, size_t, int) = NULL;
cudaError_t_dyn (*p_cudaDeviceSynchronize)(void) = NULL;
cudaError_t_dyn (*p_cudaGetLastError)(void) = NULL;
const char* (*p_cudaGetErrorString)(int) = NULL;

int load_cudart_library(void) {
    if (cudart_handle)
        return 0;

    cudart_handle = dlopen("libcudart.so", RTLD_LAZY);
    if (!cudart_handle)
        cudart_handle = dlopen("libcudart.so.12", RTLD_LAZY);
    if (!cudart_handle) {
        cudart_handle = dlopen("libcudart.so.11", RTLD_LAZY);
    }
    if (!cudart_handle) {
        fprintf(stderr, "Failed to load CUDA runtime library: %s\n", dlerror());
        return -1;
    }

    p_cudaSetDevice = dlsym(cudart_handle, "cudaSetDevice");
    p_cudaMalloc = dlsym(cudart_handle, "cudaMalloc");
    p_cudaMallocManaged = dlsym(cudart_handle, "cudaMallocManaged");
    p_cudaFree = dlsym(cudart_handle, "cudaFree");
    p_cudaMemset = dlsym(cudart_handle, "cudaMemset");
    p_cudaMemcpy = dlsym(cudart_handle, "cudaMemcpy");
    p_cudaDeviceSynchronize = dlsym(cudart_handle, "cudaDeviceSynchronize");
    p_cudaGetLastError = dlsym(cudart_handle, "cudaGetLastError");
    p_cudaGetErrorString = dlsym(cudart_handle, "cudaGetErrorString");

    if (!p_cudaSetDevice || !p_cudaMalloc || !p_cudaFree ||
        !p_cudaMemset || !p_cudaMemcpy || !p_cudaDeviceSynchronize ||
        !p_cudaGetLastError || !p_cudaGetErrorString) {
        fprintf(stderr, "Failed to load required CUDA runtime symbols\n");
        unload_cudart_library();
        return -1;
    }

    return 0;
}

void unload_cudart_library(void) {
    if (cudart_handle) {
        dlclose(cudart_handle);
        cudart_handle = NULL;
    }
    p_cudaSetDevice = NULL;
    p_cudaMalloc = NULL;
    p_cudaMallocManaged = NULL;
    p_cudaFree = NULL;
    p_cudaMemset = NULL;
    p_cudaMemcpy = NULL;
    p_cudaDeviceSynchronize = NULL;
    p_cudaGetLastError = NULL;
    p_cudaGetErrorString = NULL;
}

/* Kernel Plugin dynamic loading */
static void *kernel_plugin_handle = NULL;

validation_init_fn p_validation_init = NULL;
validation_start_fn p_validation_start = NULL;
validation_stop_fn p_validation_stop = NULL;
validation_destroy_fn p_validation_destroy = NULL;
validation_get_stats_fn p_validation_get_stats = NULL;
validation_get_error_fn p_validation_get_error = NULL;

touch_gpu_pages_fn p_touch_gpu_pages = NULL;
init_gpu_stop_flag_fn p_init_gpu_stop_flag = NULL;

validation_strerror_fn p_validation_strerror = NULL;
validation_is_running_fn p_validation_is_running = NULL;
validation_check_status_fn p_validation_check_status = NULL;

int load_kernel_plugin(void) {
    if (kernel_plugin_handle)
        return 0;

    const char *paths[] = {
        "./libperftest_kernels.so",
        "/usr/lib/libperftest_kernels.so",
        "/usr/local/lib/libperftest_kernels.so",
        "/usr/lib64/libperftest_kernels.so",
        "libperftest_kernels.so",  /* Use LD_LIBRARY_PATH */
        NULL
    };

    for (int i = 0; paths[i] != NULL; i++) {
        kernel_plugin_handle = dlopen(paths[i], RTLD_LAZY);
        if (kernel_plugin_handle) {
            break;
        }
    }

    if (!kernel_plugin_handle) {
        fprintf(stderr, "Failed to load kernel plugin library: %s\n", dlerror());
        return -1;
    }

    p_validation_init = dlsym(kernel_plugin_handle, "validation_init");
    p_validation_start = dlsym(kernel_plugin_handle, "validation_start");
    p_validation_stop = dlsym(kernel_plugin_handle, "validation_stop");
    p_validation_destroy = dlsym(kernel_plugin_handle, "validation_destroy");
    p_validation_get_stats = dlsym(kernel_plugin_handle, "validation_get_stats");
    p_validation_get_error = dlsym(kernel_plugin_handle, "validation_get_error");

    p_touch_gpu_pages = dlsym(kernel_plugin_handle, "touch_gpu_pages");
    p_init_gpu_stop_flag = dlsym(kernel_plugin_handle, "init_gpu_stop_flag");

    p_validation_strerror = dlsym(kernel_plugin_handle, "validation_strerror");
    p_validation_is_running = dlsym(kernel_plugin_handle, "validation_is_running");
    p_validation_check_status = dlsym(kernel_plugin_handle, "validation_check_status");

    if (!p_validation_init || !p_validation_start ||
        !p_validation_stop || !p_validation_destroy ||
        !p_validation_get_stats || !p_validation_get_error) {
        fprintf(stderr, "Failed to load required kernel plugin symbols\n");
        unload_kernel_plugin();
        return -1;
    }

    /* GPU touch symbols are optional */
    if (!p_touch_gpu_pages || !p_init_gpu_stop_flag) {
        fprintf(stderr, "Warning: GPU touch functions not available in kernel plugin\n");
    }

    return 0;
}

void unload_kernel_plugin(void) {
    if (kernel_plugin_handle) {
        dlclose(kernel_plugin_handle);
        kernel_plugin_handle = NULL;
    }
    p_validation_init = NULL;
    p_validation_start = NULL;
    p_validation_stop = NULL;
    p_validation_destroy = NULL;
    p_validation_get_stats = NULL;
    p_validation_get_error = NULL;
    p_touch_gpu_pages = NULL;
    p_init_gpu_stop_flag = NULL;
    p_validation_strerror = NULL;
    p_validation_is_running = NULL;
    p_validation_check_status = NULL;
}
