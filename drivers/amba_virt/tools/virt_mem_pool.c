/*
 * virt_mem_pool.c
 *
 * Dynamic memory extent manager and conflict resolution engine for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "virt_mem_pool.h"

static pthread_mutex_t g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct virt_tenant_pool g_tenant_pools[VIRT_MEM_MAX_TENANTS];
static uint32_t g_default_bar_size = VIRT_MEM_DEFAULT_BAR_SIZE;
static int g_pool_initialized = 0;

int virt_mem_pool_init(uint32_t default_bar_size)
{
	int i, j;

	pthread_mutex_lock(&g_pool_mutex);
	if (default_bar_size > 0)
		g_default_bar_size = default_bar_size;

	memset(g_tenant_pools, 0, sizeof(g_tenant_pools));
	for (i = 0; i < VIRT_MEM_MAX_TENANTS; i++) {
		pthread_mutex_init(&g_tenant_pools[i].lock, NULL);
		g_tenant_pools[i].bar_size = g_default_bar_size;
		g_tenant_pools[i].quota_max = g_default_bar_size;
		g_tenant_pools[i].quota_min = 0;
		g_tenant_pools[i].allocated_bytes = 0;
		g_tenant_pools[i].high_water_mark = 0;
		g_tenant_pools[i].in_use = 0;
		for (j = 0; j < VIRT_MEM_MAX_DEVICES; j++)
			g_tenant_pools[i].devices[j].in_use = 0;
		for (j = 0; j < VIRT_MEM_MAX_EXTENTS; j++)
			g_tenant_pools[i].extents[j].in_use = 0;
	}
	g_pool_initialized = 1;
	pthread_mutex_unlock(&g_pool_mutex);
	return 0;
}

void virt_mem_pool_cleanup(void)
{
	int i;

	pthread_mutex_lock(&g_pool_mutex);
	for (i = 0; i < VIRT_MEM_MAX_TENANTS; i++) {
		pthread_mutex_destroy(&g_tenant_pools[i].lock);
	}
	memset(g_tenant_pools, 0, sizeof(g_tenant_pools));
	g_pool_initialized = 0;
	pthread_mutex_unlock(&g_pool_mutex);
}

int virt_mem_pool_register_tenant(uint32_t cid, uint32_t tenant_idx,
				  uint32_t bar_size, uint32_t quota_max)
{
	int i;

	if (!g_pool_initialized)
		virt_mem_pool_init(VIRT_MEM_DEFAULT_BAR_SIZE);

	pthread_mutex_lock(&g_pool_mutex);

	/* Check if tenant already exists */
	for (i = 0; i < VIRT_MEM_MAX_TENANTS; i++) {
		if (g_tenant_pools[i].in_use && g_tenant_pools[i].cid == cid) {
			pthread_mutex_lock(&g_tenant_pools[i].lock);
			g_tenant_pools[i].tenant_idx = tenant_idx;
			if (bar_size > 0)
				g_tenant_pools[i].bar_size = bar_size;
			if (quota_max > 0)
				g_tenant_pools[i].quota_max = quota_max;
			pthread_mutex_unlock(&g_tenant_pools[i].lock);
			pthread_mutex_unlock(&g_pool_mutex);
			return 0;
		}
	}

	/* Find free slot */
	for (i = 0; i < VIRT_MEM_MAX_TENANTS; i++) {
		if (!g_tenant_pools[i].in_use) {
			pthread_mutex_lock(&g_tenant_pools[i].lock);
			g_tenant_pools[i].cid = cid;
			g_tenant_pools[i].tenant_idx = tenant_idx;
			g_tenant_pools[i].bar_size = (bar_size > 0) ? bar_size : g_default_bar_size;
			g_tenant_pools[i].quota_max = (quota_max > 0) ? quota_max : g_tenant_pools[i].bar_size;
			g_tenant_pools[i].quota_min = 0;
			g_tenant_pools[i].allocated_bytes = 0;
			g_tenant_pools[i].high_water_mark = 0;
			g_tenant_pools[i].in_use = 1;
			pthread_mutex_unlock(&g_tenant_pools[i].lock);
			pthread_mutex_unlock(&g_pool_mutex);
			return 0;
		}
	}

	pthread_mutex_unlock(&g_pool_mutex);
	return -ENOSPC;
}

int virt_mem_pool_unregister_tenant(uint32_t cid)
{
	int i, j;

	pthread_mutex_lock(&g_pool_mutex);
	for (i = 0; i < VIRT_MEM_MAX_TENANTS; i++) {
		if (g_tenant_pools[i].in_use && g_tenant_pools[i].cid == cid) {
			pthread_mutex_lock(&g_tenant_pools[i].lock);
			for (j = 0; j < VIRT_MEM_MAX_DEVICES; j++)
				g_tenant_pools[i].devices[j].in_use = 0;
			for (j = 0; j < VIRT_MEM_MAX_EXTENTS; j++)
				g_tenant_pools[i].extents[j].in_use = 0;
			g_tenant_pools[i].allocated_bytes = 0;
			g_tenant_pools[i].in_use = 0;
			pthread_mutex_unlock(&g_tenant_pools[i].lock);
			pthread_mutex_unlock(&g_pool_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_pool_mutex);
	return -ENOENT;
}

struct virt_tenant_pool *virt_mem_pool_get_tenant(uint32_t cid)
{
	int i;

	if (!g_pool_initialized)
		virt_mem_pool_init(VIRT_MEM_DEFAULT_BAR_SIZE);

	pthread_mutex_lock(&g_pool_mutex);
	for (i = 0; i < VIRT_MEM_MAX_TENANTS; i++) {
		if (g_tenant_pools[i].in_use && g_tenant_pools[i].cid == cid) {
			pthread_mutex_unlock(&g_pool_mutex);
			return &g_tenant_pools[i];
		}
	}

	/* Auto-register connecting CID */
	for (i = 0; i < VIRT_MEM_MAX_TENANTS; i++) {
		if (!g_tenant_pools[i].in_use) {
			pthread_mutex_lock(&g_tenant_pools[i].lock);
			g_tenant_pools[i].cid = cid;
			g_tenant_pools[i].tenant_idx = i;
			g_tenant_pools[i].bar_size = g_default_bar_size;
			g_tenant_pools[i].quota_max = g_default_bar_size;
			g_tenant_pools[i].quota_min = 0;
			g_tenant_pools[i].allocated_bytes = 0;
			g_tenant_pools[i].high_water_mark = 0;
			g_tenant_pools[i].in_use = 1;
			pthread_mutex_unlock(&g_tenant_pools[i].lock);
			pthread_mutex_unlock(&g_pool_mutex);
			return &g_tenant_pools[i];
		}
	}

	pthread_mutex_unlock(&g_pool_mutex);
	return NULL;
}

int virt_mem_pool_set_quota(uint32_t cid, uint32_t quota_mb)
{
	struct virt_tenant_pool *pool;
	uint64_t quota_bytes = (uint64_t)quota_mb * 1024 * 1024;

	pool = virt_mem_pool_get_tenant(cid);
	if (!pool)
		return -ENOENT;

	pthread_mutex_lock(&pool->lock);
	if (quota_bytes > pool->bar_size) {
		pthread_mutex_unlock(&pool->lock);
		return -ERANGE;
	}

	pool->quota_max = (uint32_t)quota_bytes;
	pthread_mutex_unlock(&pool->lock);
	return 0;
}

int virt_mem_pool_get_quota(uint32_t cid, uint32_t *out_quota_max, uint32_t *out_allocated)
{
	struct virt_tenant_pool *pool = virt_mem_pool_get_tenant(cid);

	if (!pool)
		return -ENOENT;

	pthread_mutex_lock(&pool->lock);
	if (out_quota_max)
		*out_quota_max = pool->quota_max;
	if (out_allocated)
		*out_allocated = pool->allocated_bytes;
	pthread_mutex_unlock(&pool->lock);
	return 0;
}

static int ranges_overlap(uint64_t s1, uint64_t e1, uint64_t s2, uint64_t e2)
{
	return (s1 < e2 && e1 > s2);
}

int virt_mem_pool_set_device_bounds(uint32_t cid,
				    const struct amba_virt_dev_bounds_req *req,
				    struct amba_virt_dev_bounds_resp *resp,
				    unsigned char *shm_map)
{
	struct virt_tenant_pool *pool;
	uint32_t align;
	uint32_t req_size;
	uint32_t arena_sz;
	uint64_t total_needed;
	uint32_t dev_start = 0, dev_end = 0;
	uint32_t arena_start = 0, arena_end = 0;
	uint32_t allocated_excl;
	uint32_t avail_quota;
	int existing_slot = -1;
	int target_slot = -1;
	int i;

	if (!req || !resp)
		return -EINVAL;

	memset(resp, 0, sizeof(*resp));

	if (req->dev_type == AMBA_VIRT_DEV_TYPE_NONE || req->dev_type >= AMBA_VIRT_MAX_DEV_TYPES) {
		resp->status = -EINVAL;
		resp->err_code = AMBA_VIRT_ERR_UNKNOWN_DEVICE;
		return 0;
	}

	if (req->requested_size == 0) {
		resp->status = -EINVAL;
		resp->err_code = AMBA_VIRT_ERR_INVALID_ALIGN;
		return 0;
	}

	pool = virt_mem_pool_get_tenant(cid);
	if (!pool) {
		resp->status = -ENOMEM;
		resp->err_code = AMBA_VIRT_ERR_HOST_OOM;
		return 0;
	}

	pthread_mutex_lock(&pool->lock);

	/* Determine alignment (default: 2 MiB for Cavalry/IAV, 4 KiB for others) */
	align = req->align;
	if (align == 0) {
		align = (req->dev_type == AMBA_VIRT_DEV_TYPE_CAVALRY ||
			 req->dev_type == AMBA_VIRT_DEV_TYPE_IAV) ? 0x00200000U : 4096U;
	}
	if (align < 4096U || (align & (align - 1)) != 0) {
		resp->status = -EINVAL;
		resp->err_code = AMBA_VIRT_ERR_INVALID_ALIGN;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	/* Align requested size */
	req_size = (req->requested_size + align - 1) & ~(align - 1);
	arena_sz = req->rpc_arena_size;
	if (arena_sz > 0)
		arena_sz = (arena_sz + 4095U) & ~4095U;

	total_needed = (uint64_t)req_size + arena_sz;

	/* 1. Check Total BAR Aperture Overflow */
	if (total_needed > pool->bar_size) {
		resp->status = -ERANGE;
		resp->err_code = AMBA_VIRT_ERR_BAR_OVERFLOW;
		resp->max_avail_size = (pool->quota_max > pool->allocated_bytes) ?
				       (pool->quota_max - pool->allocated_bytes) : 0;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	if (req->preferred_offset != AMBA_VIRT_OFFSET_AUTO) {
		if ((req->preferred_offset % align) != 0) {
			resp->status = -EINVAL;
			resp->err_code = AMBA_VIRT_ERR_INVALID_ALIGN;
			resp->suggested_offset = (req->preferred_offset + align - 1) & ~(align - 1);
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}
		if ((uint64_t)req->preferred_offset + req_size > pool->bar_size) {
			resp->status = -ERANGE;
			resp->err_code = AMBA_VIRT_ERR_BAR_OVERFLOW;
			resp->max_avail_size = (pool->quota_max > pool->allocated_bytes) ?
					       (pool->quota_max - pool->allocated_bytes) : 0;
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}
	}

	/* 2. Check Duplicate Device Re-Registration */
	for (i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
		if (pool->devices[i].in_use && pool->devices[i].dev_type == req->dev_type) {
			existing_slot = i;
			break;
		}
	}

	if (existing_slot >= 0 && !(req->flags & AMBA_VIRT_DEV_F_REPLACE)) {
		resp->status = -EBUSY;
		resp->err_code = AMBA_VIRT_ERR_DEV_ALREADY_REG;
		resp->max_avail_size = (pool->quota_max > pool->allocated_bytes) ?
				       (pool->quota_max - pool->allocated_bytes) : 0;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	/* 3. Check Quota Ceiling */
	allocated_excl = pool->allocated_bytes;
	if (existing_slot >= 0) {
		uint32_t old_dev_total = pool->devices[existing_slot].size +
					 pool->devices[existing_slot].rpc_arena_size;
		if (allocated_excl >= old_dev_total)
			allocated_excl -= old_dev_total;
		else
			allocated_excl = 0;
	}
	avail_quota = (pool->quota_max > allocated_excl) ? (pool->quota_max - allocated_excl) : 0;

	if (total_needed > avail_quota) {
		/* Best-effort clamping: only allowed for safe devices (SCRATCH), not Cavalry or GDMA */
		if ((req->flags & AMBA_VIRT_DEV_F_BEST_EFFORT) &&
		    req->dev_type != AMBA_VIRT_DEV_TYPE_CAVALRY &&
		    req->dev_type != AMBA_VIRT_DEV_TYPE_GDMA) {
			if (avail_quota > arena_sz) {
				uint32_t clamped = (avail_quota - arena_sz) & ~(align - 1);
				if (clamped > 0) {
					req_size = clamped;
					total_needed = (uint64_t)req_size + arena_sz;
				} else {
					resp->status = -ENOMEM;
					resp->err_code = AMBA_VIRT_ERR_QUOTA_EXCEEDED;
					resp->max_avail_size = avail_quota;
					pthread_mutex_unlock(&pool->lock);
					return 0;
				}
			} else {
				resp->status = -ENOMEM;
				resp->err_code = AMBA_VIRT_ERR_QUOTA_EXCEEDED;
				resp->max_avail_size = avail_quota;
				pthread_mutex_unlock(&pool->lock);
				return 0;
			}
		} else {
			resp->status = -ENOMEM;
			resp->err_code = AMBA_VIRT_ERR_QUOTA_EXCEEDED;
			resp->max_avail_size = avail_quota;
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}
	}

	/* 4. Offset Placement and Spatial Collision Detection */
	if (req->preferred_offset != AMBA_VIRT_OFFSET_AUTO) {
		dev_start = req->preferred_offset;
		dev_end = dev_start + req_size;

		if (arena_sz > 0) {
			arena_start = dev_end;
			arena_end = arena_start + arena_sz;
		} else {
			arena_start = 0;
			arena_end = 0;
		}

		if (arena_end > pool->bar_size || dev_end > pool->bar_size) {
			resp->status = -ERANGE;
			resp->err_code = AMBA_VIRT_ERR_BAR_OVERFLOW;
			resp->max_avail_size = avail_quota;
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}

		/* Check collisions with all other active devices of this tenant */
		for (i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
			if (i == existing_slot || !pool->devices[i].in_use)
				continue;

			uint32_t d_start = pool->devices[i].base_offset;
			uint32_t d_end = d_start + pool->devices[i].size;
			uint32_t a_start = pool->devices[i].rpc_arena_offset;
			uint32_t a_end = a_start + pool->devices[i].rpc_arena_size;

			int hit = 0;
			if (ranges_overlap(dev_start, dev_end, d_start, d_end))
				hit = 1;
			if (pool->devices[i].rpc_arena_size > 0 &&
			    ranges_overlap(dev_start, dev_end, a_start, a_end))
				hit = 1;
			if (arena_sz > 0) {
				if (ranges_overlap(arena_start, arena_end, d_start, d_end))
					hit = 1;
				if (pool->devices[i].rpc_arena_size > 0 &&
				    ranges_overlap(arena_start, arena_end, a_start, a_end))
					hit = 1;
			}

			if (hit) {
				uint32_t cand;
				uint32_t total_span = req_size + arena_sz;
				resp->status = -EEXIST;
				resp->err_code = AMBA_VIRT_ERR_OFFSET_COLLISION;
				resp->colliding_dev = pool->devices[i].dev_type;

				/* Compute suggested_offset: find next free aligned region */
				cand = (d_end > a_end) ? d_end : a_end;
				cand = (cand + align - 1) & ~(align - 1);

				/* Scan until non-overlapping candidate found or BAR exhausted */
				while (cand + total_span <= pool->bar_size) {
					int retry_hit = 0;
					int k;
					for (k = 0; k < VIRT_MEM_MAX_DEVICES; k++) {
						if (k == existing_slot || !pool->devices[k].in_use)
							continue;
						uint32_t k_start = pool->devices[k].base_offset;
						uint32_t k_end = k_start + pool->devices[k].size;
						uint32_t ka_start = pool->devices[k].rpc_arena_offset;
						uint32_t ka_end = ka_start + pool->devices[k].rpc_arena_size;

						if (ranges_overlap(cand, cand + total_span, k_start, k_end) ||
						    (pool->devices[k].rpc_arena_size > 0 &&
						     ranges_overlap(cand, cand + total_span, ka_start, ka_end))) {
							uint32_t next = (k_end > ka_end) ? k_end : ka_end;
							cand = (next + align - 1) & ~(align - 1);
							retry_hit = 1;
							break;
						}
					}
					if (!retry_hit) {
						resp->suggested_offset = cand;
						break;
					}
				}

				pthread_mutex_unlock(&pool->lock);
				return 0;
			}
		}
	} else {
		/* AUTO placement: First-fit aligned search */
		uint32_t cand = 0;

		while (cand + total_needed <= pool->bar_size) {
			int hit = 0;

			if (arena_sz > 0) {
				arena_start = cand;
				arena_end = cand + arena_sz;
				dev_start = (arena_end + align - 1) & ~(align - 1);
				dev_end = dev_start + req_size;
			} else {
				arena_start = 0;
				arena_end = 0;
				dev_start = cand;
				dev_end = cand + req_size;
			}

			if (dev_end > pool->bar_size)
				break;

			for (i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
				if (i == existing_slot || !pool->devices[i].in_use)
					continue;

				uint32_t d_start = pool->devices[i].base_offset;
				uint32_t d_end = d_start + pool->devices[i].size;
				uint32_t a_start = pool->devices[i].rpc_arena_offset;
				uint32_t a_end = a_start + pool->devices[i].rpc_arena_size;

				if (ranges_overlap(dev_start, dev_end, d_start, d_end) ||
				    (pool->devices[i].rpc_arena_size > 0 &&
				     ranges_overlap(dev_start, dev_end, a_start, a_end)) ||
				    (arena_sz > 0 && ranges_overlap(arena_start, arena_end, d_start, d_end)) ||
				    (arena_sz > 0 && pool->devices[i].rpc_arena_size > 0 &&
				     ranges_overlap(arena_start, arena_end, a_start, a_end))) {
					uint32_t next = (d_end > a_end) ? d_end : a_end;
					uint32_t step_align = (arena_sz > 0) ? 4096U : align;
					cand = (next + step_align - 1) & ~(step_align - 1);
					hit = 1;
					break;
				}
			}

			if (!hit)
				break;
		}

		if (cand + total_needed > pool->bar_size || dev_end > pool->bar_size) {
			resp->status = -ENOMEM;
			resp->err_code = AMBA_VIRT_ERR_HOST_OOM;
			resp->max_avail_size = avail_quota;
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}
	}

	/* 5. Commit Allocation */
	if (existing_slot >= 0) {
		target_slot = existing_slot;
	} else {
		for (i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
			if (!pool->devices[i].in_use) {
				target_slot = i;
				break;
			}
		}
		if (target_slot < 0) {
			resp->status = -ENOMEM;
			resp->err_code = AMBA_VIRT_ERR_HOST_OOM;
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}
	}

	pool->devices[target_slot].dev_type = req->dev_type;
	pool->devices[target_slot].base_offset = dev_start;
	pool->devices[target_slot].size = req_size;
	pool->devices[target_slot].rpc_arena_offset = (arena_sz > 0) ? arena_start : 0;
	pool->devices[target_slot].rpc_arena_size = arena_sz;
	pool->devices[target_slot].flags = req->flags;
	pool->devices[target_slot].in_use = 1;

	pool->allocated_bytes = allocated_excl + req_size + arena_sz;
	if (pool->allocated_bytes > pool->high_water_mark)
		pool->high_water_mark = pool->allocated_bytes;

	resp->status = 0;
	resp->err_code = AMBA_VIRT_ERR_NONE;
	resp->granted_offset = dev_start;
	resp->granted_size = req_size;
	resp->rpc_arena_offset = (arena_sz > 0) ? arena_start : 0;
	resp->max_avail_size = (pool->quota_max > pool->allocated_bytes) ?
			       (pool->quota_max - pool->allocated_bytes) : 0;
	resp->suggested_offset = 0;
	resp->colliding_dev = 0;

	/* 6. Zero Initialization if requested */
	if ((req->flags & AMBA_VIRT_DEV_F_ZERO_INIT) && shm_map && shm_map != (unsigned char *)-1) {
		memset(shm_map + dev_start, 0, req_size);
		if (arena_sz > 0)
			memset(shm_map + arena_start, 0, arena_sz);
	}

	pthread_mutex_unlock(&pool->lock);
	return 0;
}

int virt_mem_pool_release_device_bounds(uint32_t cid, uint32_t dev_type)
{
	struct virt_tenant_pool *pool;
	int i;

	pool = virt_mem_pool_get_tenant(cid);
	if (!pool)
		return -ENOENT;

	pthread_mutex_lock(&pool->lock);
	for (i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
		if (pool->devices[i].in_use && pool->devices[i].dev_type == dev_type) {
			uint32_t freed = pool->devices[i].size + pool->devices[i].rpc_arena_size;
			if (pool->allocated_bytes >= freed)
				pool->allocated_bytes -= freed;
			else
				pool->allocated_bytes = 0;
			pool->devices[i].in_use = 0;
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&pool->lock);
	return -ENOENT;
}

int virt_mem_pool_get_device_bounds(uint32_t cid, uint32_t dev_type,
				    uint32_t *out_base, uint32_t *out_size,
				    uint32_t *out_arena_off, uint32_t *out_arena_sz)
{
	struct virt_tenant_pool *pool;
	int i;

	pool = virt_mem_pool_get_tenant(cid);
	if (!pool)
		return -ENOENT;

	pthread_mutex_lock(&pool->lock);
	for (i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
		if (pool->devices[i].in_use && pool->devices[i].dev_type == dev_type) {
			if (out_base)
				*out_base = pool->devices[i].base_offset;
			if (out_size)
				*out_size = pool->devices[i].size;
			if (out_arena_off)
				*out_arena_off = pool->devices[i].rpc_arena_offset;
			if (out_arena_sz)
				*out_arena_sz = pool->devices[i].rpc_arena_size;
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&pool->lock);
	return -ENOENT;
}

int virt_mem_pool_alloc_extent(uint32_t cid,
			       const struct amba_virt_mem_req *req,
			       struct amba_virt_mem_resp *resp,
			       unsigned char *shm_map,
			       uint64_t phys_base)
{
	struct virt_tenant_pool *pool;
	uint32_t align;
	uint32_t alloc_sz;
	uint32_t cand = 0;
	uint32_t avail_quota;
	int slot = -1;
	int i;

	if (!req || !resp)
		return -EINVAL;

	memset(resp, 0, sizeof(*resp));

	pool = virt_mem_pool_get_tenant(cid);
	if (!pool) {
		resp->status = -ENOENT;
		return 0;
	}

	pthread_mutex_lock(&pool->lock);

	align = req->align ? req->align : 4096U;
	if (align < 4096U || (align & (align - 1)) != 0) {
		resp->status = -EINVAL;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	alloc_sz = (req->size + align - 1) & ~(align - 1);
	if (alloc_sz == 0) {
		resp->status = -EINVAL;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	if (alloc_sz > pool->bar_size) {
		resp->status = -ERANGE;
		resp->total_allocated = pool->allocated_bytes;
		resp->total_free = (pool->quota_max > pool->allocated_bytes) ?
				   (pool->quota_max - pool->allocated_bytes) : 0;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	avail_quota = (pool->quota_max > pool->allocated_bytes) ?
		      (pool->quota_max - pool->allocated_bytes) : 0;
	if (alloc_sz > avail_quota) {
		resp->status = -ENOMEM;
		resp->total_allocated = pool->allocated_bytes;
		resp->total_free = avail_quota;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	/* Find free extent slot */
	for (i = 0; i < VIRT_MEM_MAX_EXTENTS; i++) {
		if (!pool->extents[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		resp->status = -ENOMEM;
		resp->total_allocated = pool->allocated_bytes;
		resp->total_free = avail_quota;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	/* Search for free range within BAR */
	while (cand + alloc_sz <= pool->bar_size) {
		int hit = 0;

		/* Check devices */
		for (i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
			if (!pool->devices[i].in_use)
				continue;
			uint32_t ds = pool->devices[i].base_offset;
			uint32_t de = ds + pool->devices[i].size;
			uint32_t as = pool->devices[i].rpc_arena_offset;
			uint32_t ae = as + pool->devices[i].rpc_arena_size;

			if (ranges_overlap(cand, cand + alloc_sz, ds, de) ||
			    (pool->devices[i].rpc_arena_size > 0 &&
			     ranges_overlap(cand, cand + alloc_sz, as, ae))) {
				uint32_t next = (de > ae) ? de : ae;
				cand = (next + align - 1) & ~(align - 1);
				hit = 1;
				break;
			}
		}
		if (hit)
			continue;

		/* Check other extents */
		for (i = 0; i < VIRT_MEM_MAX_EXTENTS; i++) {
			if (!pool->extents[i].in_use)
				continue;
			uint32_t es = pool->extents[i].offset;
			uint32_t ee = es + pool->extents[i].size;
			if (ranges_overlap(cand, cand + alloc_sz, es, ee)) {
				cand = (ee + align - 1) & ~(align - 1);
				hit = 1;
				break;
			}
		}
		if (!hit)
			break;
	}

	if (cand + alloc_sz > pool->bar_size) {
		resp->status = -ENOMEM;
		resp->total_allocated = pool->allocated_bytes;
		resp->total_free = avail_quota;
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}

	pool->extents[slot].offset = cand;
	pool->extents[slot].size = alloc_sz;
	pool->extents[slot].dev_type = req->dev_affinity;
	pool->extents[slot].flags = req->flags;
	pool->extents[slot].in_use = 1;

	pool->allocated_bytes += alloc_sz;
	if (pool->allocated_bytes > pool->high_water_mark)
		pool->high_water_mark = pool->allocated_bytes;

	resp->status = 0;
	resp->bar_offset = cand;
	resp->allocated_size = alloc_sz;
	resp->phys_addr = phys_base ? (phys_base + cand) : 0;
	resp->total_allocated = pool->allocated_bytes;
	resp->total_free = (pool->quota_max > pool->allocated_bytes) ?
			   (pool->quota_max - pool->allocated_bytes) : 0;

	if ((req->flags & AMBA_VIRT_DEV_F_ZERO_INIT) && shm_map && shm_map != (unsigned char *)-1) {
		memset(shm_map + cand, 0, alloc_sz);
	}

	pthread_mutex_unlock(&pool->lock);
	return 0;
}

int virt_mem_pool_free_extent(uint32_t cid, uint32_t bar_offset,
			      struct amba_virt_mem_resp *resp)
{
	struct virt_tenant_pool *pool;
	int i;

	pool = virt_mem_pool_get_tenant(cid);
	if (!pool)
		return -ENOENT;

	pthread_mutex_lock(&pool->lock);
	for (i = 0; i < VIRT_MEM_MAX_EXTENTS; i++) {
		if (pool->extents[i].in_use && pool->extents[i].offset == bar_offset) {
			uint32_t sz = pool->extents[i].size;
			if (pool->allocated_bytes >= sz)
				pool->allocated_bytes -= sz;
			else
				pool->allocated_bytes = 0;
			pool->extents[i].in_use = 0;

			if (resp) {
				resp->status = 0;
				resp->bar_offset = bar_offset;
				resp->allocated_size = sz;
				resp->total_allocated = pool->allocated_bytes;
				resp->total_free = (pool->quota_max > pool->allocated_bytes) ?
						   (pool->quota_max - pool->allocated_bytes) : 0;
			}
			pthread_mutex_unlock(&pool->lock);
			return 0;
		}
	}

	if (resp) {
		resp->status = -ENOENT;
		resp->total_allocated = pool->allocated_bytes;
		resp->total_free = (pool->quota_max > pool->allocated_bytes) ?
				   (pool->quota_max - pool->allocated_bytes) : 0;
	}
	pthread_mutex_unlock(&pool->lock);
	return -ENOENT;
}

int virt_mem_pool_validate_range(uint32_t cid, uint32_t bar_offset, uint32_t size)
{
	struct virt_tenant_pool *pool;
	uint64_t req_end = (uint64_t)bar_offset + size;
	int i;

	pool = virt_mem_pool_get_tenant(cid);
	if (!pool)
		return -ENOENT;

	pthread_mutex_lock(&pool->lock);

	/* Check devices */
	for (i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
		if (pool->devices[i].in_use) {
			uint32_t ds = pool->devices[i].base_offset;
			uint64_t de = (uint64_t)ds + pool->devices[i].size;
			if (bar_offset >= ds && req_end <= de) {
				pthread_mutex_unlock(&pool->lock);
				return 0;
			}
			if (pool->devices[i].rpc_arena_size > 0) {
				uint32_t as = pool->devices[i].rpc_arena_offset;
				uint64_t ae = (uint64_t)as + pool->devices[i].rpc_arena_size;
				if (bar_offset >= as && req_end <= ae) {
					pthread_mutex_unlock(&pool->lock);
					return 0;
				}
			}
		}
	}

	/* Check extents */
	for (i = 0; i < VIRT_MEM_MAX_EXTENTS; i++) {
		if (pool->extents[i].in_use) {
			uint32_t es = pool->extents[i].offset;
			uint64_t ee = (uint64_t)es + pool->extents[i].size;
			if (bar_offset >= es && req_end <= ee) {
				pthread_mutex_unlock(&pool->lock);
				return 0;
			}
		}
	}

	pthread_mutex_unlock(&pool->lock);
	return -EFAULT;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
