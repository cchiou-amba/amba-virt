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

#endif /* _UAPI_AMBA_VIRT_IAV_TABLE_H */
