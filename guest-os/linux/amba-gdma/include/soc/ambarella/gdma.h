/*
 * gdma.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_AMBARELLA_GDMA_H__
#define __SOC_AMBARELLA_GDMA_H__

struct gdma_param {
	unsigned long dest_addr;
	unsigned long dest_virt_addr;
	unsigned long src_addr;
	unsigned long src_virt_addr;
	u8 dest_non_cached;
	u8 src_non_cached;
	u8 reserved[2];
	u16 src_pitch;
	u16 dest_pitch;
	u16 width;
	u16 height;
};

#define GDMA_SRC_1_BASE_OFFSET		0x00
#define GDMA_SRC_1_PITCH_OFFSET		0x04
#define GDMA_SRC_2_BASE_OFFSET		0x08
#define GDMA_SRC_2_PITCH_OFFSET		0x0c
#define GDMA_DST_BASE_OFFSET		0x10
#define GDMA_DST_PITCH_OFFSET		0x14
#define GDMA_WIDTH_OFFSET		0x18
#define GDMA_HIGHT_OFFSET		0x1c
#define GDMA_TRANSPARENT_OFFSET		0x20
#define GDMA_OPCODE_OFFSET		0x24
#define GDMA_PENDING_OPS_OFFSET		0x28
#define GDMA_PIXELFORMAT_OFFSET		0x2c
#define GDMA_ALPHA_OFFSET		0x30
#define GDMA_CLUT_BASE_OFFSET		0x400

#define GDMA_PIXELFORMAT_THROTTLE_DRAM	(1L << 11)

int dma_memcpy(u8 *dest_addr, u8 *src_addr, u32 size);
int dma_noncache_memcpy(u8 *dest_addr, u8 *src_addr, u32 size);
int dma_pitch_memcpy(struct gdma_param *params);

#endif
