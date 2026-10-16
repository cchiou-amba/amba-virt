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
	AMBA_VIRT_IAV_OP_SLICE = 7,	/* an ioctl on a private buffer: admitted, then checked as OP_IOCTL */
	AMBA_VIRT_IAV_OP_HELLO = 8,	/* negotiate revision and features; an older broker answers -ENOSYS */
	AMBA_VIRT_IAV_OP_ARENA = 9,	/* an ioctl naming arena blocks: translated into the caller's slice, then as OP_IOCTL */
};

/*
 * Negotiation (OP_HELLO). A module sends struct amba_virt_iav_hello as the argument, with handle 0. The
 * broker answers with struct amba_virt_iav_hello_resp as the output and the granted feature bits in
 * resp.value. The answer is kept per guest (kernel-reported CID) until the guest disconnects or the broker
 * restarts, so a module sends HELLO again whenever it reopens a session after -ESTALE. A guest that never
 * sends HELLO, or an older broker that answers -ENOSYS, runs the legacy protocol: OP_INFO lists only the
 * legacy windows (ids below AMBA_VIRT_IAV_MAX_WINDOWS) in struct amba_virt_iav_window, every ioctl argument
 * has the size the ioctl header names, and no feature bit is in effect. AMBA_VIRT_IAV_PROTO stays 1: every
 * addition is a new op, a trailing structure or a feature bit, and either side ignores bits it does not know.
 */
#define AMBA_VIRT_IAV_REVISION		2u

/* Feature bits. A bit is in effect only when the module offered it and the broker granted it. */
#define AMBA_VIRT_IAV_F_NC_WINDOWS	(1ull << 0)	/* the host maps every window Normal-NC (unaligned access is legal) */
#define AMBA_VIRT_IAV_F_SIZE_OVERRIDE	(1ull << 1)	/* arguments use the table's wire size where it differs from the header */
#define AMBA_VIRT_IAV_F_EXT_INFO	(1ull << 2)	/* OP_INFO answers with struct amba_virt_iav_entry, per guest */
#define AMBA_VIRT_IAV_F_APERTURE	(1ull << 3)	/* the per-guest aux aperture: its descriptor and entries in OP_INFO */
#define AMBA_VIRT_IAV_F_ARENA		(1ull << 4)	/* anonymous partitions in the private slice (OP_ARENA, the arena entry) */
#define AMBA_VIRT_IAV_F_INLINE		(1ull << 5)	/* user pointers of an ioctl marshaled inline (IMG_GET_MISC_INFO) */

/* The role the broker holds for the guest, as the HELLO answer reports it. */
#define AMBA_VIRT_IAV_ROLE_CONSUMER	0u	/* frames and queries only */
#define AMBA_VIRT_IAV_ROLE_PRIMARY	1u	/* may configure and run the pipeline */
#define AMBA_VIRT_IAV_ROLE_QUALIFY	2u	/* qualification server: every guest acts as the primary */

struct amba_virt_iav_hello {
	__u32 revision;		/* AMBA_VIRT_IAV_REVISION of the module */
	__u32 page_size;	/* the guest's page size in bytes (a power of two, 4 KiB to 64 KiB) */
	__u64 features;		/* AMBA_VIRT_IAV_F_* the module understands */
	__u32 max_window_id;	/* the largest window id the module can hold */
	__u32 table_rows;	/* rows in the module's ioctl table */
	__u64 table_hash;	/* FNV-1a 64 over the module's table rows (see amba_virt_iav_table.h) */
};

struct amba_virt_iav_hello_resp {
	__u32 revision;		/* AMBA_VIRT_IAV_REVISION of the broker */
	__u32 role;		/* AMBA_VIRT_IAV_ROLE_* of this guest when it asked */
	__u64 features;		/* granted: offered AND supported by this broker and host */
	__u32 max_window_id;	/* the largest window id OP_INFO will report to this guest */
	__u32 table_rows;	/* rows in the broker's ioctl table */
	__u64 table_hash;	/* the broker's table hash; a difference is reported, not refused */
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

/*
 * Argument of AMBA_VIRT_IAV_OP_SLICE: this structure, then the ioctl argument exactly as OP_IOCTL would carry
 * it (request.cmd is the ioctl). phys must be zero: the guest does not name memory. The range is admitted
 * against the caller's own private slice first; the ioctl then goes through the same allow-list and checks as
 * OP_IOCTL. The response carries no address (resp.value is 0).
 */
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

/* Window and entry flags (struct amba_virt_iav_entry.flags; struct amba_virt_iav_window.flags stays 0). */
#define AMBA_VIRT_IAV_ENT_PRIMARY_ONLY	(1u << 0)	/* only the primary guest may map it */
#define AMBA_VIRT_IAV_ENT_NORMAL_NC	(1u << 1)	/* the host maps it Normal-NC */
#define AMBA_VIRT_IAV_ENT_RING		(1u << 2)	/* may be mapped twice in a row (every entry may; informational) */
#define AMBA_VIRT_IAV_ENT_ARENA		(1u << 3)	/* the caller's private slice, the backing store of its arena */
#define AMBA_VIRT_IAV_ENT_APERTURE	(1u << 4)	/* the caller's aux aperture (its descriptor) or an entry in it */

/*
 * The arena entry (AMBA_VIRT_IAV_F_ARENA). OP_INFO lists it, with this id, to a guest that negotiated the arena: base
 * is the address of the start of the guest's private slice in the IAV address space (the partition behind it is pid,
 * the pinned private pool), size the size of the slice BAR, part_len the bytes of it the arena may use (the last page
 * is the slice's claim page, which carries the slice header below), and reserved the slice index. The guest module
 * finds the slice BAR by that header.
 */
#define AMBA_VIRT_IAV_ENT_ID_ARENA	0x100u

/*
 * The aux aperture (AMBA_VIRT_IAV_F_APERTURE, plan_iav_usb_yolo Envelope F). OP_INFO lists, to a guest that negotiated
 * it, a descriptor with id AMBA_VIRT_IAV_ENT_ID_APERTURE (flags ENT_APERTURE, size the aperture BAR, offset the BAR
 * offset of its header page, reserved the epoch its header carries, base, part_len and pid zero), and one entry per
 * partition in it with id AMBA_VIRT_IAV_ENT_ID_AUX + pid: base the address QUERY_MEMBLOCK reports for the partition,
 * part_len the bytes of it the entry covers (what QUERY_MEMBLOCK reports), offset where it starts in the aperture BAR,
 * size the bytes from offset to the next entry or the header page. The guest module finds the BAR by the header
 * (amba_virt_iav_win.h, struct amba_virt_iav_aux_hdr) and maps an entry from it at offset; OP_MAP of the entry's base
 * answers the entry id and offset.
 */
#define AMBA_VIRT_IAV_ENT_ID_APERTURE	0x200u
#define AMBA_VIRT_IAV_ENT_ID_AUX	0x300u	/* + the IAV partition id (below 0x100) */

/*
 * One entry in the OP_INFO output to a guest that negotiated AMBA_VIRT_IAV_F_EXT_INFO: only the entries this
 * guest may map, only ids up to the max_window_id it declared. One message carries up to
 * AMBA_VIRT_IAV_MAX_OUT / sizeof(struct amba_virt_iav_entry) entries; no window maximum is shared across
 * versions.
 */
struct amba_virt_iav_entry {
	__u32 id;		/* AMBA_VIRT_IAV_WIN_* below AMBA_VIRT_IAV_MAX_WINDOWS; later ids are aperture entries */
	__u32 flags;		/* AMBA_VIRT_IAV_ENT_* */
	__u64 base;		/* the address QUERY_MEMBLOCK reports for the partition */
	__u64 size;		/* size of the backing BAR region (a power of two for a legacy window) */
	__u64 part_len;		/* bytes of the entry that are the partition */
	__u64 offset;		/* offset of the entry inside its BAR (0 for a legacy window) */
	__u32 pid;		/* the IAV partition id */
	__u32 reserved;		/* zero */
};

/*
 * Argument of AMBA_VIRT_IAV_OP_ARENA: this header, then the ioctl argument exactly as OP_IOCTL would carry it
 * (request.cmd is the ioctl). The ioctl names an arena block by its virtual anonymous partition id in a partition id
 * field (SET_IMG_SCALE input_buf.pid and output_buf.pid, GDMA_COPY src_mmap_type and dst_mmap_type,
 * SYNC_CACHE_MEM_PART pid) with offsets relative to the block, as an application names an anonymous partition on the
 * bare-metal driver. blk[] says where each block it names lies in the caller's own slice: offset from the start of the
 * slice (a multiple of AMBA_VIRT_IAV_ARENA_ALIGN) and length (a multiple of 4 KiB), inside the arena. The broker keeps
 * no allocation state: it confines each block to the caller's slice, every extent the call names to its block, and
 * rewrites the field to the pinned pool partition and the offset inside it. A guest that forges a block can name only
 * its own slice. Unused slots are all zero.
 */
#define AMBA_VIRT_IAV_ARENA_ALIGN	0x10000u	/* 64 KiB: a 64 KiB-page guest can map every block */
#define AMBA_VIRT_IAV_ARENA_MAX_BLKS	2u

struct amba_virt_iav_arena_blk {
	__u32 vid;		/* the virtual anonymous partition id ((n << 8) | 0xff) */
	__u32 reserved;		/* zero */
	__u64 offset;		/* from the start of the slice */
	__u64 length;		/* bytes of the block */
};

struct amba_virt_iav_arena_req {
	__u32 count;		/* blocks in blk[], 1 or 2 */
	__u32 reserved;		/* zero */
	struct amba_virt_iav_arena_blk blk[AMBA_VIRT_IAV_ARENA_MAX_BLKS];
};

/*
 * Inline marshal of IAV_IOC_IMG_GET_MISC_INFO (AMBA_VIRT_IAV_F_INLINE). The argument is struct img_misc_info with its
 * three pointer fields replaced by presence flags (0: NULL, 1: the application gave a buffer), followed by this
 * trailer; the output is the struct (flags as sent) followed by the trailer as the driver left it. The driver reads
 * query[i] and writes valid[i] and extra[i] for i below the pipeline's channel count (at most 4 on N1-655); the module
 * copies count entries in and out of the application's buffers, count being the channel count it read with
 * GET_SYSTEM_RESOURCE. A guest pointer never crosses the wire.
 */
#define AMBA_VIRT_IAV_INLINE_MISC_CHANS		16u	/* IMG_MAX_VIN_CHANNEL_NUM */
#define AMBA_VIRT_IAV_INLINE_MISC_EXTRA_BYTES	32u	/* sizeof(struct img_extra_raw_info) */

struct amba_virt_iav_inline_misc {
	__u8 query[AMBA_VIRT_IAV_INLINE_MISC_CHANS];
	__u8 valid[AMBA_VIRT_IAV_INLINE_MISC_CHANS];
	__u8 extra[AMBA_VIRT_IAV_INLINE_MISC_CHANS][AMBA_VIRT_IAV_INLINE_MISC_EXTRA_BYTES];
	__u32 count;		/* entries the module copies, at most AMBA_VIRT_IAV_INLINE_MISC_CHANS */
	__u32 reserved;		/* zero */
};

/* The MAP output: which window and where in it to map. */
struct amba_virt_iav_map {
	__u32 window;		/* AMBA_VIRT_IAV_WIN_*, or an aperture entry id (AMBA_VIRT_IAV_ENT_ID_AUX + pid) */
	__u32 prot;		/* PROT_* bits the broker allows */
	__u64 window_offset;	/* offset into the window's BAR to map from (an aperture entry's offset in the aperture) */
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
