/*
 * virt_iav_guest_slice.h
 *
 * Admit one guest buffer inside that guest's leased slice of the pinned
 * private IAV partition. The returned physical address is the only address
 * a later DSP operation may use.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef VIRT_IAV_GUEST_SLICE_H
#define VIRT_IAV_GUEST_SLICE_H

#include <stdint.h>

struct virt_iav_guest_pool {
	uint32_t partition_id;
	uint64_t parent_base;
	uint64_t parent_bytes;
	uint32_t slice_count;
	uint64_t slice_bytes;
};

struct virt_iav_guest_req {
	uint32_t caller_slice;
	uint32_t claimed_slice;
	uint32_t partition_id;
	uint64_t offset;
	uint64_t length;
	uint64_t phys;
};

struct virt_iav_guest_dma {
	uint32_t partition_id;
	uint64_t phys;
	uint64_t length;
};

int virt_iav_guest_slice_admit(const struct virt_iav_guest_pool *pool,
			       const struct virt_iav_guest_req *req,
			       struct virt_iav_guest_dma *out);

#endif
