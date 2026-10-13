/*
 * amba_virt_iav_win.h
 *
 * Host-side IAV windows: physical ranges of the real IAV driver's memory
 * (the DSP partition, the bitstream buffer, the decode partition and the overlay partition) exposed as mmap-able nodes
 * that the hypervisor maps into a guest as ivshmem BARs.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef AMBA_VIRT_IAV_WIN_H
#define AMBA_VIRT_IAV_WIN_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

#define AMBA_VIRT_IAV_WIN_DSP_NAME	"amba_virt_iav_dsp"
#define AMBA_VIRT_IAV_WIN_BSB_NAME	"amba_virt_iav_bsb"
#define AMBA_VIRT_IAV_WIN_DEC_NAME	"amba_virt_iav_dec"
#define AMBA_VIRT_IAV_WIN_OVL_NAME	"amba_virt_iav_ovl"
#define AMBA_VIRT_IAV_WIN_DSP_PATH	"/dev/amba_virt_iav_dsp"
#define AMBA_VIRT_IAV_WIN_BSB_PATH	"/dev/amba_virt_iav_bsb"
#define AMBA_VIRT_IAV_WIN_DEC_PATH	"/dev/amba_virt_iav_dec"
#define AMBA_VIRT_IAV_WIN_OVL_PATH	"/dev/amba_virt_iav_ovl"

#define AMBA_VIRT_IAV_WIN_SETS		1	/* every attached guest maps the same nodes */
#define AMBA_VIRT_IAV_WIN_KINDS		4	/* DSP, BSB, DEC and OVL windows in each set */

/*
 * A window is `len` bytes of real memory at `phys`, followed by zeroed
 * private padding up to `size`, which must be a power of two because a
 * PCI BAR is. The guest can read and write the padding but it is not
 * DSP memory.
 */
struct amba_virt_iav_win_cfg {
	__u64 phys;		/* page aligned, below 4 GiB (the DSP is 32-bit) */
	__u64 len;		/* page multiple, non-zero */
	__u64 size;		/* power of two, >= len */
	__u32 flags;		/* must be zero */
	__u32 reserved;	/* must be zero */
};

/*
 * A window that has padding carries this header in the last bytes of its padding, so a guest
 * can tell a window from a data slice (which carries a slice claim instead) without reading
 * the real memory in front of it. A window with no padding (the DSP partition when it is
 * already a power of two) has none, and is recognized by elimination.
 */
#define AMBA_VIRT_IAV_WIN_HDR_MAGIC	0x57494d41u	/* "AMIW" little-endian */
#define AMBA_VIRT_IAV_WIN_HDR_VERSION	1u
#define AMBA_VIRT_IAV_WIN_HDR_BYTES	64u

struct amba_virt_iav_win_hdr {
	__u32 magic;		/* AMBA_VIRT_IAV_WIN_HDR_MAGIC */
	__u32 version;	/* AMBA_VIRT_IAV_WIN_HDR_VERSION */
	__u32 window_id;	/* 0: DSP, 1: BSB, 2: DEC, 3: OVL (the ids of amba_virt_iav.h) */
	__u32 reserved;
	__u64 part_len;	/* bytes of the window that are real memory */
	__u64 size;		/* size of the window */
};

#define AMBA_VIRT_IAV_WIN_MAGIC	'W'
/* CAP_SYS_ADMIN. Refused while the node is mapped. */
#define AMBA_VIRT_IAV_WIN_IOC_CONFIG \
	_IOW(AMBA_VIRT_IAV_WIN_MAGIC, 1, struct amba_virt_iav_win_cfg)
/* Returns the current configuration (all zero if unconfigured). */
#define AMBA_VIRT_IAV_WIN_IOC_GET \
	_IOR(AMBA_VIRT_IAV_WIN_MAGIC, 2, struct amba_virt_iav_win_cfg)
/* CAP_SYS_ADMIN. Drops the configuration. Refused while mapped. */
#define AMBA_VIRT_IAV_WIN_IOC_CLEAR \
	_IO(AMBA_VIRT_IAV_WIN_MAGIC, 3)

#endif /* AMBA_VIRT_IAV_WIN_H */
