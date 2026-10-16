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

#include "amba_virt_iav.h"

#include "iav_policy.h"

#define IAV_IOCTL(name, magic, kind, dir, size, cls, flags, wire) \
	{ name, #name, magic, kind, dir, (size), (wire), cls, flags, 0, NULL, NULL },

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

/* The bytes of partition pid the caller maps through its aux aperture, or 0 when it has no entry over it. */
static uint64_t aux_len_of(const struct iav_call *call, uint32_t pid)
{
	unsigned int i;

	for (i = 0; i < call->naux && i < IAV_CALL_AUX_MAX; i++)
		if (call->aux[i].pid == pid)
			return call->aux[i].len;
	return 0;
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

#define IAV_JPEG_HEADER_MAX 4096u

/* JPEG-STD-BEGIN (the app's UsbJpeg.cxx carries the same tables and rule; i_diff.sh compares the two) */
/*
 * The Huffman tables the DSP decoder takes: the four of ITU-T T.81 Annex K (K.3.3.1 and K.3.3.2), each as one DHT
 * table: the class/id byte, the 16 code-length counts, the values. A frame with any other table stalled the decoder in
 * the board test (4:2:0 as well as 4:2:2), and a stalled decoder takes the following frames with it.
 */
static const uint8_t std_dht_dc_luma[29] = {
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b,
};
static const uint8_t std_dht_dc_chroma[29] = {
    0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b,
};
static const uint8_t std_dht_ac_luma[179] = {
    0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04,
    0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05,
    0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1,
    0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19,
    0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54,
    0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t std_dht_ac_chroma[179] = {
    0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04,
    0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05,
    0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52,
    0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1,
    0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
    0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2,
    0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

const struct iav_jpeg_std_table iav_jpeg_std_dht[IAV_JPEG_DHT_N] = {
	{ std_dht_dc_luma, sizeof(std_dht_dc_luma) },
	{ std_dht_dc_chroma, sizeof(std_dht_dc_chroma) },
	{ std_dht_ac_luma, sizeof(std_dht_ac_luma) },
	{ std_dht_ac_chroma, sizeof(std_dht_ac_chroma) },
};

/*
 * The bytes the decoder writes for a picture of w x h: width and height rounded up to 16 (the decoder works in
 * macroblocks), 1.5 bytes a pixel for 4:2:0 and 2 for 4:2:2. Observed: a 1920x1080 4:2:0 picture fits in 3 MiB
 * (1920 x 1088 x 1.5 = 3133440 bytes), a 4:2:2 one needs 4177920 bytes.
 */
uint64_t iav_jpeg_decoded_bytes(uint32_t w, uint32_t h, int sub422)
{
	uint64_t pix = (uint64_t)((w + 15u) / 16u * 16u) * ((h + 15u) / 16u * 16u);

	return sub422 ? pix * 2u : pix * 3u / 2u;
}

/* The header of a JPEG the decoder takes: SOI, then DQT, one SOF0, DHT and at most one JFIF APP0, then SOS. */
int iav_jpeg_admit_info(const uint8_t *p, size_t n, uint32_t *wout, uint32_t *hout, int *sub422)
{
	size_t i = 2;
	int sof = 0, dqt = 0, app0 = 0;
	unsigned int dht = 0;
	uint32_t pw = 0, ph = 0;
	int p422 = 0;

	if (p == NULL || n < 4 || p[0] != 0xFF || p[1] != 0xD8)
		return -EINVAL;
	while (i + 1 < n) {
		unsigned int m, len;

		if (p[i] != 0xFF)
			return -EINVAL;
		m = p[i + 1];
		if (i + 4 > n)
			return -EINVAL;
		len = ((unsigned int)p[i + 2] << 8) | p[i + 3];
		if (len < 2 || i + 2 + len > n)
			return -EINVAL;
		if (m == 0xDA) {
			if (!(sof && dqt && dht == (1u << IAV_JPEG_DHT_N) - 1))
				return -EINVAL;
			if (wout)
				*wout = pw;
			if (hout)
				*hout = ph;
			if (sub422)
				*sub422 = p422;
			return 0;
		}
		if (m == 0xC0) {
			const uint8_t *s = p + i + 4;
			unsigned int h, w;

			if (sof || len != 8 + 3 * 3 || s[0] != 8 || s[5] != 3)
				return -EINVAL;
			h = ((unsigned int)s[1] << 8) | s[2];
			w = ((unsigned int)s[3] << 8) | s[4];
			if (h == 0 || w == 0 || h > 16384 || w > 16384)
				return -EINVAL;
			/* ids may be anything; the sampling is 2x2 (4:2:0) or 2x1 (4:2:2) for Y and 1x1 for Cb and Cr */
			if ((s[7] != 0x22 && s[7] != 0x21) || s[10] != 0x11 || s[13] != 0x11)
				return -EINVAL;
			sof = 1;
			pw = w;
			ph = h;
			p422 = s[7] == 0x21;
		} else if (m == 0xDB) {
			dqt = 1;
		} else if (m == 0xC4) {
			size_t k = i + 4, end = i + 2 + len;

			while (k < end) {
				unsigned int c, sum = 0, t;

				if (end - k < 17)
					return -EINVAL;
				for (c = 1; c <= 16; c++)
					sum += p[k + c];
				if (sum > 256 || end - k < 17 + sum)
					return -EINVAL;
				for (t = 0; t < IAV_JPEG_DHT_N; t++)
					if (iav_jpeg_std_dht[t].n == 17 + sum && memcmp(p + k, iav_jpeg_std_dht[t].t, iav_jpeg_std_dht[t].n) == 0)
						break;
				if (t == IAV_JPEG_DHT_N || (dht & (1u << t)))
					return -EINVAL;	/* not a standard table, or one given twice */
				dht |= 1u << t;
				k += 17 + sum;
			}
		} else if (m == 0xE0) {
			/* at most one APP0, and it is JFIF: "JFIF\0" and a length of at least 16 */
			if (app0 || len < 16 || memcmp(p + i + 4, "JFIF", 5) != 0)
				return -EINVAL;
			app0 = 1;
		} else {
			return -EINVAL;	/* any other APPn, COM, DRI, progressive and other SOF, RSTn, a second SOI, EOI */
		}
		i += 2 + len;
	}
	return -EINVAL;
}

int iav_jpeg_admit(const uint8_t *p, size_t n)
{
	return iav_jpeg_admit_info(p, n, NULL, NULL, NULL);
}
/* JPEG-STD-END */

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
	/*
	 * The decoder stalls on a JPEG it does not take, and repeated stalls crashed the DSP (observed with 4:2:2 camera
	 * frames), so the header is read here and only the kind observed to decode reaches it. The guest still owns the
	 * bytes until the DSP reads them: this refuses a wrong JPEG, it does not make a hostile one safe.
	 */
	{
		uint8_t head[IAV_JPEG_HEADER_MAX];
		size_t n = j->jpeg_size < IAV_JPEG_HEADER_MAX ? j->jpeg_size : IAV_JPEG_HEADER_MAX;
		uint32_t pw = 0, ph = 0;
		int p422 = 0, rc;

		if (call->read_dec == NULL)
			return -EPERM;
		if (call->read_dec(call->rctx, j->jpeg_start_offset, head, n) != 0)
			return -EIO;
		rc = iav_jpeg_admit_info(head, n, &pw, &ph, &p422);
		if (rc != 0)
			return rc;
		/*
		 * The DSP writes the whole picture the header describes, whatever yuv_size the caller gave (observed: a 1920x1080
		 * 4:2:2 picture decoded into 3 MiB ran about 1 MiB past the end of the partition). So the room the caller names
		 * must hold it, and the picture is then inside the window the range checks above already confined it to.
		 */
		if (j->is_dec_only && j->yuv_size < iav_jpeg_decoded_bytes(pw, ph, p422))
			return -EINVAL;
		return 0;
	}
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
 * the larger of total_size and pitch x height inside the window. A canvas overlay (id_for_canvas) is the same against
 * the CANVAS_OVERLAY entry of the caller's aux aperture (Envelope F, decision D4); without that entry it is refused.
 * The id is a stream id or a canvas id: the driver sets bit id of a 32-bit stream bitmap for a stream overlay
 * (iav_enc_overlay.c, iav_ioc_s_overlay_insert) and truncates a canvas id to eight bits, so the stream id is below 32
 * and the canvas id below IAV_MAX_CANVAS_BUF_NUM.
 */
#define OVERLAY_CLUT_BYTES 1024u
#define STREAM_IDS 32u

static int overlay_areas_ok(const struct iav_overlay_insert *o, uint64_t limit)
{
	unsigned int i;

	for (i = 0; i < MAX_NUM_OVERLAY_AREA; i++) {
		const struct iav_overlay_area *a = &o->area[i];
		uint64_t need;

		if (!a->enable)
			continue;
		need = (uint64_t)a->pitch * a->height;
		if (a->total_size > need)
			need = a->total_size;
		if (!range_ok(a->data_addr_offset, need, limit) ||
		    !range_ok(a->clut_addr_offset, OVERLAY_CLUT_BYTES, limit))
			return -EINVAL;
	}
	return 0;
}

/* An overlay of stream or canvas id (the caller of a frame-sync call passes its own id), as the driver will take it. */
static int overlay_ok(const struct iav_call *call, const struct iav_overlay_insert *o, uint32_t id, int canvas)
{
	uint64_t limit;

	if (canvas ? id >= IAV_MAX_CANVAS_BUF_NUM : id >= STREAM_IDS)
		return -EINVAL;
	if (!o->enable)
		return 0;
	limit = canvas ? aux_len_of(call, IAV_PART_CANVAS_OVERLAY) : call->ovl_len;
	if (limit == 0)
		return -EPERM;
	return overlay_areas_ok(o, limit);
}

static int check_set_overlay(struct iav_call *call)
{
	const struct iav_overlay_insert *o = call->arg;

	return overlay_ok(call, o, o->id, o->id_for_canvas);
}

/*
 * GET_OVERLAY_INSERT reads the id and fills the areas in; the offsets it returns are the window's own. A canvas
 * overlay's are offsets in the CANVAS_OVERLAY entry: only a guest with that entry asks.
 */
static int check_get_overlay(struct iav_call *call)
{
	const struct iav_overlay_insert *o = call->arg;

	if (!o->id_for_canvas)
		return 0;
	if (o->id >= IAV_MAX_CANVAS_BUF_NUM)
		return -EINVAL;
	return aux_len_of(call, IAV_PART_CANVAS_OVERLAY) ? 0 : -EPERM;
}

/*
 * ---- window pinning ----
 *
 * A partition behind a window must stay where the window points for as long as guests map it. The decode partition
 * is sized from the decoder count when preview is enabled and freed and allocated again when the count grows
 * (iav/iav_dec_api.c, iav_update_dec_ctx); the count is the number of VIN sources whose channels have
 * pre_dec_enable. sw_ce_enable resizes CE_GO_MAP at preview (iav/iav_enc_go_map.c). So a channel configuration
 * may not change pre_dec_enable, the VIN source of a decoder channel, or sw_ce_enable, and a resource
 * configuration may not change the channel map (a channel coming back with pre_dec_enable). Anything else reaches
 * the driver as before. The current values come from the driver on the caller's own descriptor; a failed query
 * refuses.
 */
static int check_chan_cfg_pinned(struct iav_call *call)
{
	const struct iav_chan_cfg *want = call->arg;
	struct iav_chan_cfg have;

	if (call->query == NULL)
		return -EPERM;
	memset(&have, 0, sizeof(have));
	have.chan_id = want->chan_id;
	if (call->query(call->qctx, IAV_IOC_GET_CHAN_CONFIG, &have) != 0)
		return -EPERM;
	if (have.pre_dec_enable != want->pre_dec_enable || have.sw_ce_enable != want->sw_ce_enable ||
	    ((have.pre_dec_enable || want->pre_dec_enable) && have.vsrc_id != want->vsrc_id)) {
		call->pinned = 1;
		return -EPERM;
	}
	return 0;
}

#define DSP_CURRENT_ENCODE_MODE 0xffu

static int check_system_resource_pinned(struct iav_call *call)
{
	const struct iav_system_resource *want = call->arg;
	struct iav_system_resource have;

	if (call->query == NULL)
		return -EPERM;
	memset(&have, 0, sizeof(have));
	have.encode_mode = DSP_CURRENT_ENCODE_MODE;
	if (call->query(call->qctx, IAV_IOC_GET_SYSTEM_RESOURCE, &have) != 0)
		return -EPERM;
	if (have.chan_map != want->chan_map) {
		call->pinned = 1;
		return -EPERM;
	}
	return 0;
}

/*
 * ---- the image set against the existing windows (plan_iav_usb_yolo, Envelope D) ----
 *
 * The rows of class W of the image inventory (plan artifacts, image_ioctl_inventory.md, sections 3 and 7): calls whose
 * buffers are offsets in the DSP, bitstream, decode or overlay window, or that name no memory at all. Every check
 * here is either a window confinement (no physical address, no dma-buf fd, every byte the DSP or the GDMA engine may
 * touch inside the window the guest holds) or a guard for a driver defect found when the handler was read: the
 * driver would dereference NULL, index past an array, shift by more than 31 or let a 32-bit sum wrap. Sums are done
 * in 64 bits. Image-changing calls are for the primary guest only (the allow-list).
 */

/* The length of the window or aperture entry over partition pid, or 0 when the guest maps none. */
static uint64_t window_len_of_pid(const struct iav_call *call, uint32_t pid)
{
	switch (pid) {
	case IAV_PART_DSP:
		return call->dsp_len;
	case IAV_PART_BSB:
		return call->bsb_len;
	case IAV_PART_DEC_BSB:
		return call->dec_len;
	case IAV_PART_OVERLAY:
		return call->ovl_len;
	default:
		return aux_len_of(call, pid);
	}
}

/*
 * The primary guest's own content: the partitions a call may write into (decode, overlay, and the content parts its
 * aux aperture maps).
 */
static int pid_is_primary_content(const struct iav_call *call, uint32_t pid)
{
	return pid == IAV_PART_DEC_BSB || pid == IAV_PART_OVERLAY || aux_len_of(call, pid) != 0;
}

/*
 * SET_IMG_SCALE. Both buffers must be offsets in the decode window (the scale target the user chose, decision D2) or
 * in a block of the caller's own arena (Envelope E): no physical address, no dma-buf fd, no other partition. The driver accepts any allocated partition, a physical
 * address with only a DSP restricted-zone check, or a dma-buf fd, and it has three defects a guest could use: the
 * input UV plane is range-checked with y_offset instead of uv_offset (iav_enc_img_scale.c, check_img_scale), the plane
 * sizes are computed in int, and the window sums (x + width, y + height) are 32-bit and wrap, so a huge x moves the
 * DSP's read or write window anywhere in memory. Here every plane is checked with its own offset in 64 bits, and the
 * input crop window (as the driver will align it for the DSP) and the output window (rounded as the DSP writes it)
 * must lie inside their buffers.
 */
#define IMG_SCALE_H_ALIGN	4u	/* IAV_IMG_SCALE_H_ALIGN */
#define IMG_SCALE_V_ALIGN	2u	/* IAV_IMG_SCALE_V_ALIGN */

static uint64_t round_up64(uint64_t v, uint64_t a)
{
	return (v + a - 1) / a * a;
}

/* Bytes of the UV plane of a buffer of this pitch and height in format fmt (YUV400: none). */
static int uv_plane_bytes(int fmt, uint64_t pitch, uint64_t height, uint64_t *bytes)
{
	switch (fmt) {
	case IAV_YUV_FORMAT_YUV422:
		*bytes = pitch * height;
		return 0;
	case IAV_YUV_FORMAT_YUV420:
		*bytes = pitch * (height / 2);
		return 0;
	case IAV_YUV_FORMAT_YUV400:
		*bytes = 0;
		return 0;
	default:
		return -1;
	}
}

/*
 * The range of partition pid a buffer of the call may use: an arena block the broker translated for this slot (the
 * caller's own memory), or the window over pid. Returns 0 with [*lo, *hi), or -EPERM when the guest holds neither.
 */
static int buf_range(const struct iav_call *call, unsigned int slot, uint32_t pid, uint64_t *lo, uint64_t *hi)
{
	if (call->arena_pid != 0 && pid == call->arena_pid) {
		if (slot >= 2 || !call->arena[slot].valid || call->arena[slot].len == 0)
			return -EPERM;
		*lo = call->arena[slot].start;
		*hi = call->arena[slot].start + call->arena[slot].len;
		return 0;
	}
	*lo = 0;
	*hi = window_len_of_pid(call, pid);
	return *hi == 0 ? -EPERM : 0;
}

/* [off, off + len) inside [lo, hi), in 64 bits. */
static int span_ok(uint64_t off, uint64_t len, uint64_t lo, uint64_t hi)
{
	return off >= lo && off <= hi && len <= hi - off;
}

/* The arena block of this slot: the buffer is the caller's own memory, so it may be written and invalidated. */
static int is_arena_slot(const struct iav_call *call, unsigned int slot, uint32_t pid)
{
	return call->arena_pid != 0 && pid == call->arena_pid && slot < 2 && call->arena[slot].valid;
}

static int scale_buf_ok(const struct iav_call *call, unsigned int slot, const struct iav_scale_buf_info *b, int fmt)
{
	uint64_t y_bytes, uv_bytes, lo, hi;

	if (b->use_phys || b->use_dma_buf_fd)
		return -EPERM;
	if (b->pid != IAV_PART_DEC_BSB && !is_arena_slot(call, slot, b->pid))
		return -EPERM;
	if (buf_range(call, slot, b->pid, &lo, &hi) != 0)
		return -EPERM;
	if (b->pitch == 0 || b->height == 0)
		return -EINVAL;
	if (uv_plane_bytes(fmt, b->pitch, b->height, &uv_bytes) != 0)
		return -EINVAL;
	y_bytes = (uint64_t)b->pitch * b->height;
	if (!span_ok(b->y_offset, y_bytes, lo, hi))
		return -EINVAL;
	if (uv_bytes && !span_ok(b->uv_offset, uv_bytes, lo, hi))
		return -EINVAL;
	return 0;
}

static int check_img_scale(struct iav_call *call)
{
	const struct iav_img_scale *s = call->arg;
	const struct iav_scale_buf_info *in = &s->input_buf, *out = &s->output_buf;
	int in_fmt, out_fmt, rc;
	uint64_t x0, y0, w, h, ow, oh;

	memcpy(&in_fmt, &s->format, sizeof(in_fmt));
	out_fmt = in_fmt;
	if (s->output_format_enable)
		memcpy(&out_fmt, &s->output_format, sizeof(out_fmt));
	rc = scale_buf_ok(call, 0, in, in_fmt);
	if (rc == 0)
		rc = scale_buf_ok(call, 1, out, out_fmt);
	if (rc != 0)
		return rc;

	/* The input crop window the DSP reads, as calc_img_scale_params (iav_enc_utils.c) aligns it. */
	if (s->input_win_format == IAV_INPUT_WIN_FORMAT_U32) {
		x0 = s->input.x - (s->input.x % 2u);
		y0 = s->input.y - (s->input.y % 2u);
		w = round_up64((uint64_t)s->input.width + (s->input.x % 2u), IMG_SCALE_H_ALIGN);
		h = round_up64((uint64_t)s->input.height + (s->input.y % 2u), IMG_SCALE_V_ALIGN);
	} else if (s->input_win_format == IAV_INPUT_WIN_FORMAT_U24_8) {
		/* u24.8 fixed point, aligned exactly as do_calc_buf_params (iav_enc_utils.c) does, in 64 bits */
		uint64_t xi = (s->input.x >> 8) - ((s->input.x >> 8) % IMG_SCALE_H_ALIGN);
		uint64_t yi = (s->input.y >> 8) - ((s->input.y >> 8) % IMG_SCALE_V_ALIGN);
		uint64_t xf = (uint64_t)s->input.x - (xi << 8);
		uint64_t yf = (uint64_t)s->input.y - (yi << 8);
		uint64_t wf = (s->input.width & 0xffu) + (xf & 0xffu);
		uint64_t hf = (s->input.height & 0xffu) + (yf & 0xffu);
		uint64_t wi = (s->input.width >> 8) + (xf >> 8) + (wf >> 8) + ((wf & 0xffu) ? 1u : 0u);
		uint64_t hi = (s->input.height >> 8) + (yf >> 8) + (hf >> 8) + ((hf & 0xffu) ? 1u : 0u);

		x0 = xi;
		y0 = yi;
		w = round_up64(wi, IMG_SCALE_H_ALIGN);
		h = round_up64(hi, IMG_SCALE_V_ALIGN);
	} else {
		return -EINVAL;
	}
	if (x0 + w > in->pitch || y0 + h > in->height)
		return -EINVAL;

	/* The output window the DSP writes: x, y as given, the size rounded up (pack_iproc_scale_job). */
	ow = round_up64(s->output.width, IMG_SCALE_H_ALIGN);
	oh = round_up64(s->output.height, IMG_SCALE_V_ALIGN);
	if ((uint64_t)s->output.x + ow > out->pitch || (uint64_t)s->output.y + oh > out->height)
		return -EINVAL;
	return 0;
}

/*
 * GET_VIDEO_PROC: a read of the stored configuration, except two cids whose "get" copies alpha and warp tables with
 * the GDMA engine inside IAV_PART_BLEND_ALPHA and IAV_PART_BLEND_WARP (iav_enc_blend.c), partitions this pipeline
 * does not have and the guest has no window over. Those two are refused; the cid is read as an int.
 */
static int check_get_vproc(struct iav_call *call)
{
	const struct iav_video_proc *v = call->arg;
	int cid;

	memcpy(&cid, &v->cid, sizeof(cid));
	if (cid == IAV_VIDEO_PROC_MULTI_BLEND || cid == IAV_VIDEO_PROC_BLEND_WARP)
		return -EPERM;
	return 0;
}

/*
 * GET_VIN_IDSP_CFG: the driver tests 1U << chan_id and 1U << vsrc_id with values a guest chooses
 * (iav_enc_misc.c, iav_ioc_get_vin_idsp_cfg). A shift of 32 or more is undefined, and on arm64 it wraps, so a large
 * id can pass the map test and the driver then copies from a context it did not find.
 */
static int check_get_vin_idsp(struct iav_call *call)
{
	const struct iav_vin_idsp_config *c = call->arg;

	if (c->idsp_cfg.chan_id >= 32u || c->vin_cfg.vsrc_id >= 32u)
		return -EINVAL;
	return 0;
}

/*
 * Blur. A blur area whose use_arb_blur is set makes the DSP read guest data from IAV_PART_ARB_BLUR (normal type) or
 * ARB_SMOOTH_BLUR (smooth type). The first is an entry of the primary's aux aperture (Envelope F): the bitmap must lie
 * in it. The driver checks arb_blur_offset + area_size against the partition with area_size a u16 computed from the
 * width (iav_enc_overlay.c, get_blur_area_size and iav_check_blur_param), while the DSP reads height rows at
 * arb_blur_pitch apart, each from arb_blur_offset + (split x - area x) / 8 for at most width / 8 rounded up plus one
 * bytes (iav_dsp_cmd_common.c, pack_blur_buf_cfg; prepare_blur_content splits an area at multiples of four pixels):
 * here the whole of that, in 64 bits, lies in the entry. ARB_SMOOTH_BLUR has no entry: refused. An enabled blur also
 * needs the blur partition of its type; the driver's parameter check dereferences it without a NULL test (the content
 * size), so an enabled blur is refused unless the driver reports the partition. The id is a stream id (bit id of a
 * 32-bit bitmap in the driver) or a canvas id.
 */
static int arb_blur_area_ok(const struct iav_call *call, const struct iav_blur_area *a, int smooth)
{
	uint64_t limit = smooth ? 0 : aux_len_of(call, IAV_PART_ARB_BLUR);
	uint64_t row = ((uint64_t)a->width + 7u) / 8u + 1u;

	if (limit == 0)
		return -EPERM;
	if (a->height == 0)
		return -EINVAL;
	if (!range_ok((uint64_t)a->arb_blur_offset + (uint64_t)(a->height - 1u) * a->arb_blur_pitch, row, limit))
		return -EINVAL;
	return 0;
}

static int blur_partition_present(struct iav_call *call, uint32_t pid)
{
	struct iav_querymem q;

	if (call->query == NULL)
		return 0;
	memset(&q, 0, sizeof(q));
	q.mid = IAV_MEM_PARTITION;
	q.arg.partition.pid = pid;
	if (call->query(call->qctx, IAV_IOC_QUERY_MEMBLOCK, &q) != 0)
		return 0;
	return q.arg.partition.mem.length != 0;
}

static int check_blur_insert(struct iav_call *call, const struct iav_blur_insert *b)
{
	unsigned int i;
	int rc;

	if (b->id_for_canvas ? b->canvas_id >= IAV_MAX_CANVAS_BUF_NUM : b->stream_id >= STREAM_IDS)
		return -EINVAL;
	if (!b->enable)
		return 0;
	if (b->type >= IAV_BLUR_TYPE_NUM)
		return -EINVAL;
	for (i = 0; i < MAX_NUM_BLUR_AREA; i++) {
		if (!b->area[i].enable || !b->area[i].use_arb_blur)
			continue;
		rc = arb_blur_area_ok(call, &b->area[i], b->type == IAV_BLUR_TYPE_SMOOTH);
		if (rc)
			return rc;
	}
	if (!blur_partition_present(call, b->type == IAV_BLUR_TYPE_SMOOTH ? IAV_PART_SMOOTH_BLUR : IAV_PART_BLUR))
		return -EINVAL;
	return 0;
}

static int check_set_blur(struct iav_call *call)
{
	return check_blur_insert(call, call->arg);
}

/*
 * CFG_CANVAS_FRAME_SYNC_PROC: blur (as SET_BLUR_INSERT), canvas overlay (against the CANVAS_OVERLAY entry, Envelope F)
 * and post-processing scale. The driver takes the canvas id from c->id for all three (iav_enc_canvas.c,
 * set_canvas_frame_sync) and bounds it by IAV_MAX_CANVAS_BUF_NUM first.
 */
static int check_canvas_sync(struct iav_call *call)
{
	const struct iav_canvas_sync_cfg *c = call->arg;
	int cid;

	if (c->id >= IAV_MAX_CANVAS_BUF_NUM)
		return -EINVAL;
	memcpy(&cid, &c->cid, sizeof(cid));
	switch (cid) {
	case IAV_CANVAS_SYNC_CFG_BLUR:
		return check_blur_insert(call, &c->arg.blur);
	case IAV_CANVAS_SYNC_CFG_POSTP_SCALE:
		return 0;
	case IAV_CANVAS_SYNC_CFG_OVERLAY:
		return overlay_ok(call, &c->arg.overlay, c->id, 1);
	default:
		return -EINVAL;
	}
}

/*
 * GDMA_COPY: offsets in windows only. The source may be any window the guest maps; the destination only the
 * primary guest's own content (decode, overlay). The driver checks offsets against the whole allocation, which for
 * the overlay partition includes the driver's command area past the content the window covers, so the window length
 * is the bound here. width <= pitch on both sides, and neither may be zero: with a zero width or height the driver
 * skips its partition checks and then dereferences the partition it did not look up (get_gdma_copy_task).
 */
static int gdma_side_ok(struct iav_call *call, unsigned int slot, uint32_t pid, uint64_t offset, uint64_t pitch,
			uint64_t height, uint64_t width, int dst)
{
	uint64_t lo, hi;

	if (buf_range(call, slot, pid, &lo, &hi) != 0)
		return -EPERM;
	if (dst && !pid_is_primary_content(call, pid) && !is_arena_slot(call, slot, pid))
		return -EPERM;
	if (pitch == 0 || width > pitch)
		return -EINVAL;
	if (!span_ok(offset, pitch * height, lo, hi))
		return -EINVAL;
	return 0;
}

static int check_gdma_copy(struct iav_call *call)
{
	const struct iav_gdma_copy *g = call->arg;
	int rc;

	if (g->src_use_phys || g->dst_use_phys || g->src_use_dma_buf_fd || g->dst_use_dma_buf_fd)
		return -EPERM;
	if (g->width == 0 || g->height == 0)
		return -EINVAL;
	rc = gdma_side_ok(call, 0, g->src_mmap_type, g->src_offset, g->src_pitch, g->height, g->width, 0);
	if (rc == 0)
		rc = gdma_side_ok(call, 1, g->dst_mmap_type, g->dst_offset, g->dst_pitch, g->height, g->width, 1);
	return rc;
}

/*
 * SYNC_CACHE_MEM_PART: inside a window or an arena block. A clean only writes back what the CPU already wrote,
 * anywhere; an invalidate discards the Dom0 kernel's dirty lines, so it is allowed only on the primary guest's own
 * content (decode, overlay, its arena blocks; the arena's pool is uncached, so the driver does nothing there).
 */
static int check_sync_cache(struct iav_call *call)
{
	const struct iav_sync_cache_mem_part *c = call->arg;
	uint64_t lo, hi;
	int opt;

	memcpy(&opt, &c->opt, sizeof(opt));
	if (buf_range(call, 0, c->pid, &lo, &hi) != 0)
		return -EPERM;
	if (opt != IAV_CACHE_CLEAN && opt != IAV_CACHE_INVALIDATE)
		return -EINVAL;
	if (opt == IAV_CACHE_INVALIDATE && !pid_is_primary_content(call, c->pid) && !is_arena_slot(call, 0, c->pid))
		return -EPERM;
	if (c->length == 0 || !span_ok(c->offset, c->length, lo, hi))
		return -EINVAL;
	return 0;
}

/* GET_ISP_NN_CFG: the configuration carries the physical addresses of the NN graphs and the FIFO; they are cleared. */
static void post_isp_nn(struct iav_call *call)
{
	struct iav_isp_nn_params *p = call->arg;
	unsigned int i;

	p->arg.nn_cfg.fifo_addr = 0;
	for (i = 0; i < IAV_ISP_NN_TOTAL_MAX; i++) {
		p->arg.nn_cfg.nn_info[i].dag_addr_phys_addr = 0;
		p->arg.nn_cfg.nn_info[i].port_map_phys_addr = 0;
		p->arg.nn_cfg.nn_info[i].port_pos_map_phys_addr = 0;
	}
}

/*
 * EFM_GET_POOL_INFO and EFM_RELEASE_ALL_FRAMES: check_efm_stream (iav_enc_efm.c) bounds the stream id by
 * max_stream_num and then dereferences the stream's context without a NULL test; a context exists for the streams of
 * the resource's stream map. The id must be in the map the driver reports now.
 */
static int stream_in_map(struct iav_call *call, unsigned int stream_id)
{
	struct iav_system_resource res;

	if (call->query == NULL)
		return -EPERM;
	memset(&res, 0, sizeof(res));
	res.encode_mode = DSP_CURRENT_ENCODE_MODE;
	if (call->query(call->qctx, IAV_IOC_GET_SYSTEM_RESOURCE, &res) != 0)
		return -EPERM;
	if (stream_id >= res.max_stream_num || stream_id >= 32u || !((res.stream_map >> stream_id) & 1u))
		return -EINVAL;
	return 0;
}

static int check_efm_pool_info(struct iav_call *call)
{
	const struct iav_efm_get_pool_info *p = call->arg;

	return stream_in_map(call, p->stream_id);
}

static int check_efm_release_all(struct iav_call *call)
{
	const struct iav_efm_release_all_frame *p = call->arg;

	return stream_in_map(call, p->stream_id);
}

/*
 * The manual-feed releases index the driver's per-channel, per-canvas and per-VIN feed tables with the guest's id
 * before any bound (iav_enc_mf.c, get_mf_info_type, reached from RELEASE_PYRAMID_BUF with no other check, and with
 * seq_num -1 it then clears the entry it found): the id must be below the table's size.
 */
static int check_release_pyramid(struct iav_call *call)
{
	const struct iav_pyramiddesc *d = call->arg;

	return d->chan_id < CONFIG_AMBARELLA_MAX_CHANNEL_NUM ? 0 : -EINVAL;
}

static int check_release_canvas(struct iav_call *call)
{
	const struct iav_canvasdesc *d = call->arg;

	return d->canvas_id < IAV_MAX_CANVAS_BUF_NUM ? 0 : -EINVAL;
}

static int check_release_raw(struct iav_call *call)
{
	const struct iav_rawbufdesc *d = call->arg;

	return d->vin_id < VIN_CONTROLLER_NUM ? 0 : -EINVAL;
}

/*
 * ---- the calls the aux aperture unlocks (plan_iav_usb_yolo, Envelope F) ----
 *
 * Inventory classes N and M (image_ioctl_inventory.md 3.2-3.6, 3.10, 3.11; f_design.md section 3). A table the driver
 * copies or the DSP reads lives in an entry of the caller's own aux aperture (MASK, WARP, QPMATRIX, CE_GO_MAP,
 * CANVAS_OVERLAY, ARB_BLUR, YUV_REMAP); a call that needs one is refused when the caller has no such entry. The ids
 * these handlers shift by or index with before any bound are bounded here.
 */
#define CHANNELS		CONFIG_AMBARELLA_MAX_CHANNEL_NUM	/* 4 on N1-655 (the driver's arrays and maps) */
#define CE_GO_MAP_BUFFERS	8u	/* CE_GO_MAP_BUFFER_NUM, include/driver/specific/iav_config.h (driver-internal) */
#define VIN_SOURCES		32u	/* bits of the driver's possible_vsrc_map */

static int canvas_id_ok(uint32_t id)
{
	return id < IAV_MAX_CANVAS_BUF_NUM;
}

/*
 * CFG_VIDEO_PROC. The cids that name no memory are served with their ids bounded (the mask handler tests
 * 1U << channel_id and then indexes per-channel arrays, iav_enc_mask.c; the vsrc update tests 1U << vsrc_id,
 * iav_enc_api.c; the canvas order shifts by every chan_order entry, iav_enc_canvas.c; DPTZ sets bit buf_id and creates
 * a source buffer context for any id it is given, iav_enc_resource.c). MASK needs the MASK entry (the driver copies the
 * mask from the user part the guest wrote), CANVAS_OVERLAY the CANVAS_OVERLAY entry. A vsrc update of a decoder
 * channel would change the decoder count behind the decode window (window pinning, Envelope C). The blend, match,
 * fusion, optical-flow and thermal cids name partitions absent on these boards with no entry: refused.
 */
static int check_cfg_vproc(struct iav_call *call)
{
	const struct iav_video_proc *v = call->arg;
	unsigned int i;
	int cid;

	memcpy(&cid, &v->cid, sizeof(cid));
	switch (cid) {
	case IAV_VIDEO_PROC_MASK:
		if (v->arg.mask.channel_id >= CHANNELS)
			return -EINVAL;
		return aux_len_of(call, IAV_PART_MASK) ? 0 : -EPERM;
	case IAV_VIDEO_PROC_DPTZ:
		if (v->arg.dptz.channel_id >= CHANNELS || v->arg.dptz.pass_id >= IAV_MAX_PASS_NUM ||
		    v->arg.dptz.buf_id >= IAV_SRCBUF_NUM)
			return -EINVAL;
		if (!canvas_id_ok(v->arg.dptz.buf_cfg.canvas_id) && v->arg.dptz.buf_cfg.canvas_id != IAV_INVALID_CANVAS_ID)
			return -EINVAL;
		return 0;
	case IAV_VIDEO_PROC_CANVAS_FPS:
		return canvas_id_ok(v->arg.canvas_fps.id) ? 0 : -EINVAL;
	case IAV_VIDEO_PROC_CANVAS_VV_SYNC_CFG:
		return canvas_id_ok(v->arg.canvas_vv_sync.id) ? 0 : -EINVAL;
	case IAV_VIDEO_PROC_CANVAS_DUMMY_LATENCY:
		return canvas_id_ok(v->arg.canvas_dummy_latency.id) ? 0 : -EINVAL;
	case IAV_VIDEO_PROC_VSRC_UPDATE: {
		struct iav_chan_cfg have;

		if (v->arg.vsrc_update.chan_id >= CHANNELS || v->arg.vsrc_update.vsrc_id >= VIN_SOURCES)
			return -EINVAL;
		if (call->query == NULL)
			return -EPERM;
		memset(&have, 0, sizeof(have));
		have.chan_id = v->arg.vsrc_update.chan_id;
		if (call->query(call->qctx, IAV_IOC_GET_CHAN_CONFIG, &have) != 0)
			return -EPERM;
		if (have.pre_dec_enable) {
			call->pinned = 1;
			return -EPERM;
		}
		return 0;
	}
	case IAV_VIDEO_PROC_CANVAS_OVERLAY:
		/* the driver forces id_for_canvas (iav_enc_overlay.c, iav_cfg_vproc_overlay) */
		return overlay_ok(call, &v->arg.overlay, v->arg.overlay.id, 1);
	case IAV_VIDEO_PROC_PYRAMID_ROI:
		return v->arg.pyramid_roi.chan_id < CHANNELS ? 0 : -EINVAL;
	case IAV_VIDEO_PROC_PYRAMID_FPS:
		return v->arg.pyramid_fps.chan_id < CHANNELS ? 0 : -EINVAL;
	case IAV_VIDEO_PROC_CANVAS_ORDER:
		if (!canvas_id_ok(v->arg.canvas_order.id) || v->arg.canvas_order.chan_num > IAV_MAX_CHANNEL_NUM)
			return -EINVAL;
		/* the driver shifts by chan_order[i] up to the stored count, which may differ from this one: all entries */
		for (i = 0; i < IAV_MAX_CHANNEL_NUM; i++)
			if (v->arg.canvas_order.chan_order[i] >= 32u)
				return -EINVAL;
		return 0;
	case IAV_VIDEO_PROC_VCAP_FLUSH:
		return v->arg.vcap_flush.channel_id < CHANNELS ? 0 : -EINVAL;
	case IAV_VIDEO_PROC_POSTP_SCALE:
		if (!canvas_id_ok(v->arg.postp_scale.canvas_id))
			return -EINVAL;
		for (i = 0; i < IAV_MAX_POSTP_SCALE_AREA_NUM; i++)
			if (v->arg.postp_scale.area[i].enable && !canvas_id_ok(v->arg.postp_scale.area[i].src_canvas_id))
				return -EINVAL;
		return 0;
	case IAV_VIDEO_PROC_CAWARP:
		return v->arg.cawarp.chan_id < CHANNELS ? 0 : -EINVAL;
	default:
		return -EPERM;
	}
}

/*
 * CFG_WARP_CTRL. The driver bounds channel_id itself (iav_enc_warp.c, iav_ioc_c_warp_ctrl) and copies each enabled
 * area's table from its fixed slot of the WARP user part, which the guest writes through its WARP entry; the map offsets
 * are only range-checked there, with a sum against an int, so here every enabled area's offsets also lie in the entry.
 * MAIN needs the WARP entry; DPTZ and MAX_CFG name no memory. ENCOFS has no handler.
 */
static int warp_map_ok(const struct iav_warp_map *m, uint64_t limit)
{
	return m->data_addr_offset < limit;
}

static int check_warp_ctrl(struct iav_call *call)
{
	const struct iav_warp_ctrl *w = call->arg;
	uint64_t limit;
	unsigned int i;
	int cid;

	memcpy(&cid, &w->cid, sizeof(cid));
	if (w->channel_id >= CHANNELS)
		return -EINVAL;
	switch (cid) {
	case IAV_WARP_CTRL_MAIN:
		limit = aux_len_of(call, IAV_PART_WARP);
		if (limit == 0)
			return -EPERM;
		for (i = 0; i < MAX_NUM_WARP_AREAS; i++) {
			const struct iav_warp_area *a = &w->arg.main.area[i];

			if (!a->enable)
				continue;
			if (!warp_map_ok(&a->h_map, limit) || !warp_map_ok(&a->v_map, limit) ||
			    !warp_map_ok(&a->me1_v_map, limit))
				return -EINVAL;
		}
		return 0;
	case IAV_WARP_CTRL_DPTZ:
		return w->arg.dptz.buf_id < IAV_SRCBUF_NUM ? 0 : -EINVAL;
	case IAV_WARP_CTRL_MAX_CFG:
		return 0;
	default:
		return -EPERM;
	}
}

/*
 * YUV remap: each enabled area's colour table (1 KiB) is read by the DSP at clut_addr_offset in IAV_PART_YUV_REMAP,
 * which the server allocates and the primary maps as an entry. The stream id is shifted into a 32-bit bitmap first.
 */
#define YUV_REMAP_CLUT_BYTES 1024u

static int yuv_remap_ok(const struct iav_call *call, const struct iav_yuv_remap *r, uint32_t id)
{
	uint64_t limit;
	unsigned int i;

	if (id >= STREAM_IDS)
		return -EINVAL;
	if (!r->enable)
		return 0;
	limit = aux_len_of(call, IAV_PART_YUV_REMAP);
	if (limit == 0)
		return -EPERM;
	for (i = 0; i < MAX_NUM_YUV_REMAP_AREA; i++)
		if (r->area[i].enable && !range_ok(r->area[i].clut_addr_offset, YUV_REMAP_CLUT_BYTES, limit))
			return -EINVAL;
	return 0;
}

static int check_set_yuv_remap(struct iav_call *call)
{
	const struct iav_yuv_remap *r = call->arg;

	return yuv_remap_ok(call, r, r->id);
}

/*
 * SET_STREAM_CONFIG and CFG_FRAME_SYNC_PROC. The stream id is shifted into 32-bit maps before anything else
 * (iav_enc_stream.c, iav_ioc_s_stream_cfg). QP ROI copies the matrix from the stream's slot of the QPMATRIX user part,
 * which the guest writes through its entry; with qpm_raw the driver uses data_offset unchecked as a CPU and a DSP
 * offset (iav_enc_codec.c), refused. Overlay, blur and YUV remap are checked as their own calls, against the id the
 * driver will use (the stream config's). Custom SEI stays refused (inventory 4.2). Every other cid names no memory.
 */
static int check_stream_cfg(struct iav_call *call)
{
	const struct iav_stream_cfg *c = call->arg;
	int cid;

	if (c->id >= STREAM_IDS)
		return -EINVAL;
	memcpy(&cid, &c->cid, sizeof(cid));
	switch (cid) {
	case IAV_STMCFG_CUSTOM_SEI:
		return -EPERM;
	case IAV_H264_CFG_QP_ROI:
	case IAV_H265_CFG_QP_ROI: {
		const struct iav_qpmatrix *q = cid == IAV_H264_CFG_QP_ROI ? &c->arg.h264_roi : &c->arg.h265_roi;

		if (q->qpm_raw)
			return -EPERM;
		return aux_len_of(call, IAV_PART_QPMATRIX) ? 0 : -EPERM;
	}
	case IAV_STMCFG_OVERLAY:
		/* the driver sets overlay.id to the stream and checks it as a stream overlay (iav_do_insert_stream_overlay) */
		return overlay_ok(call, &c->arg.overlay, c->id, 0);
	case IAV_STMCFG_BLUR:
		/* blur.stream_id is replaced by the stream's id only for frame sync; both must be in range */
		return check_blur_insert(call, &c->arg.blur);
	case IAV_STMCFG_YUV_REMAP:
		return yuv_remap_ok(call, &c->arg.yuv_remap, c->id);
	default:
		return 0;
	}
}

/*
 * CE GO map buffers live in IAV_PART_CE_GO_MAP, which the primary maps as an entry; the driver hands out a buffer
 * offset and the guest writes the map there before the handshake. The handshake indexes the channel's buffer-state
 * array with buffer_idx before any bound (iav_enc_go_map.c, check_ce_go_map_handshake).
 */
static int check_ce_request(struct iav_call *call)
{
	const struct iav_ce_go_map_request_buffer *r = call->arg;

	if (r->chan_id >= CHANNELS)
		return -EINVAL;
	return aux_len_of(call, IAV_PART_CE_GO_MAP) ? 0 : -EPERM;
}

static int check_ce_handshake(struct iav_call *call)
{
	const struct iav_ce_go_map_handshake_buffer *h = call->arg;

	if (h->chan_id >= CHANNELS || h->buffer_idx >= CE_GO_MAP_BUFFERS)
		return -EINVAL;
	return aux_len_of(call, IAV_PART_CE_GO_MAP) ? 0 : -EPERM;
}

/*
 * IMG_GET_MISC_INFO, inline marshal (amba_virt_iav.h, struct amba_virt_iav_inline_misc). The three pointer fields arrive
 * as presence flags; they are pointed at the broker's own buffers in the trailer for the call and put back after it.
 * The driver reads and writes at most the pipeline's channel count of entries (imgproc_statis.c, dsp_get_extra_raw_info:
 * chan_num, bounded by CONFIG_AMBARELLA_MAX_CHANNEL_NUM at SET_SYSTEM_RESOURCE), which the trailer's 16 entries cover.
 */
struct misc_flags {
	uintptr_t query, valid, extra;
};

static int check_misc_info(struct iav_call *call)
{
	struct img_misc_info *m = call->arg;
	struct amba_virt_iav_inline_misc *t = call->inline_buf;
	struct misc_flags f;

	if (t == NULL)
		return -EPERM;
	memcpy(&f.query, &m->query_extra_raw, sizeof(f.query));
	memcpy(&f.valid, &m->extra_raw_valid, sizeof(f.valid));
	memcpy(&f.extra, &m->extra_raw, sizeof(f.extra));
	if (f.query > 1 || f.valid > 1 || f.extra > 1 || t->reserved || t->count > AMBA_VIRT_IAV_INLINE_MISC_CHANS)
		return -EINVAL;
	memset(t->query + t->count, 0, AMBA_VIRT_IAV_INLINE_MISC_CHANS - t->count);
	memset(t->valid, 0, sizeof(t->valid));
	memset(t->extra, 0, sizeof(t->extra));
	m->query_extra_raw = f.query ? t->query : NULL;
	m->extra_raw_valid = f.valid ? t->valid : NULL;
	m->extra_raw = f.extra ? (void *)t->extra : NULL;
	return 0;
}

/* The output carries the flags again, never the broker's addresses. */
static void post_misc_info(struct iav_call *call)
{
	struct img_misc_info *m = call->arg;
	uintptr_t q = m->query_extra_raw != NULL, v = m->extra_raw_valid != NULL, e = m->extra_raw != NULL;

	memcpy(&m->query_extra_raw, &q, sizeof(q));
	memcpy(&m->extra_raw_valid, &v, sizeof(v));
	memcpy(&m->extra_raw, &e, sizeof(e));
}

/*
 * The applies of the configurations above (primary-allowed since Envelope C as PLAIN, with only the header's first byte
 * reaching the driver: the handlers copy a larger struct, the wire size). Envelope F serves the configurations, so their
 * applies get their full struct after this audit (IAV_F_WIRE_AUDITED):
 * - APPLY_VIDEO_PROC (iav_enc_api.c, iav_ioc_apply_vproc): one {apply, param} per cid; param is a channel map (mask,
 *   DPTZ, pyramid, VCAP flush) or a canvas map (post-processing scale, canvas order, canvas fps), each walked over the
 *   driver's existing contexts. The blend, arbitrary blend and thermal fusion applies act on configurations a guest
 *   cannot set (their partitions are absent and their cids refused above): refused. Maps are bounded to the ids that
 *   exist on N1-655 (channels below 4, canvases below IAV_MAX_CANVAS_BUF_NUM).
 * - APPLY_WARP_CTRL (iav_enc_warp.c, iav_ioc_a_warp_ctrl, iav_process_warp_ctrl): apply is the warp cid (MAIN or DPTZ),
 *   param a channel map.
 * - APPLY_FRAME_SYNC_PROC (iav_enc_stream.c, iav_ioc_apply_frame_sync) and APPLY_CANVAS_FRAME_SYNC_PROC
 *   (iav_enc_canvas.c, iav_ioc_apply_canvas_frame_sync): a PTS, a stream or canvas map (28 bits, walked over existing
 *   contexts) and flags; the stored configurations they apply were checked when they were set. The canvas map is bounded.
 */
#define CHAN_MAP_OK(m)		(((m) & ~((1u << CHANNELS) - 1u)) == 0)
#define CANVAS_MAP_OK(m)	(((m) & ~((1u << IAV_MAX_CANVAS_BUF_NUM) - 1u)) == 0)

static int check_apply_vproc(struct iav_call *call)
{
	const struct iav_apply_flag *f = call->arg;
	int k;

	for (k = 0; k < IAV_VIDEO_PROC_NUM; k++) {
		if (!f[k].apply)
			continue;
		switch (k) {
		case IAV_VIDEO_PROC_MASK:
		case IAV_VIDEO_PROC_DPTZ:
		case IAV_VIDEO_PROC_PYRAMID_ROI:
		case IAV_VIDEO_PROC_PYRAMID_FPS:
		case IAV_VIDEO_PROC_VCAP_FLUSH:
			if (!CHAN_MAP_OK(f[k].param))
				return -EINVAL;
			break;
		case IAV_VIDEO_PROC_POSTP_SCALE:
		case IAV_VIDEO_PROC_CANVAS_ORDER:
		case IAV_VIDEO_PROC_CANVAS_FPS:
			if (!CANVAS_MAP_OK(f[k].param))
				return -EINVAL;
			break;
		default:
			return -EPERM;
		}
	}
	return 0;
}

static int check_apply_warp(struct iav_call *call)
{
	const struct iav_apply_flag *f = call->arg;

	if (f->apply != IAV_WARP_CTRL_MAIN && f->apply != IAV_WARP_CTRL_DPTZ)
		return -EPERM;
	return CHAN_MAP_OK(f->param) ? 0 : -EINVAL;
}

static int check_apply_canvas_sync(struct iav_call *call)
{
	const struct iav_apply_canvas_frame_sync *a = call->arg;

	return CANVAS_MAP_OK(a->canvas_updated_map) ? 0 : -EINVAL;
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
	{ IAV_IOC_QUERY_DESC, BOTH, IAV_F_BOUNDED_DESC | IAV_F_WINDOW, check_query_desc, NULL },
	{ IAV_IOC_SET_POLL_EVENT_CFG, BOTH, 0, check_poll_cfg, NULL },
	{ IAV_IOC_CLEAR_POLL_EVENT_CFG, BOTH, 0, NULL, NULL },
	{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL, NULL },
	/* Version 3: decode, for the primary guest, with every offset checked against the decode window. */
	{ IAV_IOC_CREATE_DECODER, PRIMARY_SET, IAV_F_WINDOW, check_create_decoder, NULL },
	{ IAV_IOC_DECODE_JPEG, PRIMARY_SET, IAV_F_WINDOW, check_decode_jpeg, NULL },
	{ IAV_IOC_DECODE_VIDEO, PRIMARY_SET, IAV_F_WINDOW, check_decode_video, NULL },
	{ IAV_IOC_QUERY_DECODE_BSB, PRIMARY_SET, IAV_F_WINDOW, check_decode_bsb, NULL },
	{ IAV_IOC_WAIT_DECODE_BSB, PRIMARY_SET, IAV_F_WINDOW, check_decode_bsb, NULL },
	{ IAV_IOC_QUERY_DECODE_STATUS, PRIMARY_SET, 0, check_decode_status, post_decode_status },
	{ IAV_IOC_WAIT_PLAYBACK_EOS, PRIMARY_SET, 0, check_wait_eos, NULL },
	/* Version 4: stream overlays, with the data and colour table checked against the overlay window. */
	{ IAV_IOC_SET_OVERLAY_INSERT, PRIMARY_SET, IAV_F_WINDOW, check_set_overlay, NULL },
	{ IAV_IOC_GET_OVERLAY_INSERT, PRIMARY_SET, IAV_F_WINDOW, check_get_overlay, NULL },
	/*
	 * Version 5 (window pinning): the two resource calls of the primary set keep the partitions behind the windows
	 * in place. They stay in the primary set below; this entry only adds the check.
	 */
	{ IAV_IOC_SET_CHAN_CONFIG, PRIMARY_SET, 0, check_chan_cfg_pinned, NULL },
	{ IAV_IOC_SET_SYSTEM_RESOURCE, PRIMARY_SET, 0, check_system_resource_pinned, NULL },
	/*
	 * Version 6 (plan_iav_usb_yolo, Envelope D): the image calls of inventory class W, for the primary guest only.
	 * Each was read in its handler; the checks are above. Rows without a check name no memory, or only return what
	 * the driver stores (their offsets are partition offsets, never addresses).
	 */
	{ IAV_IOC_SET_IMG_SCALE, PRIMARY_SET, IAV_F_WINDOW, check_img_scale, NULL },
	{ IAV_IOC_WAIT_IMG_SCALE, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_GET_VIDEO_PROC, PRIMARY_SET, 0, check_get_vproc, NULL },
	{ IAV_IOC_GET_VIN_IDSP_CFG, PRIMARY_SET, 0, check_get_vin_idsp, NULL },
	{ IAV_IOC_WAIT_SKIP_IDSP_CFG, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_GET_WARP_CTRL, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_SET_BLUR_INSERT, PRIMARY_SET, IAV_F_WINDOW, check_set_blur, NULL },
	{ IAV_IOC_GET_BLUR_INSERT, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_GET_YUV_REMAP_INSERT, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_GET_FRAME_SYNC_PROC, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, PRIMARY_SET, IAV_F_WINDOW, check_canvas_sync, NULL },
	{ IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_GET_STREAM_CONFIG, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_QUERY_MEM_LAYOUT_INFO, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_GDMA_COPY, PRIMARY_SET, IAV_F_WINDOW, check_gdma_copy, NULL },
	{ IAV_IOC_SYNC_CACHE_MEM_PART, PRIMARY_SET, IAV_F_WINDOW, check_sync_cache, NULL },
	{ IAV_IOC_GET_ISP_NN_CFG, PRIMARY_SET, 0, NULL, post_isp_nn },
	{ IAV_IOC_EFM_GET_POOL_INFO, PRIMARY_SET, 0, check_efm_pool_info, NULL },
	{ IAV_IOC_EFM_RELEASE_ALL_FRAMES, PRIMARY_SET, IAV_F_WIRE_AUDITED, check_efm_release_all, NULL },
	{ IAV_IOC_FEED_CANVAS_BUF, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_RELEASE_CANVAS_BUF, PRIMARY_SET, 0, check_release_canvas, NULL },
	{ IAV_IOC_FEED_PYRAMID_BUF, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_RELEASE_PYRAMID_BUF, PRIMARY_SET, 0, check_release_pyramid, NULL },
	{ IAV_IOC_FEED_RAW_BUF, PRIMARY_SET, 0, NULL, NULL },
	{ IAV_IOC_RELEASE_RAW_BUF, PRIMARY_SET, IAV_F_WIRE_AUDITED, check_release_raw, NULL },
	/*
	 * Version 7 (plan_iav_usb_yolo, Envelope F): the calls the aux aperture and the server's new partitions unlock
	 * (inventory classes N and M), primary only. SET_OVERLAY_INSERT, GET_OVERLAY_INSERT, SET_BLUR_INSERT and
	 * CFG_CANVAS_FRAME_SYNC_PROC above take their canvas overlay and arbitrary blur branches from the same checks.
	 */
	{ IAV_IOC_CFG_VIDEO_PROC, PRIMARY_SET, IAV_F_WINDOW, check_cfg_vproc, NULL },
	{ IAV_IOC_CFG_WARP_CTRL, PRIMARY_SET, IAV_F_WINDOW, check_warp_ctrl, NULL },
	{ IAV_IOC_SET_STREAM_CONFIG, PRIMARY_SET, IAV_F_WINDOW, check_stream_cfg, NULL },
	{ IAV_IOC_CFG_FRAME_SYNC_PROC, PRIMARY_SET, IAV_F_WINDOW, check_stream_cfg, NULL },
	{ IAV_IOC_SET_YUV_REMAP_INSERT, PRIMARY_SET, IAV_F_WINDOW, check_set_yuv_remap, NULL },
	{ IAV_IOC_CE_GO_MAP_REQUEST_BUFFER, PRIMARY_SET, IAV_F_WINDOW, check_ce_request, NULL },
	{ IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, PRIMARY_SET, IAV_F_WINDOW, check_ce_handshake, NULL },
	{ IAV_IOC_IMG_GET_MISC_INFO, PRIMARY_SET, IAV_F_INLINE, check_misc_info, post_misc_info },
	{ IAV_IOC_APPLY_VIDEO_PROC, PRIMARY_SET, IAV_F_WIRE_AUDITED, check_apply_vproc, NULL },
	{ IAV_IOC_APPLY_WARP_CTRL, PRIMARY_SET, IAV_F_WIRE_AUDITED, check_apply_warp, NULL },
	{ IAV_IOC_APPLY_FRAME_SYNC_PROC, PRIMARY_SET, IAV_F_WIRE_AUDITED, NULL, NULL },
	{ IAV_IOC_APPLY_CANVAS_FRAME_SYNC_PROC, PRIMARY_SET, IAV_F_WIRE_AUDITED, check_apply_canvas_sync, NULL },
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
