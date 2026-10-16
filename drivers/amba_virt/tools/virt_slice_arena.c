/*
 * virt_slice_arena.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "amba_virt_iav_win.h"
#include "virt_slice_arena.h"

struct slot {
	int quarantined;
	unsigned int inflight;
	uint64_t last_end_ms;
	uint64_t released_ms;
	uint64_t scrubs;
	uint64_t scrub_failures;
	int64_t last_scrub_ms;		/* duration of the last scrub, -1 if none */
	int last_scrub_rc;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct slot g_slot[VIRT_SLICE_ARENA_COUNT];
static struct virt_slice_io g_io;
static int g_io_set;
static int g_init;

static void *default_map(void *ctx, uint32_t index, size_t bytes)
{
	char path[64];
	void *p;
	int fd;

	(void)ctx;
	snprintf(path, sizeof(path), "/dev/amba_virt_iav_slice%u", index);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return p == MAP_FAILED ? NULL : p;
}

static void default_unmap(void *ctx, void *p, size_t bytes)
{
	(void)ctx;
	munmap(p, bytes);
}

static uint64_t default_now_ms(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Called with g_lock held. */
static void init_locked(void)
{
	unsigned int i;

	if (g_init)
		return;
	memset(g_slot, 0, sizeof(g_slot));
	for (i = 0; i < VIRT_SLICE_ARENA_COUNT; i++)
		g_slot[i].last_scrub_ms = -1;
	if (!g_io_set) {
		memset(&g_io, 0, sizeof(g_io));
		g_io.map = default_map;
		g_io.unmap = default_unmap;
		g_io.now_ms = default_now_ms;
	}
	g_init = 1;
}

void virt_slice_arena_set_io(const struct virt_slice_io *io)
{
	pthread_mutex_lock(&g_lock);
	if (io) {
		g_io = *io;
		g_io_set = 1;
	} else {
		memset(&g_io, 0, sizeof(g_io));
		g_io.map = default_map;
		g_io.unmap = default_unmap;
		g_io.now_ms = default_now_ms;
		g_io_set = 0;
	}
	pthread_mutex_unlock(&g_lock);
}

void virt_slice_arena_reset(void)
{
	pthread_mutex_lock(&g_lock);
	g_init = 0;
	init_locked();
	pthread_mutex_unlock(&g_lock);
}

uint64_t virt_slice_arena_now_ms(void)
{
	uint64_t now;

	pthread_mutex_lock(&g_lock);
	init_locked();
	now = g_io.now_ms(g_io.ctx);
	pthread_mutex_unlock(&g_lock);
	return now;
}

int virt_slice_arena_begin(uint32_t index)
{
	int rc = 0;

	if (index >= VIRT_SLICE_ARENA_COUNT)
		return -EINVAL;
	pthread_mutex_lock(&g_lock);
	init_locked();
	if (g_slot[index].quarantined)
		rc = -EBUSY;
	else
		g_slot[index].inflight++;
	pthread_mutex_unlock(&g_lock);
	return rc;
}

void virt_slice_arena_end(uint32_t index)
{
	if (index >= VIRT_SLICE_ARENA_COUNT)
		return;
	pthread_mutex_lock(&g_lock);
	init_locked();
	if (g_slot[index].inflight > 0)
		g_slot[index].inflight--;
	g_slot[index].last_end_ms = g_io.now_ms(g_io.ctx);
	pthread_mutex_unlock(&g_lock);
}

void virt_slice_arena_quarantine(uint32_t index, uint64_t released_ms)
{
	if (index >= VIRT_SLICE_ARENA_COUNT)
		return;
	pthread_mutex_lock(&g_lock);
	init_locked();
	g_slot[index].quarantined = 1;
	g_slot[index].released_ms = released_ms;
	pthread_mutex_unlock(&g_lock);
}

int virt_slice_arena_quarantined(uint32_t index)
{
	int q;

	if (index >= VIRT_SLICE_ARENA_COUNT)
		return 0;
	pthread_mutex_lock(&g_lock);
	init_locked();
	q = g_slot[index].quarantined;
	pthread_mutex_unlock(&g_lock);
	return q;
}

int virt_slice_arena_scrub_due(uint32_t index, uint64_t *wait_ms)
{
	uint64_t now, from, until;
	int due = 0;

	if (wait_ms)
		*wait_ms = 0;
	if (index >= VIRT_SLICE_ARENA_COUNT)
		return 0;
	pthread_mutex_lock(&g_lock);
	init_locked();
	if (g_slot[index].quarantined && g_slot[index].inflight == 0) {
		now = g_io.now_ms(g_io.ctx);
		from = g_slot[index].released_ms > g_slot[index].last_end_ms ? g_slot[index].released_ms :
									      g_slot[index].last_end_ms;
		until = from + VIRT_SLICE_QUARANTINE_MS;
		if (now >= until)
			due = 1;
		else if (wait_ms)
			*wait_ms = until - now;
	}
	pthread_mutex_unlock(&g_lock);
	return due;
}

/* Make the zeroes reach memory before the slice is read back or leased: the Dom0 mapping is write-combined. */
static void store_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#else
	__sync_synchronize();
#endif
}

int virt_slice_arena_scrub(uint32_t index)
{
	struct virt_slice_io io;
	const size_t bytes = (size_t)VIRT_SLICE_ARENA_BYTES;
	volatile uint64_t *w;
	uint64_t t0, t1;
	size_t i, n = bytes / sizeof(uint64_t);
	void *p;
	int rc = 0;

	if (index >= VIRT_SLICE_ARENA_COUNT)
		return -EINVAL;
	pthread_mutex_lock(&g_lock);
	init_locked();
	io = g_io;
	pthread_mutex_unlock(&g_lock);
	t0 = io.now_ms(io.ctx);
	errno = 0;
	p = io.map(io.ctx, index, bytes);
	if (p == NULL) {
		rc = errno ? -errno : -EIO;
	} else {
		memset(p, 0, bytes);
		store_barrier();
		w = (volatile uint64_t *)p;
		for (i = 0; i < n; i++) {
			if (w[i] != 0) {
				rc = -EIO;
				break;
			}
		}
		io.unmap(io.ctx, p, bytes);
	}
	t1 = io.now_ms(io.ctx);
	pthread_mutex_lock(&g_lock);
	g_slot[index].last_scrub_rc = rc;
	g_slot[index].last_scrub_ms = (int64_t)(t1 - t0);
	if (rc == 0)
		g_slot[index].scrubs++;
	else
		g_slot[index].scrub_failures++;
	pthread_mutex_unlock(&g_lock);
	return rc;
}

void virt_slice_arena_unquarantine(uint32_t index)
{
	if (index >= VIRT_SLICE_ARENA_COUNT)
		return;
	pthread_mutex_lock(&g_lock);
	init_locked();
	g_slot[index].quarantined = 0;
	pthread_mutex_unlock(&g_lock);
}

int virt_slice_arena_write_header(uint32_t index)
{
	struct virt_slice_io io;
	struct amba_virt_iav_win_hdr hdr;
	const size_t bytes = (size_t)VIRT_SLICE_ARENA_BYTES;
	volatile uint8_t *dst;
	const uint8_t *src = (const uint8_t *)&hdr;
	void *p;
	size_t i;

	if (index >= VIRT_SLICE_ARENA_COUNT)
		return -EINVAL;
	pthread_mutex_lock(&g_lock);
	init_locked();
	io = g_io;
	pthread_mutex_unlock(&g_lock);
	errno = 0;
	p = io.map(io.ctx, index, bytes);
	if (p == NULL)
		return errno ? -errno : -EIO;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = AMBA_VIRT_IAV_WIN_HDR_MAGIC;
	hdr.version = AMBA_VIRT_IAV_WIN_HDR_VERSION;
	hdr.window_id = AMBA_VIRT_IAV_WIN_ID_SLICE + index;
	hdr.part_len = VIRT_SLICE_ARENA_BYTES - VIRT_SLICE_ARENA_PAGE;
	hdr.size = VIRT_SLICE_ARENA_BYTES;
	/* the last AMBA_VIRT_IAV_WIN_HDR_BYTES of the claim page; the rest of the block stays zero */
	dst = (volatile uint8_t *)p + bytes - AMBA_VIRT_IAV_WIN_HDR_BYTES;
	for (i = 0; i < AMBA_VIRT_IAV_WIN_HDR_BYTES; i++)
		dst[i] = i < sizeof(hdr) ? src[i] : 0;
	store_barrier();
	io.unmap(io.ctx, p, bytes);
	return 0;
}

int virt_slice_arena_format(char *buf, size_t len)
{
	unsigned int i;
	int n;

	if (buf == NULL || len == 0)
		return -1;
	pthread_mutex_lock(&g_lock);
	init_locked();
	n = snprintf(buf, len, "SLICES quarantine_ms=%u", VIRT_SLICE_QUARANTINE_MS);
	for (i = 0; i < VIRT_SLICE_ARENA_COUNT && n >= 0 && (size_t)n < len; i++) {
		const struct slot *s = &g_slot[i];

		n += snprintf(buf + n, len - (size_t)n, " %u:%s,inflight=%u,scrubs=%llu,failed=%llu,last_ms=%lld,last_rc=%d", i,
			      s->quarantined ? "quarantined" : "open", s->inflight, (unsigned long long)s->scrubs,
			      (unsigned long long)s->scrub_failures, (long long)s->last_scrub_ms, s->last_scrub_rc);
	}
	if (n >= 0 && (size_t)n + 1 < len) {
		buf[n++] = '\n';
		buf[n] = '\0';
	}
	pthread_mutex_unlock(&g_lock);
	if (n < 0 || (size_t)n >= len)
		return -1;
	return n;
}
