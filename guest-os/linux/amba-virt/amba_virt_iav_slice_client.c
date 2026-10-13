/*
 * amba_virt_iav_slice_client.c
 *
 * Compiles iav_slice_client.c inside amba_virt.ko. A wrapper keeps the object in this
 * directory; Kbuild would otherwise write it next to the shared source.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include "iav_slice_client.c"
