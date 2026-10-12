/*
 * iav_vsrc_list.c
 *
 * Diagnostic probe for IAV bringup: list every video source the IAV driver
 * registers, with its VIN controller, device type and name. Read-only.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <iav_ioctl.h>
#include <iav_vin_ioctl.h>

int main(void)
{
	struct vin_global_info g;
	struct vindev_devinfo d;
	int fd = open("/dev/iav", O_RDWR);
	unsigned int i;

	if (fd < 0) {
		perror("open /dev/iav");
		return 1;
	}
	memset(&g, 0, sizeof(g));
	if (ioctl(fd, IAV_IOC_VIN_GET_GLOBAL_INFO, &g) < 0) {
		perror("IAV_IOC_VIN_GET_GLOBAL_INFO");
		return 1;
	}
	printf("total_vsrc_num %u active_vsrc_map 0x%x\n", g.total_vsrc_num, g.active_vsrc_map);
	for (i = 0; i < g.total_vsrc_num; i++) {
		memset(&d, 0, sizeof(d));
		d.vsrc_id = i;
		if (ioctl(fd, IAV_IOC_VIN_GET_DEVINFO, &d) < 0) {
			printf("vsrc %u: devinfo failed\n", i);
			continue;
		}
		printf("vsrc %u vinc %u sw %u type %u sub %u name %.32s status %u\n", i, d.vinc_id,
		       d.vinc_sw_id, d.dev_type, d.sub_type, d.name, d.vsrc_status);
	}
	close(fd);
	return 0;
}
