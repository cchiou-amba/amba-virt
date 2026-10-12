/*
 * virt_pool_inventory.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>

#include "virt_pool_inventory.h"

static int write_side(char *buf, size_t len, int off, const struct virt_pool_view *side)
{
	uint32_t i;

	if (!side || !side->name || side->count > VIRT_POOL_INVENTORY_MAX)
		return -1;
	off += snprintf(buf + off, len - (size_t)off,
			"%s parent_base=0x%llx parent_bytes=%llu slice_count=%u slice_bytes=%llu usable=%u geometry=%s\n",
			side->name,
			(unsigned long long)side->parent_base,
			(unsigned long long)side->parent_bytes,
			side->count,
			(unsigned long long)side->slice_bytes,
			side->usable,
			side->geometry_match ? "match" : "reject");
	if (off < 0 || (size_t)off >= len)
		return -1;
	for (i = 0; i < side->count; i++) {
		off += snprintf(buf + off, len - (size_t)off,
				"%s slice index=%u base=0x%llx bytes=%llu state=%s\n",
				side->name, i,
				(unsigned long long)side->slices[i].base,
				(unsigned long long)side->slices[i].bytes,
				side->slices[i].leased ? "leased" : "free");
		if (off < 0 || (size_t)off >= len)
			return -1;
	}
	return off;
}

int virt_pool_inventory_write(char *buf, size_t len,
			      const struct virt_pool_view *cavalry,
			      const struct virt_pool_view *low)
{
	int off;

	if (!buf || len < 64)
		return -1;
	off = snprintf(buf, len, "OK pools version=%u\n", VIRT_POOL_INVENTORY_VERSION);
	if (off < 0 || (size_t)off >= len)
		return -1;
	off = write_side(buf, len, off, cavalry);
	if (off < 0)
		return -1;
	off = write_side(buf, len, off, low);
	return off < 0 ? -1 : 0;
}
