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

/*
 * The ticket and the IAV role recorded by the reserve that holds Cavalry index `index`. Returns 0 with ticket
 * (empty for a lease reserved without a ticket, by RESERVE_SLICE or by a server that did not record one) and
 * *primary, or -1 if no lease holds the index.
 */
int virt_slice_lease_ticket_of_index(uint32_t index, char *ticket, size_t ticket_len, int *primary);
/* How many leases name ticket. */
unsigned int virt_slice_lease_ticket_count(const char *ticket);

/*
 * The aux aperture of a lease (plan_iav_usb_yolo Envelope F). attach is called at every ticketed reserve (idempotent
 * ones included) with the lease's index, epoch and whether it gets the aperture (the camera and the primary role); it
 * returns 0 and the aperture size (0: none), which the grant then carries as aux=<size>. detach is called when a lease is
 * released. Both run under the lease table's lock and must not call back into this module. NULL: no aperture.
 */
void virt_slice_lease_set_aux_hooks(int (*attach)(uint32_t index, uint64_t epoch, int primary, uint64_t *size),
				    void (*detach)(uint32_t index));
/* Every lease held now: its index, epoch, recorded role and whether it carries a ticket. Under the table's lock. */
void virt_slice_lease_foreach(void (*cb)(uint32_t index, uint64_t epoch, int primary, int ticketed, void *ctx),
			      void *ctx);

#endif
