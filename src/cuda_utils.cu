#include <stdint.h>
#include <stdio.h>
#include "cuda.h"

#include <cuda_runtime.h>
#define GPU_TOUCH_STEP 4096

__global__ void cuda_touch_pages(volatile uint8_t *c, int size,
		volatile int *stop_flag, int is_infinite) {
	do {
		for (int iter = 0; iter < size; iter += GPU_TOUCH_STEP)
			c[iter] = 0;
	}
	while (is_infinite && !*stop_flag);
}

extern "C" int touch_gpu_pages(uint8_t *addr, int buf_size,
		int is_infinite, volatile int **stop_flag)
{
	cudaError_t ret = cudaMallocManaged((void **)stop_flag, sizeof(int));
	if (ret) {
		printf("failed to allocate stop flag\n");
		return -1;
	}

	**stop_flag = 0;
	cuda_touch_pages<<<1, 1>>>(addr, buf_size, *stop_flag, is_infinite);

	return 0;
}
