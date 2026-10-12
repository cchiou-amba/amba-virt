/*
 * iav_low_pool.h
 *
 * One 512 MiB private-bank partition for guest IAV slices. Query it before
 * allocating. Never free it.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef IAV_LOW_POOL_H
#define IAV_LOW_POOL_H

#include <stdint.h>

#define IAV_LOW_POOL_PID 192u
#define IAV_LOW_POOL_BYTES 536870912ull
#define IAV_LOW_POOL_ALIGN 0x4000000ull
#define IAV_LOW_POOL_BANK_BASE 0x40000000ull
#define IAV_LOW_POOL_BANK_END 0x100000000ull
#define IAV_LOW_POOL_ACS_PRIVATE 0x5u

#define IAV_LOW_POOL_REUSE 1
#define IAV_LOW_POOL_ALLOC 2
#define IAV_LOW_POOL_REFUSE 3

struct iav_low_pool_part {
	uint64_t addr;
	uint64_t length;
	unsigned pid;
	unsigned from_private;
	unsigned acs;
	int present;
};

struct iav_low_pool_view {
	struct iav_low_pool_part self;
	const struct iav_low_pool_part *others;
	unsigned nother;
};

int iav_low_pool_decide(const struct iav_low_pool_view *view, uint64_t *base_out);
int iav_low_pool_pin(int fd);
int iav_low_pool_pin_dev(void);
int iav_low_pool_report(int fd);
uint64_t iav_low_pool_pinned_base(void);
uint32_t iav_low_pool_pinned_pid(void);

#endif
