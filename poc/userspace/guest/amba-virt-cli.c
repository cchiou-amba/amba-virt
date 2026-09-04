/*
 * amba-virt-cli.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "amba_virt.h"

static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	int ret = ioctl(fd, req, arg);

	if (ret < 0)
		fprintf(stderr, "%s: %s\n", what, strerror(errno));
	return ret;
}

static int do_ping(int fd, unsigned int seq)
{
	struct amba_virt_xfer xfer, rx;
	struct amba_virt_msg *m = (struct amba_virt_msg *)xfer.data;
	struct amba_virt_msg *r = (struct amba_virt_msg *)rx.data;

	memset(&xfer, 0, sizeof(xfer));
	m->type = AMBA_VIRT_MSG_PING;
	m->seq = seq;
	xfer.len = sizeof(*m);

	if (xioctl(fd, AMBA_VIRT_IOC_SEND, &xfer, "SEND") < 0)
		return -1;

	memset(&rx, 0, sizeof(rx));
	rx.timeout_ms = 5000;
	if (xioctl(fd, AMBA_VIRT_IOC_RECV, &rx, "RECV") < 0)
		return -1;
	if (rx.len < sizeof(*r) || r->type != AMBA_VIRT_MSG_PONG || r->seq != seq) {
		fprintf(stderr, "bad PONG type=%u seq=%u len=%u\n",
			r->type, r->seq, rx.len);
		return -1;
	}
	printf("PONG seq=%u\n", r->seq);
	return 0;
}

static int do_shm(int fd, size_t shm_size, unsigned int seq)
{
	struct amba_virt_xfer xfer, rx;
	struct amba_virt_msg *m = (struct amba_virt_msg *)xfer.data;
	struct amba_virt_msg *r = (struct amba_virt_msg *)rx.data;
	unsigned char *map;
	unsigned int off = 0;
	unsigned int len = 256;
	unsigned int i;

	if (shm_size < len) {
		fprintf(stderr, "shm too small (%zu)\n", shm_size);
		return -1;
	}
	map = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	for (i = 0; i < len; i++)
		map[off + i] = (unsigned char)(seq + i);

	memset(&xfer, 0, sizeof(xfer));
	m->type = AMBA_VIRT_MSG_SHM_NOTIFY;
	m->seq = seq;
	m->shm_off = off;
	m->shm_len = len;
	xfer.len = sizeof(*m);
	if (xioctl(fd, AMBA_VIRT_IOC_SEND, &xfer, "SEND") < 0) {
		munmap(map, shm_size);
		return -1;
	}
	memset(&rx, 0, sizeof(rx));
	if (xioctl(fd, AMBA_VIRT_IOC_RECV, &rx, "RECV") < 0) {
		munmap(map, shm_size);
		return -1;
	}
	if (rx.len < sizeof(*r) || r->type != AMBA_VIRT_MSG_SHM_ACK) {
		fprintf(stderr, "bad SHM_ACK\n");
		munmap(map, shm_size);
		return -1;
	}
	printf("SHM_ACK seq=%u off=%u len=%u first=%u\n",
	       r->seq, r->shm_off, r->shm_len, map[off]);
	munmap(map, shm_size);
	return 0;
}

int main(int argc, char **argv)
{
	struct amba_virt_info info;
	const char *cmd = "ping";
	int fd;

	if (argc > 1)
		cmd = argv[1];

	fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror(AMBA_VIRT_DEV_PATH);
		return 1;
	}
	if (xioctl(fd, AMBA_VIRT_IOC_GET_INFO, &info, "GET_INFO") < 0)
		return 1;
	printf("proto=%u role=%u shm=%u connected=%u cid=%u port=%u\n",
	       info.proto, info.role, info.shm_size, info.connected,
	       info.vsock_cid, info.vsock_port);

	if (xioctl(fd, AMBA_VIRT_IOC_CONNECT, NULL, "CONNECT") < 0)
		return 1;

	if (strcmp(cmd, "info") == 0)
		return 0;
	if (strcmp(cmd, "ping") == 0)
		return do_ping(fd, 1) ? 1 : 0;
	if (strcmp(cmd, "shm") == 0)
		return do_shm(fd, info.shm_size, 7) ? 1 : 0;

	fprintf(stderr, "usage: %s [info|ping|shm]\n", argv[0]);
	return 1;
}
