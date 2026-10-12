/*
 * iav_low_slice.h
 *
 * Eight fixed 64 MiB slices of the pinned low pool. The last page is the claim.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef IAV_LOW_SLICE_H
#define IAV_LOW_SLICE_H

#if defined(__KERNEL__)
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define IAV_LOW_SLICE_COUNT 8u
#define IAV_LOW_SLICE_BYTES 0x4000000ull
#define IAV_LOW_SLICE_PAGE 4096u
#define IAV_LOW_SLICE_USABLE (IAV_LOW_SLICE_BYTES - IAV_LOW_SLICE_PAGE)

struct iav_low_slice {
	uint64_t phys;
	uint64_t nonce;
	uint32_t cid;
	int claimed;
};

int iav_low_slice_init(struct iav_low_slice *slices, unsigned count,
		       uint64_t parent, uint64_t parent_bytes);
int iav_low_slice_claim(struct iav_low_slice *slice, uint32_t cid, uint64_t *nonce);
int iav_low_slice_release(struct iav_low_slice *slice, uint32_t cid);
int iav_low_slice_check(const struct iav_low_slice *slice, uint32_t cid,
			uint64_t offset, uint64_t length);

#endif
