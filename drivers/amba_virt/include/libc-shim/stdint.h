/*
 * stdint.h
 *
 * Kernel build only: forwards a libc header to its kernel equivalent so that
 * the shared Nanopb and iav_slice_client sources compile inside amba_virt.ko.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef AMBA_VIRT_LIBC_SHIM_STDINT_H
#define AMBA_VIRT_LIBC_SHIM_STDINT_H

#include <linux/limits.h>
#include <linux/types.h>

#ifndef INT8_MAX
#define INT8_MAX	S8_MAX
#define INT16_MAX	S16_MAX
#define INT32_MAX	S32_MAX
#define INT64_MAX	S64_MAX
#define INT8_MIN	S8_MIN
#define INT16_MIN	S16_MIN
#define INT32_MIN	S32_MIN
#define INT64_MIN	S64_MIN
#define UINT8_MAX	U8_MAX
#define UINT16_MAX	U16_MAX
#define UINT32_MAX	U32_MAX
#define UINT64_MAX	U64_MAX
#endif
typedef s8  int_least8_t;
typedef u8  uint_least8_t;
typedef s16 int_least16_t;
typedef u16 uint_least16_t;
typedef s32 int_least32_t;
typedef u32 uint_least32_t;
typedef s64 int_least64_t;
typedef u64 uint_least64_t;
typedef s8  int_fast8_t;
typedef u8  uint_fast8_t;
typedef s16 int_fast16_t;
typedef u16 uint_fast16_t;
typedef s32 int_fast32_t;
typedef u32 uint_fast32_t;
typedef s64 int_fast64_t;
typedef u64 uint_fast64_t;

#ifndef SIZE_MAX
#define SIZE_MAX	(~(size_t)0)
#endif

#endif
