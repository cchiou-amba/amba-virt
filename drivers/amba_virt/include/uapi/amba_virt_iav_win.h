/*
 * amba_virt_iav_win.h
 *
 * Host-side IAV windows: physical ranges of the real IAV driver's memory
 * (the DSP partition, the bitstream buffer, the decode partition and the overlay partition) exposed as mmap-able nodes
 * that the hypervisor maps into a guest as ivshmem BARs, and the per-guest aux apertures that compose several
 * partitions into one BAR.
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
	__u32 flags;		/* zero in CONFIG; GET reports AMBA_VIRT_IAV_WIN_F_* */
	__u32 reserved;	/* must be zero */
};

/*
 * Reported by GET, never accepted by CONFIG: the module maps the real range of the window Normal-NC at stage 2
 * (VM_ALLOW_ANY_UNCACHED), so a guest may access it unaligned and zero it with DC ZVA.
 */
#define AMBA_VIRT_IAV_WIN_F_NORMAL_NC	0x1u

/*
 * A window that has padding carries this header in the last bytes of its padding, so a guest
 * can tell a window from a data slice (which carries a slice claim instead) without reading
 * the real memory in front of it. A window with no padding (the DSP partition when it is
 * already a power of two) has none, and is recognized by elimination.
 */
#define AMBA_VIRT_IAV_WIN_HDR_MAGIC	0x57494d41u	/* "AMIW" little-endian */
#define AMBA_VIRT_IAV_WIN_HDR_VERSION	1u
#define AMBA_VIRT_IAV_WIN_HDR_BYTES	64u

/*
 * A private IAV slice (the backing store of a guest's arena, amba_virt_iav.h AMBA_VIRT_IAV_F_ARENA) carries the same
 * header in the last bytes of its claim page, written by the server when it leases the slice, with window_id
 * AMBA_VIRT_IAV_WIN_ID_SLICE + the slice index, part_len the usable bytes and size the slice size. A window id at or
 * above AMBA_VIRT_IAV_WIN_ID_SLICE is never a legacy window, so a module that knows no arena leaves it alone.
 */
#define AMBA_VIRT_IAV_WIN_ID_SLICE	0x100u

struct amba_virt_iav_win_hdr {
	__u32 magic;		/* AMBA_VIRT_IAV_WIN_HDR_MAGIC */
	__u32 version;	/* AMBA_VIRT_IAV_WIN_HDR_VERSION */
	__u32 window_id;	/* 0: DSP, 1: BSB, 2: DEC, 3: OVL (the ids of amba_virt_iav.h); AMBA_VIRT_IAV_WIN_ID_SLICE + n: slice n */
	__u32 reserved;
	__u64 part_len;	/* bytes of the window that are real memory */
	__u64 size;		/* size of the window */
};

/*
 * The per-guest aux aperture (plan_iav_usb_yolo Envelope F, memory_adapter_proposal_v2 3e): one node per Cavalry
 * lease index, /dev/amba_virt_iav_aux<n>, that the server composes for the guest holding lease n from the partitions
 * its role may map. The node is one BAR of `size` bytes (a power of two): each range maps `len` bytes of real memory at
 * `offset` (64 KiB aligned); every other page below the last maps one zeroed scratch page of the node; the last page
 * carries struct amba_virt_iav_aux_hdr in its last 64 bytes. A range must equal a source the server registered with
 * AUX_SOURCES. The node is reconfigured only while nothing maps it.
 */
#define AMBA_VIRT_IAV_AUX_NAME_FMT	"amba_virt_iav_aux%u"
#define AMBA_VIRT_IAV_AUX_PATH_FMT	"/dev/amba_virt_iav_aux%u"
#define AMBA_VIRT_IAV_AUX_NODES		8u	/* one per Cavalry lease index */
#define AMBA_VIRT_IAV_AUX_MAX_RANGES	16u
#define AMBA_VIRT_IAV_AUX_MAX_SOURCES	32u
#define AMBA_VIRT_IAV_AUX_ALIGN		0x10000u	/* range offsets in the BAR */
#define AMBA_VIRT_IAV_AUX_MIN_SIZE	0x2000ull	/* a range page and the header page */
#define AMBA_VIRT_IAV_AUX_MAX_SIZE	0x40000000ull
/* Range flags. Read-only is defined for phase 3 and refused now. */
#define AMBA_VIRT_IAV_AUX_F_READONLY	0x1u

struct amba_virt_iav_aux_range {
	__u64 phys;		/* page aligned */
	__u64 len;		/* page multiple, non-zero */
	__u64 offset;		/* in the BAR, AMBA_VIRT_IAV_AUX_ALIGN aligned */
	__u32 pid;		/* the IAV partition (informational for the module, checked against the source) */
	__u32 flags;		/* AMBA_VIRT_IAV_AUX_F_*; must be zero */
};

struct amba_virt_iav_aux_cfg {
	__u64 size;		/* power of two, AMBA_VIRT_IAV_AUX_MIN_SIZE..AMBA_VIRT_IAV_AUX_MAX_SIZE */
	__u64 epoch;		/* non-zero; the lease epoch of the guest the node is composed for */
	__u32 count;		/* ranges, 1..AMBA_VIRT_IAV_AUX_MAX_RANGES */
	__u32 flags;		/* zero in AUX_CONFIG; AUX_GET reports AMBA_VIRT_IAV_WIN_F_NORMAL_NC */
	__u32 maps;		/* AUX_GET: the mappings of the node now; zero in AUX_CONFIG */
	__u32 reserved;		/* zero */
	struct amba_virt_iav_aux_range range[AMBA_VIRT_IAV_AUX_MAX_RANGES];	/* unused entries zero */
};

/* The physical ranges a node may map (module-wide). */
struct amba_virt_iav_aux_source {
	__u64 phys;
	__u64 len;
	__u32 pid;
	__u32 reserved;		/* zero */
};

struct amba_virt_iav_aux_sources {
	__u32 count;		/* 0..AMBA_VIRT_IAV_AUX_MAX_SOURCES */
	__u32 reserved;		/* zero */
	struct amba_virt_iav_aux_source src[AMBA_VIRT_IAV_AUX_MAX_SOURCES];	/* unused entries zero */
};

/* The identity of an aperture, in the last 64 bytes of its last page. */
#define AMBA_VIRT_IAV_AUX_HDR_MAGIC	0x50414d41u	/* "AMAP" little-endian */
#define AMBA_VIRT_IAV_AUX_HDR_VERSION	1u

struct amba_virt_iav_aux_hdr {
	__u32 magic;		/* AMBA_VIRT_IAV_AUX_HDR_MAGIC */
	__u32 version;		/* AMBA_VIRT_IAV_AUX_HDR_VERSION */
	__u32 node;		/* the node index */
	__u32 count;		/* ranges */
	__u64 epoch;		/* as configured */
	__u64 size;		/* the BAR */
	__u32 reserved[8];	/* zero */
};

#define AMBA_VIRT_IAV_WIN_MAGIC	'W'
/* CAP_SYS_ADMIN. Refused while the node is mapped. */
#define AMBA_VIRT_IAV_WIN_IOC_CONFIG \
	_IOW(AMBA_VIRT_IAV_WIN_MAGIC, 1, struct amba_virt_iav_win_cfg)
/* Returns the current configuration (all zero if unconfigured). */
#define AMBA_VIRT_IAV_WIN_IOC_GET \
	_IOR(AMBA_VIRT_IAV_WIN_MAGIC, 2, struct amba_virt_iav_win_cfg)
/* CAP_SYS_ADMIN. Drops the configuration (of a window or an aperture). Refused while mapped. */
#define AMBA_VIRT_IAV_WIN_IOC_CLEAR \
	_IO(AMBA_VIRT_IAV_WIN_MAGIC, 3)
/* Aperture nodes only. CAP_SYS_ADMIN. Refused while the node is mapped. */
#define AMBA_VIRT_IAV_WIN_IOC_AUX_CONFIG \
	_IOW(AMBA_VIRT_IAV_WIN_MAGIC, 4, struct amba_virt_iav_aux_cfg)
/* Aperture nodes only. The current configuration (all zero if unconfigured) and the mapping count. */
#define AMBA_VIRT_IAV_WIN_IOC_AUX_GET \
	_IOR(AMBA_VIRT_IAV_WIN_MAGIC, 5, struct amba_virt_iav_aux_cfg)
/*
 * Aperture nodes only (any of them; the list is module-wide). CAP_SYS_ADMIN. Replaces the registered sources; refused
 * (EBUSY) if a configured node holds a range the new list does not contain.
 */
#define AMBA_VIRT_IAV_WIN_IOC_AUX_SOURCES \
	_IOW(AMBA_VIRT_IAV_WIN_MAGIC, 6, struct amba_virt_iav_aux_sources)

#endif /* AMBA_VIRT_IAV_WIN_H */
