/*
 * iav_low_slice.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#if defined(__KERNEL__)
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#include "iav_low_slice.h"

int iav_low_slice_init(struct iav_low_slice *slices, unsigned count,
		       uint64_t parent, uint64_t parent_bytes)
{
	unsigned i;

	if (!slices || count != IAV_LOW_SLICE_COUNT)
		return -EINVAL;
	if (parent_bytes != count * IAV_LOW_SLICE_BYTES)
		return -EINVAL;
	if (parent % IAV_LOW_SLICE_BYTES)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		slices[i].phys = parent + i * IAV_LOW_SLICE_BYTES;
		slices[i].nonce = 0;
		slices[i].cid = 0;
		slices[i].claimed = 0;
	}
	return 0;
}

int iav_low_slice_claim(struct iav_low_slice *slice, uint32_t cid, uint64_t *nonce)
{
	if (!slice || !nonce || cid == 0)
		return -EINVAL;
	if (slice->claimed)
		return -EBUSY;
	slice->claimed = 1;
	slice->cid = cid;
	slice->nonce = ((uint64_t)cid << 32) ^ slice->phys ^ 0x9e3779b97f4a7c15ull;
	*nonce = slice->nonce;
	return 0;
}

int iav_low_slice_release(struct iav_low_slice *slice, uint32_t cid)
{
	if (!slice || cid == 0)
		return -EINVAL;
	if (!slice->claimed)
		return -EINVAL;
	if (slice->cid != cid)
		return -EPERM;
	slice->claimed = 0;
	slice->cid = 0;
	slice->nonce = 0;
	return 0;
}

int iav_low_slice_check(const struct iav_low_slice *slice, uint32_t cid,
			uint64_t offset, uint64_t length)
{
	if (!slice || cid == 0)
		return -EINVAL;
	if (!slice->claimed || slice->cid != cid)
		return -EPERM;
	if (length == 0 || offset % IAV_LOW_SLICE_PAGE)
		return -EINVAL;
	if (offset > IAV_LOW_SLICE_USABLE || length > IAV_LOW_SLICE_USABLE - offset)
		return -EPERM;
	return 0;
}
