/*
 * limits.h
 *
 * Kernel build only: forwards a libc header to its kernel equivalent so that
 * the shared Nanopb and iav_slice_client sources compile inside amba_virt.ko.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef AMBA_VIRT_LIBC_SHIM_LIMITS_H
#define AMBA_VIRT_LIBC_SHIM_LIMITS_H

#include <linux/limits.h>
#include <linux/kernel.h>

#ifndef CHAR_BIT
#define CHAR_BIT	8
#endif

#endif
