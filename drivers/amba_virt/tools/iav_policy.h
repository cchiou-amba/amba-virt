/*
 * iav_policy.h
 *
 * The ioctl table of the IAV broker. iav_ioctl_table.inc, generated from the IAV driver sources,
 * lists every ioctl the driver serves on /dev/iav with its argument kind and size. The broker
 * refuses everything that is not on the verified allow-list in iav_policy.c.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAV_POLICY_H
#define IAV_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "amba_virt_iav_table.h"

/* Set by the allow-list: the broker runs the call with bounded blocking. */
#define IAV_F_BOUNDED_DESC	0x100u
/*
 * Set by the allow-list: the call names memory through a window (an offset or a descriptor into a window), so it
 * is refused while the windows no longer match the partitions behind them (window pinning).
 */
#define IAV_F_WINDOW		0x200u
/*
 * Set by the allow-list: the struct the handler really copies (the wire size) was audited field by field, so a
 * negotiated guest's full argument reaches the driver. Without it the bytes past the header size are zeroed in
 * and out, which is exactly what the driver saw before the wire size existed.
 */
#define IAV_F_WIRE_AUDITED	0x400u
/*
 * Set by the allow-list: the argument carries user pointers that the guest module marshals inline after it
 * (amba_virt_iav.h, AMBA_VIRT_IAV_F_INLINE). The broker accepts the call only from a guest that negotiated the inline
 * marshal, hands the check the trailer (struct iav_call.inline_buf), and returns the trailer after the output.
 */
#define IAV_F_INLINE		0x800u

/* The most aperture entries a call can see (struct iav_call.aux). */
#define IAV_CALL_AUX_MAX	16u

enum iav_profile {
	IAV_PROFILE_CONSUMER = 0,	/* frames and queries only */
	IAV_PROFILE_QUALIFICATION = 1,	/* every client may do what the primary client may, for qualification runs */
	IAV_PROFILE_PRIMARY = 2,	/* the one guest that configures and runs the pipeline; never set server-wide */
};
#define IAV_PROFILE_BIT(p)	(1u << (p))

struct iav_call;

struct iav_policy_entry {
	unsigned int cmd;
	const char *name;
	enum iav_magic magic;
	enum iav_arg_kind kind;
	enum iav_dir dir;
	size_t size;				/* the size the ioctl header names */
	size_t wire;				/* what the handler copies when that differs, else 0 */
	unsigned int cls;
	unsigned int flags;
	/* Filled from the allow-list. */
	unsigned int profiles;			/* bit per profile that may call it; 0 = denied */
	int (*check)(struct iav_call *call);	/* may reject (negative errno) or clamp */
	void (*post)(struct iav_call *call);	/* runs on the output after a successful call, may clear fields */
};

/* What a check function can see and change. */
struct iav_call {
	const struct iav_policy_entry *entry;
	uint32_t cid;
	void *arg;			/* the broker's copy of a pointer argument */
	unsigned long value;		/* a value argument */
	unsigned int block_max_ms;	/* the longest a blocking call may wait */
	uint64_t dec_len;		/* length of the decode partition window the guests may map; 0 if there is none */
	uint64_t ovl_len;		/* length of the overlay partition window; 0 if there is none */
	uint64_t dsp_len;		/* length of the DSP partition window; 0 if there is none */
	uint64_t bsb_len;		/* length of the bitstream partition window; 0 if there is none */
	/*
	 * Run a read-only query on the caller's real descriptor (GET_CHAN_CONFIG, GET_SYSTEM_RESOURCE, QUERY_MEMBLOCK),
	 * for checks that compare a request with the driver's current state. NULL: no query is possible and such
	 * checks refuse.
	 */
	int (*query)(void *qctx, unsigned long cmd, void *arg);
	void *qctx;
	/*
	 * Read len bytes of the decode partition at offset off, as the host sees them after the guest's writes (cache
	 * invalidated first). 0 on success. NULL: the host cannot read the window, so a JPEG is not admitted.
	 */
	int (*read_dec)(void *rctx, uint64_t off, void *buf, size_t len);
	void *rctx;
	int pinned;			/* set by a check that refused because the call would move a windowed partition */
	/*
	 * Arena blocks (OP_ARENA, Envelope E of plan_iav_usb_yolo): the broker has rewritten a buffer that named a block of
	 * the caller's arena to arena_pid (the pinned private pool) and an offset inside it. Slot 0 is the first buffer of
	 * the call (SET_IMG_SCALE input_buf, GDMA_COPY source, SYNC_CACHE_MEM_PART), slot 1 the second (output_buf,
	 * destination). A buffer whose pid is arena_pid may use only [start, start + len) of its slot; a buffer that names
	 * arena_pid without a valid slot (OP_IOCTL, or a slot the translation did not set) is refused.
	 */
	uint32_t arena_pid;
	struct {
		int valid;
		uint64_t start;
		uint64_t len;
	} arena[2];
	/*
	 * The entries of the caller's own aux aperture (Envelope F of plan_iav_usb_yolo): the partition and the bytes of it
	 * the guest maps (what QUERY_MEMBLOCK reports). A buffer or table in one of these partitions may use only
	 * [0, len); a partition with no entry here is one the guest holds no mapping of.
	 */
	unsigned int naux;
	struct {
		uint32_t pid;
		uint64_t len;
	} aux[IAV_CALL_AUX_MAX];
	/* The inline trailer of an IAV_F_INLINE call (its argument bytes follow the ioctl argument); NULL otherwise. */
	void *inline_buf;
};

/*
 * Build the table and apply the allow-list. Returns 0, or -1 if the allow-list names an ioctl
 * that is not in the generated table (the driver changed under it). Safe to call repeatedly.
 */
int iav_policy_init(void);

size_t iav_policy_count(void);
const struct iav_policy_entry *iav_policy_at(size_t index);

/* NULL if cmd is not an ioctl of the IAV driver. */
const struct iav_policy_entry *iav_policy_lookup(unsigned int cmd);

/*
 * The JPEGs the DSP decoder is known to decode (observed on the boards, see doc/IavVirtualization.md section 9):
 * baseline sequential (SOF0), 8 bit, three components, Y sampled 2x2 (4:2:0) or 2x1 (4:2:2) and Cb, Cr 1x1; the
 * header holds SOI, quantization tables, the SOF0, the four standard Huffman tables of ITU-T T.81 Annex K (any other
 * table stalled the decoder, on 4:2:0 as well as 4:2:2), at most one APP0 and only if it is JFIF, then SOS, and nothing
 * else (no other APPn, COM or restart interval: an AVI1 APP0, as the AVerMedia camera sends, stalled it); complete
 * within the n bytes given. Anything else is refused (-EINVAL) because a JPEG the decoder does not take stalls it, and
 * a stalled decoder takes the frames after it along; a run of such calls crashed the DSP (observed). Returns 0 or
 * -EINVAL.
 */
#define IAV_JPEG_DHT_N 4
struct iav_jpeg_std_table {
	const uint8_t *t;	/* class/id byte, 16 code-length counts, the values */
	size_t n;
};
extern const struct iav_jpeg_std_table iav_jpeg_std_dht[IAV_JPEG_DHT_N];	/* DC luma, DC chroma, AC luma, AC chroma */
int iav_jpeg_admit(const uint8_t *p, size_t n);

/* The same check, and on success the picture size and whether the sampling is 4:2:2 (else 4:2:0). */
int iav_jpeg_admit_info(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h, int *sub422);

/* The bytes the decoder writes for a picture of w x h (16-aligned both ways; 1.5 or, for 4:2:2, 2 bytes a pixel). */
uint64_t iav_jpeg_decoded_bytes(uint32_t w, uint32_t h, int sub422);

#endif /* IAV_POLICY_H */