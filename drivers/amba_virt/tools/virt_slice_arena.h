/*
 * virt_slice_arena.h
 *
 * The server's side of the per-guest arena (plan_iav_usb_yolo, Envelope E): which private slices are in use by DSP
 * and GDMA jobs, which are quarantined after their lease was released, the scrub that zeroes a quarantined slice
 * before it is leased again, and the slice header in the claim page that tells the guest module which BAR is its
 * slice. The arena itself (blocks, ids) lives in the guest module; the broker translates blocks statelessly.
 *
 * A released slice is quarantined until no arena call naming it is in flight and VIRT_SLICE_QUARANTINE_MS have
 * passed since its release and since the last arena call naming it returned: a non-blocking SET_IMG_SCALE returns
 * before the DSP runs the job, and the driver bounds the queue-full wait at 2 s and the completion wait at 2 s
 * (iav/iav_enc_img_scale.c), so a job queued by a returned call is inferred to have run within 4 s. GDMA_COPY returns
 * only after the copy completed (dma_pitch_memcpy waits for the transfer). Then the whole slice is zeroed and read back.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef VIRT_SLICE_ARENA_H
#define VIRT_SLICE_ARENA_H

#include <stddef.h>
#include <stdint.h>

#define VIRT_SLICE_ARENA_COUNT		8u
#define VIRT_SLICE_ARENA_BYTES		0x4000000ull
#define VIRT_SLICE_ARENA_PAGE		4096u
#define VIRT_SLICE_QUARANTINE_MS	4000u

/* How the server reaches a slice's memory and the clock; replaceable for host tests. */
struct virt_slice_io {
	void *ctx;
	/* Map the whole slice index read-write: a pointer, or NULL with errno set. */
	void *(*map)(void *ctx, uint32_t index, size_t bytes);
	void (*unmap)(void *ctx, void *p, size_t bytes);
	uint64_t (*now_ms)(void *ctx);
};

/* NULL restores the default: /dev/amba_virt_iav_slice<n> and CLOCK_MONOTONIC. */
void virt_slice_arena_set_io(const struct virt_slice_io *io);
void virt_slice_arena_reset(void);
uint64_t virt_slice_arena_now_ms(void);

/* An arena call names slice index: 0 (counted in flight), -EBUSY while it is quarantined, -EINVAL for a bad index. */
int virt_slice_arena_begin(uint32_t index);
/* The call that virt_slice_arena_begin admitted returned. */
void virt_slice_arena_end(uint32_t index);

/* The lease of slice index was released (or the slice was never scrubbed since the server started). */
void virt_slice_arena_quarantine(uint32_t index, uint64_t released_ms);
int virt_slice_arena_quarantined(uint32_t index);
/*
 * 1 when a quarantined slice may be scrubbed now: nothing in flight, and the quarantine has passed since the release
 * and since the last call returned. 0 otherwise, with *wait_ms (if not NULL) the time still to wait (0 while a call is
 * in flight).
 */
int virt_slice_arena_scrub_due(uint32_t index, uint64_t *wait_ms);
/* Zero the whole slice and read it back. 0, -EIO when a word did not read back zero, or -errno of the mapping. */
int virt_slice_arena_scrub(uint32_t index);
/* The scrub succeeded: the slice is free to lease. */
void virt_slice_arena_unquarantine(uint32_t index);

/* Write the slice header (amba_virt_iav_win.h, AMBA_VIRT_IAV_WIN_ID_SLICE + index) in the claim page. 0 or -errno. */
int virt_slice_arena_write_header(uint32_t index);

/* One line for iav-status: quarantined slices, in-flight calls, scrub counts and the last scrub. Bytes or -1. */
int virt_slice_arena_format(char *buf, size_t len);

#endif
