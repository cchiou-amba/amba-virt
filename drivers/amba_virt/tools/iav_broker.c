/*
 * iav_broker.c
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <poll.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <iav_ioctl.h>

#include "iav_broker.h"

#define PAGE_BYTES		4096ull
#define AUX_ALIGN_BYTES		0x10000ull	/* AMBA_VIRT_IAV_AUX_ALIGN of amba_virt_iav_win.h: an aperture entry's offset */
#define HANDLE_INDEX_MASK	0xffffull
#define POLL_ALLOWED_EVENTS	(POLLIN | POLLRDNORM | POLLPRI | POLLOUT | POLLWRNORM)

/* The request, and the buffer a response is built in. */
struct reply {
	uint8_t *buf;
	size_t cap;
	size_t *len;
};

static int is_pow2(uint64_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

/* Build a response. out may be NULL when out_len is 0. Returns 0, or -1 if it does not fit. */
static int reply_build(struct reply *r, int32_t status, int32_t rc, int32_t err, uint64_t handle,
		       uint64_t value, const void *out, size_t out_len)
{
	struct amba_virt_iav_resp resp;

	if (r->cap < sizeof(resp) || out_len > r->cap - sizeof(resp))
		return -1;
	memset(&resp, 0, sizeof(resp));
	resp.proto = AMBA_VIRT_IAV_PROTO;
	resp.status = status;
	resp.rc = rc;
	resp.err = err;
	resp.handle = handle;
	resp.value = value;
	resp.out_len = (uint32_t)out_len;
	memcpy(r->buf, &resp, sizeof(resp));
	if (out_len)
		memcpy(r->buf + sizeof(resp), out, out_len);
	*r->len = sizeof(resp) + out_len;
	return 0;
}

static int refuse(struct reply *r, int err)
{
	return reply_build(r, -err, -1, 0, 0, 0, NULL, 0);
}

/* ---- setup ---- */

int iav_broker_init(struct iav_broker *b, const struct iav_broker_ops *ops,
		    const struct iav_broker_cfg *cfg)
{
	unsigned int i;

	if (b == NULL || ops == NULL || cfg == NULL || ops->open == NULL || ops->close == NULL ||
	    ops->ioctl == NULL || ops->poll == NULL || ops->sleep_ms == NULL ||
	    ops->random32 == NULL || ops->now_ms == NULL)
		return -EINVAL;
	if (cfg->profile != IAV_PROFILE_CONSUMER && cfg->profile != IAV_PROFILE_QUALIFICATION)
		return -EINVAL;
	if (iav_policy_init() != 0)
		return -EPROTO;

	memset(b, 0, sizeof(*b));
	b->ops = *ops;
	b->cfg = *cfg;
	if (b->cfg.max_sessions_per_cid == 0)
		b->cfg.max_sessions_per_cid = IAV_BROKER_DEFAULT_PER_CID;
	if (b->cfg.block_max_ms == 0)
		b->cfg.block_max_ms = IAV_BROKER_DEFAULT_BLOCK_MS;
	if (b->cfg.rate_per_sec && b->cfg.rate_burst == 0)
		b->cfg.rate_burst = b->cfg.rate_per_sec;
	pthread_mutex_init(&b->lock, NULL);
	b->table_hash = iav_broker_table_hash(&b->table_rows);
	b->epoch = b->ops.random32(b->ops.ctx);
	if (b->epoch == 0)
		b->epoch = 1;
	for (i = 0; i < IAV_BROKER_MAX_SESSIONS; i++) {
		pthread_mutex_init(&b->sess[i].lock, NULL);
		b->sess[i].fd = -1;
		b->sess[i].gen = 1;
	}
	return 0;
}

void iav_broker_set_guest_pool(struct iav_broker *b, const struct virt_iav_guest_pool *pool)
{
	if (b == NULL || pool == NULL)
		return;
	pthread_mutex_lock(&b->lock);
	b->guest_pool = *pool;
	b->guest_pool_set = 1;
	pthread_mutex_unlock(&b->lock);
}

void iav_broker_set_host_features(struct iav_broker *b, uint64_t features)
{
	if (b == NULL)
		return;
	pthread_mutex_lock(&b->lock);
	b->host_features = features & AMBA_VIRT_IAV_F_NC_WINDOWS;
	pthread_mutex_unlock(&b->lock);
}

int iav_broker_set_aux_sources(struct iav_broker *b, const struct iav_broker_aux_entry *src, unsigned int n)
{
	unsigned int i;

	if (b == NULL || n > IAV_BROKER_AUX_MAX || (n && src == NULL))
		return -EINVAL;
	for (i = 0; i < n; i++)
		if (src[i].len == 0 || src[i].base % PAGE_BYTES || src[i].len % PAGE_BYTES ||
		    src[i].base + src[i].len < src[i].base)
			return -EINVAL;
	pthread_mutex_lock(&b->lock);
	memset(b->aux_src, 0, sizeof(b->aux_src));
	if (n)
		memcpy(b->aux_src, src, n * sizeof(*src));
	b->naux_src = n;
	pthread_mutex_unlock(&b->lock);
	return 0;
}

uint64_t iav_broker_table_hash(uint32_t *rows)
{
	uint64_t h = IAV_TABLE_HASH_INIT;
	size_t i, n = iav_policy_count();

	for (i = 0; i < n; i++) {
		const struct iav_policy_entry *e = iav_policy_at(i);

		h = iav_table_hash_u32(h, e->cmd);
		h = iav_table_hash_u32(h, (unsigned int)e->kind);
		h = iav_table_hash_u32(h, (unsigned int)e->dir);
		h = iav_table_hash_u32(h, (unsigned int)e->size);
		h = iav_table_hash_u32(h, (unsigned int)e->wire);
	}
	if (rows)
		*rows = (uint32_t)n;
	return h;
}

void iav_broker_destroy(struct iav_broker *b)
{
	unsigned int i;

	for (i = 0; i < IAV_BROKER_MAX_SESSIONS; i++) {
		if (b->sess[i].in_use && b->sess[i].fd >= 0)
			b->ops.close(b->ops.ctx, b->sess[i].fd);
		pthread_mutex_destroy(&b->sess[i].lock);
	}
	pthread_mutex_destroy(&b->lock);
}

int iav_broker_set_windows(struct iav_broker *b, const struct iav_broker_window *w, unsigned int n)
{
	unsigned int i, j;

	if (n > AMBA_VIRT_IAV_MAX_WINDOWS || (n && w == NULL))
		return -EINVAL;
	for (i = 0; i < n; i++) {
		if (w[i].id >= AMBA_VIRT_IAV_MAX_WINDOWS || !is_pow2(w[i].size) ||
		    w[i].size % PAGE_BYTES || w[i].base % PAGE_BYTES || w[i].part_len == 0 ||
		    w[i].part_len > w[i].size || w[i].base + w[i].size < w[i].base)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			if (w[j].id == w[i].id)
				return -EINVAL;
			if (w[i].base < w[j].base + w[j].size && w[j].base < w[i].base + w[i].size)
				return -EINVAL;
		}
	}
	pthread_mutex_lock(&b->lock);
	memset(b->windows, 0, sizeof(b->windows));
	memcpy(b->windows, w, n * sizeof(*w));
	b->nwindows = n;
	pthread_mutex_unlock(&b->lock);
	return 0;
}

/* ---- per-CID state: quota and rate ---- */

/* Called with b->lock held. Returns the state for cid, creating it, or NULL if the table is full. */
static struct iav_cid_state *cid_state(struct iav_broker *b, uint32_t cid)
{
	struct iav_cid_state *free_slot = NULL;
	unsigned int i;

	for (i = 0; i < IAV_BROKER_MAX_CIDS; i++) {
		if (b->cids[i].used && b->cids[i].cid == cid)
			return &b->cids[i];
		if (!b->cids[i].used && free_slot == NULL)
			free_slot = &b->cids[i];
	}
	if (free_slot == NULL) {
		/*
		 * The table is full. Guests come and go with new ids, so reuse the entry of the CID
		 * that has been idle longest and holds no session. CIDs are reported by the kernel, so
		 * a guest cannot make up ids to push others out.
		 */
		for (i = 0; i < IAV_BROKER_MAX_CIDS; i++) {
			if (b->cids[i].sessions == 0 &&
			    (free_slot == NULL || b->cids[i].last_ms < free_slot->last_ms))
				free_slot = &b->cids[i];
		}
		if (free_slot == NULL)
			return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = 1;
	free_slot->cid = cid;
	free_slot->tokens = (double)b->cfg.rate_burst;
	free_slot->last_ms = b->ops.now_ms(b->ops.ctx);
	return free_slot;
}

/* Called with b->lock held. Returns 0 if the request may go on, -EAGAIN if rate limited. */
static int rate_check(struct iav_broker *b, struct iav_cid_state *cs)
{
	uint64_t now;
	double add;

	if (b->cfg.rate_per_sec == 0)
		return 0;
	now = b->ops.now_ms(b->ops.ctx);
	if (now > cs->last_ms) {
		add = (double)(now - cs->last_ms) * (double)b->cfg.rate_per_sec / 1000.0;
		cs->tokens += add;
		if (cs->tokens > (double)b->cfg.rate_burst)
			cs->tokens = (double)b->cfg.rate_burst;
		cs->last_ms = now;
	}
	if (cs->tokens < 1.0)
		return -EAGAIN;
	cs->tokens -= 1.0;
	return 0;
}

/* ---- sessions and handles ---- */

static uint64_t make_handle(const struct iav_broker *b, unsigned int index, uint16_t gen)
{
	return ((uint64_t)b->epoch << 32) | ((uint64_t)gen << 16) | (uint64_t)(index + 1u);
}

/*
 * Find the session a handle names for cid and take a reference. A handle that is not a handle
 * is -EBADF. One from before a broker restart, one that was closed, or one whose slot was reused
 * is -ESTALE. One that belongs to another guest is -EACCES.
 */
static int session_acquire(struct iav_broker *b, uint32_t cid, uint64_t handle,
			   struct iav_session **out)
{
	struct iav_session *s;
	unsigned int idx = (unsigned int)(handle & HANDLE_INDEX_MASK);
	uint16_t gen = (uint16_t)((handle >> 16) & 0xffffu);
	uint32_t epoch = (uint32_t)(handle >> 32);
	int err = 0;

	if (handle == 0 || idx == 0 || idx > IAV_BROKER_MAX_SESSIONS)
		return -EBADF;
	pthread_mutex_lock(&b->lock);
	s = &b->sess[idx - 1];
	if (epoch != b->epoch || !s->in_use || s->closing || s->gen != gen)
		err = -ESTALE;
	else if (s->cid != cid)
		err = -EACCES;
	else {
		s->refs++;
		s->last_used_ms = b->ops.now_ms(b->ops.ctx);
		*out = s;
	}
	pthread_mutex_unlock(&b->lock);
	return err;
}

/* Called with b->lock held: the slot is free. Returns the fd to close. */
static int session_free_locked(struct iav_broker *b, struct iav_session *s)
{
	int fd = s->fd;
	struct iav_cid_state *cs;

	cs = cid_state(b, s->cid);
	if (cs != NULL && cs->sessions > 0)
		cs->sessions--;
	s->in_use = 0;
	s->closing = 0;
	s->fd = -1;
	s->cid = 0;
	s->gen++;
	if (s->gen == 0)
		s->gen = 1;
	return fd;
}

static void session_release(struct iav_broker *b, struct iav_session *s)
{
	int fd = -1;

	pthread_mutex_lock(&b->lock);
	s->refs--;
	if (s->closing && s->refs == 0)
		fd = session_free_locked(b, s);
	pthread_mutex_unlock(&b->lock);
	if (fd >= 0)
		b->ops.close(b->ops.ctx, fd);
}

/* Mark closing and drop the caller's reference (or none if own_ref is 0). */
static void session_close(struct iav_broker *b, struct iav_session *s, int own_ref)
{
	int fd = -1;

	pthread_mutex_lock(&b->lock);
	s->closing = 1;
	if (own_ref)
		s->refs--;
	if (s->refs == 0)
		fd = session_free_locked(b, s);
	pthread_mutex_unlock(&b->lock);
	if (fd >= 0)
		b->ops.close(b->ops.ctx, fd);
}

void iav_broker_client_gone(struct iav_broker *b, uint32_t cid)
{
	unsigned int i;

	/* What the guest negotiated goes with it: the next guest with this id starts in the legacy protocol. */
	pthread_mutex_lock(&b->lock);
	for (i = 0; i < IAV_BROKER_MAX_CIDS; i++) {
		if (b->cids[i].used && b->cids[i].cid == cid)
			memset(&b->cids[i].hello, 0, sizeof(b->cids[i].hello));
	}
	pthread_mutex_unlock(&b->lock);

	for (i = 0; i < IAV_BROKER_MAX_SESSIONS; i++) {
		struct iav_session *s = &b->sess[i];
		int fd = -1;

		pthread_mutex_lock(&b->lock);
		if (s->in_use && !s->closing && s->cid == cid) {
			s->closing = 1;
			if (s->refs == 0)
				fd = session_free_locked(b, s);
		}
		pthread_mutex_unlock(&b->lock);
		if (fd >= 0)
			b->ops.close(b->ops.ctx, fd);
	}
}

unsigned int iav_broker_reap_idle(struct iav_broker *b, uint64_t idle_ms)
{
	unsigned int i, n = 0;
	uint64_t now = b->ops.now_ms(b->ops.ctx);

	for (i = 0; i < IAV_BROKER_MAX_SESSIONS; i++) {
		struct iav_session *s = &b->sess[i];
		int fd = -1;

		pthread_mutex_lock(&b->lock);
		if (s->in_use && !s->closing && s->refs == 0 && now >= s->last_used_ms &&
		    now - s->last_used_ms >= idle_ms) {
			s->closing = 1;
			fd = session_free_locked(b, s);
			n++;
		}
		pthread_mutex_unlock(&b->lock);
		if (fd >= 0)
			b->ops.close(b->ops.ctx, fd);
	}
	return n;
}

unsigned int iav_broker_list_cids(struct iav_broker *b, uint32_t *out, unsigned int max)
{
	unsigned int i, j, n = 0;

	pthread_mutex_lock(&b->lock);
	for (i = 0; i < IAV_BROKER_MAX_SESSIONS && n < max; i++) {
		int seen = 0;

		if (!b->sess[i].in_use)
			continue;
		for (j = 0; j < n; j++)
			seen = seen || out[j] == b->sess[i].cid;
		if (!seen)
			out[n++] = b->sess[i].cid;
	}
	pthread_mutex_unlock(&b->lock);
	return n;
}

int iav_broker_cid_has_sessions(struct iav_broker *b, uint32_t cid)
{
	return iav_broker_cid_sessions(b, cid) > 0;
}

unsigned int iav_broker_session_count(struct iav_broker *b)
{
	unsigned int i, n = 0;

	pthread_mutex_lock(&b->lock);
	for (i = 0; i < IAV_BROKER_MAX_SESSIONS; i++)
		n += b->sess[i].in_use ? 1u : 0u;
	pthread_mutex_unlock(&b->lock);
	return n;
}

unsigned int iav_broker_cid_sessions(struct iav_broker *b, uint32_t cid)
{
	unsigned int i, n = 0;

	pthread_mutex_lock(&b->lock);
	for (i = 0; i < IAV_BROKER_MAX_SESSIONS; i++)
		n += (b->sess[i].in_use && b->sess[i].cid == cid) ? 1u : 0u;
	pthread_mutex_unlock(&b->lock);
	return n;
}

/* ---- operations ---- */

static int op_open(struct iav_broker *b, uint32_t cid, struct reply *r)
{
	struct iav_cid_state *cs;
	struct iav_session *s = NULL;
	unsigned int i;
	int fd, err = 0;

	pthread_mutex_lock(&b->lock);
	cs = cid_state(b, cid);
	if (cs == NULL) {
		err = EMFILE;
	} else if (cs->sessions >= b->cfg.max_sessions_per_cid) {
		err = EMFILE;
	} else {
		for (i = 0; i < IAV_BROKER_MAX_SESSIONS; i++) {
			if (!b->sess[i].in_use) {
				s = &b->sess[i];
				break;
			}
		}
		if (s == NULL) {
			err = EMFILE;
		} else {
			/* Reserve the slot before opening so the quota holds under concurrency. */
			s->in_use = 1;
			s->closing = 0;
			s->cid = cid;
			s->fd = -1;
			s->refs = 1;
			cs->sessions++;
		}
	}
	pthread_mutex_unlock(&b->lock);
	if (err)
		return refuse(r, err);

	errno = 0;
	fd = b->ops.open(b->ops.ctx);
	if (fd < 0) {
		int open_err = errno ? errno : EIO;

		pthread_mutex_lock(&b->lock);
		s->refs = 0;
		session_free_locked(b, s);
		pthread_mutex_unlock(&b->lock);
		return reply_build(r, 0, -1, open_err, 0, 0, NULL, 0);
	}

	pthread_mutex_lock(&b->lock);
	s->fd = fd;
	s->last_used_ms = b->ops.now_ms(b->ops.ctx);
	s->refs = 0;
	if (s->closing) {
		/* The guest went away while the real open was in progress. */
		int cfd = session_free_locked(b, s);

		pthread_mutex_unlock(&b->lock);
		b->ops.close(b->ops.ctx, cfd);
		return refuse(r, ECONNRESET);
	}
	pthread_mutex_unlock(&b->lock);
	return reply_build(r, 0, 0, 0, make_handle(b, (unsigned int)(s - b->sess), s->gen), 0, NULL, 0);
}

static int op_close(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q,
		    struct reply *r)
{
	struct iav_session *s;
	int err = session_acquire(b, cid, q->handle, &s);

	if (err)
		return refuse(r, -err);
	session_close(b, s, 1);
	return reply_build(r, 0, 0, 0, 0, 0, NULL, 0);
}

/* The profile this guest's calls are checked against: the primary guest gets the primary set, the
 * others the server-wide profile. A server-wide profile is never the primary one. */
static enum iav_profile effective_profile(const struct iav_broker *b, uint32_t cid)
{
	if (b->ops.is_primary != NULL && b->ops.is_primary(b->ops.ctx, cid))
		return IAV_PROFILE_PRIMARY;
	return b->cfg.profile;
}

/* What cid negotiated, copied out under the lock. A guest that never sent HELLO gets all zero. */
static void hello_of(struct iav_broker *b, uint32_t cid, struct iav_broker_hello *out)
{
	unsigned int i;

	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&b->lock);
	for (i = 0; i < IAV_BROKER_MAX_CIDS; i++) {
		if (b->cids[i].used && b->cids[i].cid == cid) {
			*out = b->cids[i].hello;
			break;
		}
	}
	pthread_mutex_unlock(&b->lock);
}

void iav_broker_cid_hello(struct iav_broker *b, uint32_t cid, struct iav_broker_hello *out)
{
	hello_of(b, cid, out);
}

/*
 * The aux aperture of cid, as the server reports it, checked before anything trusts it: at most IAV_BROKER_AUX_MAX
 * entries, each page aligned and non-empty, inside the BAR below its header page, and no two in one place. A report that
 * fails these is treated as no aperture. Returns the entry count (0: none).
 */
static unsigned int aperture_of(struct iav_broker *b, uint32_t cid, struct iav_broker_aperture *ap)
{
	uint32_t i, j;

	memset(ap, 0, sizeof(*ap));
	if (b->ops.aperture_of == NULL || b->ops.aperture_of(b->ops.ctx, cid, ap) != 0)
		goto none;
	if (ap->count == 0 || ap->count > IAV_BROKER_AUX_MAX || !is_pow2(ap->size) || ap->size < 2 * PAGE_BYTES ||
	    ap->epoch == 0 || ap->epoch > 0xffffffffull)
		goto none;
	for (i = 0; i < ap->count; i++) {
		const struct iav_broker_aux_entry *e = &ap->e[i];

		if (e->pid > 0xffu || e->len == 0 || e->len % PAGE_BYTES || e->base % PAGE_BYTES ||
		    e->offset % AUX_ALIGN_BYTES || e->offset > ap->size - PAGE_BYTES ||
		    e->len > ap->size - PAGE_BYTES - e->offset || e->base + e->len < e->base)
			goto none;
		for (j = 0; j < i; j++)
			if (ap->e[j].pid == e->pid ||
			    (e->offset < ap->e[j].offset + ap->e[j].len && ap->e[j].offset < e->offset + e->len))
				goto none;
	}
	return ap->count;
none:
	memset(ap, 0, sizeof(*ap));
	return 0;
}

/* Run one real ioctl. A blocking canvas or pyramid descriptor query is forwarded as the guest
 * sent it: the driver itself bounds that wait (two seconds, then -EAGAIN) and consumes the next
 * frame only on the blocking path, so converting it to polling would change what the app sees. */
static int run_ioctl(struct iav_broker *b, struct iav_session *s, const struct iav_policy_entry *e,
		     void *buf, unsigned long value, int *err)
{
	unsigned long arg = e->kind == IAV_ARG_PTR ? (unsigned long)buf :
			    e->kind == IAV_ARG_VALUE ? value : 0ul;
	int rc;

	errno = 0;
	rc = b->ops.ioctl(b->ops.ctx, s->fd, e->cmd, arg);
	*err = rc < 0 ? errno : 0;
	return rc;
}

/* The read-only query a check may run, on the caller's own real descriptor (the session lock is held). */
struct query_ctx {
	struct iav_broker *b;
	int fd;
};

static int query_on_session(void *qctx, unsigned long cmd, void *arg)
{
	struct query_ctx *q = qctx;

	if (cmd != IAV_IOC_GET_CHAN_CONFIG && cmd != IAV_IOC_GET_SYSTEM_RESOURCE && cmd != IAV_IOC_QUERY_MEMBLOCK)
		return -EPERM;
	errno = 0;
	return q->b->ops.ioctl(q->b->ops.ctx, q->fd, cmd, (unsigned long)arg) < 0 ? -(errno ? errno : EIO) : 0;
}

/*
 * Compare every windowed partition with its window on the descriptor fd. Called with no broker lock held.
 * Returns 1 stale, 0 matching.
 */
static int recheck_on_fd(struct iav_broker *b, int fd)
{
	struct iav_broker_window w[AMBA_VIRT_IAV_MAX_WINDOWS];
	struct virt_iav_guest_pool pool;
	unsigned int i, n;
	int stale = 0, pool_set;

	pthread_mutex_lock(&b->lock);
	n = b->nwindows;
	memcpy(w, b->windows, n * sizeof(w[0]));
	pthread_mutex_unlock(&b->lock);
	for (i = 0; i < n; i++) {
		struct iav_querymem q;

		if (w[i].pid == 0 && w[i].id != AMBA_VIRT_IAV_WIN_DSP)
			continue;	/* a window without a recorded partition is not re-queried */
		memset(&q, 0, sizeof(q));
		q.mid = IAV_MEM_PARTITION;
		q.arg.partition.pid = w[i].pid;
		if (b->ops.ioctl(b->ops.ctx, fd, IAV_IOC_QUERY_MEMBLOCK, (unsigned long)&q) < 0 ||
		    q.arg.partition.mem.addr != w[i].base || q.arg.partition.mem.length != w[i].part_len)
			stale = 1;
	}
	/* The partitions behind the aux apertures (Envelope F) are checked the same way. */
	{
		struct iav_broker_aux_entry src[IAV_BROKER_AUX_MAX];
		unsigned int na;

		pthread_mutex_lock(&b->lock);
		na = b->naux_src;
		memcpy(src, b->aux_src, sizeof(src));
		pthread_mutex_unlock(&b->lock);
		for (i = 0; i < na && i < IAV_BROKER_AUX_MAX; i++) {
			struct iav_querymem q;

			memset(&q, 0, sizeof(q));
			q.mid = IAV_MEM_PARTITION;
			q.arg.partition.pid = src[i].pid;
			if (b->ops.ioctl(b->ops.ctx, fd, IAV_IOC_QUERY_MEMBLOCK, (unsigned long)&q) < 0 ||
			    q.arg.partition.mem.addr != src[i].base || q.arg.partition.mem.length != src[i].len)
				stale = 1;
		}
	}
	/* The pinned pool behind the arenas is checked the same way: an arena offset is an offset into it. */
	pthread_mutex_lock(&b->lock);
	pool_set = b->guest_pool_set;
	pool = b->guest_pool;
	pthread_mutex_unlock(&b->lock);
	if (pool_set) {
		struct iav_querymem q;

		memset(&q, 0, sizeof(q));
		q.mid = IAV_MEM_PARTITION;
		q.arg.partition.pid = pool.partition_id;
		if (b->ops.ioctl(b->ops.ctx, fd, IAV_IOC_QUERY_MEMBLOCK, (unsigned long)&q) < 0 ||
		    q.arg.partition.mem.addr != pool.parent_base || q.arg.partition.mem.length != pool.parent_bytes)
			stale = 1;
	}
	pthread_mutex_lock(&b->lock);
	b->n_rechecks++;
	b->windows_stale = stale;
	pthread_mutex_unlock(&b->lock);
	return stale;
}

int iav_broker_recheck_windows(struct iav_broker *b)
{
	int fd, stale;

	errno = 0;
	fd = b->ops.open(b->ops.ctx);
	if (fd < 0)
		return -(errno ? errno : EIO);
	stale = recheck_on_fd(b, fd);
	b->ops.close(b->ops.ctx, fd);
	return stale;
}

int iav_broker_windows_stale(struct iav_broker *b)
{
	int stale;

	pthread_mutex_lock(&b->lock);
	stale = b->windows_stale;
	pthread_mutex_unlock(&b->lock);
	return stale;
}

/*
 * The size an ioctl argument has on the wire for this guest: the wire size when the guest negotiated it and the
 * row has one, else the size the header names.
 */
static size_t wire_size_for(const struct iav_policy_entry *e, const struct iav_broker_hello *h)
{
	if (e->wire && h->done && (h->features & AMBA_VIRT_IAV_F_SIZE_OVERRIDE))
		return e->wire;
	return e->size;
}

/* ---- the arena (OP_ARENA): virtual anonymous partitions in the caller's own slice ---- */

/* An OP_ARENA call after admission: where the caller's slice is and the blocks it declared. */
struct arena_xlat {
	uint32_t pid;			/* the pinned pool partition */
	uint32_t index;			/* the caller's slice */
	uint64_t slice_off;		/* its offset in the pool */
	uint32_t count;
	struct amba_virt_iav_arena_blk blk[AMBA_VIRT_IAV_ARENA_MAX_BLKS];
};

#define ANON_TAG	0xffu

/* The bare-metal anonymous id format ((n << 8) | 0xff, n > 0), as the guest module issues them. */
static int is_anon_id(uint32_t id)
{
	return (id & 0xffu) == ANON_TAG && (id >> 8) != 0;
}

static int arena_cmd(uint32_t cmd)
{
	return cmd == IAV_IOC_SET_IMG_SCALE || cmd == IAV_IOC_GDMA_COPY || cmd == IAV_IOC_SYNC_CACHE_MEM_PART;
}

/* The block a field names, or -1. */
static int arena_blk_of(const struct arena_xlat *x, uint32_t vid)
{
	uint32_t k;

	for (k = 0; k < x->count; k++)
		if (x->blk[k].vid == vid)
			return (int)k;
	return -1;
}

/*
 * Rewrite one buffer that names a block: the pid becomes the pool partition and each offset moves into the caller's
 * slice. Every offset must lie inside its block before it moves (so the sum cannot wrap); the full extent of what the
 * DSP or the GDMA engine touches is checked by the policy against the slot this sets. Returns 0, or a negative errno.
 */
static int arena_field(const struct arena_xlat *x, struct iav_call *call, unsigned int slot, uint32_t *pid,
		       unsigned long *off1, unsigned long *off2, int *used)
{
	int k = arena_blk_of(x, *pid);
	uint64_t base;

	if (k < 0)
		return -EINVAL;		/* an anonymous id the call did not declare: unknown, as the driver says */
	if (*off1 > x->blk[k].length || (off2 && *off2 > x->blk[k].length))
		return -EINVAL;
	base = x->slice_off + x->blk[k].offset;
	*pid = x->pid;
	*off1 += base;
	if (off2)
		*off2 += base;
	call->arena[slot].valid = 1;
	call->arena[slot].start = base;
	call->arena[slot].len = x->blk[k].length;
	used[k]++;
	return 0;
}

static int scale_has_uv(const struct iav_img_scale *s, int output)
{
	int fmt;

	memcpy(&fmt, output && s->output_format_enable ? (const void *)&s->output_format : (const void *)&s->format,
	       sizeof(fmt));
	return fmt != IAV_YUV_FORMAT_YUV400;
}

/* Translate the fields of the call in buf that name declared blocks. Returns 0 or a negative errno. */
static int arena_translate(const struct arena_xlat *x, uint32_t cmd, uint8_t *buf, struct iav_call *call)
{
	int used[AMBA_VIRT_IAV_ARENA_MAX_BLKS] = { 0, 0 };
	int rc = 0;
	uint32_t k;

	call->arena_pid = x->pid;
	if (cmd == IAV_IOC_SET_IMG_SCALE) {
		struct iav_img_scale *s = (struct iav_img_scale *)buf;
		struct iav_scale_buf_info *b[2] = { &s->input_buf, &s->output_buf };
		unsigned int i;

		for (i = 0; i < 2 && rc == 0; i++) {
			uint32_t pid = b[i]->pid;
			unsigned long y = b[i]->y_offset, uv = b[i]->uv_offset;

			if (b[i]->use_phys || b[i]->use_dma_buf_fd || !is_anon_id(pid))
				continue;
			rc = arena_field(x, call, i, &pid, &y, scale_has_uv(s, (int)i) ? &uv : NULL, used);
			if (rc == 0) {
				b[i]->pid = pid;
				b[i]->y_offset = y;
				b[i]->uv_offset = uv;
			}
		}
	} else if (cmd == IAV_IOC_GDMA_COPY) {
		struct iav_gdma_copy *g = (struct iav_gdma_copy *)buf;

		if (!g->src_use_phys && !g->src_use_dma_buf_fd && is_anon_id(g->src_mmap_type)) {
			uint32_t pid = g->src_mmap_type;
			unsigned long off = g->src_offset;

			rc = arena_field(x, call, 0, &pid, &off, NULL, used);
			if (rc == 0) {
				g->src_mmap_type = pid;
				g->src_offset = off;
			}
		}
		if (rc == 0 && !g->dst_use_phys && !g->dst_use_dma_buf_fd && is_anon_id(g->dst_mmap_type)) {
			uint32_t pid = g->dst_mmap_type;
			unsigned long off = g->dst_offset;

			rc = arena_field(x, call, 1, &pid, &off, NULL, used);
			if (rc == 0) {
				g->dst_mmap_type = pid;
				g->dst_offset = off;
			}
		}
	} else if (cmd == IAV_IOC_SYNC_CACHE_MEM_PART) {
		struct iav_sync_cache_mem_part *c = (struct iav_sync_cache_mem_part *)buf;

		if (is_anon_id(c->pid)) {
			uint32_t pid = c->pid;
			unsigned long off = c->offset;

			rc = arena_field(x, call, 0, &pid, &off, NULL, used);
			if (rc == 0) {
				c->pid = pid;
				c->offset = off;
			}
		}
	} else {
		rc = -EPERM;
	}
	/* Every declared block is named by the call: a declaration that names nothing is malformed. */
	for (k = 0; rc == 0 && k < x->count; k++)
		if (used[k] == 0)
			rc = -EINVAL;
	return rc;
}

/* Put the guest's own ids and offsets back into an output the driver wrote (SET_IMG_SCALE is read back). */
static void arena_restore(uint32_t cmd, uint8_t *buf, const uint8_t *orig)
{
	if (cmd == IAV_IOC_SET_IMG_SCALE) {
		struct iav_img_scale *s = (struct iav_img_scale *)buf;
		const struct iav_img_scale *o = (const struct iav_img_scale *)orig;

		s->input_buf.pid = o->input_buf.pid;
		s->input_buf.y_offset = o->input_buf.y_offset;
		s->input_buf.uv_offset = o->input_buf.uv_offset;
		s->output_buf.pid = o->output_buf.pid;
		s->output_buf.y_offset = o->output_buf.y_offset;
		s->output_buf.uv_offset = o->output_buf.uv_offset;
	}
}

/*
 * One ioctl, from OP_IOCTL or (after admission) OP_SLICE. arg and arg_len are the guest's argument bytes. The
 * entry decides the profile, the argument shape and the check; the session is taken before the check so a check
 * may query the driver on the caller's own descriptor.
 */
static int do_ioctl(struct iav_broker *b, uint32_t cid, uint64_t handle, uint32_t cmd, uint64_t arg_value,
		    const uint8_t *arg, uint32_t arg_len, const struct arena_xlat *x, struct reply *r)
{
	alignas(16) uint8_t buf[AMBA_VIRT_MAX_MSG];
	alignas(16) uint8_t orig[AMBA_VIRT_MAX_MSG];
	uint32_t window = 0;
	int arena_on = 0;
	const struct iav_policy_entry *e;
	struct iav_broker_hello hello;
	struct iav_session *s;
	struct iav_call call;
	struct query_ctx qc;
	unsigned long value = 0;
	int rc, err = 0, serr, stale, cerr = 0;
	unsigned int i;
	size_t out_len = 0, size, inline_len = 0;
	struct iav_broker_aperture ap;

	e = iav_policy_lookup(cmd);
	if (e == NULL)
		return refuse(r, ENOTTY);
	if (!(e->profiles & IAV_PROFILE_BIT(effective_profile(b, cid))))
		return refuse(r, EPERM);

	hello_of(b, cid, &hello);
	size = wire_size_for(e, &hello);
	memset(buf, 0, sizeof(buf));
	switch (e->kind) {
	case IAV_ARG_NONE:
		if (arg_len != 0)
			return refuse(r, EMSGSIZE);
		break;
	case IAV_ARG_VALUE:
		if (arg_len != 0)
			return refuse(r, EMSGSIZE);
		value = (unsigned long)arg_value;
		break;
	case IAV_ARG_PTR:
		if (size == 0 || size > AMBA_VIRT_IAV_MAX_ARG || size > AMBA_VIRT_IAV_MAX_OUT || size > sizeof(buf))
			return refuse(r, EMSGSIZE);
		if (e->flags & IAV_F_INLINE) {
			/* the user pointers travel after the argument (amba_virt_iav.h, AMBA_VIRT_IAV_F_INLINE) */
			if (!hello.done || !(hello.features & AMBA_VIRT_IAV_F_INLINE) || !(e->dir & IAV_DIR_W) || x != NULL)
				return refuse(r, EPERM);
			if (size + sizeof(struct amba_virt_iav_inline_misc) > AMBA_VIRT_IAV_MAX_OUT ||
			    arg_len != size + sizeof(struct amba_virt_iav_inline_misc))
				return refuse(r, EMSGSIZE);
			inline_len = sizeof(struct amba_virt_iav_inline_misc);
			memcpy(buf, arg, arg_len);
			break;
		}
		if ((e->dir & IAV_DIR_W) ? arg_len != size : arg_len != 0)
			return refuse(r, EMSGSIZE);
		if (arg_len)
			memcpy(buf, arg, arg_len);
		/*
		 * Fields past the header size that nobody audited do not reach the driver: it sees what it saw before
		 * the wire size existed (the header's bytes, then zeros).
		 */
		if (size > e->size && !(e->flags & IAV_F_WIRE_AUDITED))
			memset(buf + e->size, 0, size - e->size);
		break;
	default:
		return refuse(r, EPROTO);
	}

	pthread_mutex_lock(&b->lock);
	stale = b->windows_stale;
	if (stale && (e->flags & IAV_F_WINDOW))
		b->n_stale_refused++;
	pthread_mutex_unlock(&b->lock);
	if (stale && (e->flags & IAV_F_WINDOW))
		return refuse(r, EIO);

	memset(&call, 0, sizeof(call));
	call.entry = e;
	call.cid = cid;
	call.arg = buf;
	call.value = value;
	call.block_max_ms = b->cfg.block_max_ms;
	if (inline_len)
		call.inline_buf = buf + size;
	/* The caller's own aux aperture entries: the only partitions beyond the legacy windows a call may name. */
	if (e->flags & IAV_F_WINDOW) {
		call.naux = aperture_of(b, cid, &ap);
		for (i = 0; i < call.naux && i < IAV_CALL_AUX_MAX; i++) {
			call.aux[i].pid = ap.e[i].pid;
			call.aux[i].len = ap.e[i].len;
		}
	}
	if (x != NULL) {
		int xerr;

		if (e->kind != IAV_ARG_PTR || !(e->dir & IAV_DIR_W) || size > sizeof(orig))
			return refuse(r, EPERM);
		memcpy(orig, buf, size);
		xerr = arena_translate(x, cmd, buf, &call);
		if (xerr < 0) {
			pthread_mutex_lock(&b->lock);
			b->n_arena_refused++;
			pthread_mutex_unlock(&b->lock);
			return refuse(r, -xerr);
		}
	}
	pthread_mutex_lock(&b->lock);
	for (i = 0; i < b->nwindows; i++) {
		if (b->windows[i].id == AMBA_VIRT_IAV_WIN_DEC)
			call.dec_len = b->windows[i].part_len;
		if (b->windows[i].id == AMBA_VIRT_IAV_WIN_OVL)
			call.ovl_len = b->windows[i].part_len;
		if (b->windows[i].id == AMBA_VIRT_IAV_WIN_DSP)
			call.dsp_len = b->windows[i].part_len;
		if (b->windows[i].id == AMBA_VIRT_IAV_WIN_BSB)
			call.bsb_len = b->windows[i].part_len;
	}
	pthread_mutex_unlock(&b->lock);

	serr = session_acquire(b, cid, handle, &s);
	if (serr)
		return refuse(r, -serr);
	pthread_mutex_lock(&s->lock);
	if (e->check != NULL) {
		qc.b = b;
		qc.fd = s->fd;
		call.query = query_on_session;
		call.qctx = &qc;
		call.read_dec = b->ops.read_dec;
		call.rctx = b->ops.ctx;
		cerr = e->check(&call);
		value = call.value;
	}
	if (cerr < 0) {
		pthread_mutex_unlock(&s->lock);
		session_release(b, s);
		if (call.pinned) {
			pthread_mutex_lock(&b->lock);
			b->n_pinned++;
			pthread_mutex_unlock(&b->lock);
		}
		return refuse(r, -cerr);
	}
	if (x != NULL && b->ops.arena_begin != NULL) {
		/*
		 * The job names the caller's slice: count it in flight, so a release of the slice waits for it, and make
		 * sure the slice is still this guest's (a released slice is quarantined and refuses new jobs).
		 */
		if (b->ops.arena_begin(b->ops.ctx, x->index) != 0) {
			pthread_mutex_unlock(&s->lock);
			session_release(b, s);
			return refuse(r, EPERM);
		}
		arena_on = 1;
		if (b->ops.slice_of == NULL || b->ops.slice_of(b->ops.ctx, cid, &window) != 0 || window != x->index) {
			b->ops.arena_end(b->ops.ctx, x->index);
			pthread_mutex_unlock(&s->lock);
			session_release(b, s);
			return refuse(r, EPERM);
		}
	}
	rc = run_ioctl(b, s, e, buf, value, &err);
	if (arena_on)
		b->ops.arena_end(b->ops.ctx, x->index);
	if (x != NULL) {
		pthread_mutex_lock(&b->lock);
		b->n_arena++;
		pthread_mutex_unlock(&b->lock);
	}
	/*
	 * Preview allocates the decode and CE partitions: after it, every windowed partition must still be where
	 * its window points, or window-backed calls stop until it is again.
	 */
	if (rc >= 0 && e->cmd == IAV_IOC_ENABLE_PREVIEW)
		recheck_on_fd(b, s->fd);
	pthread_mutex_unlock(&s->lock);
	session_release(b, s);

	if (rc >= 0 && e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_R)) {
		out_len = size + inline_len;
		if (size > e->size && !(e->flags & IAV_F_WIRE_AUDITED))
			memset(buf + e->size, 0, size - e->size);
		if (e->post != NULL) {
			call.arg = buf;
			e->post(&call);
		}
		if (x != NULL)
			arena_restore(cmd, buf, orig);
	}
	return reply_build(r, 0, rc < 0 ? -1 : rc, err, 0, 0, buf, out_len);
}

static int op_ioctl(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q,
		    const uint8_t *arg, struct reply *r)
{
	return do_ioctl(b, cid, q->handle, q->cmd, q->arg_value, arg, q->arg_len, NULL, r);
}

/*
 * An ioctl on a private buffer: the slice request is admitted against the caller's own slice, then the ioctl
 * (the bytes after the slice request) goes through the allow-list exactly as OP_IOCTL. No address is returned.
 */
static int op_slice(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q,
		    const uint8_t *arg, struct reply *r)
{
	struct amba_virt_iav_slice_req in;
	struct virt_iav_guest_pool pool;
	struct virt_iav_guest_req req;
	struct virt_iav_guest_dma dma;
	uint32_t window = 0;
	int set;

	if (q->arg_len < sizeof(in))
		return refuse(r, EMSGSIZE);
	memcpy(&in, arg, sizeof(in));
	pthread_mutex_lock(&b->lock);
	set = b->guest_pool_set;
	pool = b->guest_pool;
	pthread_mutex_unlock(&b->lock);
	if (!set || b->ops.slice_of == NULL || b->ops.slice_of(b->ops.ctx, cid, &window) != 0)
		return refuse(r, EPERM);

	memset(&req, 0, sizeof(req));
	req.caller_slice = window;
	req.claimed_slice = in.claimed_slice;
	req.partition_id = in.partition_id;
	req.offset = in.offset;
	req.length = in.length;
	req.phys = in.phys;
	memset(&dma, 0, sizeof(dma));
	if (virt_iav_guest_slice_admit(&pool, &req, &dma) != 0)
		return refuse(r, EPERM);
	return do_ioctl(b, cid, q->handle, q->cmd, q->arg_value, arg + sizeof(in),
			q->arg_len - (uint32_t)sizeof(in), NULL, r);
}

/* The caller's slice and the pool, for the arena: 0, or -EPERM when the caller holds no slice. */
static int arena_slice(struct iav_broker *b, uint32_t cid, struct virt_iav_guest_pool *pool, uint32_t *index)
{
	int set;

	pthread_mutex_lock(&b->lock);
	set = b->guest_pool_set;
	*pool = b->guest_pool;
	pthread_mutex_unlock(&b->lock);
	if (!set || pool->slice_bytes <= PAGE_BYTES || pool->slice_count == 0 || pool->partition_id == 0)
		return -EPERM;
	if (b->ops.slice_of == NULL || b->ops.slice_of(b->ops.ctx, cid, index) != 0 || *index >= pool->slice_count)
		return -EPERM;
	return 0;
}

/*
 * Admit the arena header: every block inside the caller's arena (its slice less the claim page), aligned, non-empty,
 * distinct and not overlapping, unused slots zero. Out of the slice is -EPERM (the confinement), malformed -EINVAL.
 */
static int arena_admit(const struct amba_virt_iav_arena_req *h, uint64_t usable)
{
	uint32_t k, j;

	if (h->reserved != 0 || h->count == 0 || h->count > AMBA_VIRT_IAV_ARENA_MAX_BLKS)
		return -EINVAL;
	for (k = 0; k < AMBA_VIRT_IAV_ARENA_MAX_BLKS; k++) {
		const struct amba_virt_iav_arena_blk *bk = &h->blk[k];

		if (k >= h->count) {
			if (bk->vid || bk->reserved || bk->offset || bk->length)
				return -EINVAL;
			continue;
		}
		if (bk->reserved != 0 || !is_anon_id(bk->vid))
			return -EINVAL;
		if (bk->length == 0 || bk->offset % AMBA_VIRT_IAV_ARENA_ALIGN || bk->length % PAGE_BYTES)
			return -EINVAL;
		if (bk->offset >= usable || bk->length > usable - bk->offset)
			return -EPERM;
		for (j = 0; j < k; j++) {
			const struct amba_virt_iav_arena_blk *o = &h->blk[j];

			if (o->vid == bk->vid)
				return -EINVAL;
			if (bk->offset < o->offset + o->length && o->offset < bk->offset + bk->length)
				return -EINVAL;
		}
	}
	return 0;
}

/*
 * An ioctl that names blocks of the caller's arena (Envelope E of plan_iav_usb_yolo). Only for a guest that negotiated
 * the arena and holds the primary role, only SET_IMG_SCALE, GDMA_COPY and SYNC_CACHE_MEM_PART (the calls that use a
 * buffer for the duration of the call: no configuration that keeps naming the arena afterwards is accepted). The
 * header is admitted against the caller's own slice; the ioctl then goes through the same allow-list, profile, size
 * and checks as OP_IOCTL, with the translated buffers confined to their blocks.
 */
static int op_arena(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q, const uint8_t *arg,
		    struct reply *r)
{
	struct amba_virt_iav_arena_req h;
	struct virt_iav_guest_pool pool;
	struct iav_broker_hello hello;
	struct arena_xlat x;
	uint32_t index = 0;
	int err;

	if (q->arg_len < sizeof(h))
		err = -EMSGSIZE;
	else {
		memcpy(&h, arg, sizeof(h));
		hello_of(b, cid, &hello);
		if (!hello.done || !(hello.features & AMBA_VIRT_IAV_F_ARENA))
			err = -EPERM;
		else if (effective_profile(b, cid) == IAV_PROFILE_CONSUMER || !arena_cmd(q->cmd))
			err = -EPERM;
		else if (arena_slice(b, cid, &pool, &index) != 0)
			err = -EPERM;
		else
			err = arena_admit(&h, pool.slice_bytes - PAGE_BYTES);
	}
	if (err) {
		pthread_mutex_lock(&b->lock);
		b->n_arena_refused++;
		pthread_mutex_unlock(&b->lock);
		return refuse(r, -err);
	}
	memset(&x, 0, sizeof(x));
	x.pid = pool.partition_id;
	x.index = index;
	x.slice_off = (uint64_t)index * pool.slice_bytes;
	x.count = h.count;
	memcpy(x.blk, h.blk, sizeof(x.blk));
	return do_ioctl(b, cid, q->handle, q->cmd, q->arg_value, arg + sizeof(h), q->arg_len - (uint32_t)sizeof(h), &x,
			r);
}

static int op_poll(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q,
		   struct reply *r)
{
	struct iav_session *s;
	int events = (int)(q->cmd & (unsigned int)POLL_ALLOWED_EVENTS);
	int timeout = q->arg_value > AMBA_VIRT_IAV_POLL_SLICE_MS ? (int)AMBA_VIRT_IAV_POLL_SLICE_MS :
		      (int)q->arg_value;
	int revents = 0, n, err = 0, serr;

	if (q->arg_len != 0)
		return refuse(r, EMSGSIZE);
	serr = session_acquire(b, cid, q->handle, &s);
	if (serr)
		return refuse(r, -serr);
	pthread_mutex_lock(&s->lock);
	errno = 0;
	n = b->ops.poll(b->ops.ctx, s->fd, events, timeout, &revents);
	err = n < 0 ? errno : 0;
	pthread_mutex_unlock(&s->lock);
	session_release(b, s);
	if (n < 0)
		return reply_build(r, 0, -1, err, 0, 0, NULL, 0);
	return reply_build(r, 0, n, 0, 0, (uint64_t)(unsigned int)revents, NULL, 0);
}

/* The decode and overlay windows belong to the primary guest's calls; a consumer has none that use them. */
static int window_primary_only(uint32_t id)
{
	return id == AMBA_VIRT_IAV_WIN_DEC || id == AMBA_VIRT_IAV_WIN_OVL;
}

/*
 * The windows this guest may map. To a guest that sent no HELLO: the legacy windows its role attaches (DSP and
 * BSB for every camera guest, decode and overlay too for the primary), in the legacy entry, so a module built for
 * four windows never sees more. To a negotiated guest with EXT_INFO: the same windows in the extended entry, only
 * up to the window id it declared.
 */
static int op_info(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q, struct reply *r)
{
	uint8_t out[AMBA_VIRT_IAV_MAX_OUT];
	struct iav_broker_hello hello;
	enum iav_profile prof;
	unsigned int i, n = 0;
	size_t len = 0;
	int ext;

	if (q->arg_len != 0)
		return refuse(r, EMSGSIZE);
	hello_of(b, cid, &hello);
	prof = effective_profile(b, cid);
	ext = hello.done && (hello.features & AMBA_VIRT_IAV_F_EXT_INFO);
	memset(out, 0, sizeof(out));
	pthread_mutex_lock(&b->lock);
	for (i = 0; i < b->nwindows; i++) {
		const struct iav_broker_window *w = &b->windows[i];

		if (window_primary_only(w->id) && prof == IAV_PROFILE_CONSUMER)
			continue;
		if (ext) {
			struct amba_virt_iav_entry ent;

			if (w->id > hello.max_window_id || len + sizeof(ent) > sizeof(out))
				continue;
			memset(&ent, 0, sizeof(ent));
			ent.id = w->id;
			ent.flags = AMBA_VIRT_IAV_ENT_RING;
			if (window_primary_only(w->id))
				ent.flags |= AMBA_VIRT_IAV_ENT_PRIMARY_ONLY;
			if (hello.features & AMBA_VIRT_IAV_F_NC_WINDOWS)
				ent.flags |= AMBA_VIRT_IAV_ENT_NORMAL_NC;
			ent.base = w->base;
			ent.size = w->size;
			ent.part_len = w->part_len;
			ent.offset = 0;
			ent.pid = w->pid;
			memcpy(out + len, &ent, sizeof(ent));
			len += sizeof(ent);
		} else {
			struct amba_virt_iav_window lw;

			if (w->id >= AMBA_VIRT_IAV_MAX_WINDOWS || n >= AMBA_VIRT_IAV_MAX_WINDOWS)
				continue;
			memset(&lw, 0, sizeof(lw));
			lw.id = w->id;
			lw.base = w->base;
			lw.size = w->size;
			lw.part_len = w->part_len;
			memcpy(out + len, &lw, sizeof(lw));
			len += sizeof(lw);
		}
		n++;
	}
	pthread_mutex_unlock(&b->lock);
	/* The aux aperture: its descriptor and its entries, to a primary guest that negotiated it (Envelope F). */
	if (ext && (hello.features & AMBA_VIRT_IAV_F_APERTURE) && prof != IAV_PROFILE_CONSUMER) {
		struct iav_broker_aperture ap;
		uint32_t k, na = aperture_of(b, cid, &ap);

		if (na && len + (na + 1) * sizeof(struct amba_virt_iav_entry) <= sizeof(out)) {
			struct amba_virt_iav_entry ent;

			memset(&ent, 0, sizeof(ent));
			ent.id = AMBA_VIRT_IAV_ENT_ID_APERTURE;
			ent.flags = AMBA_VIRT_IAV_ENT_APERTURE | AMBA_VIRT_IAV_ENT_PRIMARY_ONLY;
			ent.size = ap.size;
			ent.offset = ap.size - PAGE_BYTES;
			ent.reserved = (uint32_t)ap.epoch;
			memcpy(out + len, &ent, sizeof(ent));
			len += sizeof(ent);
			n++;
			for (k = 0; k < na; k++) {
				uint64_t next = ap.size - PAGE_BYTES;
				uint32_t j;

				/* the room up to the next entry or the header page */
				for (j = 0; j < na; j++)
					if (ap.e[j].offset > ap.e[k].offset && ap.e[j].offset < next)
						next = ap.e[j].offset;
				memset(&ent, 0, sizeof(ent));
				ent.id = AMBA_VIRT_IAV_ENT_ID_AUX + ap.e[k].pid;
				ent.flags = AMBA_VIRT_IAV_ENT_APERTURE | AMBA_VIRT_IAV_ENT_PRIMARY_ONLY | AMBA_VIRT_IAV_ENT_RING;
				if (hello.features & AMBA_VIRT_IAV_F_NC_WINDOWS)
					ent.flags |= AMBA_VIRT_IAV_ENT_NORMAL_NC;
				ent.base = ap.e[k].base;
				ent.size = next - ap.e[k].offset;
				ent.part_len = ap.e[k].len;
				ent.offset = ap.e[k].offset;
				ent.pid = ap.e[k].pid;
				memcpy(out + len, &ent, sizeof(ent));
				len += sizeof(ent);
				n++;
			}
		}
	}
	/* The arena entry: the caller's own slice, to a primary guest that negotiated the arena. */
	if (ext && (hello.features & AMBA_VIRT_IAV_F_ARENA) && prof != IAV_PROFILE_CONSUMER) {
		struct virt_iav_guest_pool pool;
		uint32_t index = 0;

		if (arena_slice(b, cid, &pool, &index) == 0 && len + sizeof(struct amba_virt_iav_entry) <= sizeof(out)) {
			struct amba_virt_iav_entry ent;

			memset(&ent, 0, sizeof(ent));
			ent.id = AMBA_VIRT_IAV_ENT_ID_ARENA;
			ent.flags = AMBA_VIRT_IAV_ENT_ARENA | AMBA_VIRT_IAV_ENT_PRIMARY_ONLY;
			ent.base = pool.parent_base + (uint64_t)index * pool.slice_bytes;
			ent.size = pool.slice_bytes;
			ent.part_len = pool.slice_bytes - PAGE_BYTES;
			ent.offset = 0;
			ent.pid = pool.partition_id;
			ent.reserved = index;
			memcpy(out + len, &ent, sizeof(ent));
			len += sizeof(ent);
			n++;
		}
	}
	return reply_build(r, 0, 0, 0, 0, n, out, len);
}

#define HELLO_MIN_PAGE	4096u
#define HELLO_MAX_PAGE	65536u
#define BROKER_FEATURES	(AMBA_VIRT_IAV_F_SIZE_OVERRIDE | AMBA_VIRT_IAV_F_EXT_INFO | AMBA_VIRT_IAV_F_INLINE)

/*
 * Negotiate. Every field is a value from the guest: the page size must be a power of two the broker knows, the
 * features are intersected with what this broker and host provide, unknown bits are ignored, and the window limit
 * only ever lowers what OP_INFO reports. A table hash that differs from the broker's is counted, not refused.
 */
static int op_hello(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q, const uint8_t *arg,
		    struct reply *r)
{
	struct amba_virt_iav_hello in;
	struct amba_virt_iav_hello_resp out;
	struct iav_cid_state *cs;
	enum iav_profile prof;
	uint64_t granted;
	uint32_t max_id;
	int arena_ok, aperture_ok;

	if (q->handle != 0 || q->arg_len != sizeof(in))
		return refuse(r, EMSGSIZE);
	memcpy(&in, arg, sizeof(in));
	if (in.revision == 0 || in.page_size < HELLO_MIN_PAGE || in.page_size > HELLO_MAX_PAGE ||
	    !is_pow2(in.page_size))
		return refuse(r, EINVAL);
	prof = effective_profile(b, cid);
	{
		struct virt_iav_guest_pool pool;
		uint32_t index = 0;

		/* The arena: a primary guest with a slice, that also takes the extended OP_INFO (the arena entry). */
		arena_ok = prof != IAV_PROFILE_CONSUMER && (in.features & AMBA_VIRT_IAV_F_EXT_INFO) &&
			   arena_slice(b, cid, &pool, &index) == 0;
	}
	{
		struct iav_broker_aperture ap;

		/* The aperture: a primary guest that takes the extended OP_INFO (its entries) and has one attached. */
		aperture_ok = prof != IAV_PROFILE_CONSUMER && (in.features & AMBA_VIRT_IAV_F_EXT_INFO) &&
			      aperture_of(b, cid, &ap) > 0;
	}
	pthread_mutex_lock(&b->lock);
	granted = in.features & (BROKER_FEATURES | b->host_features | (arena_ok ? AMBA_VIRT_IAV_F_ARENA : 0) |
				 (aperture_ok ? AMBA_VIRT_IAV_F_APERTURE : 0));
	max_id = in.max_window_id < AMBA_VIRT_IAV_MAX_WINDOWS - 1 ? in.max_window_id : AMBA_VIRT_IAV_MAX_WINDOWS - 1;
	cs = cid_state(b, cid);
	if (cs == NULL) {
		pthread_mutex_unlock(&b->lock);
		return refuse(r, EMFILE);
	}
	cs->hello.done = 1;
	cs->hello.revision = in.revision;
	cs->hello.features = granted;
	cs->hello.max_window_id = max_id;
	cs->hello.page_size = in.page_size;
	cs->hello.table_hash = in.table_hash;
	b->n_hello++;
	memset(&out, 0, sizeof(out));
	out.revision = AMBA_VIRT_IAV_REVISION;
	out.role = prof == IAV_PROFILE_PRIMARY ? AMBA_VIRT_IAV_ROLE_PRIMARY :
		   prof == IAV_PROFILE_QUALIFICATION ? AMBA_VIRT_IAV_ROLE_QUALIFY : AMBA_VIRT_IAV_ROLE_CONSUMER;
	out.features = granted;
	out.max_window_id = max_id;
	out.table_rows = b->table_rows;
	out.table_hash = b->table_hash;
	pthread_mutex_unlock(&b->lock);
	return reply_build(r, 0, 0, 0, 0, granted, &out, sizeof(out));
}

/*
 * Authorize a mapping the way the real driver's mmap does: the address must be the base of a
 * partition. Here it must be the base of a window, within twice the partition's length (the driver
 * accepts a mapping up to twice the partition for any partition, so a ring maps it twice in a row),
 * and the protection must be allowed. Nothing maps while the windows are stale.
 */
static int op_map(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q,
		  struct reply *r)
{
	struct amba_virt_iav_map out;
	struct iav_session *s;
	const struct iav_broker_window *w = NULL;
	unsigned int i;
	uint64_t length = q->arg2, limit;
	int serr;

	if (q->arg_len != 0)
		return refuse(r, EMSGSIZE);
	serr = session_acquire(b, cid, q->handle, &s);
	if (serr)
		return refuse(r, -serr);
	session_release(b, s);

	if (length == 0 || (q->arg_value % PAGE_BYTES) != 0 || (length % PAGE_BYTES) != 0)
		return refuse(r, EINVAL);
	if (q->cmd & ~(unsigned int)(PROT_READ | PROT_WRITE))
		return refuse(r, EPERM);
	if (!(q->cmd & PROT_READ))
		return refuse(r, EPERM);

	pthread_mutex_lock(&b->lock);
	if (b->windows_stale) {
		b->n_stale_refused++;
		pthread_mutex_unlock(&b->lock);
		return refuse(r, EIO);
	}
	for (i = 0; i < b->nwindows; i++) {
		if (b->windows[i].base == q->arg_value) {
			w = &b->windows[i];
			break;
		}
	}
	if (w == NULL) {
		struct iav_broker_hello hello;
		struct iav_broker_aperture ap;
		uint32_t k, na;

		pthread_mutex_unlock(&b->lock);
		/*
		 * An entry of the caller's own aux aperture (Envelope F): only to a non-consumer guest that negotiated it, at the
		 * partition's address, up to twice the entry (G8). The entry is mapped from the aperture BAR at its offset.
		 */
		hello_of(b, cid, &hello);
		if (!hello.done || !(hello.features & AMBA_VIRT_IAV_F_APERTURE) ||
		    effective_profile(b, cid) == IAV_PROFILE_CONSUMER)
			return refuse(r, EFAULT);
		na = aperture_of(b, cid, &ap);
		for (k = 0; k < na; k++) {
			if (ap.e[k].base != q->arg_value)
				continue;
			if (length > 2 * ap.e[k].len)
				return refuse(r, EINVAL);
			memset(&out, 0, sizeof(out));
			out.window = AMBA_VIRT_IAV_ENT_ID_AUX + ap.e[k].pid;
			out.prot = q->cmd;
			out.window_offset = ap.e[k].offset;
			out.length = length;
			pthread_mutex_lock(&b->lock);
			b->n_aux_maps++;
			pthread_mutex_unlock(&b->lock);
			return reply_build(r, 0, 0, 0, 0, out.window, &out, sizeof(out));
		}
		return refuse(r, EFAULT);
	}
	/*
	 * The decode window holds the primary guest's JPEG input and decoded output, and the overlay window the
	 * pictures it blends into streams; a consumer has no call that uses either. Only the primary guest (or a
	 * qualification run, where every client acts as the primary) may map them.
	 */
	if (window_primary_only(w->id) && effective_profile(b, cid) == IAV_PROFILE_CONSUMER) {
		pthread_mutex_unlock(&b->lock);
		return refuse(r, EPERM);
	}
	limit = 2 * w->part_len;
	if (length > limit) {
		pthread_mutex_unlock(&b->lock);
		return refuse(r, EINVAL);
	}
	memset(&out, 0, sizeof(out));
	out.window = w->id;
	out.prot = q->cmd;
	out.window_offset = 0;
	out.length = length;
	pthread_mutex_unlock(&b->lock);
	return reply_build(r, 0, 0, 0, 0, out.window, &out, sizeof(out));
}

int iav_broker_format_status(struct iav_broker *b, char *buf, size_t len)
{
	unsigned int i, negotiated = 0;
	int n;

	if (b == NULL || buf == NULL || len == 0)
		return -1;
	pthread_mutex_lock(&b->lock);
	for (i = 0; i < IAV_BROKER_MAX_CIDS; i++)
		negotiated += (b->cids[i].used && b->cids[i].hello.done) ? 1u : 0u;
	n = snprintf(buf, len, "OK windows=%s revision=%u host_features=0x%llx table_rows=%u table_hash=0x%016llx "
		     "requests=%llu refused=%llu hello=%llu negotiated=%u pinned=%llu stale_refused=%llu rechecks=%llu "
		     "arena=%llu arena_refused=%llu aux_sources=%u aux_maps=%llu\n",
		     b->windows_stale ? "stale" : "pinned", AMBA_VIRT_IAV_REVISION,
		     (unsigned long long)b->host_features, b->table_rows, (unsigned long long)b->table_hash,
		     (unsigned long long)b->n_requests, (unsigned long long)b->n_refused,
		     (unsigned long long)b->n_hello, negotiated, (unsigned long long)b->n_pinned,
		     (unsigned long long)b->n_stale_refused, (unsigned long long)b->n_rechecks,
		     (unsigned long long)b->n_arena, (unsigned long long)b->n_arena_refused, b->naux_src,
		     (unsigned long long)b->n_aux_maps);
	for (i = 0; i < IAV_BROKER_MAX_CIDS && n >= 0 && (size_t)n < len; i++) {
		const struct iav_cid_state *c = &b->cids[i];

		if (!c->used || !c->hello.done)
			continue;
		n += snprintf(buf + n, len - (size_t)n, "GUEST cid=%u revision=%u features=0x%llx max_window_id=%u "
			      "page_size=%u table=%s\n", c->cid, c->hello.revision,
			      (unsigned long long)c->hello.features, c->hello.max_window_id, c->hello.page_size,
			      c->hello.table_hash == b->table_hash ? "same" : "differs");
	}
	pthread_mutex_unlock(&b->lock);
	if (n < 0 || (size_t)n >= len)
		return -1;
	return n;
}

/* ---- entry point ---- */

int iav_broker_handle(struct iav_broker *b, uint32_t cid, const void *req, size_t req_len,
		      void *resp, size_t resp_cap, size_t *resp_len)
{
	struct amba_virt_iav_req q;
	struct reply r;
	struct iav_cid_state *cs;
	int limited = 0;
	int ret;

	r.buf = resp;
	r.cap = resp_cap;
	r.len = resp_len;
	*resp_len = 0;

	pthread_mutex_lock(&b->lock);
	b->n_requests++;
	cs = cid_state(b, cid);
	if (cs == NULL || rate_check(b, cs) != 0)
		limited = 1;
	pthread_mutex_unlock(&b->lock);
	if (limited) {
		ret = refuse(&r, cs == NULL ? EMFILE : EAGAIN);
		goto done;
	}

	if (req == NULL || req_len < sizeof(q)) {
		ret = refuse(&r, EPROTO);
		goto done;
	}
	memcpy(&q, req, sizeof(q));
	if (q.proto != AMBA_VIRT_IAV_PROTO) {
		ret = refuse(&r, EPROTO);
		goto done;
	}
	if (q.arg_len > AMBA_VIRT_IAV_MAX_ARG || q.arg_len != req_len - sizeof(q)) {
		ret = refuse(&r, EMSGSIZE);
		goto done;
	}

	switch (q.op) {
	case AMBA_VIRT_IAV_OP_OPEN:
		ret = q.arg_len ? refuse(&r, EMSGSIZE) : op_open(b, cid, &r);
		break;
	case AMBA_VIRT_IAV_OP_CLOSE:
		ret = q.arg_len ? refuse(&r, EMSGSIZE) : op_close(b, cid, &q, &r);
		break;
	case AMBA_VIRT_IAV_OP_IOCTL:
		ret = op_ioctl(b, cid, &q, (const uint8_t *)req + sizeof(q), &r);
		break;
	case AMBA_VIRT_IAV_OP_MAP:
		ret = op_map(b, cid, &q, &r);
		break;
	case AMBA_VIRT_IAV_OP_POLL:
		ret = op_poll(b, cid, &q, &r);
		break;
	case AMBA_VIRT_IAV_OP_INFO:
		ret = op_info(b, cid, &q, &r);
		break;
	case AMBA_VIRT_IAV_OP_SLICE:
		ret = op_slice(b, cid, &q, (const uint8_t *)req + sizeof(q), &r);
		break;
	case AMBA_VIRT_IAV_OP_HELLO:
		ret = op_hello(b, cid, &q, (const uint8_t *)req + sizeof(q), &r);
		break;
	case AMBA_VIRT_IAV_OP_ARENA:
		ret = op_arena(b, cid, &q, (const uint8_t *)req + sizeof(q), &r);
		break;
	default:
		ret = refuse(&r, ENOSYS);
		break;
	}

done:
	if (ret == 0 && *resp_len >= sizeof(struct amba_virt_iav_resp)) {
		struct amba_virt_iav_resp head;

		memcpy(&head, resp, sizeof(head));
		if (head.status != 0) {
			pthread_mutex_lock(&b->lock);
			b->n_refused++;
			pthread_mutex_unlock(&b->lock);
		}
	}
	return ret;
}
