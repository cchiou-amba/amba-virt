/*
 * reset_dma_leases.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "amba_virt.h"

int main(int argc, char **argv)
{
	int fd = open("/dev/amba_dma_ctl", O_RDWR);
	if (fd < 0) {
		perror("/dev/amba_dma_ctl");
		return 1;
	}

	printf("Resetting all DMA leases...\n");
	for (int i = 0; i < 4; i++) {
		struct amba_dma_lease_control ctrl;
		memset(&ctrl, 0, sizeof(ctrl));
		ctrl.lease_id = i;
		ctrl.epoch = 0;
		ctrl.command = AMBA_DMA_LEASE_CMD_FORCE_DRAIN;
		ioctl(fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);

		ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
		ioctl(fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);
	}

	for (int argi = 1; argi < argc && argi <= 4; argi++) {
		uint32_t cid = (uint32_t)strtoul(argv[argi], NULL, 0);
		struct amba_dma_lease_alloc alloc;
		memset(&alloc, 0, sizeof(alloc));
		alloc.vsock_cid = cid;
		alloc.boot_generation = 1;
		if (ioctl(fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc) == 0) {
			printf("Allocated lease %u for CID %u (epoch %llu)\n",
			       alloc.lease_id, cid, (unsigned long long)alloc.epoch);
		} else {
			perror("LEASE_ALLOC");
		}
	}

	close(fd);
	return 0;
}
