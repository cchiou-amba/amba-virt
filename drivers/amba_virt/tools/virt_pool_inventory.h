/*
 * virt_pool_inventory.h
 *
 * One read-only inventory of the Cavalry window and the low IAV pool.
 * The text has no UUID and no epoch.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef VIRT_POOL_INVENTORY_H
#define VIRT_POOL_INVENTORY_H

#include <stddef.h>
#include <stdint.h>

#define VIRT_POOL_INVENTORY_VERSION 1u
#define VIRT_POOL_INVENTORY_MAX 8u

struct virt_pool_slice {
	uint64_t base;
	uint64_t bytes;
	int leased;
};

struct virt_pool_view {
	const char *name;
	uint64_t parent_base;
	uint64_t parent_bytes;
	uint32_t count;
	uint64_t slice_bytes;
	uint32_t usable;
	int geometry_match;
	struct virt_pool_slice slices[VIRT_POOL_INVENTORY_MAX];
};

int virt_pool_inventory_write(char *buf, size_t len,
			      const struct virt_pool_view *cavalry,
			      const struct virt_pool_view *low);

#endif
