/*
 * virt_camera_policy.h
 *
 * Which guests get the shared camera windows (DSP, bitstream, decode, overlay).
 *
 * The answer for one guest combines three things, in the server:
 *   - the fleet intent the hypervisor read from the ticket's cbattr ("camera": on, off, or absent),
 *   - a persisted Dom0 override for the ticket (on, off, or inherit), and
 *   - whether the DSP and bitstream window nodes were configured when the broker started.
 * effective = windows_ready AND (override if override is not inherit, else intent is on).
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef VIRT_CAMERA_POLICY_H
#define VIRT_CAMERA_POLICY_H

#include <stddef.h>
#include <stdint.h>

#define VIRT_CAMERA_STORE		"/persist/etc/amba-virt/camera-tickets"
#define VIRT_CAMERA_TICKET_MAX		48
#define VIRT_CAMERA_MAX_ENTRIES		64
#define VIRT_CAMERA_STORE_MAX_BYTES	4096
#define VIRT_CAMERA_GET_MAX_LINES	32

enum virt_camera_intent {
	VIRT_CAMERA_INTENT_ABSENT = 0,
	VIRT_CAMERA_INTENT_OFF,
	VIRT_CAMERA_INTENT_ON,
};

/* A configured window: id is an AMBA_VIRT_IAV_WIN_* value, size its power-of-two window size. */
struct virt_camera_window {
	uint32_t id;
	uint64_t size;
};

struct virt_camera_verdict {
	int camera;		/* 1: attach the camera windows */
	uint64_t dsp;		/* sizes; 0 when the window is not attached */
	uint64_t bsb;
	uint64_t dec;
	uint64_t ovl;
};

/* CLOCK_MONOTONIC in milliseconds, the clock of the daemon log. */
uint64_t virt_camera_policy_now_ms(void);

/* Back to the start-up state: no windows, no overrides, the default store path. For tests and restart. */
void virt_camera_policy_reset(void);
void virt_camera_policy_set_store(const char *path);

/* Read the store. A missing file is empty. A corrupt file is logged and treated as empty. Always returns 0. */
int virt_camera_policy_load(void);

/*
 * Record the windows the broker configured and when. Called once per server run, after the broker
 * configured the nodes. The windows are ready only if both DSP and BSB are present.
 */
void virt_camera_policy_set_windows(const struct virt_camera_window *w, unsigned int n, uint64_t ready_ms);

/* 1 if ticket is 1 to VIRT_CAMERA_TICKET_MAX characters from [A-Za-z0-9_.-]. */
int virt_camera_ticket_ok(const char *ticket);

/* "on", "off" or "absent". Returns 0 or -1. */
int virt_camera_intent_parse(const char *s, enum virt_camera_intent *out);

/*
 * The verdict for one reserve. index is the Cavalry slice index just leased: only index 0 may get the
 * decode and overlay windows. Records intent as the ticket's last seen intent. Returns 0, or -1 for a
 * bad ticket (the verdict is then camera 0).
 */
int virt_camera_policy_decide(const char *ticket, enum virt_camera_intent intent, uint32_t index,
			      struct virt_camera_verdict *v);

/* " camera=0" or " camera=1 dsp=<n> bsb=<n>[ dec=<n>][ ovl=<n>]" appended to buf. Returns the bytes written or -1. */
int virt_camera_verdict_format(const struct virt_camera_verdict *v, char *buf, size_t len);

/*
 * SET_CAMERA <ticket> <on|off|inherit>, GET_CAMERA [<ticket>] and CAMERA_STATUS. Always builds a
 * reply. Returns 0.
 */
int virt_camera_policy_admin(const char *req, char *resp, size_t resp_len);

#endif /* VIRT_CAMERA_POLICY_H */
