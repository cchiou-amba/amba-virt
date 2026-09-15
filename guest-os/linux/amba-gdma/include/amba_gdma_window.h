/*
 * amba_gdma_window.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMBA_GDMA_WINDOW_H
#define AMBA_GDMA_WINDOW_H

#include <linux/io.h>
#include <linux/types.h>

void __iomem *gdma_window_alloc(phys_addr_t *phys, size_t len);
void gdma_window_free(void __iomem *addr, size_t len);

#endif
