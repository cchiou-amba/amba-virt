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
	{ name, #name, magic, kind, dir, (size), cls, flags, 0, NULL, NULL },

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

/*
 * ---- decode ----
 *
 * The decoder reads its input from, and writes its output to, the decode partition (IAV_PART_DEC_BSB). A guest
 * holds that partition as a window, so every offset and length it names must lie inside the window; the driver
 * checks them too, and a hostile guest gets the same refusal here first. dec_len is the window's length (0 when
 * the server has no decode partition, which refuses every call that names an offset).
 */
static int range_ok(uint64_t off, uint64_t len, uint64_t limit)
{
	return off <= limit && len <= limit - off;
}

static int check_decoder_id(unsigned int id)
{
	return id < IAV_MAX_DECODER_NUMBER ? 0 : -EINVAL;
}

/* CREATE_DECODER: the decoder's share of the partition (bsb_start_offset, bsb_size) lies inside it. */
static int check_create_decoder(struct iav_call *call)
{
	const struct iav_decoder_info *d = call->arg;

	if (call->dec_len == 0)
		return -EPERM;
	if (check_decoder_id(d->decoder_id))
		return -EINVAL;
	if (d->bsb_size == 0 || !range_ok(d->bsb_start_offset, d->bsb_size, call->dec_len))
		return -EINVAL;
	return 0;
}

/* DECODE_JPEG: the JPEG to read and, for a decode-only call, the place the picture is written. */
static int check_decode_jpeg(struct iav_call *call)
{
	const struct iav_decode_jpeg *j = call->arg;

	if (call->dec_len == 0)
		return -EPERM;
	if (check_decoder_id(j->decoder_id))
		return -EINVAL;
	if (j->jpeg_size == 0 || !range_ok(j->jpeg_start_offset, j->jpeg_size, call->dec_len))
		return -EINVAL;
	if (j->is_dec_only && (j->yuv_size == 0 || !range_ok(j->yuv_start_offset, j->yuv_size, call->dec_len)))
		return -EINVAL;
	return 0;
}

/* DECODE_VIDEO: the bitstream the decoder is handed lies between two offsets of the partition. */
static int check_decode_video(struct iav_call *call)
{
	const struct iav_decode_video *v = call->arg;

	if (call->dec_len == 0)
		return -EPERM;
	if (check_decoder_id(v->decoder_id))
		return -EINVAL;
	if (v->end_ptr_offset < v->start_ptr_offset ||
	    !range_ok(v->start_ptr_offset, v->end_ptr_offset - v->start_ptr_offset, call->dec_len))
		return -EINVAL;
	return 0;
}

/* QUERY_DECODE_BSB and WAIT_DECODE_BSB: the decoder id, and for the wait the free zone asked for. */
static int check_decode_bsb(struct iav_call *call)
{
	const struct iav_decode_bsb *b = call->arg;

	if (call->dec_len == 0)
		return -EPERM;
	if (check_decoder_id(b->decoder_id))
		return -EINVAL;
	if (call->entry->cmd == IAV_IOC_WAIT_DECODE_BSB && !range_ok(b->start_offset, b->room, call->dec_len))
		return -EINVAL;
	return 0;
}

/* QUERY_DECODE_STATUS: the decoder id; the physical display addresses it reports are cleared on the way out. */
static int check_decode_status(struct iav_call *call)
{
	const struct iav_decode_status *st = call->arg;

	return check_decoder_id(st->decoder_id);
}

static void post_decode_status(struct iav_call *call)
{
	struct iav_decode_status *st = call->arg;

	st->yuv422_y_addr = 0;
	st->yuv422_uv_addr = 0;
}

/*
 * WAIT_PLAYBACK_EOS: the timeout is in ticks of the host clock (u8; HZ is 100, so at most 2.55 s) and 0 means ten
 * seconds, longer than the guest waits for a reply. Zero becomes the longest a non-zero value can be.
 */
static int check_wait_eos(struct iav_call *call)
{
	struct iav_decode_wait_eos *w = call->arg;

	if (check_decoder_id(w->decoder_id))
		return -EINVAL;
	if (w->timeout_ticks == 0)
		w->timeout_ticks = 255;
	return 0;
}

/*
 * ---- overlay ----
 *
 * An overlay is a picture the DSP blends into a stream: a colour table (CLUT, 1 KiB) and the pixel data, both in the
 * overlay partition, which the primary guest holds as a window. The driver checks each area against the partition,
 * but not that the data covers pitch x height bytes, which is what the DSP reads: a short total_size would make the
 * DSP blend the neighbouring memory into the guest's stream. Here every enabled area must have its colour table and
 * the larger of total_size and pitch x height inside the window. Canvas overlays live in a partition that has no
 * window: refused.
 */
#define OVERLAY_CLUT_BYTES 1024u

static int check_set_overlay(struct iav_call *call)
{
	const struct iav_overlay_insert *o = call->arg;
	unsigned int i;

	if (o->id_for_canvas)
		return -EPERM;
	if (!o->enable)
		return 0;
	if (call->ovl_len == 0)
		return -EPERM;
	for (i = 0; i < MAX_NUM_OVERLAY_AREA; i++) {
		const struct iav_overlay_area *a = &o->area[i];
		uint64_t need;

		if (!a->enable)
			continue;
		need = (uint64_t)a->pitch * a->height;
		if (a->total_size > need)
			need = a->total_size;
		if (!range_ok(a->data_addr_offset, need, call->ovl_len) ||
		    !range_ok(a->clut_addr_offset, OVERLAY_CLUT_BYTES, call->ovl_len))
			return -EINVAL;
	}
	return 0;
}

/* GET_OVERLAY_INSERT reads the id and fills the areas in; the offsets it returns are the window's own. */
static int check_get_overlay(struct iav_call *call)
{
	const struct iav_overlay_insert *o = call->arg;

	return o->id_for_canvas ? -EPERM : 0;
}

/* ---- the allow-list ---- */

#define BOTH (IAV_PROFILE_BIT(IAV_PROFILE_CONSUMER) | IAV_PROFILE_BIT(IAV_PROFILE_QUALIFICATION) | \
	      IAV_PROFILE_BIT(IAV_PROFILE_PRIMARY))
#define PRIMARY_SET (IAV_PROFILE_BIT(IAV_PROFILE_QUALIFICATION) | IAV_PROFILE_BIT(IAV_PROFILE_PRIMARY))

struct iav_allow {
	unsigned int cmd;
	unsigned int profiles;
	unsigned int add_flags;
	int (*check)(struct iav_call *call);
	void (*post)(struct iav_call *call);
};

/*
 * Version 1 of the supported set: state and capability queries, memory partition queries,
 * descriptors for frames, canvases and pyramids, and event polling. Each entry was checked
 * against its handler for the argument kind and size in the generated table. None changes the
 * pipeline state: configuring and starting the pipeline stays with the host in this release.
 */
static const struct iav_allow g_allow[] = {
	{ IAV_IOC_GET_IAV_STATE, BOTH, 0, NULL, NULL },
	{ IAV_IOC_GET_CHIP_ID, BOTH, 0, NULL, NULL },
	{ IAV_IOC_QUERY_ENC_MODE_CAP, BOTH, 0, NULL, NULL },
	{ IAV_IOC_QUERY_ENC_BUF_CAP, BOTH, 0, NULL, NULL },
	{ IAV_IOC_QUERY_IAV_CAP, BOTH, 0, NULL, NULL },
	{ IAV_IOC_GET_SYSTEM_RESOURCE, BOTH, 0, NULL, NULL },
	{ IAV_IOC_GET_CHAN_CONFIG, BOTH, 0, NULL, NULL },
	{ IAV_IOC_GET_CANVAS_CONFIG, BOTH, 0, NULL, NULL },
	{ IAV_IOC_DRV_DSP_INFO, BOTH, 0, NULL, NULL },
	{ IAV_IOC_QUERY_INFO, BOTH, 0, NULL, NULL },
	{ IAV_IOC_QUERY_MEMBLOCK, BOTH, 0, NULL, NULL },
	{ IAV_IOC_QUERY_DESC, BOTH, IAV_F_BOUNDED_DESC, check_query_desc, NULL },
	{ IAV_IOC_SET_POLL_EVENT_CFG, BOTH, 0, check_poll_cfg, NULL },
	{ IAV_IOC_CLEAR_POLL_EVENT_CFG, BOTH, 0, NULL, NULL },
	{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL, NULL },
	/* Version 3: decode, for the primary guest, with every offset checked against the decode window. */
	{ IAV_IOC_CREATE_DECODER, PRIMARY_SET, 0, check_create_decoder, NULL },
	{ IAV_IOC_DECODE_JPEG, PRIMARY_SET, 0, check_decode_jpeg, NULL },
	{ IAV_IOC_DECODE_VIDEO, PRIMARY_SET, 0, check_decode_video, NULL },
	{ IAV_IOC_QUERY_DECODE_BSB, PRIMARY_SET, 0, check_decode_bsb, NULL },
	{ IAV_IOC_WAIT_DECODE_BSB, PRIMARY_SET, 0, check_decode_bsb, NULL },
	{ IAV_IOC_QUERY_DECODE_STATUS, PRIMARY_SET, 0, check_decode_status, post_decode_status },
	{ IAV_IOC_WAIT_PLAYBACK_EOS, PRIMARY_SET, 0, check_wait_eos, NULL },
	/* Version 4: stream overlays, with the data and colour table checked against the overlay window. */
	{ IAV_IOC_SET_OVERLAY_INSERT, PRIMARY_SET, 0, check_set_overlay, NULL },
	{ IAV_IOC_GET_OVERLAY_INSERT, PRIMARY_SET, 0, check_get_overlay, NULL },
};

#define IAV_ALLOW_COUNT (sizeof(g_allow) / sizeof(g_allow[0]))

/*
 * Version 2: the ioctls with plain arguments that the primary client may call, generated by
 * tools/iav-policy/gen_primary_allow.py from the table and the struct audit; the review record is
 * in the plan artifacts. They configure and run the pipeline (state, resources, streams, VIN and
 * VOUT controls, decode control). Each is PLAIN: no pointer, address, offset or fd in its argument.
 */
static const unsigned int g_primary_allow[] = {
#define PRIMARY_ALLOW(name) name,
#include "iav_allow_primary.inc"
#undef PRIMARY_ALLOW
};

#define IAV_PRIMARY_COUNT (sizeof(g_primary_allow) / sizeof(g_primary_allow[0]))

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
		g_table[i].post = NULL;
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
		e->post = g_allow[i].post;
	}
	for (i = 0; i < IAV_PRIMARY_COUNT; i++) {
		struct iav_policy_entry *e = NULL;

		for (j = 0; j < IAV_TABLE_COUNT; j++) {
			if (g_table[j].cmd == g_primary_allow[i]) {
				e = &g_table[j];
				break;
			}
		}
		if (e == NULL)
			return -1;
		e->profiles |= PRIMARY_SET;
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
