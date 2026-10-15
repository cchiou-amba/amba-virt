/*
 * amba_virt_usb_dma.h
 *
 * ABI of /dev/amba_virt_usb_dma: the 32 MiB physical DMA carveout
 * (virt_usb_dma@15c00000) that backs the passed-through xHCI controller.
 * The host driver writes the "AVUD" descriptor into the first page; the
 * guest amba_virt_usb.ko reads it through the ivshmem BAR.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMBA_VIRT_USB_DMA_H
#define AMBA_VIRT_USB_DMA_H

#include <linux/types.h>

#define AMBA_VIRT_USB_DMA_DEV_NAME	"amba_virt_usb_dma"
#define AMBA_VIRT_USB_DMA_OF_PATH	"/reserved-memory/virt_usb_dma@15c00000"

/* "AVUD" (Ambarella Virtual USB DMA), as a 32-bit value. */
#define AMBA_VIRT_USB_DMA_MAGIC		0x41565544U
#define AMBA_VIRT_USB_DMA_VERSION	1U

#define AMBA_VIRT_USB_DMA_PHYS_BASE	0x15c00000ULL
#define AMBA_VIRT_USB_DMA_TOTAL_SIZE	0x02000000ULL	/* 32 MiB */

/* Offsets are from the start of the carveout. */
#define AMBA_VIRT_USB_DMA_HDR_BYTES	0x1000ULL
#define AMBA_VIRT_USB_DMA_RINGS_OFFSET	0x00001000ULL	/* xHCI rings/contexts */
#define AMBA_VIRT_USB_DMA_RINGS_SIZE	0x003ff000ULL
#define AMBA_VIRT_USB_DMA_URB_OFFSET	0x00400000ULL	/* usbcore URB pool */
#define AMBA_VIRT_USB_DMA_URB_SIZE	0x01c00000ULL

/* Physical xHCI controller (xhci-hcd.0.auto) that owns this carveout. */
#define AMBA_VIRT_USB_XHCI_PHYS		0xff3c008000ULL
#define AMBA_VIRT_USB_XHCI_SIZE		0x10000ULL
#define AMBA_VIRT_USB_XHCI_DEV_NAME	"xhci-hcd.0.auto"

struct amba_virt_usb_dma_hdr {
	__u32 magic;		/* AMBA_VIRT_USB_DMA_MAGIC */
	__u32 version;		/* AMBA_VIRT_USB_DMA_VERSION */
	__u64 phys_base;
	__u64 total_size;
	__u64 rings_offset;
	__u64 rings_size;
	__u64 urb_offset;
	__u64 urb_size;
	__u8 reserved[AMBA_VIRT_USB_DMA_HDR_BYTES - 56];
};

#endif /* AMBA_VIRT_USB_DMA_H */
