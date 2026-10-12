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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <iav_ioctl.h>

int main(int argc, char **argv)
{
	struct iav_querymem q;
	int pid = argc > 1 ? atoi(argv[1]) : IAV_PART_DEC_BSB;
	int fd = open("/dev/iav", O_RDWR);

	if (fd < 0) {
		perror("open /dev/iav");
		return 1;
	}
	memset(&q, 0, sizeof(q));
	q.mid = IAV_MEM_PARTITION;
	q.arg.partition.pid = pid;
	if (ioctl(fd, IAV_IOC_QUERY_MEMBLOCK, &q) < 0) {
		perror("IAV_IOC_QUERY_MEMBLOCK");
		return 1;
	}
	printf("pid %d addr 0x%lx length 0x%lx private %u acs 0x%x\n", pid,
	       q.arg.partition.mem.addr, q.arg.partition.mem.length,
	       q.arg.partition.from_private, q.arg.partition.accessibility);
	close(fd);
	return 0;
}
