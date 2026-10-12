/*
 * iav_memblock.c
 *
 * Diagnostic probe for IAV bringup: print the address, length and pool of one
 * IAV memory partition (default IAV_PART_DEC_BSB) through IAV_IOC_QUERY_MEMBLOCK.
 * Read-only. Build with the IAV uapi headers on the include path
 * (see the Makefile).
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <iav_ioctl.h>

static int query_one(int fd, int pid)
{
	struct iav_querymem q;

	memset(&q, 0, sizeof(q));
	q.mid = IAV_MEM_PARTITION;
	q.arg.partition.pid = pid;
	if (ioctl(fd, IAV_IOC_QUERY_MEMBLOCK, &q) < 0) {
		printf("pid %d ioctl_errno %d\n", pid, errno);
		return 0;
	}
	printf("pid %d addr 0x%lx length 0x%lx private %u acs 0x%x\n", pid,
	       q.arg.partition.mem.addr, q.arg.partition.mem.length,
	       q.arg.partition.from_private, q.arg.partition.accessibility);
	return q.arg.partition.mem.length > 0;
}

int main(int argc, char **argv)
{
	int fd = open("/dev/iav", O_RDWR);
	int pid;
	int found = 0;

	if (fd < 0) {
		perror("open /dev/iav");
		return 1;
	}
	if (argc > 1 && strcmp(argv[1], "all") == 0) {
		for (pid = 0; pid < IAV_PART_NUM; pid++)
			found += query_one(fd, pid);
		printf("nonzero_or_reported %d\n", found);
	} else {
		pid = argc > 1 ? atoi(argv[1]) : IAV_PART_DEC_BSB;
		if (!query_one(fd, pid)) {
			perror("IAV_IOC_QUERY_MEMBLOCK");
			close(fd);
			return 1;
		}
	}
	close(fd);
	return 0;
}
