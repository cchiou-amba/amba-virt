/*
 * iav_policy.c
 *
 * The IAV broker's ioctl table and allow-list.
 *
 * The table is generated: every ioctl the driver serves, with its argument kind and size. The
 * allow-list is written by hand and names only ioctls whose handlers and argument structs have
 * been read. Anything not on it is refused. The generated argument classification is a guess for
 * scalar-typed ioctls, so it is not used to allow anything by itself.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <iav_ioctl.h>
#include <ambas_imgproc_ioctl_arch.h>

#include "iav_policy.h"

#define IAV_IOCTL(name, magic, kind, dir, size, cls, flags) \
	{ name, #name, magic, kind, dir, (size), cls, flags, 0, NULL },

static struct iav_policy_entry g_table[] = {
#include "iav_ioctl_table.inc"
};

#undef IAV_IOCTL

#define IAV_TABLE_COUNT (sizeof(g_table) / sizeof(g_table[0]))

/* Pointers into g_table, sorted by cmd. */
static const struct iav_policy_entry *g_index[sizeof(g_table) / sizeof(g_table[0])];
static int g_ready;

/* ---- checks ---- */

/* QUERY_DESC: only the frame, canvas and pyramid descriptors, and never a dma-buf fd. */
static int check_query_desc(struct iav_call *call)
{
	struct iav_querydesc *q = call->arg;

	switch (q->qid) {
	case IAV_DESC_CANVAS:
		if (q->arg.canvas.yuv_use_dma_buf_fd || q->arg.canvas.me_use_dma_buf_fd ||
		    q->arg.canvas.mo_use_dma_buf_fd)
			return -EPERM;
		return 0;
	case IAV_DESC_PYRAMID:
		if (q->arg.pyramid.use_dma_buf_fd)
			return -EPERM;
		return 0;
	case IAV_DESC_FRAME:
		/* 0xffffffff is non-blocking and stays so; 0 means forever and is bounded. */
		if (q->arg.frame.time_ms != 0xffffffffU &&
		    (q->arg.frame.time_ms == 0 || q->arg.frame.time_ms > call->block_max_ms))
			q->arg.frame.time_ms = call->block_max_ms;
		return 0;
	default:
		return -EPERM;
	}
}

static int poll_qid_allowed(unsigned int qid)
{
	return qid == IAV_DESC_FRAME || qid == IAV_DESC_CANVAS || qid == IAV_DESC_PYRAMID;
}

/* SET_POLL_EVENT_CFG: bounded count, and only descriptor types the broker supports. */
static int check_poll_cfg(struct iav_call *call)
{
	struct iav_poll_event_config *c = call->arg;
	unsigned int i;

	if (c->valid_num > MAX_POLL_EVENT)
		return -EINVAL;
	for (i = 0; i < c->valid_num; i++) {
		if (!poll_qid_allowed((unsigned int)c->desc_event[i].qid))
			return -EPERM;
	}
	return 0;
}

/* ---- the allow-list ---- */

#define BOTH (IAV_PROFILE_BIT(IAV_PROFILE_CONSUMER) | IAV_PROFILE_BIT(IAV_PROFILE_QUALIFICATION))

struct iav_allow {
	unsigned int cmd;
	unsigned int profiles;
	unsigned int add_flags;
	int (*check)(struct iav_call *call);
};

/*
 * Version 1 of the supported set: state and capability queries, memory partition queries,
 * descriptors for frames, canvases and pyramids, and event polling. Each entry was checked
 * against its handler for the argument kind and size in the generated table. None changes the
 * pipeline state: configuring and starting the pipeline stays with the host in this release.
 */
static const struct iav_allow g_allow[] = {
	{ IAV_IOC_GET_IAV_STATE, BOTH, 0, NULL },
	{ IAV_IOC_GET_CHIP_ID, BOTH, 0, NULL },
	{ IAV_IOC_QUERY_ENC_MODE_CAP, BOTH, 0, NULL },
	{ IAV_IOC_QUERY_ENC_BUF_CAP, BOTH, 0, NULL },
	{ IAV_IOC_QUERY_IAV_CAP, BOTH, 0, NULL },
	{ IAV_IOC_GET_SYSTEM_RESOURCE, BOTH, 0, NULL },
	{ IAV_IOC_GET_CHAN_CONFIG, BOTH, 0, NULL },
	{ IAV_IOC_GET_CANVAS_CONFIG, BOTH, 0, NULL },
	{ IAV_IOC_DRV_DSP_INFO, BOTH, 0, NULL },
	{ IAV_IOC_QUERY_INFO, BOTH, 0, NULL },
	{ IAV_IOC_QUERY_MEMBLOCK, BOTH, 0, NULL },
	{ IAV_IOC_QUERY_DESC, BOTH, IAV_F_BOUNDED_DESC, check_query_desc },
	{ IAV_IOC_SET_POLL_EVENT_CFG, BOTH, 0, check_poll_cfg },
	{ IAV_IOC_CLEAR_POLL_EVENT_CFG, BOTH, 0, NULL },
	{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL },
};

#define IAV_ALLOW_COUNT (sizeof(g_allow) / sizeof(g_allow[0]))

static int cmp_index(const void *a, const void *b)
{
	const struct iav_policy_entry *x = *(const struct iav_policy_entry *const *)a;
	const struct iav_policy_entry *y = *(const struct iav_policy_entry *const *)b;

	if (x->cmd != y->cmd)
		return x->cmd < y->cmd ? -1 : 1;
	return strcmp(x->name, y->name);
}

int iav_policy_init(void)
{
	size_t i, j;

	if (g_ready)
		return 0;

	for (i = 0; i < IAV_TABLE_COUNT; i++) {
		g_table[i].profiles = 0;
		g_table[i].check = NULL;
		g_index[i] = &g_table[i];
	}
	qsort(g_index, IAV_TABLE_COUNT, sizeof(g_index[0]), cmp_index);
	/* Two ioctls with one number would make the table ambiguous. */
	for (i = 1; i < IAV_TABLE_COUNT; i++) {
		if (g_index[i - 1]->cmd == g_index[i]->cmd)
			return -2;
	}

	for (i = 0; i < IAV_ALLOW_COUNT; i++) {
		struct iav_policy_entry *e = NULL;

		for (j = 0; j < IAV_TABLE_COUNT; j++) {
			if (g_table[j].cmd == g_allow[i].cmd) {
				e = &g_table[j];
				break;
			}
		}
		if (e == NULL)
			return -1;
		e->profiles = g_allow[i].profiles;
		e->flags |= g_allow[i].add_flags;
		e->check = g_allow[i].check;
	}
	g_ready = 1;
	return 0;
}

size_t iav_policy_count(void)
{
	return IAV_TABLE_COUNT;
}

const struct iav_policy_entry *iav_policy_at(size_t index)
{
	return index < IAV_TABLE_COUNT ? g_index[index] : NULL;
}

const struct iav_policy_entry *iav_policy_lookup(unsigned int cmd)
{
	size_t lo = 0, hi = IAV_TABLE_COUNT;

	if (!g_ready && iav_policy_init() != 0)
		return NULL;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (g_index[mid]->cmd == cmd)
			return g_index[mid];
		if (g_index[mid]->cmd < cmd)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}
