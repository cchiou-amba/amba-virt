/*
 * stdbool.h
 *
 * Kernel build only: forwards a libc header to its kernel equivalent so that
 * the shared Nanopb and iav_slice_client sources compile inside amba_virt.ko.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef AMBA_VIRT_LIBC_SHIM_STDBOOL_H
#define AMBA_VIRT_LIBC_SHIM_STDBOOL_H

#include <linux/stddef.h>
#include <linux/types.h>

#endif
