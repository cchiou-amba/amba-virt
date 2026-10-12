/*
 * iav_slice_isolation.c
 *
 * Write a different pattern into two guest slices and read both back.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_BYTES 8192u

static int check_pair(void *a, void *b, unsigned char mark_a, unsigned char mark_b)
{
	unsigned char expect_a[256];
	unsigned char expect_b[256];
	unsigned char *pa = a;
	unsigned char *pb = b;

	memset(expect_a, mark_a, sizeof(expect_a));
	memset(expect_b, mark_b, sizeof(expect_b));
	memcpy(pa, expect_a, sizeof(expect_a));
	memcpy(pb, expect_b, sizeof(expect_b));
	memcpy(pa + 4096, expect_a, sizeof(expect_a));
	memcpy(pb + 4096, expect_b, sizeof(expect_b));
	if (memcmp(pa, expect_a, sizeof(expect_a)) != 0 ||
	    memcmp(pa + 4096, expect_a, sizeof(expect_a)) != 0)
		return -1;
	if (memcmp(pb, expect_b, sizeof(expect_b)) != 0 ||
	    memcmp(pb + 4096, expect_b, sizeof(expect_b)) != 0)
		return -1;
	if (memcmp(pa, expect_b, sizeof(expect_b)) == 0 ||
	    memcmp(pb, expect_a, sizeof(expect_a)) == 0)
		return -1;
	return 0;
}

int main(void)
{
	int fd0, fd1;
	void *a, *b;

	fd0 = open("/dev/amba_virt_iav_slice0", O_RDWR);
	fd1 = open("/dev/amba_virt_iav_slice1", O_RDWR);
	if (fd0 < 0 || fd1 < 0) {
		fprintf(stderr, "slice device missing: %s\n", strerror(errno));
		return 1;
	}
	a = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd0, 0);
	b = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
	if (a == MAP_FAILED || b == MAP_FAILED) {
		fprintf(stderr, "slice map failed: %s\n", strerror(errno));
		return 1;
	}
	if (check_pair(a, b, 0xa1, 0xb2) != 0) {
		fprintf(stderr, "cross-slice modification\n");
		return 1;
	}
	printf("slice0 ok\nslice1 ok\nslices differ\n");
	return 0;
}
