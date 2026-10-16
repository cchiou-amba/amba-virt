/*
 * virt_slice_lease.c
 *
 * Reserves one published high-memory slice for a domain UUID.
 * The grant is idempotent, epoch-checked on release, and rewritten
 * to the store before the caller is told it succeeded.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "virt_camera_policy.h"
#include "virt_slice_lease.h"

#define LEASE_MAX 8

struct lease_slot {
	int in_use;
	char uuid[64];
	uint32_t index;
	uint64_t epoch;
	uint64_t bytes;
	char ticket[VIRT_CAMERA_TICKET_MAX + 1];	/* empty: reserved without a ticket */
	int primary;					/* the IAV role the ticket held at the last reserve */
};

static struct lease_slot g_slots[LEASE_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* The aux aperture of a lease (Envelope F of plan_iav_usb_yolo), when the server runs one; called with g_lock held. */
static int (*g_aux_attach)(uint32_t index, uint64_t epoch, int primary, uint64_t *size);
static void (*g_aux_detach)(uint32_t index);
static int g_matched;
static uint32_t g_count;
static uint64_t g_slice_bytes;
static uint64_t g_next_epoch = 1;
static char g_store[256] = VIRT_SLICE_LEASE_STORE;

static int uuid_ok(const char *uuid)
{
	size_t n;
	size_t i;

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

static int parse_u64(const char *s, uint64_t *out)
{
	char *end = NULL;
	unsigned long long v;

	if (!s || !*s)
		return -1;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || end == s || !end || *end)
		return -1;
	*out = (uint64_t)v;
	return 0;
}

static void store_dir(const char *path, char *dir, size_t dir_len)
{
	const char *slash = strrchr(path, '/');

	if (!slash || slash == path) {
		snprintf(dir, dir_len, ".");
		return;
	}
	if ((size_t)(slash - path) >= dir_len) {
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
	uint32_t i;

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
	for (i = 0; i < LEASE_MAX; i++) {
		if (!g_slots[i].in_use)
			continue;
		/* The ticket and role are trailing fields: a server that reads four fields ignores them. */
		if (fprintf(fp, "%s %u %llu %llu %s %s\n", g_slots[i].uuid, g_slots[i].index,
			    (unsigned long long)g_slots[i].epoch,
			    (unsigned long long)g_slots[i].bytes,
			    g_slots[i].ticket[0] ? g_slots[i].ticket : "-",
			    g_slots[i].primary ? "primary" : "consumer") < 0) {
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

/* ticket was checked by virt_camera_ticket_ok: at most VIRT_CAMERA_TICKET_MAX characters. */
static void copy_ticket(char dst[VIRT_CAMERA_TICKET_MAX + 1], const char *ticket)
{
	size_t n = strnlen(ticket, VIRT_CAMERA_TICKET_MAX);

	memcpy(dst, ticket, n);
	dst[n] = '\0';
}

static void clear_slot(struct lease_slot *slot)
{
	memset(slot, 0, sizeof(*slot));
}

static struct lease_slot *find_uuid(const char *uuid)
{
	uint32_t i;

	for (i = 0; i < LEASE_MAX; i++) {
		if (g_slots[i].in_use && strcmp(g_slots[i].uuid, uuid) == 0)
			return &g_slots[i];
	}
	return NULL;
}

static int index_taken(uint32_t index)
{
	uint32_t i;

	for (i = 0; i < LEASE_MAX; i++) {
		if (g_slots[i].in_use && g_slots[i].index == index)
			return 1;
	}
	return 0;
}

static void format_ok(char *resp, size_t resp_len, const struct lease_slot *slot)
{
	snprintf(resp, resp_len, "OK index=%u path=/dev/amba_virt_shm%u epoch=%llu\n",
		 slot->index, slot->index, (unsigned long long)slot->epoch);
}

static int reserve_range_locked(const char *uuid, uint64_t bytes,
			       uint32_t min_index, int exact,
			       char *resp, size_t resp_len)
{
	struct lease_slot *slot;
	struct lease_slot *free_slot = NULL;
	uint32_t index;
	uint32_t i;

	if (!g_matched || g_count == 0 || g_slice_bytes == 0) {
		snprintf(resp, resp_len, "ERR GEOMETRY\n");
		return 0;
	}
	if (!uuid_ok(uuid)) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}
	if (bytes == 0) {
		snprintf(resp, resp_len, "ERR ZERO\n");
		return 0;
	}
	if ((bytes % 4096) != 0) {
		snprintf(resp, resp_len, "ERR ALIGN\n");
		return 0;
	}
	if (bytes != g_slice_bytes) {
		snprintf(resp, resp_len, "ERR SIZE\n");
		return 0;
	}
	slot = find_uuid(uuid);
	if (slot) {
		format_ok(resp, resp_len, slot);
		return 0;
	}
	if (g_next_epoch == 0) {
		snprintf(resp, resp_len, "ERR EXHAUSTED\n");
		return 0;
	}
	if (exact) {
		if (min_index >= g_count || index_taken(min_index)) {
			snprintf(resp, resp_len, "ERR EXHAUSTED\n");
			return 0;
		}
		index = min_index;
	} else {
		for (index = min_index; index < g_count; index++) {
			if (!index_taken(index))
				break;
		}
		if (index >= g_count) {
			snprintf(resp, resp_len, "ERR EXHAUSTED\n");
			return 0;
		}
	}
	for (i = 0; i < LEASE_MAX; i++) {
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
	free_slot->bytes = bytes;
	free_slot->in_use = 1;
	if (save_locked() != 0) {
		clear_slot(free_slot);
		snprintf(resp, resp_len, "ERR PERSIST\n");
		return 0;
	}
	format_ok(resp, resp_len, free_slot);
	return 0;
}

static int reserve_locked(const char *uuid, uint64_t bytes, char *resp, size_t resp_len)
{
	return reserve_range_locked(uuid, bytes, 0, 0, resp, resp_len);
}

static int release_locked(const char *uuid, uint64_t epoch, char *resp, size_t resp_len)
{
	struct lease_slot *slot;

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
	{
		struct lease_slot saved = *slot;

		clear_slot(slot);
		if (save_locked() != 0) {
			*slot = saved;
			snprintf(resp, resp_len, "ERR PERSIST\n");
			return 0;
		}
		if (g_aux_detach)
			g_aux_detach(saved.index);
	}
	snprintf(resp, resp_len, "OK\n");
	return 0;
}

static int query_locked(const char *uuid, char *resp, size_t resp_len)
{
	struct lease_slot *slot;

	if (!uuid_ok(uuid)) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}
	slot = find_uuid(uuid);
	if (!slot) {
		snprintf(resp, resp_len, "ERR NOT_FOUND\n");
		return 0;
	}
	format_ok(resp, resp_len, slot);
	return 0;
}

void virt_slice_lease_reset(void)
{
	pthread_mutex_lock(&g_lock);
	memset(g_slots, 0, sizeof(g_slots));
	g_matched = 0;
	g_count = 0;
	g_slice_bytes = 0;
	g_next_epoch = 1;
	snprintf(g_store, sizeof(g_store), "%s", VIRT_SLICE_LEASE_STORE);
	g_aux_attach = NULL;
	g_aux_detach = NULL;
	pthread_mutex_unlock(&g_lock);
}

void virt_slice_lease_set_store(const char *path)
{
	pthread_mutex_lock(&g_lock);
	if (path && path[0] && strlen(path) < sizeof(g_store))
		snprintf(g_store, sizeof(g_store), "%s", path);
	pthread_mutex_unlock(&g_lock);
}

int virt_slice_lease_set_geometry(int matched, uint32_t count, uint64_t slice_bytes)
{
	uint32_t i;
	int drop = 0;

	pthread_mutex_lock(&g_lock);
	if (matched && count >= 1 && count <= LEASE_MAX && slice_bytes != 0 &&
	    (slice_bytes % 4096) == 0) {
		g_matched = 1;
		g_count = count;
		g_slice_bytes = slice_bytes;
		for (i = 0; i < LEASE_MAX; i++) {
			if (!g_slots[i].in_use)
				continue;
			if (g_slots[i].index >= g_count || g_slots[i].bytes != g_slice_bytes) {
				clear_slot(&g_slots[i]);
				drop = 1;
			}
		}
	} else {
		g_matched = 0;
		g_count = 0;
		g_slice_bytes = 0;
	}
	if (drop)
		save_locked();
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int virt_slice_lease_load(void)
{
	FILE *fp;
	char line[256];

	pthread_mutex_lock(&g_lock);
	fp = fopen(g_store, "r");
	if (!fp) {
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	while (fgets(line, sizeof(line), fp)) {
		char uuid[64];
		char ticket[64];
		char role[64];
		unsigned int index = 0;
		unsigned long long epoch = 0;
		unsigned long long bytes = 0;
		struct lease_slot *slot;
		uint32_t i;
		int fields;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		ticket[0] = role[0] = '\0';
		fields = sscanf(line, "%63s %u %llu %llu %63s %63s", uuid, &index, &epoch, &bytes, ticket, role);
		if (fields < 4)
			continue;
		/*
		 * The lease itself is kept whatever follows its four fields: dropping it would free an index a guest
		 * may still map. A lease written by an older server, or whose ticket or role does not parse, keeps no
		 * ticket, and the legacy role rule applies to it.
		 */
		if (fields != 6 || strcmp(ticket, "-") == 0 || !virt_camera_ticket_ok(ticket) ||
		    (strcmp(role, "primary") != 0 && strcmp(role, "consumer") != 0))
			fields = 4;
		if (!uuid_ok(uuid) || epoch == 0 || bytes == 0)
			continue;
		if (!g_matched || index >= g_count || bytes != g_slice_bytes)
			continue;
		if (find_uuid(uuid) || index_taken(index))
			continue;
		slot = NULL;
		for (i = 0; i < LEASE_MAX; i++) {
			if (!g_slots[i].in_use) {
				slot = &g_slots[i];
				break;
			}
		}
		if (!slot)
			break;
		memset(slot, 0, sizeof(*slot));
		snprintf(slot->uuid, sizeof(slot->uuid), "%s", uuid);
		slot->index = index;
		slot->epoch = (uint64_t)epoch;
		slot->bytes = (uint64_t)bytes;
		if (fields == 6 && strcmp(ticket, "-") != 0) {
			copy_ticket(slot->ticket, ticket);
			slot->primary = strcmp(role, "primary") == 0;
		}
		slot->in_use = 1;
		if (epoch >= g_next_epoch) {
			if (epoch == UINT64_MAX)
				g_next_epoch = 0;
			else
				g_next_epoch = (uint64_t)epoch + 1;
		}
	}
	fclose(fp);
	pthread_mutex_unlock(&g_lock);
	return 0;
}

/*
 * Append the camera verdict to a successful reserve reply. ticket is NULL for the legacy
 * RESERVE_SLICE, which never attaches the camera windows. The lock is held.
 */
static void append_camera_locked(const char *uuid, const char *ticket, enum virt_camera_intent intent,
				 char *resp, size_t resp_len)
{
	struct lease_slot *slot;
	struct virt_camera_verdict v;
	char suffix[160];
	size_t n, add;

	if (strncmp(resp, "OK index=", 9) != 0)
		return;
	slot = find_uuid(uuid);
	if (!slot)
		return;
	memset(&v, 0, sizeof(v));
	if (ticket) {
		struct lease_slot saved = *slot;
		int primary = virt_iav_role_of(ticket) == VIRT_IAV_ROLE_PRIMARY;

		/*
		 * Every reserve, idempotent ones included, records the ticket and the role it holds now, so the role
		 * follows the ticket from the guest's next start. The record is persisted before the grant is sent.
		 */
		copy_ticket(slot->ticket, ticket);
		slot->primary = primary;
		if (save_locked() != 0) {
			*slot = saved;
			snprintf(resp, resp_len, "ERR PERSIST\n");
			return;
		}
		if (virt_camera_policy_decide(ticket, intent, primary, &v) != 0)
			memset(&v, 0, sizeof(v));
	}
	if (virt_camera_verdict_format(&v, suffix, sizeof(suffix)) < 0)
		return;
	if (ticket) {
		size_t sl = strlen(suffix);
		uint64_t aux = 0;

		/*
		 * The aux aperture goes with the camera, to the primary only; for anyone else the node of this index is
		 * cleared of what a previous holder had. A failure leaves the guest without an aperture, nothing more.
		 */
		if (g_aux_attach &&
		    g_aux_attach(slot->index, slot->epoch, v.camera && slot->primary, &aux) == 0 && aux && v.camera) {
			if (snprintf(suffix + sl, sizeof(suffix) - sl, " aux=%llu", (unsigned long long)aux) >=
			    (int)(sizeof(suffix) - sl))
				return;
			sl = strlen(suffix);
		}
		if (snprintf(suffix + sl, sizeof(suffix) - sl, " role=%s", slot->primary ? "primary" : "consumer") >=
		    (int)(sizeof(suffix) - sl))
			return;
	}
	n = strlen(resp);
	if (n && resp[n - 1] == '\n')
		n--;
	add = strlen(suffix);
	if (n + add + 2 > resp_len)
		return;
	memcpy(resp + n, suffix, add);
	resp[n + add] = '\n';
	resp[n + add + 1] = '\0';
}

/*
 * RESERVE_SLICE_AT and RESERVE_SLICE_MIN carry the ticket and the fleet camera intent:
 *   RESERVE_SLICE_AT|MIN <uuid> <bytes> <index> <ticket> <intent>
 * Exactly six tokens; a bad ticket or intent is refused before any slice is taken.
 */
int virt_slice_lease_admin(const char *req, char *resp, size_t resp_len)
{
	char cmd[64];
	char uuid[64];
	char num[64];
	char extra[64];
	char ticket[64];
	char intent_s[64];
	char trailing[64];
	enum virt_camera_intent intent = VIRT_CAMERA_INTENT_ABSENT;
	uint64_t value = 0;
	uint64_t index_value = 0;
	int fields;

	if (!resp || resp_len == 0)
		return -1;
	resp[0] = '\0';
	if (!req) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}
	cmd[0] = uuid[0] = num[0] = extra[0] = ticket[0] = intent_s[0] = trailing[0] = '\0';
	fields = sscanf(req, "%63s %63s %63s %63s %63s %63s %63s", cmd, uuid, num, extra, ticket,
			intent_s, trailing);
	if (fields < 1) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}

	pthread_mutex_lock(&g_lock);
	if (strcmp(cmd, "RESERVE_SLICE") == 0) {
		if (parse_u64(num, &value) != 0) {
			snprintf(resp, resp_len, "ERR INVAL\n");
		} else {
			reserve_locked(uuid, value, resp, resp_len);
			append_camera_locked(uuid, NULL, VIRT_CAMERA_INTENT_ABSENT, resp, resp_len);
		}
	} else if (strcmp(cmd, "RESERVE_SLICE_AT") == 0 ||
		   strcmp(cmd, "RESERVE_SLICE_MIN") == 0) {
		if (fields != 6 || parse_u64(num, &value) != 0 ||
		    parse_u64(extra, &index_value) != 0 ||
		    index_value > UINT32_MAX || !virt_camera_ticket_ok(ticket) ||
		    virt_camera_intent_parse(intent_s, &intent) != 0) {
			snprintf(resp, resp_len, "ERR INVAL\n");
		} else {
			reserve_range_locked(uuid, value, (uint32_t)index_value,
					     strcmp(cmd, "RESERVE_SLICE_AT") == 0,
					     resp, resp_len);
			append_camera_locked(uuid, ticket, intent, resp, resp_len);
		}
	} else if (strcmp(cmd, "RELEASE_SLICE") == 0) {
		if (parse_u64(num, &value) != 0)
			snprintf(resp, resp_len, "ERR INVAL\n");
		else
			release_locked(uuid, value, resp, resp_len);
	} else if (strcmp(cmd, "QUERY_SLICE") == 0) {
		query_locked(uuid, resp, resp_len);
	} else {
		snprintf(resp, resp_len, "ERR UNKNOWN_CMD\n");
	}
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int virt_slice_lease_index_used(uint32_t index)
{
	unsigned i;
	int used = 0;

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < LEASE_MAX; i++) {
		if (g_slots[i].in_use && g_slots[i].index == index)
			used = 1;
	}
	pthread_mutex_unlock(&g_lock);
	return used;
}

int virt_slice_lease_ticket_of_index(uint32_t index, char *ticket, size_t ticket_len, int *primary)
{
	unsigned int i;
	int rc = -1;

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < LEASE_MAX; i++) {
		if (g_slots[i].in_use && g_slots[i].index == index) {
			if (ticket && ticket_len)
				snprintf(ticket, ticket_len, "%s", g_slots[i].ticket);
			if (primary)
				*primary = g_slots[i].primary;
			rc = 0;
			break;
		}
	}
	pthread_mutex_unlock(&g_lock);
	return rc;
}

unsigned int virt_slice_lease_ticket_count(const char *ticket)
{
	unsigned int i, n = 0;

	if (!ticket || !ticket[0])
		return 0;
	pthread_mutex_lock(&g_lock);
	for (i = 0; i < LEASE_MAX; i++)
		n += (g_slots[i].in_use && strcmp(g_slots[i].ticket, ticket) == 0) ? 1u : 0u;
	pthread_mutex_unlock(&g_lock);
	return n;
}

void virt_slice_lease_set_aux_hooks(int (*attach)(uint32_t index, uint64_t epoch, int primary, uint64_t *size),
				    void (*detach)(uint32_t index))
{
	pthread_mutex_lock(&g_lock);
	g_aux_attach = attach;
	g_aux_detach = detach;
	pthread_mutex_unlock(&g_lock);
}

void virt_slice_lease_foreach(void (*cb)(uint32_t index, uint64_t epoch, int primary, int ticketed, void *ctx),
			      void *ctx)
{
	unsigned int i;

	if (!cb)
		return;
	pthread_mutex_lock(&g_lock);
	for (i = 0; i < LEASE_MAX; i++)
		if (g_slots[i].in_use)
			cb(g_slots[i].index, g_slots[i].epoch, g_slots[i].primary, g_slots[i].ticket[0] != '\0', ctx);
	pthread_mutex_unlock(&g_lock);
}
