/*
 * iav_new_parts.c
 *
 * Allocate (or reuse) the server-owned BLUR, ARB_BLUR and YUV_REMAP partitions. See iav_new_parts.h.
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

#include "iav_new_parts.h"

/*
 * Sizes (f_design.md 0.1): BLUR content 0x100000 plus BLUR_RSV_SIZE 0x81000; YUV_REMAP content 0x10000 (64 CLUTs of 1 KiB)
 * plus YUV_REMAP_RSV_SIZE 0x38000; ARB_BLUR 0x100000, all of it content (include/driver/specific/iav_config.h). Total
 * 0x2C9000.
 */
const struct iav_new_part iav_new_parts[IAV_NEW_PARTS_COUNT] = {
	{ IAV_PART_BLUR, "IAV_PART_BLUR", 0x181000ull, 0x100000ull },
	{ IAV_PART_ARB_BLUR, "IAV_PART_ARB_BLUR", 0x100000ull, 0x100000ull },
	{ IAV_PART_YUV_REMAP, "IAV_PART_YUV_REMAP", 0x48000ull, 0x10000ull },
};

/* The IAV private pool, accessible by the ARM and the DSP: attribute 0x0, uncached (the guest pool uses the same). */
#define NEW_PART_ACS (IAV_MEM_PART_ACS_ARM | IAV_MEM_PART_ACS_DSP)

static int query(iav_new_parts_ioctl_fn fn, void *ctx, uint32_t pid, uint64_t *addr, uint64_t *len)
{
	struct iav_querymem q;
	int rc;

	memset(&q, 0, sizeof(q));
	q.mid = IAV_MEM_PARTITION;
	q.arg.partition.pid = pid;
	rc = fn(ctx, IAV_IOC_QUERY_MEMBLOCK, &q);
	if (rc == 0) {
		*addr = q.arg.partition.mem.addr;
		*len = q.arg.partition.mem.length;
	}
	return rc;
}

int iav_new_parts_ensure(iav_new_parts_ioctl_fn fn, void *ctx, struct iav_new_parts_state *st)
{
	unsigned int i;
	int usable = 0;

	if (!fn || !st)
		return -EINVAL;
	memset(st, 0, sizeof(*st));
	for (i = 0; i < IAV_NEW_PARTS_COUNT; i++) {
		const struct iav_new_part *p = &iav_new_parts[i];
		uint64_t addr = 0, len = 0;
		int rc = query(fn, ctx, p->pid, &addr, &len);

		if (rc == 0 && len == p->report_len) {
			st->state[i] = IAV_NEW_PART_REUSED;
		} else if (rc == 0 && len == 0) {
			struct iav_alloc_mem_part a;

			memset(&a, 0, sizeof(a));
			a.pid = p->pid;
			a.from_private = 1;
			a.accessibility = NEW_PART_ACS;
			a.enable_cache = 0;
			a.length = p->alloc_len;
			rc = fn(ctx, IAV_IOC_ALLOC_MEM_PART, &a);
			if (rc == 0)
				rc = query(fn, ctx, p->pid, &addr, &len);
			if (rc == 0 && len == p->report_len)
				st->state[i] = IAV_NEW_PART_ALLOCATED;
			else
				st->state[i] = rc == 0 && len ? IAV_NEW_PART_MISMATCH : IAV_NEW_PART_ABSENT;
		} else {
			/* there with another length, or a query the driver refuses (a length no larger than the reserve) */
			st->state[i] = IAV_NEW_PART_MISMATCH;
		}
		st->rc[i] = rc;
		st->addr[i] = addr;
		st->len[i] = len;
		if (st->state[i] == IAV_NEW_PART_ALLOCATED || st->state[i] == IAV_NEW_PART_REUSED)
			usable++;
	}
	return usable;
}

int iav_new_parts_free(iav_new_parts_ioctl_fn fn, void *ctx, struct iav_new_parts_state *st)
{
	unsigned int i;
	int freed = 0;

	if (!fn || !st)
		return -EINVAL;
	memset(st, 0, sizeof(*st));
	for (i = 0; i < IAV_NEW_PARTS_COUNT; i++) {
		const struct iav_new_part *p = &iav_new_parts[i];
		uint64_t addr = 0, len = 0;
		int rc = query(fn, ctx, p->pid, &addr, &len);

		st->addr[i] = addr;
		st->len[i] = len;
		if (rc == 0 && len == p->report_len) {
			struct iav_free_mem_part f;

			memset(&f, 0, sizeof(f));
			f.pid = p->pid;
			rc = fn(ctx, IAV_IOC_FREE_MEM_PART, &f);
			if (rc == 0) {
				st->state[i] = IAV_NEW_PART_FREED;
				freed++;
			} else {
				st->state[i] = IAV_NEW_PART_REUSED;
			}
		} else {
			st->state[i] = rc == 0 && len == 0 ? IAV_NEW_PART_ABSENT : IAV_NEW_PART_MISMATCH;
		}
		st->rc[i] = rc;
	}
	return freed;
}

const char *iav_new_part_state_name(int state)
{
	switch (state) {
	case IAV_NEW_PART_ALLOCATED:
		return "allocated";
	case IAV_NEW_PART_REUSED:
		return "reused";
	case IAV_NEW_PART_MISMATCH:
		return "mismatch";
	case IAV_NEW_PART_FREED:
		return "freed";
	default:
		return "absent";
	}
}

static int dev_ioctl(void *ctx, unsigned long cmd, void *arg)
{
	int fd = *(int *)ctx;

	return ioctl(fd, cmd, arg) == 0 ? 0 : -errno;
}

static int run_dev(int do_free)
{
	struct iav_new_parts_state st;
	unsigned int i;
	int fd = open("/dev/iav", O_RDWR | O_CLOEXEC);
	int n;

	if (fd < 0)
		return -errno;
	n = do_free ? iav_new_parts_free(dev_ioctl, &fd, &st) : iav_new_parts_ensure(dev_ioctl, &fd, &st);
	close(fd);
	for (i = 0; i < IAV_NEW_PARTS_COUNT; i++)
		printf("new partition pid %u (%s): %s addr 0x%llx length 0x%llx rc %d\n", iav_new_parts[i].pid,
		       iav_new_parts[i].name, iav_new_part_state_name(st.state[i]), (unsigned long long)st.addr[i],
		       (unsigned long long)st.len[i], st.rc[i]);
	return n;
}

int iav_new_parts_ensure_dev(void)
{
	int n = run_dev(0);

	/* A partition that is not there leaves only its calls refused: the pipeline goes on. */
	return n < 0 ? n : 0;
}

int iav_new_parts_free_dev(void)
{
	return run_dev(1);
}
