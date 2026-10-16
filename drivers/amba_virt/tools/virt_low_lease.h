/*
 * virt_low_lease.h
 *
 * One 64 MiB slice of the pinned low pool, paired with a Cavalry lease.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef VIRT_LOW_LEASE_H
#define VIRT_LOW_LEASE_H

#include <stddef.h>
#include <stdint.h>

#define VIRT_LOW_LEASE_STORE "/run/amba-virt/low-leases"
#define VIRT_LOW_LEASE_COUNT 8u
#define VIRT_LOW_LEASE_BYTES 0x4000000ull
#define VIRT_LOW_LEASE_USABLE (VIRT_LOW_LEASE_BYTES - 4096u)

void virt_low_lease_reset(void);
void virt_low_lease_set_store(const char *path);
int virt_low_lease_set_parent(uint64_t base);
int virt_low_lease_set_partition(uint32_t pid);
int virt_low_lease_geometry(uint64_t *base, uint32_t *pid);
int virt_low_lease_load(void);
int virt_low_lease_admin(const char *req, char *resp, size_t resp_len);
int virt_low_lease_index_used(uint32_t index);
int virt_low_lease_arm_module(uint64_t base);

/*
 * The scrub of released slices (virt_slice_arena.h). A pass scrubs every quarantined slice whose quarantine is over
 * and frees it; it returns how many were freed. The scrubber thread runs a pass every 200 ms (started by
 * virt_low_lease_arm_module). A reserve that finds every free slice in quarantine waits for a scrub, at most the
 * reserve wait (0: does not wait).
 */
int virt_low_lease_scrub_pass(void);
int virt_low_lease_scrub_pending(uint32_t index);
void virt_low_lease_set_reserve_wait_ms(unsigned int ms);
int virt_low_lease_start_scrubber(void);
void virt_low_lease_stop_scrubber(void);

#endif
