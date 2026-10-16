/*
 * iav_newpart.c
 *
 * Diagnostic probe for IAV bringup (plan_iav_usb_yolo Envelope F): the partitions amba-virt-server allocates at start
 * (IAV_PART_BLUR, IAV_PART_ARB_BLUR, IAV_PART_YUV_REMAP) and the partitions the aux aperture maps, as QUERY_MEMBLOCK
 * reports them, and the blur flags of each canvas (GET_CANVAS_CONFIG). Build with the IAV uapi headers on the include
 * path (see the Makefile).
 *
 *   iav_newpart show    read only
 *   iav_newpart free    free BLUR, ARB_BLUR and YUV_REMAP, each only when it reports the length the server gives it
 *                       (0x100000, 0x100000, 0x10000): returns a board to the state of an image without them
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

static const struct {
	unsigned int pid;
	const char *name;
	unsigned long server_len;	/* what the server's allocation reports; 0: not the server's */
} parts[] = {
	{ IAV_PART_BLUR, "blur", 0x100000ul },
	{ IAV_PART_ARB_BLUR, "arb_blur", 0x100000ul },
	{ IAV_PART_YUV_REMAP, "yuv_remap", 0x10000ul },
	{ IAV_PART_MASK, "mask", 0 },
	{ IAV_PART_WARP, "warp", 0 },
	{ IAV_PART_QPMATRIX, "qpmatrix", 0 },
	{ IAV_PART_CE_GO_MAP, "ce_go_map", 0 },
	{ IAV_PART_CANVAS_OVERLAY, "canvas_overlay", 0 },
	{ IAV_PART_DSP, "dsp", 0 },
	{ IAV_PART_BSB, "bsb", 0 },
	{ IAV_PART_OVERLAY, "overlay", 0 },
	{ IAV_PART_DEC_BSB, "dec_bsb", 0 },
};

static int query(int fd, unsigned int pid, unsigned long *addr, unsigned long *len)
{
	struct iav_querymem q;

	memset(&q, 0, sizeof(q));
	q.mid = IAV_MEM_PARTITION;
	q.arg.partition.pid = pid;
	if (ioctl(fd, IAV_IOC_QUERY_MEMBLOCK, &q) < 0)
		return -errno;
	*addr = q.arg.partition.mem.addr;
	*len = q.arg.partition.mem.length;
	return 0;
}

static int show(int fd)
{
	struct iav_system_resource r;
	unsigned int i, n = 0;

	for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		unsigned long addr = 0, len = 0;
		int rc = query(fd, parts[i].pid, &addr, &len);

		printf("part %s pid %u addr 0x%lx length 0x%lx rc %d\n", parts[i].name, parts[i].pid, addr, len, rc);
	}
	memset(&r, 0, sizeof(r));
	r.encode_mode = DSP_CURRENT_MODE;
	if (ioctl(fd, IAV_IOC_GET_SYSTEM_RESOURCE, &r) == 0)
		n = r.canvas_num;
	for (i = 0; i < n && i < IAV_MAX_CANVAS_BUF_NUM; i++) {
		struct iav_canvas_cfg c;

		memset(&c, 0, sizeof(c));
		c.canvas_id = (u8)i;
		if (ioctl(fd, IAV_IOC_GET_CANVAS_CONFIG, &c) < 0) {
			printf("canvas %u errno %d\n", i, errno);
			continue;
		}
		printf("canvas %u blur_enable %u smooth_blur_enable %u max_blur_area_num %u\n", i, c.blur_enable,
		       c.smooth_blur_enable, c.max_blur_area_num);
	}
	return 0;
}

static int free_server_parts(int fd)
{
	unsigned int i;
	int bad = 0;

	for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		unsigned long addr = 0, len = 0;
		struct iav_free_mem_part f;

		if (!parts[i].server_len)
			continue;
		if (query(fd, parts[i].pid, &addr, &len) != 0 || len == 0) {
			printf("free %s: absent\n", parts[i].name);
			continue;
		}
		if (len != parts[i].server_len) {
			printf("free %s: length 0x%lx is not the server's: left alone\n", parts[i].name, len);
			bad = 1;
			continue;
		}
		memset(&f, 0, sizeof(f));
		f.pid = parts[i].pid;
		if (ioctl(fd, IAV_IOC_FREE_MEM_PART, &f) < 0) {
			printf("free %s: errno %d\n", parts[i].name, errno);
			bad = 1;
		} else {
			printf("free %s: freed\n", parts[i].name);
		}
	}
	return bad;
}

int main(int argc, char **argv)
{
	int fd, rc;

	if (argc != 2 || (strcmp(argv[1], "show") && strcmp(argv[1], "free"))) {
		fprintf(stderr, "usage: %s show|free\n", argv[0]);
		return 2;
	}
	fd = open("/dev/iav", O_RDWR);
	if (fd < 0) {
		perror("open /dev/iav");
		return 1;
	}
	rc = strcmp(argv[1], "show") == 0 ? show(fd) : free_server_parts(fd);
	close(fd);
	return rc;
}
