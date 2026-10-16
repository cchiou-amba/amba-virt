/*
 * iav_pin_probe.c
 *
 * Window pinning probe for the virtual /dev/iav (plan_iav_usb_yolo, Envelope C). Run as root in the primary guest
 * while the pipeline is in preview or encode. It reads the current channel and resource configuration, then asks
 * for configurations that would move a windowed partition (a decoder channel added or removed, software CE turned
 * on or off, the channel map changed) and, as controls, for the unchanged configurations.
 *
 * Every call is harmless whoever answers it: the driver accepts SET_CHAN_CONFIG and SET_SYSTEM_RESOURCE only in
 * IDLE, so in preview or encode it refuses them too (EPERM) and changes nothing. The probe therefore cannot tell
 * from errno who refused; the caller compares the broker's pinning counter (amba-virt-ctl iav-status, pinned=)
 * and the driver's own "must be called in IAV IDLE state" messages in the Dom0 kernel log before and after.
 *
 * Output: one JSON object per line. Exit 0 when every call returned EPERM, 1 otherwise, 2 on a setup error.
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

#define CURRENT_ENCODE_MODE 0xff

static int report(const char *what, int expect_broker, int rc, int err)
{
	printf("{\"kind\":\"call\",\"what\":\"%s\",\"changes_windowed_partition\":%d,\"rc\":%d,\"errno\":%d,"
	       "\"eperm\":%d}\n", what, expect_broker, rc, rc < 0 ? err : 0, rc < 0 && err == EPERM);
	return rc < 0 && err == EPERM;
}

int main(void)
{
	struct iav_system_resource res, want_res;
	struct iav_chan_cfg cfg, want;
	unsigned int chan = 0xffu;
	int fd, ok = 1, rc, i;

	fd = open("/dev/iav", O_RDWR);
	if (fd < 0) {
		perror("open /dev/iav");
		return 2;
	}
	memset(&res, 0, sizeof(res));
	res.encode_mode = CURRENT_ENCODE_MODE;
	if (ioctl(fd, IAV_IOC_GET_SYSTEM_RESOURCE, &res) != 0) {
		perror("IAV_IOC_GET_SYSTEM_RESOURCE");
		return 2;
	}
	printf("{\"kind\":\"resource\",\"encode_mode\":%u,\"chan_num\":%u,\"chan_map\":%u}\n", res.encode_mode,
	       res.chan_num, res.chan_map);
	for (i = 0; i < 32; i++) {
		if (!(res.chan_map & (1u << i)))
			continue;
		memset(&cfg, 0, sizeof(cfg));
		cfg.chan_id = (unsigned int)i;
		if (ioctl(fd, IAV_IOC_GET_CHAN_CONFIG, &cfg) != 0) {
			perror("IAV_IOC_GET_CHAN_CONFIG");
			return 2;
		}
		printf("{\"kind\":\"channel\",\"chan\":%d,\"vsrc\":%u,\"pre_dec_enable\":%u,\"sw_ce_enable\":%u}\n", i,
		       cfg.vsrc_id, cfg.pre_dec_enable, cfg.sw_ce_enable);
		if (chan == 0xffu)
			chan = (unsigned int)i;
	}
	if (chan == 0xffu) {
		fprintf(stderr, "no channel in the channel map\n");
		return 2;
	}
	memset(&cfg, 0, sizeof(cfg));
	cfg.chan_id = chan;
	if (ioctl(fd, IAV_IOC_GET_CHAN_CONFIG, &cfg) != 0) {
		perror("IAV_IOC_GET_CHAN_CONFIG");
		return 2;
	}

	/* control: the configuration as it is */
	want = cfg;
	rc = ioctl(fd, IAV_IOC_SET_CHAN_CONFIG, &want);
	ok &= report("set_chan_config_unchanged", 0, rc, errno);
	/* a decoder appears or disappears */
	want = cfg;
	want.pre_dec_enable = !cfg.pre_dec_enable;
	rc = ioctl(fd, IAV_IOC_SET_CHAN_CONFIG, &want);
	ok &= report("set_chan_config_pre_dec_flipped", 1, rc, errno);
	/* software CE resizes CE_GO_MAP */
	want = cfg;
	want.sw_ce_enable = !cfg.sw_ce_enable;
	rc = ioctl(fd, IAV_IOC_SET_CHAN_CONFIG, &want);
	ok &= report("set_chan_config_sw_ce_flipped", 1, rc, errno);

	/* control: the resource as it is */
	want_res = res;
	rc = ioctl(fd, IAV_IOC_SET_SYSTEM_RESOURCE, &want_res);
	ok &= report("set_system_resource_unchanged", 0, rc, errno);
	/* another channel map */
	want_res = res;
	want_res.chan_map = res.chan_map ^ (1u << 7);
	rc = ioctl(fd, IAV_IOC_SET_SYSTEM_RESOURCE, &want_res);
	ok &= report("set_system_resource_chan_map_changed", 1, rc, errno);

	printf("{\"kind\":\"summary\",\"all_eperm\":%d}\n", ok);
	close(fd);
	return ok ? 0 : 1;
}
