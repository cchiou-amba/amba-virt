/*
 * amba-virt-server.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include "amba_virt.h"

static int ensure_dev_node(const char *path)
{
	struct stat st;
	int major = -1, minor = 0;
	FILE *fp;
	char line[256];

	if (stat(path, &st) == 0) {
		if (S_ISCHR(st.st_mode))
			return 0;
		fprintf(stderr, "%s exists but is not a char device\n", path);
		return -1;
	}

	/* 1. Try reading major:minor from sysfs */
	fp = fopen("/sys/class/amba_virt/amba_virt/dev", "r");
	if (fp) {
		if (fscanf(fp, "%d:%d", &major, &minor) == 2) {
			fclose(fp);
			goto create_node;
		}
		fclose(fp);
	}

	/* 2. Fallback: Parse /proc/devices for "amba_virt" */
	fp = fopen("/proc/devices", "r");
	if (fp) {
		int in_char = 0;
		while (fgets(line, sizeof(line), fp)) {
			if (strstr(line, "Character devices:")) {
				in_char = 1;
				continue;
			}
			if (strstr(line, "Block devices:")) {
				in_char = 0;
				break;
			}
			if (in_char) {
				int num;
				char name[64];
				if (sscanf(line, "%d %63s", &num, name) == 2) {
					if (strcmp(name, AMBA_VIRT_DEV_NAME) == 0) {
						major = num;
						minor = 0;
						break;
					}
				}
			}
		}
		fclose(fp);
	}

	if (major < 0) {
		fprintf(stderr, "Cannot find %s in /sys or /proc/devices (is amba_virt.ko loaded?)\n",
			AMBA_VIRT_DEV_NAME);
		return -1;
	}

create_node:
	if (mknod(path, S_IFCHR | 0666, makedev(major, minor)) < 0) {
		if (errno != EEXIST) {
			fprintf(stderr, "mknod %s (c %d %d) failed: %s\n",
				path, major, minor, strerror(errno));
			return -1;
		}
	} else {
		chmod(path, 0666);
		printf("Auto-created device node %s (c %d %d)\n", path, major, minor);
	}

	return 0;
}

static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	int ret = ioctl(fd, req, arg);

	if (ret < 0)
		fprintf(stderr, "%s: %s\n", what, strerror(errno));
	return ret;
}

int main(void)
{
	struct amba_virt_info info;
	unsigned char *map = MAP_FAILED;
	int fd;

	if (ensure_dev_node(AMBA_VIRT_DEV_PATH) < 0)
		return 1;

	fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror(AMBA_VIRT_DEV_PATH);
		return 1;
	}
	if (xioctl(fd, AMBA_VIRT_IOC_GET_INFO, &info, "GET_INFO") < 0)
		return 1;
	printf("amba-virt-server proto=%u role=%u shm=%u port=%u (waiting)\n",
	       info.proto, info.role, info.shm_size, info.vsock_port);

	if (info.shm_size) {
		map = mmap(NULL, info.shm_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
		if (map == MAP_FAILED)
			perror("mmap (continuing without shm)");
	}

	for (;;) {
		struct amba_virt_xfer rx, tx;
		struct amba_virt_msg *in;
		struct amba_virt_msg *out;

		memset(&rx, 0, sizeof(rx));
		if (ioctl(fd, AMBA_VIRT_IOC_RECV, &rx) < 0) {
			if (errno == ENOTCONN) {
				sleep(1);
				continue;
			}
			perror("RECV");
			return 1;
		}
		if (rx.len < sizeof(*in)) {
			fprintf(stderr, "short msg %u\n", rx.len);
			continue;
		}
		in = (struct amba_virt_msg *)rx.data;
		memset(&tx, 0, sizeof(tx));
		out = (struct amba_virt_msg *)tx.data;
		out->seq = in->seq;
		out->shm_off = in->shm_off;
		out->shm_len = in->shm_len;
		tx.len = sizeof(*out);

		if (in->type == AMBA_VIRT_MSG_PING) {
			out->type = AMBA_VIRT_MSG_PONG;
			printf("PING seq=%u -> PONG\n", in->seq);
		} else if (in->type == AMBA_VIRT_MSG_SHM_NOTIFY) {
			out->type = AMBA_VIRT_MSG_SHM_ACK;
			if (map != MAP_FAILED &&
			    (uint64_t)in->shm_off + in->shm_len <= info.shm_size) {
				printf("SHM_NOTIFY seq=%u off=%u len=%u first=%u\n",
				       in->seq, in->shm_off, in->shm_len,
				       map[in->shm_off]);
			} else {
				printf("SHM_NOTIFY seq=%u (no map or OOB)\n", in->seq);
			}
		} else {
			fprintf(stderr, "unknown type %u\n", in->type);
			continue;
		}
		if (xioctl(fd, AMBA_VIRT_IOC_SEND, &tx, "SEND") < 0)
			return 1;
	}
}
