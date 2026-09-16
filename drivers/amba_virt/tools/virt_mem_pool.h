/*
 * virt_mem_pool.h
 *
 * Dynamic memory extent manager and conflict resolution engine for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_MEM_POOL_H_
#define _VIRT_MEM_POOL_H_

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "amba_virt.h"

#define VIRT_MEM_DEFAULT_BAR_SIZE  0x40000000U /* 1 GiB */
#define VIRT_MEM_MAX_TENANTS       8
#define VIRT_MEM_MAX_DEVICES       8
#define VIRT_MEM_MAX_EXTENTS       32

struct virt_dev_partition {
	uint32_t dev_type;          /* enum amba_virt_dev_type */
	uint32_t base_offset;       /* Base offset in guest BAR */
	uint32_t size;              /* Allocated size in bytes */
	uint32_t rpc_arena_offset;  /* Dedicated control/RPC arena offset */
	uint32_t rpc_arena_size;    /* Dedicated control/RPC arena size */
	uint32_t flags;             /* AMBA_VIRT_DEV_F_* */
	int in_use;
};

struct virt_mem_extent {
	uint32_t offset;
	uint32_t size;
	uint32_t dev_type;
	uint32_t flags;
	int in_use;
};

struct virt_tenant_pool {
	uint32_t cid;
	uint32_t tenant_idx;
	uint32_t bar_size;          /* Hypervisor BAR size (e.g. 1 GiB) */
	uint32_t quota_min;         /* Reserved minimum */
	uint32_t quota_max;         /* Maximum quota ceiling */
	uint32_t allocated_bytes;   /* Current live allocations */
	uint32_t high_water_mark;   /* Peak allocated bytes */
	pthread_mutex_t lock;       /* Per-tenant lock */
	struct virt_dev_partition devices[VIRT_MEM_MAX_DEVICES];
	struct virt_mem_extent extents[VIRT_MEM_MAX_EXTENTS];
	int in_use;
};

/* Pool lifecycle */
int virt_mem_pool_init(uint32_t default_bar_size);
void virt_mem_pool_cleanup(void);

/* Tenant pool registration & configuration */
int virt_mem_pool_register_tenant(uint32_t cid, uint32_t tenant_idx,
				  uint32_t bar_size, uint32_t quota_max);
int virt_mem_pool_unregister_tenant(uint32_t cid);
struct virt_tenant_pool *virt_mem_pool_get_tenant(uint32_t cid);

int virt_mem_pool_set_quota(uint32_t cid, uint32_t quota_mb);
int virt_mem_pool_get_quota(uint32_t cid, uint32_t *out_quota_max, uint32_t *out_allocated);

/* In-band Device Boundary Negotiation */
int virt_mem_pool_set_device_bounds(uint32_t cid,
				    const struct amba_virt_dev_bounds_req *req,
				    struct amba_virt_dev_bounds_resp *resp,
				    unsigned char *shm_map);

int virt_mem_pool_release_device_bounds(uint32_t cid, uint32_t dev_type);

int virt_mem_pool_get_device_bounds(uint32_t cid, uint32_t dev_type,
				    uint32_t *out_base, uint32_t *out_size,
				    uint32_t *out_arena_off, uint32_t *out_arena_sz);

/* Dynamic Memory Extents API */
int virt_mem_pool_alloc_extent(uint32_t cid,
			       const struct amba_virt_mem_req *req,
			       struct amba_virt_mem_resp *resp,
			       unsigned char *shm_map,
			       uint64_t phys_base);

int virt_mem_pool_free_extent(uint32_t cid, uint32_t bar_offset,
			      struct amba_virt_mem_resp *resp);

/* Validation */
int virt_mem_pool_validate_range(uint32_t cid, uint32_t bar_offset, uint32_t size);

#endif /* _VIRT_MEM_POOL_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
