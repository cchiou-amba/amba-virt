/*
 * virt_camera_policy.c
 *
 * Decides, for each reserve of a guest's admission ticket, whether the guest gets the shared camera
 * windows, and holds the persisted per-ticket Dom0 overrides. See virt_camera_policy.h.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "amba_virt_iav.h"
#include "virt_camera_policy.h"

#define OV_INHERIT	0
#define OV_ON		1
#define OV_OFF		2

struct entry {
	int in_use;
	char ticket[VIRT_CAMERA_TICKET_MAX + 1];
	int override;		/* OV_* */
	int seen;		/* a reserve recorded an intent */
	enum virt_camera_intent last;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct entry g_entries[VIRT_CAMERA_MAX_ENTRIES];
static char g_store[256] = VIRT_CAMERA_STORE;
static int g_present[AMBA_VIRT_IAV_MAX_WINDOWS];
static uint64_t g_size[AMBA_VIRT_IAV_MAX_WINDOWS];
static int g_windows_ready;
static uint64_t g_ready_ms;

uint64_t virt_camera_policy_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

int virt_camera_ticket_ok(const char *ticket)
{
	size_t n, i;

	if (!ticket)
		return 0;
	n = strlen(ticket);
	if (n < 1 || n > VIRT_CAMERA_TICKET_MAX)
		return 0;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)ticket[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '_' || c == '.' || c == '-'))
			return 0;
	}
	return 1;
}

int virt_camera_intent_parse(const char *s, enum virt_camera_intent *out)
{
	if (!s || !out)
		return -1;
	if (strcmp(s, "on") == 0)
		*out = VIRT_CAMERA_INTENT_ON;
	else if (strcmp(s, "off") == 0)
		*out = VIRT_CAMERA_INTENT_OFF;
	else if (strcmp(s, "absent") == 0)
		*out = VIRT_CAMERA_INTENT_ABSENT;
	else
		return -1;
	return 0;
}

static struct entry *find_entry(const char *ticket)
{
	unsigned int i;

	for (i = 0; i < VIRT_CAMERA_MAX_ENTRIES; i++) {
		if (g_entries[i].in_use && strcmp(g_entries[i].ticket, ticket) == 0)
			return &g_entries[i];
	}
	return NULL;
}

static struct entry *add_entry(const char *ticket)
{
	unsigned int i;

	for (i = 0; i < VIRT_CAMERA_MAX_ENTRIES; i++) {
		if (!g_entries[i].in_use) {
			memset(&g_entries[i], 0, sizeof(g_entries[i]));
			g_entries[i].in_use = 1;
			snprintf(g_entries[i].ticket, sizeof(g_entries[i].ticket), "%s", ticket);
			return &g_entries[i];
		}
	}
	return NULL;
}

/* An entry that holds neither an override nor a last seen intent is dropped. */
static void drop_if_idle(struct entry *e)
{
	if (e && e->override == OV_INHERIT && !e->seen)
		memset(e, 0, sizeof(*e));
}

static void clear_overrides(void)
{
	unsigned int i;

	for (i = 0; i < VIRT_CAMERA_MAX_ENTRIES; i++) {
		if (!g_entries[i].in_use)
			continue;
		g_entries[i].override = OV_INHERIT;
		drop_if_idle(&g_entries[i]);
	}
}

void virt_camera_policy_reset(void)
{
	pthread_mutex_lock(&g_lock);
	memset(g_entries, 0, sizeof(g_entries));
	memset(g_present, 0, sizeof(g_present));
	memset(g_size, 0, sizeof(g_size));
	g_windows_ready = 0;
	g_ready_ms = 0;
	snprintf(g_store, sizeof(g_store), "%s", VIRT_CAMERA_STORE);
	pthread_mutex_unlock(&g_lock);
}

void virt_camera_policy_set_store(const char *path)
{
	pthread_mutex_lock(&g_lock);
	if (path && path[0] && strlen(path) < sizeof(g_store) - 8)
		snprintf(g_store, sizeof(g_store), "%s", path);
	pthread_mutex_unlock(&g_lock);
}

/*
 * One store line is "<ticket> <on|off>". Returns 0 and fills the outputs, or -1. line is not
 * NUL terminated; len bytes.
 */
static int parse_line(const char *line, size_t len, char *ticket, int *override)
{
	size_t i, sp = 0;
	int have_space = 0;

	if (len < 1)
		return -1;
	for (i = 0; i < len; i++) {
		if (line[i] == ' ') {
			if (have_space)
				return -1;
			have_space = 1;
			sp = i;
		}
	}
	if (!have_space || sp == 0 || sp > VIRT_CAMERA_TICKET_MAX)
		return -1;
	memcpy(ticket, line, sp);
	ticket[sp] = '\0';
	if (!virt_camera_ticket_ok(ticket))
		return -1;
	if (len - sp - 1 == 2 && memcmp(line + sp + 1, "on", 2) == 0)
		*override = OV_ON;
	else if (len - sp - 1 == 3 && memcmp(line + sp + 1, "off", 3) == 0)
		*override = OV_OFF;
	else
		return -1;
	return 0;
}

/* Load the whole file or none of it. Returns 0 (valid or missing) or -1 (corrupt, table left empty). */
static int load_locked(void)
{
	char buf[VIRT_CAMERA_STORE_MAX_BYTES + 1];
	size_t n = 0, pos = 0, lines = 0;
	char tickets[VIRT_CAMERA_MAX_ENTRIES][VIRT_CAMERA_TICKET_MAX + 1];
	int ovs[VIRT_CAMERA_MAX_ENTRIES];
	unsigned int count = 0, i;
	FILE *fp;

	clear_overrides();
	fp = fopen(g_store, "r");
	if (!fp)
		return 0;
	n = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);
	if (n > VIRT_CAMERA_STORE_MAX_BYTES)
		return -1;
	if (memchr(buf, '\0', n))
		return -1;
	while (pos < n) {
		const char *nl = memchr(buf + pos, '\n', n - pos);
		size_t len = nl ? (size_t)(nl - (buf + pos)) : n - pos;
		char ticket[VIRT_CAMERA_TICKET_MAX + 1];
		int ov = OV_INHERIT;

		if (++lines > VIRT_CAMERA_MAX_ENTRIES)
			return -1;
		if (parse_line(buf + pos, len, ticket, &ov) != 0)
			return -1;
		for (i = 0; i < count; i++) {
			if (strcmp(tickets[i], ticket) == 0)
				return -1;
		}
		snprintf(tickets[count], sizeof(tickets[count]), "%s", ticket);
		ovs[count] = ov;
		count++;
		pos += len + (nl ? 1 : 0);
	}
	for (i = 0; i < count; i++) {
		struct entry *e = find_entry(tickets[i]);

		if (!e)
			e = add_entry(tickets[i]);
		if (!e)
			return -1;
		e->override = ovs[i];
	}
	return 0;
}

int virt_camera_policy_load(void)
{
	int rc;

	pthread_mutex_lock(&g_lock);
	rc = load_locked();
	if (rc != 0) {
		clear_overrides();
		fprintf(stderr, "amba-virt-server: camera tickets %s is not valid; every ticket is inherit\n", g_store);
	}
	pthread_mutex_unlock(&g_lock);
	return 0;
}

/* Write the overrides atomically: a temporary file in the same directory, fsync, rename. */
static int save_locked(void)
{
	char dir[256], tmp[288];
	char buf[VIRT_CAMERA_STORE_MAX_BYTES];
	size_t len = 0;
	const char *slash;
	unsigned int i;
	int fd;

	for (i = 0; i < VIRT_CAMERA_MAX_ENTRIES; i++) {
		int w;

		if (!g_entries[i].in_use || g_entries[i].override == OV_INHERIT)
			continue;
		w = snprintf(buf + len, sizeof(buf) - len, "%s %s\n", g_entries[i].ticket,
			     g_entries[i].override == OV_ON ? "on" : "off");
		if (w < 0 || (size_t)w >= sizeof(buf) - len)
			return -1;
		len += (size_t)w;
	}

	slash = strrchr(g_store, '/');
	if (slash && slash != g_store) {
		memcpy(dir, g_store, (size_t)(slash - g_store));
		dir[slash - g_store] = '\0';
		if (mkdir(dir, 0755) < 0 && errno != EEXIST)
			return -1;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", g_store);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	if (fchmod(fd, 0600) < 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	for (i = 0; i < len;) {
		ssize_t w = write(fd, buf + i, len - i);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp);
			return -1;
		}
		i += (unsigned int)w;
	}
	if (fsync(fd) != 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	close(fd);
	if (rename(tmp, g_store) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

void virt_camera_policy_set_windows(const struct virt_camera_window *w, unsigned int n, uint64_t ready_ms)
{
	unsigned int i;

	pthread_mutex_lock(&g_lock);
	memset(g_present, 0, sizeof(g_present));
	memset(g_size, 0, sizeof(g_size));
	for (i = 0; w && i < n; i++) {
		if (w[i].id >= AMBA_VIRT_IAV_MAX_WINDOWS || w[i].size == 0)
			continue;
		g_present[w[i].id] = 1;
		g_size[w[i].id] = w[i].size;
	}
	g_windows_ready = g_present[AMBA_VIRT_IAV_WIN_DSP] && g_present[AMBA_VIRT_IAV_WIN_BSB];
	g_ready_ms = g_windows_ready ? ready_ms : 0;
	pthread_mutex_unlock(&g_lock);
}

static int camera_on(int ov, enum virt_camera_intent intent)
{
	return (ov == OV_INHERIT) ? (intent == VIRT_CAMERA_INTENT_ON) : (ov == OV_ON);
}

int virt_camera_policy_decide(const char *ticket, enum virt_camera_intent intent, uint32_t index,
			      struct virt_camera_verdict *v)
{
	struct entry *e;
	int ov = OV_INHERIT, on;

	if (!v)
		return -1;
	memset(v, 0, sizeof(*v));
	if (!virt_camera_ticket_ok(ticket))
		return -1;
	if (intent != VIRT_CAMERA_INTENT_ABSENT && intent != VIRT_CAMERA_INTENT_OFF &&
	    intent != VIRT_CAMERA_INTENT_ON)
		return -1;

	pthread_mutex_lock(&g_lock);
	e = find_entry(ticket);
	if (!e)
		e = add_entry(ticket);
	if (e) {
		e->seen = 1;
		e->last = intent;
		ov = e->override;
	}
	on = camera_on(ov, intent);
	v->camera = (g_windows_ready && on) ? 1 : 0;
	if (v->camera) {
		v->dsp = g_size[AMBA_VIRT_IAV_WIN_DSP];
		v->bsb = g_size[AMBA_VIRT_IAV_WIN_BSB];
		if (index == 0) {
			if (g_present[AMBA_VIRT_IAV_WIN_DEC])
				v->dec = g_size[AMBA_VIRT_IAV_WIN_DEC];
			if (g_present[AMBA_VIRT_IAV_WIN_OVL])
				v->ovl = g_size[AMBA_VIRT_IAV_WIN_OVL];
		}
	}
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int virt_camera_verdict_format(const struct virt_camera_verdict *v, char *buf, size_t len)
{
	int n;

	if (!v || !buf || len == 0)
		return -1;
	if (!v->camera)
		n = snprintf(buf, len, " camera=0");
	else {
		n = snprintf(buf, len, " camera=1 dsp=%llu bsb=%llu", (unsigned long long)v->dsp,
			     (unsigned long long)v->bsb);
		if (n >= 0 && (size_t)n < len && v->dec)
			n += snprintf(buf + n, len - (size_t)n, " dec=%llu", (unsigned long long)v->dec);
		if (n >= 0 && (size_t)n < len && v->ovl)
			n += snprintf(buf + n, len - (size_t)n, " ovl=%llu", (unsigned long long)v->ovl);
	}
	if (n < 0 || (size_t)n >= len)
		return -1;
	return n;
}

static const char *ov_name(int ov)
{
	return ov == OV_ON ? "on" : ov == OV_OFF ? "off" : "inherit";
}

static const char *intent_name(const struct entry *e)
{
	if (!e->seen)
		return "none";
	return e->last == VIRT_CAMERA_INTENT_ON ? "on" : e->last == VIRT_CAMERA_INTENT_OFF ? "off" : "absent";
}

static int effective_of(const struct entry *e)
{
	enum virt_camera_intent in = e->seen ? e->last : VIRT_CAMERA_INTENT_ABSENT;

	return (g_windows_ready && camera_on(e->override, in)) ? 1 : 0;
}

static int append_entry(char *resp, size_t resp_len, size_t *pos, const struct entry *e)
{
	int w = snprintf(resp + *pos, resp_len - *pos, "TICKET %s override=%s intent=%s effective=%d\n",
			 e->ticket, ov_name(e->override), intent_name(e), effective_of(e));

	if (w < 0 || (size_t)w >= resp_len - *pos)
		return -1;
	*pos += (size_t)w;
	return 0;
}

static void get_locked(const char *ticket, char *resp, size_t resp_len)
{
	char body[2048];
	size_t pos = 0;
	unsigned int i, count = 0, total = 0;
	int truncated = 0;

	body[0] = '\0';
	for (i = 0; i < VIRT_CAMERA_MAX_ENTRIES; i++) {
		if (!g_entries[i].in_use)
			continue;
		if (ticket && strcmp(g_entries[i].ticket, ticket) != 0)
			continue;
		total++;
		if (count >= VIRT_CAMERA_GET_MAX_LINES || append_entry(body, sizeof(body), &pos, &g_entries[i]) != 0) {
			truncated = 1;
			continue;
		}
		count++;
	}
	(void)total;
	snprintf(resp, resp_len, "OK COUNT %u\n%s%s", count, body, truncated ? "TRUNCATED\n" : "");
}

int virt_camera_policy_admin(const char *req, char *resp, size_t resp_len)
{
	char cmd[64], a1[64], a2[64], a3[64];
	int fields;

	if (!resp || resp_len == 0)
		return -1;
	resp[0] = '\0';
	if (!req) {
		snprintf(resp, resp_len, "ERR INVAL\n");
		return 0;
	}
	cmd[0] = a1[0] = a2[0] = a3[0] = '\0';
	fields = sscanf(req, "%63s %63s %63s %63s", cmd, a1, a2, a3);

	if (fields >= 1 && strcmp(cmd, "SET_CAMERA") == 0) {
		int ov;
		struct entry *e;
		int old;

		if (fields != 3 || !virt_camera_ticket_ok(a1)) {
			snprintf(resp, resp_len, "ERR INVAL\n");
			return 0;
		}
		if (strcmp(a2, "on") == 0)
			ov = OV_ON;
		else if (strcmp(a2, "off") == 0)
			ov = OV_OFF;
		else if (strcmp(a2, "inherit") == 0)
			ov = OV_INHERIT;
		else {
			snprintf(resp, resp_len, "ERR INVAL\n");
			return 0;
		}
		pthread_mutex_lock(&g_lock);
		e = find_entry(a1);
		if (!e && ov != OV_INHERIT)
			e = add_entry(a1);
		if (!e) {
			/* inherit of an unknown ticket is already the state; a full table is a persist failure */
			if (ov == OV_INHERIT)
				snprintf(resp, resp_len, "OK\n");
			else
				snprintf(resp, resp_len, "ERR PERSIST\n");
			pthread_mutex_unlock(&g_lock);
			return 0;
		}
		old = e->override;
		e->override = ov;
		if (save_locked() != 0) {
			e->override = old;
			drop_if_idle(e);
			snprintf(resp, resp_len, "ERR PERSIST\n");
		} else {
			drop_if_idle(e);
			snprintf(resp, resp_len, "OK\n");
		}
		pthread_mutex_unlock(&g_lock);
	} else if (fields >= 1 && strcmp(cmd, "GET_CAMERA") == 0) {
		if (fields > 2 || (fields == 2 && !virt_camera_ticket_ok(a1))) {
			snprintf(resp, resp_len, "ERR INVAL\n");
			return 0;
		}
		pthread_mutex_lock(&g_lock);
		get_locked(fields == 2 ? a1 : NULL, resp, resp_len);
		pthread_mutex_unlock(&g_lock);
	} else if (fields >= 1 && strcmp(cmd, "CAMERA_STATUS") == 0) {
		if (fields != 1) {
			snprintf(resp, resp_len, "ERR INVAL\n");
			return 0;
		}
		pthread_mutex_lock(&g_lock);
		snprintf(resp, resp_len, "OK windows=%s dsp=%llu bsb=%llu dec=%llu ovl=%llu ready_ms=%llu\n",
			 g_windows_ready ? "ready" : "none",
			 (unsigned long long)g_size[AMBA_VIRT_IAV_WIN_DSP],
			 (unsigned long long)g_size[AMBA_VIRT_IAV_WIN_BSB],
			 (unsigned long long)g_size[AMBA_VIRT_IAV_WIN_DEC],
			 (unsigned long long)g_size[AMBA_VIRT_IAV_WIN_OVL],
			 (unsigned long long)g_ready_ms);
		pthread_mutex_unlock(&g_lock);
	} else {
		snprintf(resp, resp_len, "ERR UNKNOWN_CMD\n");
	}
	return 0;
}
