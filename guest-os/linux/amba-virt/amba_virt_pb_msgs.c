/*
 * amba_virt_pb_msgs.c
 *
 * Compiles amba_virt.pb.c inside amba_virt.ko. A wrapper keeps the object in this
 * directory; Kbuild would otherwise write it next to the shared source.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include "amba_virt.pb.c"
