/*
 * amba_virt_pb_common.c
 *
 * Compiles pb_common.c inside amba_virt.ko. A wrapper keeps the object in this
 * directory; Kbuild would otherwise write it next to the shared source.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include "pb_common.c"
