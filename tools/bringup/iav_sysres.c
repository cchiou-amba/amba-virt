/*
 * iav_sysres.c
 *
 * Diagnostic probe for IAV bringup: print the current system resource
 * configuration that decides which image calls the driver accepts (the
 * image scaler configuration, 3A statistics zero copy, VIN/IDSP instant
 * update, overlay and arbitrary blend flags) through IAV_IOC_GET_SYSTEM_RESOURCE
 * with the current encode mode. Read-only. Build with the IAV uapi headers on
 * the include path (see the Makefile).
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <iav_ioctl.h>

int main(void)
{
	struct iav_system_resource r;
	int fd = open("/dev/iav", O_RDWR);

	if (fd < 0) {
		perror("open /dev/iav");
		return 1;
	}
	memset(&r, 0, sizeof(r));
	r.encode_mode = DSP_CURRENT_MODE;
	if (ioctl(fd, IAV_IOC_GET_SYSTEM_RESOURCE, &r) < 0) {
		printf("IAV_IOC_GET_SYSTEM_RESOURCE errno %d\n", errno);
		close(fd);
		return 1;
	}
	printf("encode_mode %u chan_num %u canvas_num %u max_stream_num %u\n",
	       r.encode_mode, r.chan_num, r.canvas_num, r.max_stream_num);
	printf("chan_map 0x%x canvas_map 0x%x stream_map 0x%x\n",
	       r.chan_map, r.canvas_map, r.stream_map);
	printf("img_scale_cfg.enable %u format %u job_queue_depth %u "
	       "max_input %ux%u max_output %ux%u\n",
	       r.img_scale_cfg.enable, r.img_scale_cfg.format,
	       r.img_scale_cfg.job_queue_depth,
	       r.img_scale_cfg.max_input.width, r.img_scale_cfg.max_input.height,
	       r.img_scale_cfg.max_output.width, r.img_scale_cfg.max_output.height);
	printf("img_statis_zero_copy_enable %u enable_vin_idsp_instant_update %u\n",
	       r.img_statis_zero_copy_enable, r.enable_vin_idsp_instant_update);
	printf("overlay_disable %u arb_blend_enable %u arb_blend_max_areas %u\n",
	       r.overlay_disable, r.arb_blend_enable, r.arb_blend_max_areas);
	printf("enc_raw_rgb %u enc_raw_yuv %u enc_raw_nv12 %u\n",
	       r.enc_raw_rgb, r.enc_raw_yuv, r.enc_raw_nv12);
	close(fd);
	return 0;
}
