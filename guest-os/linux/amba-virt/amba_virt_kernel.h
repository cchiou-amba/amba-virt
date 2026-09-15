/*
 * amba_virt_kernel.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMBA_VIRT_KERNEL_H
#define AMBA_VIRT_KERNEL_H

#include <linux/io.h>
#include <linux/types.h>

int amba_virt_get_window(phys_addr_t *phys, void __iomem **iomem,
			 size_t *size);
int amba_virt_rpc(const void *request, u32 request_len, void *response,
		  u32 *response_len, unsigned int timeout_ms);

#endif
