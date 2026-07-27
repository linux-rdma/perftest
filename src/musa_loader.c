#include "musa_loader.h"

#include <dlfcn.h>
#include <stdio.h>

static void *musa_handle;

MUresult (*p_muInit)(unsigned int);
MUresult (*p_muDeviceGetCount)(int *);
MUresult (*p_muDeviceGet)(MUdevice *, int);
MUresult (*p_muDeviceGetAttribute)(int *, MUdevice_attribute, MUdevice);
MUresult (*p_muDeviceGetName)(char *, int, MUdevice);
MUresult (*p_muCtxCreate)(MUcontext *, unsigned int, MUdevice);
MUresult (*p_muCtxSetCurrent)(MUcontext);
MUresult (*p_muCtxDestroy)(MUcontext);
MUresult (*p_muDeviceGetByPCIBusId)(MUdevice *, const char *);
MUresult (*p_muMemAllocHost)(void **, size_t);
MUresult (*p_muMemAlloc)(MUdeviceptr *, size_t);
MUresult (*p_muMemFreeHost)(void *);
MUresult (*p_muMemFree)(MUdeviceptr);
MUresult (*p_muMemcpy)(MUdeviceptr, MUdeviceptr, size_t);
MUresult (*p_muMemcpyDtoD)(MUdeviceptr, MUdeviceptr, size_t);
#ifdef HAVE_MUSA_DMABUF
MUresult (*p_muMemGetHandleForAddressRange)(void *, MUdeviceptr, size_t,
					    MUmemRangeHandleType,
					    unsigned long long);
#endif

static int load_musa_symbol(void **func_ptr, const char *func_name)
{
	*func_ptr = dlsym(musa_handle, func_name);
	if (!*func_ptr) {
		fprintf(stderr, "Failed to resolve %s: %s\n", func_name, dlerror());
		return -1;
	}

	return 0;
}

int load_musa_library(void)
{
	static const char *libraries[] = {
		"libmusa.so",
		"libmusa.so.1",
		NULL
	};
	static const MusaSymbol symbols[] = {
		{ (void **)&p_muInit, "muInit" },
		{ (void **)&p_muDeviceGetCount, "muDeviceGetCount" },
		{ (void **)&p_muDeviceGet, "muDeviceGet" },
		{ (void **)&p_muDeviceGetAttribute, "muDeviceGetAttribute" },
		{ (void **)&p_muDeviceGetName, "muDeviceGetName" },
		{ (void **)&p_muCtxCreate, "muCtxCreate" },
		{ (void **)&p_muCtxSetCurrent, "muCtxSetCurrent" },
		{ (void **)&p_muCtxDestroy, "muCtxDestroy" },
		{ (void **)&p_muDeviceGetByPCIBusId, "muDeviceGetByPCIBusId" },
		{ (void **)&p_muMemAllocHost, "muMemAllocHost" },
		{ (void **)&p_muMemAlloc, "muMemAlloc" },
		{ (void **)&p_muMemFreeHost, "muMemFreeHost" },
		{ (void **)&p_muMemFree, "muMemFree" },
		{ (void **)&p_muMemcpy, "muMemcpy" },
		{ (void **)&p_muMemcpyDtoD, "muMemcpyDtoD" },
#ifdef HAVE_MUSA_DMABUF
		{ (void **)&p_muMemGetHandleForAddressRange, "muMemGetHandleForAddressRange" },
#endif
	};
	size_t i;

	if (musa_handle)
		return 0;

	for (i = 0; libraries[i]; i++) {
		musa_handle = dlopen(libraries[i], RTLD_LAZY);
		if (musa_handle)
			break;
	}

	if (!musa_handle) {
		fprintf(stderr, "Failed to load MUSA library: %s\n", dlerror());
		return -1;
	}

	for (i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
		if (load_musa_symbol(symbols[i].func_ptr, symbols[i].name)) {
			unload_musa_library();
			return -1;
		}
	}

	return 0;
}

void unload_musa_library(void)
{
	if (musa_handle) {
		dlclose(musa_handle);
		musa_handle = NULL;
	}
}
