/*
 * virt_iav_guest_slice.c
 *
 * Refuse a guest buffer that is not wholly inside the slice the server
 * leased to that guest. A physical address from the guest is refused.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "virt_iav_guest_slice.h"

int virt_iav_guest_slice_admit(const struct virt_iav_guest_pool *pool,
			       const struct virt_iav_guest_req *req,
			       struct virt_iav_guest_dma *out)
{
	uint64_t slice_off, slice_end, end;

	if (!pool || !req || !out)
		return -EINVAL;
	if (pool->slice_count == 0 || pool->slice_count > 8)
		return -EINVAL;
	if (pool->slice_bytes == 0 ||
	    pool->parent_bytes / pool->slice_bytes != pool->slice_count ||
	    pool->parent_bytes % pool->slice_bytes != 0)
		return -EINVAL;
	if (pool->parent_base % pool->slice_bytes)
		return -EINVAL;
	if (req->phys != 0)
		return -EPERM;
	if (req->partition_id != pool->partition_id)
		return -EPERM;
	if (req->claimed_slice != req->caller_slice)
		return -EPERM;
	if (req->caller_slice >= pool->slice_count)
		return -EPERM;
	if (req->length == 0)
		return -EINVAL;

	slice_off = (uint64_t)req->caller_slice * pool->slice_bytes;
	slice_end = slice_off + pool->slice_bytes;
	if (req->offset < slice_off)
		return -EPERM;
	if (req->offset > pool->parent_bytes ||
	    req->length > pool->parent_bytes - req->offset)
		return -EINVAL;
	end = req->offset + req->length;
	if (end > slice_end)
		return -EPERM;
	if (pool->parent_base > UINT64_MAX - req->offset)
		return -EINVAL;

	out->partition_id = pool->partition_id;
	out->phys = pool->parent_base + req->offset;
	out->length = req->length;
	return 0;
}
