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
#include <linux/notifier.h>
#include <amba_virt.h>

int amba_virt_get_window(phys_addr_t *phys, void __iomem **iomem,
			 size_t *size);
int amba_virt_rpc(const void *request, u32 request_len, void *response,
		  u32 *response_len, unsigned int timeout_ms);

int amba_virt_register_state_notifier(struct notifier_block *nb);
int amba_virt_unregister_state_notifier(struct notifier_block *nb);
void amba_virt_dispatch_state_event(struct amba_virt_dev_state_event *evt);

#endif

