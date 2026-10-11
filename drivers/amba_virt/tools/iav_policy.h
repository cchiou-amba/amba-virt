/*
 * iav_policy.h
 *
 * The ioctl table of the IAV broker. iav_ioctl_table.inc, generated from the IAV driver sources,
 * lists every ioctl the driver serves on /dev/iav with its argument kind and size. The broker
 * refuses everything that is not on the verified allow-list in iav_policy.c.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAV_POLICY_H
#define IAV_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "amba_virt_iav_table.h"

/* Set by the allow-list: the broker runs the call with bounded blocking. */
#define IAV_F_BOUNDED_DESC	0x100u

enum iav_profile {
	IAV_PROFILE_CONSUMER = 0,	/* frames and queries only */
	IAV_PROFILE_QUALIFICATION = 1,	/* the primary client, for qualification runs */
};
#define IAV_PROFILE_BIT(p)	(1u << (p))

struct iav_call;

struct iav_policy_entry {
	unsigned int cmd;
	const char *name;
	enum iav_magic magic;
	enum iav_arg_kind kind;
	enum iav_dir dir;
	size_t size;
	unsigned int cls;
	unsigned int flags;
	/* Filled from the allow-list. */
	unsigned int profiles;			/* bit per profile that may call it; 0 = denied */
	int (*check)(struct iav_call *call);	/* may reject (negative errno) or clamp */
};

/* What a check function can see and change. */
struct iav_call {
	const struct iav_policy_entry *entry;
	uint32_t cid;
	void *arg;			/* the broker's copy of a pointer argument */
	unsigned long value;		/* a value argument */
	unsigned int block_max_ms;	/* the longest a blocking call may wait */
};

/*
 * Build the table and apply the allow-list. Returns 0, or -1 if the allow-list names an ioctl
 * that is not in the generated table (the driver changed under it). Safe to call repeatedly.
 */
int iav_policy_init(void);

size_t iav_policy_count(void);
const struct iav_policy_entry *iav_policy_at(size_t index);

/* NULL if cmd is not an ioctl of the IAV driver. */
const struct iav_policy_entry *iav_policy_lookup(unsigned int cmd);

#endif /* IAV_POLICY_H */
