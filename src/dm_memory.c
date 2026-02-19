#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "dm_memory.h"
#include "perftest_parameters.h"

struct dm_memory_ctx {
	struct memory_ctx base;
	char *ib_devname;
	struct ibv_context *ib_ctx;
	struct ibv_dm *dm;
	uint64_t dm_size;
	bool use_dmabuf;
	enum verbosity_level output;
};

static int dm_memory_init(struct memory_ctx *ctx)
{
	struct dm_memory_ctx *dm_ctx = container_of(ctx, struct dm_memory_ctx, base);
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev = NULL;
	int num_devices;
	int i;

	dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list) {
		fprintf(stderr, "Failed to get IB devices list\n");
		return FAILURE;
	}

	for (i = 0; i < num_devices; i++) {
		if (!strcmp(ibv_get_device_name(dev_list[i]), dm_ctx->ib_devname)) {
			ib_dev = dev_list[i];
			break;
		}
	}

	if (!ib_dev) {
		fprintf(stderr, "IB device %s not found\n", dm_ctx->ib_devname);
		ibv_free_device_list(dev_list);
		return FAILURE;
	}

	dm_ctx->ib_ctx = ibv_open_device(ib_dev);
	ibv_free_device_list(dev_list);

	if (!dm_ctx->ib_ctx) {
		fprintf(stderr, "Couldn't open IB device %s\n", dm_ctx->ib_devname);
		return FAILURE;
	}

	if (dm_ctx->output == FULL_VERBOSITY)
		printf("Opened IB device %s for device memory\n", dm_ctx->ib_devname);
	return SUCCESS;
}

static int dm_memory_destroy(struct memory_ctx *ctx)
{
	struct dm_memory_ctx *dm_ctx = container_of(ctx, struct dm_memory_ctx, base);

	if (dm_ctx->ib_ctx)
		ibv_close_device(dm_ctx->ib_ctx);

	free(dm_ctx);
	return SUCCESS;
}

static int dm_memory_allocate_buffer(struct memory_ctx *ctx, int alignment,
				     uint64_t size, int *dmabuf_fd,
				     uint64_t *dmabuf_offset, void **addr,
				     bool *can_init)
{
	struct dm_memory_ctx *dm_ctx = container_of(ctx, struct dm_memory_ctx, base);
	struct ibv_alloc_dm_attr dm_attr = {};
	struct ibv_device_attr_ex dev_attr = {};
	int ret;

	ret = ibv_query_device_ex(dm_ctx->ib_ctx, NULL, &dev_attr);
	if (ret) {
		fprintf(stderr, "Failed to query device attributes: %d\n", ret);
		return FAILURE;
	}

	if (size > dev_attr.max_dm_size) {
		fprintf(stderr, "Requested DM size %lu exceeds device max %lu\n",
			(unsigned long)size, (unsigned long)dev_attr.max_dm_size);
		return FAILURE;
	}

	dm_attr.length = size;

	dm_ctx->dm = ibv_alloc_dm(dm_ctx->ib_ctx, &dm_attr);
	if (!dm_ctx->dm) {
		fprintf(stderr, "Failed to allocate device memory of size %lu: %s\n",
			(unsigned long)size, strerror(errno));
		return FAILURE;
	}

	dm_ctx->dm_size = size;
	*addr = NULL;
	*can_init = false;

	if (dm_ctx->output == FULL_VERBOSITY)
		printf("Allocated %lu bytes of device memory\n", (unsigned long)size);

	if (dm_ctx->use_dmabuf) {
		int fd = ibv_dm_export_dmabuf_fd(dm_ctx->dm);
		if (fd < 0) {
			fprintf(stderr, "Failed to export DM as dmabuf fd: %s\n",
				strerror(errno));
			ibv_free_dm(dm_ctx->dm);
			dm_ctx->dm = NULL;
			return FAILURE;
		}

		*dmabuf_fd = fd;
		*dmabuf_offset = 0;
		if (dm_ctx->output == FULL_VERBOSITY)
			printf("Exported DM as dmabuf fd %d\n", fd);
	}

	return SUCCESS;
}

static int dm_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd,
				 void *addr, uint64_t size)
{
	struct dm_memory_ctx *dm_ctx = container_of(ctx, struct dm_memory_ctx, base);

	if (dm_ctx->dm) {
		ibv_free_dm(dm_ctx->dm);
		dm_ctx->dm = NULL;
	}

	return SUCCESS;
}

static void *dm_memory_copy_host_to_buffer(void *dest, const void *src, size_t size)
{
	/*
	 * DM buffers don't have a direct host address. The actual data path
	 * goes through RDMA operations, so host-side copy is a no-op.
	 */
	return dest;
}

static void *dm_memory_copy_buffer_to_host(void *dest, const void *src, size_t size)
{
	return dest;
}

static void *dm_memory_copy_buffer_to_buffer(void *dest, const void *src, size_t size)
{
	return dest;
}

bool dm_memory_dmabuf_supported()
{
	return true;
}

struct memory_ctx *dm_memory_create(struct perftest_parameters *params)
{
	struct dm_memory_ctx *ctx;

	ALLOCATE(ctx, struct dm_memory_ctx, 1);
	ctx->base.init = dm_memory_init;
	ctx->base.destroy = dm_memory_destroy;
	ctx->base.allocate_buffer = dm_memory_allocate_buffer;
	ctx->base.free_buffer = dm_memory_free_buffer;
	ctx->base.copy_host_to_buffer = dm_memory_copy_host_to_buffer;
	ctx->base.copy_buffer_to_host = dm_memory_copy_buffer_to_host;
	ctx->base.copy_buffer_to_buffer = dm_memory_copy_buffer_to_buffer;
	ctx->ib_devname = params->dm_ib_devname;
	ctx->use_dmabuf = params->use_ib_dm_dmabuf;
	ctx->output = params->output;
	ctx->dm = NULL;
	ctx->ib_ctx = NULL;

	return &ctx->base;
}
