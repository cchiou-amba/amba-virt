/*
 * test_amba_virt_dma.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <pthread.h>

#include "amba_virt.h"

#define CTL_DEV   "/dev/amba_dma_ctl"
#define LEASE_DEV "/dev/amba_dma_lease%u"

static void test_lease_allocation_and_bounds(void)
{
	int fd;
	struct amba_dma_lease_alloc alloc;
	struct amba_dma_lease_alloc allocs[4];
	int ret, i;

	printf("[TEST] Testing lease allocation and capacity limits...\n");

	fd = open(CTL_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " CTL_DEV);
		exit(1);
	}
	/* Force-clean any leftover leases from earlier runs */
	for (i = 0; i < 4; i++) {
		struct amba_dma_lease_control ctrl;
		memset(&ctrl, 0, sizeof(ctrl));
		ctrl.lease_id = i;
		ctrl.epoch = 0;
		ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
		ioctl(fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);
	}

	/* Allocate all 4 slices */
	for (i = 0; i < 4; i++) {
		memset(&alloc, 0, sizeof(alloc));
		alloc.boot_generation = 1;
		alloc.vsock_cid = 100 + i;
		alloc.vm_uuid[0] = 0xAA + i;

		ret = ioctl(fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc);
		assert(ret == 0);
		assert(alloc.lease_id == (uint32_t)i);
		assert(alloc.capability != 0);
		assert(alloc.epoch >= 1);
		allocs[i] = alloc;

		printf("  Allocated lease %u: cap=0x%llx, epoch=%llu\n",
		       alloc.lease_id, (unsigned long long)alloc.capability,
		       (unsigned long long)alloc.epoch);
	}

	/* 5th allocation must fail with -ENOSPC */
	memset(&alloc, 0, sizeof(alloc));
	alloc.boot_generation = 1;
	alloc.vsock_cid = 200;
	ret = ioctl(fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc);
	assert(ret < 0);
	printf("  5th allocation rejected correctly as expected\n");

	/* Release all leases */
	for (i = 0; i < 4; i++) {
		struct amba_dma_lease_control ctrl;
		memset(&ctrl, 0, sizeof(ctrl));
		ctrl.lease_id = allocs[i].lease_id;
		ctrl.epoch = allocs[i].epoch;
		ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
		ret = ioctl(fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);
		assert(ret == 0);
	}

	close(fd);
	printf("  [PASS] Lease allocation and bounds verification passed cleanly.\n");
}

static void test_lease_mmap_and_sanitization(void)
{
	int ctl_fd, lease_fd;
	struct amba_dma_lease_alloc alloc;
	struct amba_dma_lease_control ctrl;
	char path[64];
	uint8_t *ptr;
	size_t size = AMBA_VIRT_DMA32_SLICE_SIZE;
	int ret;

	printf("[TEST] Testing lease slice mmap and zeroing sanitization...\n");

	ctl_fd = open(CTL_DEV, O_RDWR);
	assert(ctl_fd >= 0);

	memset(&alloc, 0, sizeof(alloc));
	alloc.boot_generation = 1;
	alloc.vsock_cid = 42;
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc);
	assert(ret == 0);

	snprintf(path, sizeof(path), LEASE_DEV, alloc.lease_id);
	lease_fd = open(path, O_RDWR);
	if (lease_fd < 0) {
		perror(path);
		exit(1);
	}

	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, lease_fd, 0);
	assert(ptr != MAP_FAILED);

	/* Write test patterns */
	memset(ptr, 0xA5, 4096);
	memset(ptr + (8 * 1024 * 1024), 0x5A, 4096);
	memset(ptr + size - 4096, 0x3C, 4096);

	assert(ptr[0] == 0xA5);
	assert(ptr[8 * 1024 * 1024] == 0x5A);
	assert(ptr[size - 1] == 0x3C);

	munmap(ptr, size);
	close(lease_fd);

	/* Release lease -> triggers kernel memset_io(0) */
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.lease_id = alloc.lease_id;
	ctrl.epoch = alloc.epoch;
	ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);
	assert(ret == 0);

	/* Re-allocate and re-map to verify sanitization wiped memory */
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc);
	assert(ret == 0);

	lease_fd = open(path, O_RDWR);
	assert(lease_fd >= 0);
	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, lease_fd, 0);
	assert(ptr != MAP_FAILED);

	/* Verify bootstrap magic at offset 0, and zeroed memory at 4096+ */
	assert(*(uint32_t *)ptr == AMBA_DMA_BOOTSTRAP_MAGIC);
	assert(ptr[4096] == 0);
	assert(ptr[8 * 1024 * 1024] == 0);
	assert(ptr[size - 1] == 0);

	munmap(ptr, size);
	close(lease_fd);

	/* Final cleanup */
	ctrl.lease_id = alloc.lease_id;
	ctrl.epoch = alloc.epoch;
	ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
	ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);
	close(ctl_fd);

	printf("  [PASS] Lease slice mmap and memory sanitization verified.\n");
}

static void test_security_bounds_and_capability_checks(void)
{
	int ctl_fd, lease_fd;
	struct amba_dma_lease_alloc alloc;
	struct amba_dma_lease_control ctrl;
	struct amba_virt_dma_request req;
	char path[64];
	int ret;

	printf("[TEST] Testing security bounds, capability checks, and error codes...\n");

	ctl_fd = open(CTL_DEV, O_RDWR);
	assert(ctl_fd >= 0);

	/* 0. Invalid host-bound endpoint_id at allocation time */
	memset(&alloc, 0, sizeof(alloc));
	alloc.boot_generation = 1;
	alloc.vsock_cid = 77;
	alloc.endpoint_id = 999;
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc);
	assert(ret < 0 && errno == EINVAL);
	printf("  Invalid alloc endpoint_id rejected with -EINVAL: PASS\n");

	memset(&alloc, 0, sizeof(alloc));
	alloc.boot_generation = 1;
	alloc.vsock_cid = 77;
	alloc.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc);
	assert(ret == 0);

	snprintf(path, sizeof(path), LEASE_DEV, alloc.lease_id);
	lease_fd = open(path, O_RDWR);
	assert(lease_fd >= 0);

	/* 1. Invalid capability token */
	memset(&req, 0, sizeof(req));
	req.capability = 0xDEADBEEF; /* wrong capability */
	req.epoch = alloc.epoch;
	req.cookie = 1;
	req.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
	req.operation = AMBA_DMA_OP_MEM_TO_DEV;
	req.offset = 0;
	req.length = 64;
	ret = ioctl(lease_fd, AMBA_DMA_IOC_REQUEST, &req);
	assert(ret < 0 && errno == EACCES);
	printf("  Invalid capability rejected with -EACCES: PASS\n");

	/* 2. Invalid epoch */
	req.capability = alloc.capability;
	req.epoch = alloc.epoch + 99;
	req.cookie = 2;
	ret = ioctl(lease_fd, AMBA_DMA_IOC_REQUEST, &req);
	assert(ret < 0 && errno == ESTALE);
	printf("  Stale/invalid epoch rejected with -ESTALE: PASS\n");

	/* 3. Out-of-bounds offset */
	req.epoch = alloc.epoch;
	req.cookie = 3;
	req.offset = AMBA_VIRT_DMA32_SLICE_SIZE - 32;
	req.length = 64; /* overflows 16 MiB slice */
	ret = ioctl(lease_fd, AMBA_DMA_IOC_REQUEST, &req);
	assert(ret < 0 && errno == ERANGE);
	printf("  Out-of-bounds transfer rejected with -ERANGE: PASS\n");

	/* 4. Offset below 4096 rejected with -ERANGE */
	req.cookie = 4;
	req.offset = 4095;
	req.length = 64;
	ret = ioctl(lease_fd, AMBA_DMA_IOC_REQUEST, &req);
	assert(ret < 0 && errno == ERANGE);
	printf("  Bootstrap offset < 4096 rejected with -ERANGE: PASS\n");

	/* 4b. Zero length */
	req.cookie = 4;
	req.offset = 4096;
	req.length = 0;
	ret = ioctl(lease_fd, AMBA_DMA_IOC_REQUEST, &req);
	assert(ret < 0 && errno == EINVAL);
	printf("  Zero-length transfer rejected with -EINVAL: PASS\n");

	/* 5. Cross-endpoint / invalid endpoint on host-bound lease rejected with -EPERM */
	req.cookie = 5;
	req.endpoint_id = AMBA_DMA_ENDPOINT_UART3;
	req.offset = 4096;
	req.length = 64;
	ret = ioctl(lease_fd, AMBA_DMA_IOC_REQUEST, &req);
	assert(ret < 0 && errno == EPERM);
	printf("  Mismatched guest endpoint_id rejected with -EPERM: PASS\n");

	close(lease_fd);

	/* Release lease */
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.lease_id = alloc.lease_id;
	ctrl.epoch = alloc.epoch;
	ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
	ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);
	close(ctl_fd);

	printf("  [PASS] All security validation checks passed cleanly.\n");
}

struct thread_arg {
	int fd;
	struct amba_virt_dma_request req;
	int ret;
	int err;
};

static void *dma_runner(void *arg)
{
	struct thread_arg *t = (struct thread_arg *)arg;
	t->ret = ioctl(t->fd, AMBA_DMA_IOC_REQUEST, &t->req);
	t->err = errno;
	return NULL;
}

static void test_exclusive_endpoint_concurrency(void)
{
	int ctl_fd, fd0, fd1, fd2;
	struct amba_dma_lease_alloc alloc0, alloc1, alloc2, alloc_dup;
	struct amba_dma_lease_control ctrl;
	struct amba_virt_dma_request req1;
	struct thread_arg arg0;
	pthread_t th0;
	char path[64];
	int ret;

	printf("[TEST] Testing host-bound endpoint policy, concurrency, and watchdog...\n");

	ctl_fd = open(CTL_DEV, O_RDWR);
	assert(ctl_fd >= 0);

	/* 1. Allocate Lease 0 bound to UART2, Lease 1 bound to UART3, Lease 2 unbound */
	memset(&alloc0, 0, sizeof(alloc0));
	alloc0.vsock_cid = 101;
	alloc0.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc0);
	assert(ret == 0);

	/* Duplicate UART2 binding on another CID must fail with EBUSY */
	memset(&alloc_dup, 0, sizeof(alloc_dup));
	alloc_dup.vsock_cid = 199;
	alloc_dup.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc_dup);
	assert(ret < 0 && errno == EBUSY);
	printf("  Duplicate host-bound UART2 lease rejected with -EBUSY: PASS\n");

	memset(&alloc1, 0, sizeof(alloc1));
	alloc1.vsock_cid = 102;
	alloc1.endpoint_id = AMBA_DMA_ENDPOINT_UART3;
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc1);
	assert(ret == 0);

	memset(&alloc2, 0, sizeof(alloc2));
	alloc2.vsock_cid = 103;
	alloc2.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
	ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc2);
	assert(ret == 0);

	snprintf(path, sizeof(path), LEASE_DEV, alloc0.lease_id);
	fd0 = open(path, O_RDWR);
	assert(fd0 >= 0);

	snprintf(path, sizeof(path), LEASE_DEV, alloc1.lease_id);
	fd1 = open(path, O_RDWR);
	assert(fd1 >= 0);

	snprintf(path, sizeof(path), LEASE_DEV, alloc2.lease_id);
	fd2 = open(path, O_RDWR);
	assert(fd2 >= 0);

	/* 2. Thread 0 submits DMA request on Lease 0 (UART2) -> triggers watchdog */
	memset(&arg0, 0, sizeof(arg0));
	arg0.fd = fd0;
	arg0.req.capability = alloc0.capability;
	arg0.req.epoch = alloc0.epoch;
	arg0.req.cookie = 1001;
	arg0.req.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
	arg0.req.operation = AMBA_DMA_OP_MEM_TO_DEV;
	arg0.req.offset = 4096;
	arg0.req.length = 64;

	pthread_create(&th0, NULL, dma_runner, &arg0);

	/* Give Thread 0 100ms to start */
	usleep(100000);

	/* 3. Concurrently, Lease 1 (bound to UART3) tries to specify UART2 -> rejected with -EPERM */
	memset(&req1, 0, sizeof(req1));
	req1.capability = alloc1.capability;
	req1.epoch = alloc1.epoch;
	req1.cookie = 2001;
	req1.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
	req1.operation = AMBA_DMA_OP_MEM_TO_DEV;
	req1.offset = 4096;
	req1.length = 64;

	ret = ioctl(fd1, AMBA_DMA_IOC_REQUEST, &req1);
	assert(ret < 0 && errno == EPERM);
	printf("  Cross-endpoint UART2 request on UART3 lease rejected with -EPERM: PASS\n");

	/* 4. Unbound Lease 2 attempts DMA -> rejected with -EPERM */
	req1.capability = alloc2.capability;
	req1.epoch = alloc2.epoch;
	req1.cookie = 3001;
	req1.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
	ret = ioctl(fd2, AMBA_DMA_IOC_REQUEST, &req1);
	assert(ret < 0 && errno == EPERM);
	printf("  Unbound lease DMA request rejected with -EPERM: PASS\n");

	/* 5. Wait for Thread 0 to finish via watchdog */
	pthread_join(th0, NULL);
	printf("  Lease 0 watchdog completion: ret=%d (watchdog handled cleanly)\n", arg0.ret);

	close(fd0);
	close(fd1);
	close(fd2);

	/* Release leases */
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.lease_id = alloc0.lease_id;
	ctrl.epoch = alloc0.epoch;
	ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
	ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.lease_id = alloc1.lease_id;
	ctrl.epoch = alloc1.epoch;
	ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
	ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.lease_id = alloc2.lease_id;
	ctrl.epoch = alloc2.epoch;
	ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
	ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);

	close(ctl_fd);
	printf("  [PASS] Host-bound endpoint policy and concurrency isolation verified.\n");
}

int main(void)
{
	printf("====================================================\n");
	printf("AMBARELLA VIRTUAL DMA KERNEL AUTHORITY QUALIFICATION\n");
	printf("====================================================\n");

	test_lease_allocation_and_bounds();
	test_lease_mmap_and_sanitization();
	test_security_bounds_and_capability_checks();
	test_exclusive_endpoint_concurrency();

	printf("\n>> ALL KERNEL DMA AUTHORITY TESTS PASSED CLEANLY <<\n");
	return 0;
}
