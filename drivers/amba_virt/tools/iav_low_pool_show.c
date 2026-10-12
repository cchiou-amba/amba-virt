/*
 * iav_low_pool_show.c
 *
 * Report the pinned guest pool. This program does not allocate or free.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "iav_low_pool.h"

int main(void)
{
	int fd = open("/dev/iav", O_RDWR);
	int rc;

	if (fd < 0) {
		perror("open /dev/iav");
		return 1;
	}
	rc = iav_low_pool_report(fd);
	close(fd);
	if (rc < 0) {
		fprintf(stderr, "low-pool check failed: %d\n", rc);
		return 1;
	}
	return 0;
}
