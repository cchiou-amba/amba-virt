/*
 * amba_virt_iav.h
 *
 * Wire protocol of the virtual /dev/iav: a guest frontend (iav_virt) asks the IAV broker in
 * amba-virt-server to open, close and ioctl the real IAV driver on its behalf, and to authorize
 * mappings of the IAV windows.
 *
 * Messages travel as the payload of an AMBA_VIRT_MSG_IAV_REQ / AMBA_VIRT_MSG_IAV_RESP message,
 * after the common struct amba_virt_msg header. All fields are little-endian and fixed width. No
 * field is a pointer. A guest never sends a guest virtual address or a host pointer, and it never
 * has to know where its windows sit in its own physical address space: every reference to memory
 * is an address inside a window the broker bound to that guest, or a window id with an offset.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_AMBA_VIRT_IAV_H
#define _UAPI_AMBA_VIRT_IAV_H

#include "amba_virt.h"

#define AMBA_VIRT_MSG_IAV_REQ	72u
#define AMBA_VIRT_MSG_IAV_RESP	73u

#define AMBA_VIRT_IAV_PROTO	1u

enum amba_virt_iav_op {
	AMBA_VIRT_IAV_OP_OPEN = 1,	/* open a real /dev/iav fd; response.handle names it */
	AMBA_VIRT_IAV_OP_CLOSE = 2,	/* close the fd named by request.handle */
	AMBA_VIRT_IAV_OP_IOCTL = 3,	/* ioctl cmd on the fd; arg bytes follow the request */
	AMBA_VIRT_IAV_OP_MAP = 4,	/* authorize a mapping: arg_value = address, arg2 = length */
	AMBA_VIRT_IAV_OP_POLL = 5,	/* wait up to arg_value ms for the events in cmd */
	AMBA_VIRT_IAV_OP_INFO = 6,	/* list the windows bound to the calling guest */
	AMBA_VIRT_IAV_OP_SLICE = 7,	/* a private buffer; admitted before any driver ioctl */
};

/* Window ids. The identity of a window is fixed; its size and base come from OP_INFO. */
#define AMBA_VIRT_IAV_WIN_DSP	0u	/* the IAV_PART_DSP partition */
#define AMBA_VIRT_IAV_WIN_BSB	1u	/* the IAV_PART_BSB partition, padded to a power of two */
#define AMBA_VIRT_IAV_WIN_DEC	2u	/* the IAV_PART_DEC_BSB partition (the decoder's bitstream and output), padded to a power of two */
#define AMBA_VIRT_IAV_WIN_OVL	3u	/* the IAV_PART_OVERLAY partition (overlay data and colour tables) */
#define AMBA_VIRT_IAV_MAX_WINDOWS	4u

/*
 * The longest a single POLL occupies the guest's RPC channel, which every other call from the guest waits behind. It
 * was 100 ms; a poller then delayed other calls by up to 200 ms (plan_iav_virtualization_phase2, finding F13), so it is
 * short: the worker asks about 50 times a second while a descriptor is being polled, which costs well under one percent
 * of a CPU at the measured 40 us per call.
 */
#define AMBA_VIRT_IAV_POLL_SLICE_MS	20u

/* Argument of AMBA_VIRT_IAV_OP_SLICE. phys must be zero: the guest does not name memory. */
struct amba_virt_iav_slice_req {
	__u32 claimed_slice;
	__u32 partition_id;
	__u64 offset;
	__u64 length;
	__u64 phys;
};
struct amba_virt_iav_req {
	__u32 proto;		/* AMBA_VIRT_IAV_PROTO */
	__u32 op;		/* enum amba_virt_iav_op */
	__u64 handle;		/* 0 for OPEN and INFO */
	__u32 cmd;		/* IOCTL: the ioctl number; POLL: poll event mask; MAP: PROT_* bits */
	__u32 arg_len;		/* bytes after this struct */
	__u64 arg_value;	/* IOCTL: value-in-arg; MAP: address; POLL: timeout in ms */
	__u64 arg2;		/* MAP: length */
};

/* Response: follows struct amba_virt_msg. out_len bytes of output follow it. */
struct amba_virt_iav_resp {
	__u32 proto;		/* AMBA_VIRT_IAV_PROTO */
	__s32 status;		/* 0: handled; negative errno: refused by the broker */
	__s32 rc;		/* IOCTL: the real ioctl's return value, or -1 */
	__s32 err;		/* IOCTL: errno when rc is -1 */
	__u64 handle;		/* OPEN: the new handle */
	__u64 value;		/* POLL: revents; MAP: window id */
	__u32 out_len;		/* bytes after this struct */
	__u32 reserved;		/* zero */
};

/* One window in the OP_INFO output (an array of these follows the response). */
struct amba_virt_iav_window {
	__u32 id;		/* AMBA_VIRT_IAV_WIN_* */
	__u32 flags;
	__u64 base;		/* the address the IAV driver reports for the start of the window */
	__u64 size;		/* size of the window and of its backing device (a power of two) */
	__u64 part_len;		/* bytes of the window that are the partition; the rest is padding */
};

/* The MAP output: which window and where in it to map. */
struct amba_virt_iav_map {
	__u32 window;		/* AMBA_VIRT_IAV_WIN_* */
	__u32 prot;		/* PROT_* bits the broker allows */
	__u64 window_offset;	/* offset into the window's BAR to map from */
	__u64 length;		/* length to map */
};

/* Largest argument or output the protocol can carry in one message. */
#define AMBA_VIRT_IAV_MAX_ARG \
	(AMBA_VIRT_MAX_MSG - (unsigned int)sizeof(struct amba_virt_msg) - \
	 (unsigned int)sizeof(struct amba_virt_iav_req))
#define AMBA_VIRT_IAV_MAX_OUT \
	(AMBA_VIRT_MAX_MSG - (unsigned int)sizeof(struct amba_virt_msg) - \
	 (unsigned int)sizeof(struct amba_virt_iav_resp))

#endif /* _UAPI_AMBA_VIRT_IAV_H */
