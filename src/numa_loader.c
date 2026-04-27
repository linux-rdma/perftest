#include "numa_loader.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stddef.h>

static void *numa_handle = NULL;

int             (*p_numa_available)(void)                                        = NULL;
int             (*p_numa_max_node)(void)                                         = NULL;
int             (*p_numa_run_on_node)(int)                                       = NULL;
struct bitmask *(*p_numa_allocate_nodemask)(void)                                = NULL;
struct bitmask *(*p_numa_bitmask_setbit)(struct bitmask *, unsigned int)         = NULL;
void            (*p_numa_set_membind)(struct bitmask *)                          = NULL;
long            (*p_numa_migrate_pages)(int, struct bitmask *, struct bitmask *) = NULL;
void            (*p_numa_bitmask_free)(struct bitmask *)                         = NULL;
struct bitmask **p_numa_all_nodes                                                = NULL;

int load_numa_library(void)
{
	numa_handle = dlopen("libnuma.so", RTLD_LAZY);

	if (!numa_handle)
		return -1;

	static const NumaSymbol symbols[] = {
		{ (void**)&p_numa_available,         "numa_available"         },
		{ (void**)&p_numa_max_node,          "numa_max_node"          },
		{ (void**)&p_numa_run_on_node,       "numa_run_on_node"       },
		{ (void**)&p_numa_allocate_nodemask, "numa_allocate_nodemask" },
		{ (void**)&p_numa_bitmask_setbit,    "numa_bitmask_setbit"    },
		{ (void**)&p_numa_set_membind,       "numa_set_membind"       },
		{ (void**)&p_numa_migrate_pages,     "numa_migrate_pages"     },
		{ (void**)&p_numa_bitmask_free,      "numa_bitmask_free"      },
		{ (void**)&p_numa_all_nodes,         "numa_all_nodes_ptr"     }
	};

	for (size_t i = 0; i < sizeof(symbols)/sizeof(symbols[0]); ++i) {
		*(symbols[i].func_ptr) = dlsym(numa_handle, symbols[i].name);

		if (!*(symbols[i].func_ptr)) {
			unload_numa_library();
			return -1;
		}
	}

	return 0;
}

void unload_numa_library(void)
{
	if (numa_handle) {
		dlclose(numa_handle);
		numa_handle = NULL;
	}
	p_numa_available         = NULL;
	p_numa_max_node          = NULL;
	p_numa_run_on_node       = NULL;
	p_numa_allocate_nodemask = NULL;
	p_numa_bitmask_setbit    = NULL;
	p_numa_set_membind       = NULL;
	p_numa_migrate_pages     = NULL;
	p_numa_bitmask_free      = NULL;
	p_numa_all_nodes         = NULL;
}
