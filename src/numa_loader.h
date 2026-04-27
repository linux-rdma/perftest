#ifndef NUMA_LOADER_H
#define NUMA_LOADER_H

struct bitmask;

typedef struct {
	void **func_ptr;
	const char *name;
} NumaSymbol;

extern int             (*p_numa_available)(void);
extern int             (*p_numa_max_node)(void);
extern int             (*p_numa_run_on_node)(int node);
extern struct bitmask *(*p_numa_allocate_nodemask)(void);
extern struct bitmask *(*p_numa_bitmask_setbit)(struct bitmask *bmp, unsigned int n);
extern void            (*p_numa_set_membind)(struct bitmask *nodemask);
extern long            (*p_numa_migrate_pages)(int pid, struct bitmask *from, struct bitmask *to);
extern void            (*p_numa_bitmask_free)(struct bitmask *bmp);
extern struct bitmask **p_numa_all_nodes;

int  load_numa_library(void);
void unload_numa_library(void);

#endif /* NUMA_LOADER_H */
