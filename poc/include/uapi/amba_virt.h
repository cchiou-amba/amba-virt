/*
 * amba_virt.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_AMBA_VIRT_H
#define _UAPI_AMBA_VIRT_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define AMBA_VIRT_DEV_NAME	"amba_virt"
#define AMBA_VIRT_DEV_PATH	"/dev/amba_virt"

#define AMBA_VIRT_PROTO		1u

/* Host CID on EVE/QEMU vhost-vsock. Do not use port 2000 (VComLink). */
#define AMBA_VIRT_VSOCK_CID	2u
#define AMBA_VIRT_VSOCK_PORT	5555u

#define AMBA_VIRT_MAX_MSG	4096u

#define AMBA_VIRT_ROLE_GUEST	0u
#define AMBA_VIRT_ROLE_HOST	1u

struct amba_virt_info {
	__u32 proto;
	__u32 role;
	__u32 shm_size;
	__u32 connected;
	__u32 vsock_cid;
	__u32 vsock_port;
	__u32 reserved[2];
};

struct amba_virt_xfer {
	__u32 len;
	__s32 timeout_ms;	/* RECV only; 0 = 5000 ms; <0 = wait forever */
	__u8  data[AMBA_VIRT_MAX_MSG];
};

/* Userspace payload (inside xfer.data), not interpreted by the kmod. */
#define AMBA_VIRT_MSG_PING		1u
#define AMBA_VIRT_MSG_PONG		2u
#define AMBA_VIRT_MSG_SHM_NOTIFY	3u
#define AMBA_VIRT_MSG_SHM_ACK		4u

struct amba_virt_msg {
	__u32 type;
	__u32 seq;
	__u32 shm_off;
	__u32 shm_len;
};

#define AMBA_VIRT_IOC_MAGIC	'A'
#define AMBA_VIRT_IOC_GET_INFO	_IOR(AMBA_VIRT_IOC_MAGIC, 1, struct amba_virt_info)
#define AMBA_VIRT_IOC_CONNECT	_IO(AMBA_VIRT_IOC_MAGIC, 2)
#define AMBA_VIRT_IOC_SEND	_IOW(AMBA_VIRT_IOC_MAGIC, 3, struct amba_virt_xfer)
#define AMBA_VIRT_IOC_RECV	_IOWR(AMBA_VIRT_IOC_MAGIC, 4, struct amba_virt_xfer)

#endif /* _UAPI_AMBA_VIRT_H */
