/*
 * virt_slice_lease.h
 *
 * UUID-keyed reservation of one high-memory slice.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef VIRT_SLICE_LEASE_H
#define VIRT_SLICE_LEASE_H

#include <stddef.h>
#include <stdint.h>

#define VIRT_SLICE_LEASE_STORE "/run/amba-virt/slice-leases"

void virt_slice_lease_reset(void);
void virt_slice_lease_set_store(const char *path);
int virt_slice_lease_set_geometry(int matched, uint32_t count, uint64_t slice_bytes);
int virt_slice_lease_load(void);
int virt_slice_lease_admin(const char *req, char *resp, size_t resp_len);
int virt_slice_lease_index_used(uint32_t index);

#endif
