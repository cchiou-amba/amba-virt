/*
 * iav_broker.c
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <poll.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <iav_ioctl.h>

#include "iav_broker.h"

#define PAGE_BYTES		4096ull
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

static int op_ioctl(struct iav_broker *b, uint32_t cid, const struct amba_virt_iav_req *q,
		    const uint8_t *arg, struct reply *r)
{
	alignas(16) uint8_t buf[AMBA_VIRT_MAX_MSG];
	const struct iav_policy_entry *e;
	struct iav_session *s;
	struct iav_call call;
	unsigned long value = 0;
	int rc, err = 0, serr;
	unsigned int i;
	size_t out_len = 0;

	e = iav_policy_lookup(q->cmd);
	if (e == NULL)
		return refuse(r, ENOTTY);
	if (!(e->profiles & IAV_PROFILE_BIT(effective_profile(b, cid))))
		return refuse(r, EPERM);

	memset(buf, 0, sizeof(buf));
	switch (e->kind) {
	case IAV_ARG_NONE:
		if (q->arg_len != 0)
			return refuse(r, EMSGSIZE);
		break;
	case IAV_ARG_VALUE:
		if (q->arg_len != 0)
			return refuse(r, EMSGSIZE);
		value = (unsigned long)q->arg_value;
		break;
	case IAV_ARG_PTR:
		if (e->size == 0 || e->size > AMBA_VIRT_IAV_MAX_ARG || e->size > sizeof(buf))
			return refuse(r, EMSGSIZE);
		if ((e->dir & IAV_DIR_W) ? q->arg_len != e->size : q->arg_len != 0)
			return refuse(r, EMSGSIZE);
		if (q->arg_len)
			memcpy(buf, arg, q->arg_len);
		break;
	default:
		return refuse(r, EPROTO);
	}

	memset(&call, 0, sizeof(call));
	call.entry = e;
	call.cid = cid;
	call.arg = buf;
	call.value = value;
	call.block_max_ms = b->cfg.block_max_ms;
	pthread_mutex_lock(&b->lock);
	for (i = 0; i < b->nwindows; i++) {
		if (b->windows[i].id == AMBA_VIRT_IAV_WIN_DEC)
			call.dec_len = b->windows[i].part_len;
		if (b->windows[i].id == AMBA_VIRT_IAV_WIN_OVL)
			call.ovl_len = b->windows[i].part_len;
	}
	pthread_mutex_unlock(&b->lock);
	if (e->check != NULL) {
		int cerr = e->check(&call);

		if (cerr < 0)
			return refuse(r, -cerr);
		value = call.value;
	}

	serr = session_acquire(b, cid, q->handle, &s);
	if (serr)
		return refuse(r, -serr);
	pthread_mutex_lock(&s->lock);
	rc = run_ioctl(b, s, e, buf, value, &err);
	pthread_mutex_unlock(&s->lock);
	session_release(b, s);

	if (rc >= 0 && e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_R)) {
		out_len = e->size;
		if (e->post != NULL) {
			call.arg = buf;
			e->post(&call);
		}
	}
	return reply_build(r, 0, rc < 0 ? -1 : rc, err, 0, 0, buf, out_len);
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

static int op_info(struct iav_broker *b, const struct amba_virt_iav_req *q, struct reply *r)
{
	struct amba_virt_iav_window out[AMBA_VIRT_IAV_MAX_WINDOWS];
	unsigned int i, n;

	if (q->arg_len != 0)
		return refuse(r, EMSGSIZE);
	memset(out, 0, sizeof(out));
	pthread_mutex_lock(&b->lock);
	n = b->nwindows;
	for (i = 0; i < n; i++) {
		out[i].id = b->windows[i].id;
		out[i].base = b->windows[i].base;
		out[i].size = b->windows[i].size;
		out[i].part_len = b->windows[i].part_len;
	}
	pthread_mutex_unlock(&b->lock);
	return reply_build(r, 0, 0, 0, 0, n, out, n * sizeof(out[0]));
}

/*
 * Authorize a mapping the way the real driver's mmap does: the address must be the base of a
 * partition. Here it must be the base of a window, within the partition's length (twice that for
 * a ring), and the protection must be allowed.
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
	for (i = 0; i < b->nwindows; i++) {
		if (b->windows[i].base == q->arg_value) {
			w = &b->windows[i];
			break;
		}
	}
	if (w == NULL) {
		pthread_mutex_unlock(&b->lock);
		return refuse(r, EFAULT);
	}
	/*
	 * The decode window holds the primary guest's JPEG input and decoded output, and the overlay window the
	 * pictures it blends into streams; a consumer has no call that uses either. Only the primary guest (or a
	 * qualification run, where every client acts as the primary) may map them.
	 */
	if ((w->id == AMBA_VIRT_IAV_WIN_DEC || w->id == AMBA_VIRT_IAV_WIN_OVL) &&
	    effective_profile(b, cid) == IAV_PROFILE_CONSUMER) {
		pthread_mutex_unlock(&b->lock);
		return refuse(r, EPERM);
	}
	limit = w->ring ? 2 * w->part_len : w->part_len;
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
		ret = op_info(b, &q, &r);
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
