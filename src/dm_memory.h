#ifndef DM_MEMORY_H
#define DM_MEMORY_H

#include "memory.h"
#include "config.h"


struct perftest_parameters;

bool dm_memory_dmabuf_supported();

struct memory_ctx *dm_memory_create(struct perftest_parameters *params);


#ifndef HAVE_IB_DM_DMABUF

inline bool dm_memory_dmabuf_supported() {
	return false;
}

inline struct memory_ctx *dm_memory_create(struct perftest_parameters *params) {
	return NULL;
}

#endif

#endif /* DM_MEMORY_H */
