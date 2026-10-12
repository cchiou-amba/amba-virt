/*
 * iav_ioctl_dump.c
 *
 * Prints the broker's ioctl table with the allow-list applied, one tab-separated row per ioctl:
 * name, magic, ioctl number, argument kind, direction, class, flags, profile bits. The support
 * table in doc/IavVirtualization.md is generated from this output (tools/iav-policy), so the
 * document cannot drift from what the broker does.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <sys/ioctl.h>

#include "iav_policy.h"

int main(void)
{
	size_t i;

	if (iav_policy_init() != 0) {
		fprintf(stderr, "iav_policy_init failed\n");
		return 1;
	}
	for (i = 0; i < iav_policy_count(); i++) {
		const struct iav_policy_entry *e = iav_policy_at(i);

		printf("%s\t%d\t%u\t%d\t%d\t0x%x\t0x%x\t0x%x\n", e->name, (int)e->magic,
		       (unsigned int)_IOC_NR(e->cmd), (int)e->kind, (int)e->dir, e->cls, e->flags,
		       e->profiles);
	}
	return 0;
}
