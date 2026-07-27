#ifndef MUSA_LOADER_H
#define MUSA_LOADER_H

#include <stddef.h>

#include "config.h"
#include MUSA_PATH

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	void **func_ptr;
	const char *name;
} MusaSymbol;

extern MUresult (*p_muInit)(unsigned int);
extern MUresult (*p_muDeviceGetCount)(int *);
extern MUresult (*p_muDeviceGet)(MUdevice *, int);
extern MUresult (*p_muDeviceGetAttribute)(int *, MUdevice_attribute, MUdevice);
extern MUresult (*p_muDeviceGetName)(char *, int, MUdevice);
extern MUresult (*p_muCtxCreate)(MUcontext *, unsigned int, MUdevice);
extern MUresult (*p_muCtxSetCurrent)(MUcontext);
extern MUresult (*p_muCtxDestroy)(MUcontext);
extern MUresult (*p_muDeviceGetByPCIBusId)(MUdevice *, const char *);
extern MUresult (*p_muMemAllocHost)(void **, size_t);
extern MUresult (*p_muMemAlloc)(MUdeviceptr *, size_t);
extern MUresult (*p_muMemFreeHost)(void *);
extern MUresult (*p_muMemFree)(MUdeviceptr);
extern MUresult (*p_muMemcpy)(MUdeviceptr, MUdeviceptr, size_t);
extern MUresult (*p_muMemcpyDtoD)(MUdeviceptr, MUdeviceptr, size_t);
#ifdef HAVE_MUSA_DMABUF
extern MUresult (*p_muMemGetHandleForAddressRange)(void *, MUdeviceptr, size_t,
						   MUmemRangeHandleType,
						   unsigned long long);
#endif

int load_musa_library(void);
void unload_musa_library(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSA_LOADER_H */
