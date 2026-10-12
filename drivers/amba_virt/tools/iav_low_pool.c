/*
 * iav_low_pool.c
 *
 * Pin the guest pool with the existing IAV allocation ioctl. A restart
 * reuses a matching partition and does not allocate a second one.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <iav_ioctl.h>

#include "iav_low_pool.h"

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
	if (alen == 0 || blen == 0)
		return 0;
	return a < b + blen && b < a + alen;
}

int iav_low_pool_decide(const struct iav_low_pool_view *view, uint64_t *base_out)
{
	unsigned i;

	if (!view)
		return IAV_LOW_POOL_REFUSE;
	if (!view->self.present)
		return IAV_LOW_POOL_ALLOC;
	if (view->self.length != IAV_LOW_POOL_BYTES)
		return IAV_LOW_POOL_REFUSE;
	if (view->self.addr % IAV_LOW_POOL_ALIGN)
		return IAV_LOW_POOL_REFUSE;
	if (view->self.addr < IAV_LOW_POOL_BANK_BASE)
		return IAV_LOW_POOL_REFUSE;
	if (view->self.addr > IAV_LOW_POOL_BANK_END - IAV_LOW_POOL_BYTES)
		return IAV_LOW_POOL_REFUSE;
	if (!view->self.from_private || view->self.acs != IAV_LOW_POOL_ACS_PRIVATE)
		return IAV_LOW_POOL_REFUSE;
	for (i = 0; i < view->nother; i++) {
		if (overlaps(view->self.addr, view->self.length,
			     view->others[i].addr, view->others[i].length))
			return IAV_LOW_POOL_REFUSE;
	}
	if (base_out)
		*base_out = view->self.addr;
	return IAV_LOW_POOL_REUSE;
}

static uint64_t g_pinned_base;
static uint32_t g_pinned_pid;

uint64_t iav_low_pool_pinned_base(void)
{
	return g_pinned_base;
}

uint32_t iav_low_pool_pinned_pid(void)
{
	return g_pinned_pid;
}

static int query_part(int fd, unsigned pid, struct iav_low_pool_part *out)
{
	struct iav_querymem q;

	memset(&q, 0, sizeof(q));
	memset(out, 0, sizeof(*out));
	q.mid = IAV_MEM_PARTITION;
	q.arg.partition.pid = pid;
	if (ioctl(fd, IAV_IOC_QUERY_MEMBLOCK, &q) < 0)
		return -errno;
	out->pid = pid;
	out->addr = q.arg.partition.mem.addr;
	out->length = q.arg.partition.mem.length;
	out->from_private = q.arg.partition.from_private;
	out->acs = q.arg.partition.accessibility;
	out->present = out->length > 0;
	return 0;
}

static int collect(int fd, struct iav_low_pool_part *parts, unsigned cap, unsigned *n_out)
{
	unsigned pid, n = 0;
	int rc;

	for (pid = 0; pid < 56 && n < cap; pid++) {
		rc = query_part(fd, pid, &parts[n]);
		if (rc < 0 || !parts[n].present)
			continue;
		n++;
	}
	for (pid = 192; pid < 255 && n < cap; pid++) {
		rc = query_part(fd, pid, &parts[n]);
		if (rc < 0 || !parts[n].present)
			continue;
		n++;
	}
	*n_out = n;
	return 0;
}

static int alloc_part(int fd, unsigned pid, uint64_t length)
{
	struct iav_alloc_mem_part alloc;

	memset(&alloc, 0, sizeof(alloc));
	alloc.pid = pid;
	alloc.from_private = 1;
	alloc.accessibility = IAV_LOW_POOL_ACS_PRIVATE;
	alloc.length = length;
	if (ioctl(fd, IAV_IOC_ALLOC_MEM_PART, &alloc) < 0)
		return -errno;
	return 0;
}

static int reuse_existing(struct iav_low_pool_part *parts, unsigned n, uint64_t *base)
{
	unsigned i, j, o;
	struct iav_low_pool_part others[128];

	for (i = 0; i < n; i++) {
		struct iav_low_pool_view view;
		int decision;

		if (parts[i].pid < 192 || parts[i].pid > 254)
			continue;
		o = 0;
		for (j = 0; j < n; j++) {
			if (j == i)
				continue;
			others[o++] = parts[j];
		}
		memset(&view, 0, sizeof(view));
		view.self = parts[i];
		view.others = others;
		view.nother = o;
		decision = iav_low_pool_decide(&view, base);
		if (decision == IAV_LOW_POOL_REUSE)
			return (int)parts[i].pid;
	}
	return -1;
}

int iav_low_pool_pin(int fd)
{
	struct iav_low_pool_part parts[128];
	uint64_t base = 0, high = 0, aligned, gap;
	unsigned n = 0, i, pad_pid = 0, pool_pid = 0;
	int rc, pid;

	rc = collect(fd, parts, 128, &n);
	if (rc < 0)
		return rc;
	pid = reuse_existing(parts, n, &base);
	if (pid < 0) {
		for (i = 0; i < n; i++) {
			uint64_t end = parts[i].addr + parts[i].length;

			if (end > high)
				high = end;
		}
		aligned = (high + IAV_LOW_POOL_ALIGN - 1) & ~(IAV_LOW_POOL_ALIGN - 1);
		gap = aligned - high;
		for (i = 192; i < 255 && pad_pid == 0; i++) {
			unsigned j, used = 0;

			for (j = 0; j < n; j++)
				if (parts[j].pid == i)
					used = 1;
			if (!used)
				pad_pid = i;
		}
		if (gap) {
			if (!pad_pid)
				return -ENOSPC;
			rc = alloc_part(fd, pad_pid, gap);
			if (rc < 0)
				return rc;
			pool_pid = 0;
			for (i = pad_pid + 1; i < 255; i++) {
				unsigned j, used = 0;

				for (j = 0; j < n; j++)
					if (parts[j].pid == i)
						used = 1;
				if (!used) {
					pool_pid = i;
					break;
				}
			}
		} else {
			pool_pid = pad_pid;
		}
		if (!pool_pid)
			return -ENOSPC;
		rc = alloc_part(fd, pool_pid, IAV_LOW_POOL_BYTES);
		if (rc < 0)
			return rc;
		rc = collect(fd, parts, 128, &n);
		if (rc < 0)
			return rc;
		pid = reuse_existing(parts, n, &base);
		if (pid < 0)
			return -EEXIST;
	}
	g_pinned_base = base;
	g_pinned_pid = pid;
	printf("low-pool pid %u addr 0x%llx length %llu\n", pid,
	       (unsigned long long)base, (unsigned long long)IAV_LOW_POOL_BYTES);
	for (i = 0; i < n; i++) {
		printf("part pid %u addr 0x%llx length 0x%llx private %u acs 0x%x\n",
		       parts[i].pid, (unsigned long long)parts[i].addr,
		       (unsigned long long)parts[i].length,
		       parts[i].from_private, parts[i].acs);
	}
	return 0;
}

int iav_low_pool_report(int fd)
{
	struct iav_low_pool_part parts[128];
	struct iav_low_pool_part others[128];
	unsigned n = 0, i, matches = 0;
	int rc, pid = -1;
	uint64_t base = 0;

	rc = collect(fd, parts, 128, &n);
	if (rc < 0)
		return rc;
	for (i = 0; i < n; i++) {
		struct iav_low_pool_view view;
		unsigned j, o = 0;
		uint64_t one = 0;

		if (parts[i].pid < 192 || parts[i].pid > 254)
			continue;
		for (j = 0; j < n; j++) {
			if (j != i)
				others[o++] = parts[j];
		}
		memset(&view, 0, sizeof(view));
		view.self = parts[i];
		view.others = others;
		view.nother = o;
		if (iav_low_pool_decide(&view, &one) == IAV_LOW_POOL_REUSE) {
			matches++;
			pid = (int)parts[i].pid;
			base = one;
		}
	}
	if (matches != 1)
		return -ENOENT;
	printf("low-pool pid %u addr 0x%llx length %llu\n", pid,
	       (unsigned long long)base, (unsigned long long)IAV_LOW_POOL_BYTES);
	for (i = 0; i < n; i++) {
		printf("part pid %u addr 0x%llx length 0x%llx private %u acs 0x%x\n",
		       parts[i].pid, (unsigned long long)parts[i].addr,
		       (unsigned long long)parts[i].length,
		       parts[i].from_private, parts[i].acs);
	}
	return 0;
}

int iav_low_pool_pin_dev(void)
{
	int fd = open("/dev/iav", O_RDWR);
	int rc;

	if (fd < 0)
		return -errno;
	rc = iav_low_pool_pin(fd);
	close(fd);
	return rc;
}
