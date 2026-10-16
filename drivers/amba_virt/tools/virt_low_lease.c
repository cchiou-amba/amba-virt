/*
 * virt_low_lease.c
 *
 * Leases eight fixed slices. A guest release does not come through here.
 * EVE release does. A released slice is quarantined, scrubbed and only then
 * leased again (the per-guest arena lives in it; plan_iav_usb_yolo, Envelope E):
 * the quarantine persists in the store as a "scrub <index> 0 0" line, which an
 * older server skips. Every slice that holds no lease when the server starts is
 * scrubbed too, before it is leased.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "iav_slice_rpc.h"
#include "virt_host_ops.h"
#include "virt_low_lease.h"
#include "virt_slice_arena.h"
#include "virt_slice_lease.h"

struct low_slot {
	int in_use;
	char uuid[64];
	uint32_t index;
	uint64_t epoch;
	uint32_t cid;
};

static struct low_slot g_slots[VIRT_LOW_LEASE_COUNT];
static int g_scrub[VIRT_LOW_LEASE_COUNT];	/* released or never scrubbed: not leased until the scrub succeeded */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_scrubbed = PTHREAD_COND_INITIALIZER;
static unsigned int g_reserve_wait_ms = VIRT_SLICE_QUARANTINE_MS + 6000u;
static pthread_t g_scrubber;
static int g_scrubber_started;
static volatile int g_scrubber_stop;
static int g_ready;
static uint64_t g_base;
static uint32_t g_pid;
static int g_have_pid;
static uint64_t g_next_epoch = 1;
static char g_store[256] = VIRT_LOW_LEASE_STORE;

static int uuid_ok(const char *uuid)
{
	size_t n, i;

	if (!uuid)
		return 0;
	n = strlen(uuid);
	if (n < 1 || n > 36)
		return 0;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)uuid[i];

		if (c != '-' && !isxdigit(c))
			return 0;
	}
	return 1;
}

static int parse_u32(const char *s, uint32_t *out)
{
	char *end = NULL;
	unsigned long v;

	if (!s || !*s)
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || end == s || !end || *end || v > 0xfffffffful)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static int cavalry_held(const char *uuid)
{
	char req[96];
	char resp[160];

	snprintf(req, sizeof(req), "QUERY_SLICE %s", uuid);
	if (virt_slice_lease_admin(req, resp, sizeof(resp)) != 0)
		return 0;
	return strncmp(resp, "OK ", 3) == 0;
}

static void store_dir(const char *path, char *dir, size_t dir_len)
{
	const char *slash = strrchr(path, '/');

	if (!slash || slash == path || (size_t)(slash - path) >= dir_len) {
		snprintf(dir, dir_len, ".");
		return;
	}
	memcpy(dir, path, (size_t)(slash - path));
	dir[slash - path] = '\0';
}

static int save_locked(void)
{
	char dir[256];
	char tmp[288];
	FILE *fp;
	unsigned i;

	store_dir(g_store, dir, sizeof(dir));
	if (mkdir(dir, 0755) < 0 && errno != EEXIST)
		return -1;
	snprintf(tmp, sizeof(tmp), "%s.tmp", g_store);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	if (fchmod(fileno(fp), 0600) < 0) {
		fclose(fp);
		unlink(tmp);
		return -1;
	}
	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
		if (!g_slots[i].in_use)
			continue;
		if (fprintf(fp, "%s %u %llu %u\n", g_slots[i].uuid, g_slots[i].index,
			    (unsigned long long)g_slots[i].epoch, g_slots[i].cid) < 0) {
			fclose(fp);
			unlink(tmp);
			return -1;
		}
	}
	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
		if (g_scrub[i] && fprintf(fp, "scrub %u 0 0\n", i) < 0) {
			fclose(fp);
			unlink(tmp);
			return -1;
		}
	}
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
		fclose(fp);
		unlink(tmp);
		return -1;
	}
	fclose(fp);
	if (rename(tmp, g_store) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static struct low_slot *find_uuid(const char *uuid)
{
	unsigned i;

	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
		if (g_slots[i].in_use && strcmp(g_slots[i].uuid, uuid) == 0)
			return &g_slots[i];
	}
	return NULL;
}

static int index_leased(uint32_t index)
{
	unsigned i;

	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
		if (g_slots[i].in_use && g_slots[i].index == index)
			return 1;
	}
	return 0;
}

/* Leased, or waiting for its scrub. */
static int index_taken(uint32_t index)
{
	return index_leased(index) || (index < VIRT_LOW_LEASE_COUNT && g_scrub[index]);
}

static int any_scrub_pending(void)
{
	unsigned i;

	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++)
		if (g_scrub[i])
			return 1;
	return 0;
}

/* The slice header tells the guest module which BAR is its slice; a failure only leaves the guest without an arena. */
static void write_header(uint32_t index)
{
	int rc = virt_slice_arena_write_header(index);

	if (rc != 0)
		fprintf(stderr, "amba-virt-server: low slice %u: cannot write the slice header (%d)\n", index, rc);
}

static void format_ok(char *resp, size_t resp_len, const struct low_slot *slot)
{
	snprintf(resp, resp_len,
		 "OK index=%u path=/dev/amba_virt_iav_slice%u epoch=%llu usable=%u\n",
		 slot->index, slot->index, (unsigned long long)slot->epoch,
		 (unsigned)VIRT_LOW_LEASE_USABLE);
}

static void bind_slot(const struct low_slot *slot)
{
	if (slot->cid)
		iav_slice_rpc_bind(slot->cid, slot->index, VIRT_LOW_LEASE_USABLE);
}

static int reserve_locked(const char *uuid, uint32_t cid, char *resp, size_t resp_len)
{
	struct low_slot *slot;
	struct low_slot *free_slot = NULL;
	uint32_t index, i;

	if (!g_ready) {
		snprintf(resp, resp_len, "ERR GEOMETRY\n");
		return 0;
	}
	if (!uuid_ok(uuid) || cid == 0) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}
	slot = find_uuid(uuid);
	if (slot) {
		if (slot->cid != cid)
			iav_slice_rpc_unbind(slot->cid);
		slot->cid = cid;
		if (save_locked() != 0) {
			snprintf(resp, resp_len, "ERR PERSIST\n");
			return 0;
		}
		bind_slot(slot);
		write_header(slot->index);
		format_ok(resp, resp_len, slot);
		return 0;
	}
	if (g_next_epoch == 0) {
		snprintf(resp, resp_len, "ERR EXHAUSTED\n");
		return 0;
	}
	for (;;) {
		for (index = 0; index < VIRT_LOW_LEASE_COUNT; index++) {
			if (!index_taken(index))
				break;
		}
		if (index < VIRT_LOW_LEASE_COUNT || !any_scrub_pending() || g_reserve_wait_ms == 0)
			break;
		/* Every free slice is still in quarantine: wait (bounded) for a scrub to finish. */
		{
			struct timespec until;

			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += g_reserve_wait_ms / 1000u;
			until.tv_nsec += (long)(g_reserve_wait_ms % 1000u) * 1000000L;
			if (until.tv_nsec >= 1000000000L) {
				until.tv_sec++;
				until.tv_nsec -= 1000000000L;
			}
			if (pthread_cond_timedwait(&g_scrubbed, &g_lock, &until) != 0) {
				for (index = 0; index < VIRT_LOW_LEASE_COUNT; index++)
					if (!index_taken(index))
						break;
				break;
			}
		}
		slot = find_uuid(uuid);
		if (slot) {
			/* the same owner reserved meanwhile */
			bind_slot(slot);
			format_ok(resp, resp_len, slot);
			return 0;
		}
	}
	if (index >= VIRT_LOW_LEASE_COUNT) {
		snprintf(resp, resp_len, "ERR EXHAUSTED\n");
		return 0;
	}
	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
		if (!g_slots[i].in_use) {
			free_slot = &g_slots[i];
			break;
		}
	}
	if (!free_slot) {
		snprintf(resp, resp_len, "ERR EXHAUSTED\n");
		return 0;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	snprintf(free_slot->uuid, sizeof(free_slot->uuid), "%s", uuid);
	free_slot->index = index;
	free_slot->epoch = g_next_epoch++;
	free_slot->cid = cid;
	free_slot->in_use = 1;
	if (save_locked() != 0) {
		memset(free_slot, 0, sizeof(*free_slot));
		snprintf(resp, resp_len, "ERR PERSIST\n");
		return 0;
	}
	bind_slot(free_slot);
	write_header(free_slot->index);
	format_ok(resp, resp_len, free_slot);
	return 0;
}

static int release_locked(const char *uuid, uint64_t epoch, char *resp, size_t resp_len)
{
	struct low_slot *slot;
	struct low_slot saved;

	if (!uuid_ok(uuid)) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}
	slot = find_uuid(uuid);
	if (!slot) {
		snprintf(resp, resp_len, "ERR NOT_FOUND\n");
		return 0;
	}
	if (slot->epoch != epoch) {
		snprintf(resp, resp_len, "ERR STALE\n");
		return 0;
	}
	saved = *slot;
	/*
	 * Quarantine first, so no new DSP or GDMA job can name the slice, then drop the binding. The slice is leased again
	 * only after the scrubber zeroed it.
	 */
	virt_slice_arena_quarantine(saved.index, virt_slice_arena_now_ms());
	g_scrub[saved.index] = 1;
	iav_slice_rpc_unbind(slot->cid);
	memset(slot, 0, sizeof(*slot));
	if (save_locked() != 0) {
		*slot = saved;
		g_scrub[saved.index] = 0;
		virt_slice_arena_unquarantine(saved.index);
		bind_slot(slot);
		snprintf(resp, resp_len, "ERR PERSIST\n");
		return 0;
	}
	snprintf(resp, resp_len, "OK\n");
	return 0;
}

int virt_low_lease_scrub_pass(void)
{
	unsigned i;
	int done = 0;

	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
		int pending;
		int rc;

		pthread_mutex_lock(&g_lock);
		pending = g_scrub[i] && !index_leased(i);
		pthread_mutex_unlock(&g_lock);
		if (!pending || !virt_slice_arena_scrub_due(i, NULL))
			continue;
		rc = virt_slice_arena_scrub(i);
		if (rc != 0) {
			fprintf(stderr, "amba-virt-server: low slice %u: scrub failed (%d); it stays quarantined\n", i, rc);
			continue;
		}
		pthread_mutex_lock(&g_lock);
		g_scrub[i] = 0;
		virt_slice_arena_unquarantine(i);
		if (save_locked() != 0)
			fprintf(stderr, "amba-virt-server: low slice %u: scrubbed, but the store was not written\n", i);
		pthread_cond_broadcast(&g_scrubbed);
		pthread_mutex_unlock(&g_lock);
		printf("amba-virt-server: low slice %u scrubbed and free\n", i);
		fflush(stdout);
		done++;
	}
	return done;
}

int virt_low_lease_scrub_pending(uint32_t index)
{
	int p;

	if (index >= VIRT_LOW_LEASE_COUNT)
		return 0;
	pthread_mutex_lock(&g_lock);
	p = g_scrub[index];
	pthread_mutex_unlock(&g_lock);
	return p;
}

void virt_low_lease_set_reserve_wait_ms(unsigned int ms)
{
	pthread_mutex_lock(&g_lock);
	g_reserve_wait_ms = ms;
	pthread_mutex_unlock(&g_lock);
}

static void *scrubber_main(void *arg)
{
	(void)arg;
	while (!g_scrubber_stop) {
		struct timespec ts = { 0, 200 * 1000000L };

		virt_low_lease_scrub_pass();
		nanosleep(&ts, NULL);
	}
	return NULL;
}

int virt_low_lease_start_scrubber(void)
{
	int rc = 0;

	pthread_mutex_lock(&g_lock);
	if (!g_scrubber_started) {
		g_scrubber_stop = 0;
		if (pthread_create(&g_scrubber, NULL, scrubber_main, NULL) == 0)
			g_scrubber_started = 1;
		else
			rc = -EAGAIN;
	}
	pthread_mutex_unlock(&g_lock);
	return rc;
}

void virt_low_lease_stop_scrubber(void)
{
	int started;

	pthread_mutex_lock(&g_lock);
	started = g_scrubber_started;
	g_scrubber_stop = 1;
	pthread_mutex_unlock(&g_lock);
	if (started) {
		pthread_join(g_scrubber, NULL);
		pthread_mutex_lock(&g_lock);
		g_scrubber_started = 0;
		pthread_mutex_unlock(&g_lock);
	}
}

void virt_low_lease_reset(void)
{
	virt_low_lease_stop_scrubber();
	pthread_mutex_lock(&g_lock);
	memset(g_slots, 0, sizeof(g_slots));
	memset(g_scrub, 0, sizeof(g_scrub));
	g_reserve_wait_ms = VIRT_SLICE_QUARANTINE_MS + 6000u;
	g_ready = 0;
	g_base = 0;
	g_pid = 0;
	g_have_pid = 0;
	g_next_epoch = 1;
	snprintf(g_store, sizeof(g_store), "%s", VIRT_LOW_LEASE_STORE);
	pthread_mutex_unlock(&g_lock);
	iav_slice_rpc_reset();
	virt_slice_arena_reset();
}

void virt_low_lease_set_store(const char *path)
{
	pthread_mutex_lock(&g_lock);
	if (path && path[0] && strlen(path) < sizeof(g_store))
		snprintf(g_store, sizeof(g_store), "%s", path);
	pthread_mutex_unlock(&g_lock);
}

int virt_low_lease_set_partition(uint32_t pid)
{
	pthread_mutex_lock(&g_lock);
	g_pid = pid;
	g_have_pid = 1;
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int virt_low_lease_geometry(uint64_t *base, uint32_t *pid)
{
	int rc = -ENODEV;

	if (!base || !pid)
		return -EINVAL;
	pthread_mutex_lock(&g_lock);
	if (g_ready && g_have_pid) {
		*base = g_base;
		*pid = g_pid;
		rc = 0;
	}
	pthread_mutex_unlock(&g_lock);
	return rc;
}

int virt_low_lease_set_parent(uint64_t base)
{
	if (base == 0 || (base % VIRT_LOW_LEASE_BYTES) != 0)
		return -EINVAL;
	pthread_mutex_lock(&g_lock);
	g_base = base;
	g_ready = 1;
	pthread_mutex_unlock(&g_lock);
	return 0;
}

/*
 * When the server starts, a slice that holds no lease may still hold what an earlier holder left (a release whose scrub
 * was lost with the board's /run, or a server that did not scrub): every such slice is scrubbed before it is leased.
 * The quarantine is already over (released long ago), so the scrubber takes them at once. Called with g_lock held.
 */
static void quarantine_unleased_locked(void)
{
	uint64_t now = virt_slice_arena_now_ms();
	unsigned i;

	if (!g_ready)
		return;
	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
		if (index_leased(i))
			continue;
		if (!g_scrub[i]) {
			g_scrub[i] = 1;
			virt_slice_arena_quarantine(i, now > VIRT_SLICE_QUARANTINE_MS ? now - VIRT_SLICE_QUARANTINE_MS : 0);
		} else {
			virt_slice_arena_quarantine(i, now);
		}
	}
}

int virt_low_lease_load(void)
{
	FILE *fp;
	char line[256];

	pthread_mutex_lock(&g_lock);
	fp = fopen(g_store, "r");
	if (!fp) {
		quarantine_unleased_locked();
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	while (fgets(line, sizeof(line), fp)) {
		char uuid[64];
		unsigned index = 0, cid = 0;
		unsigned long long epoch = 0;
		struct low_slot *slot = NULL;
		unsigned i;

		if (sscanf(line, "%63s %u %llu %u", uuid, &index, &epoch, &cid) != 4)
			continue;
		if (strcmp(uuid, "scrub") == 0) {
			/* a release whose scrub had not finished: quarantined again from now */
			if (index < VIRT_LOW_LEASE_COUNT)
				g_scrub[index] = 1;
			continue;
		}
		if (!g_ready || !uuid_ok(uuid) || epoch == 0 || cid == 0)
			continue;
		if (index >= VIRT_LOW_LEASE_COUNT || find_uuid(uuid) || index_taken(index))
			continue;
		for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
			if (!g_slots[i].in_use) {
				slot = &g_slots[i];
				break;
			}
		}
		if (!slot)
			break;
		snprintf(slot->uuid, sizeof(slot->uuid), "%s", uuid);
		slot->index = index;
		slot->epoch = epoch;
		slot->cid = cid;
		slot->in_use = 1;
		if (epoch >= g_next_epoch)
			g_next_epoch = epoch == UINT64_MAX ? 0 : epoch + 1;
		bind_slot(slot);
	}
	fclose(fp);
	quarantine_unleased_locked();
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int virt_low_lease_admin(const char *req, char *resp, size_t resp_len)
{
	char cmd[64], uuid[64], num[64], extra[64];
	uint32_t cid = 0;
	uint64_t epoch = 0;
	int paired;

	if (!resp || resp_len == 0)
		return -1;
	resp[0] = '\0';
	if (!req) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}
	cmd[0] = uuid[0] = num[0] = extra[0] = '\0';
	if (sscanf(req, "%63s %63s %63s %63s", cmd, uuid, num, extra) < 1) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}
	if (strcmp(cmd, "RESERVE_LOW") == 0) {
		if (parse_u32(num, &cid) != 0) {
			snprintf(resp, resp_len, "ERR INVAL\n");
			return 0;
		}
		paired = cavalry_held(uuid);
		if (!paired) {
			snprintf(resp, resp_len, "ERR NOT_PAIRED\n");
			return 0;
		}
		pthread_mutex_lock(&g_lock);
		reserve_locked(uuid, cid, resp, resp_len);
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	if (strcmp(cmd, "RELEASE_LOW") == 0) {
		char *end = NULL;

		errno = 0;
		epoch = strtoull(num, &end, 10);
		if (!num[0] || errno || !end || *end) {
			snprintf(resp, resp_len, "ERR INVAL\n");
			return 0;
		}
		pthread_mutex_lock(&g_lock);
		release_locked(uuid, epoch, resp, resp_len);
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	if (strcmp(cmd, "QUERY_LOW") == 0) {
		struct low_slot *slot;

		pthread_mutex_lock(&g_lock);
		slot = find_uuid(uuid);
		if (!slot)
			snprintf(resp, resp_len, "ERR NOT_FOUND\n");
		else
			format_ok(resp, resp_len, slot);
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	snprintf(resp, resp_len, "ERR UNKNOWN_CMD\n");
	return 0;
}

int virt_low_lease_arm_module(uint64_t base)
{
	char params[64];
	int rc;

	rc = virt_low_lease_set_parent(base);
	if (rc < 0)
		return rc;
	if (g_base != base)
		return -EINVAL;
	virt_low_lease_load();
	snprintf(params, sizeof(params), "pool_base=0x%llx", (unsigned long long)base);
	rc = virt_host_module_load("amba_iav_pool.ko", params);
	if (rc == -EEXIST)
		rc = 0;
	if (rc == 0) {
		unsigned i;

		/* The leases this server took over carry the slice header too (an older server wrote none). */
		pthread_mutex_lock(&g_lock);
		for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++)
			if (g_slots[i].in_use)
				write_header(g_slots[i].index);
		pthread_mutex_unlock(&g_lock);
		if (virt_low_lease_start_scrubber() != 0)
			fprintf(stderr, "amba-virt-server: cannot start the slice scrubber; released slices stay quarantined\n");
	}
	return rc;
}

int virt_low_lease_index_used(uint32_t index)
{
	unsigned i;
	int used = 0;

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < VIRT_LOW_LEASE_COUNT; i++) {
		if (g_slots[i].in_use && g_slots[i].index == index)
			used = 1;
	}
	pthread_mutex_unlock(&g_lock);
	return used;
}
