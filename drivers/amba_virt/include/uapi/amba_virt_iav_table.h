/*
 * amba_virt_iav_table.h
 *
 * The vocabulary of the generated IAV ioctl table (tools/iav_ioctl_table.inc), shared by the
 * Dom0 broker and the guest iav_virt module so that both read one table.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_AMBA_VIRT_IAV_TABLE_H
#define _UAPI_AMBA_VIRT_IAV_TABLE_H

enum iav_magic {
	IAV_MAGIC_ENC,
	IAV_MAGIC_DEC,
	IAV_MAGIC_VIN,
	IAV_MAGIC_VOUT,
	IAV_MAGIC_IMAGE,
};

enum iav_arg_kind {
	IAV_ARG_NONE,		/* the argument is ignored */
	IAV_ARG_VALUE,		/* the argument is a value, not a pointer */
	IAV_ARG_PTR,		/* the argument points to a struct or scalar of the entry's size */
};

enum iav_dir {
	IAV_DIR_NONE = 0,
	IAV_DIR_R = 1,		/* the driver writes the argument */
	IAV_DIR_W = 2,		/* the driver reads the argument */
	IAV_DIR_RW = 3,
};

/* What the generator found in the argument struct. */
#define IAV_CLS_PLAIN		0x01u
#define IAV_CLS_POINTER		0x02u
#define IAV_CLS_PHYS		0x04u
#define IAV_CLS_OFFSET_FD	0x08u

#define IAV_F_STATE		0x01u	/* changes global pipeline state */
#define IAV_F_DEBUG		0x02u
#define IAV_F_BLOCKING		0x04u	/* the handler waits */
#define IAV_F_LEADING_ID	0x08u	/* the first u32 of the argument is a vsrc or vout id */

/*
 * The last column of a row is the wire size: the bytes the handler really copies when that differs from the
 * type its ioctl header names (the header names a scalar, the handler copies a struct), else 0. With
 * AMBA_VIRT_IAV_F_SIZE_OVERRIDE in effect the module and the broker both carry the wire size.
 */
#define IAV_WIRE_SIZE(size, wire)	((wire) ? (wire) : (size))

/*
 * The table hash a module and a broker exchange in OP_HELLO: FNV-1a 64 over each row's cmd, kind, dir, size
 * and wire size as little-endian u32 values, in table order. It tells a module and a broker built from
 * different IAV headers apart; a difference is reported, the per-call size check still decides.
 */
#define IAV_TABLE_HASH_INIT	0xcbf29ce484222325ull

static inline unsigned long long iav_table_hash_u32(unsigned long long h, unsigned int v)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		h ^= (v >> (8 * i)) & 0xffu;
		h *= 0x100000001b3ull;
	}
	return h;
}

#endif /* _UAPI_AMBA_VIRT_IAV_TABLE_H */
