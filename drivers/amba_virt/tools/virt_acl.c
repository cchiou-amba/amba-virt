/*
 * virt_acl.c
 *
 * Access Control List and Policy Engine for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "virt_acl.h"

static pthread_mutex_t g_acl_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct virt_acl_entry g_acl_entries[VIRT_ACL_MAX_RULES];
static int g_acl_initialized = 0;

int virt_acl_init(void)
{
	int i;

	pthread_mutex_lock(&g_acl_mutex);
	memset(g_acl_entries, 0, sizeof(g_acl_entries));
	for (i = 0; i < VIRT_ACL_MAX_RULES; i++) {
		g_acl_entries[i].in_use = 0;
	}
	g_acl_initialized = 1;
	pthread_mutex_unlock(&g_acl_mutex);

	/* Try loading default policies from disk */
	virt_acl_load_policies(VIRT_ACL_POLICY_FILE);
	return 0;
}

void virt_acl_cleanup(void)
{
	pthread_mutex_lock(&g_acl_mutex);
	memset(g_acl_entries, 0, sizeof(g_acl_entries));
	g_acl_initialized = 0;
	pthread_mutex_unlock(&g_acl_mutex);
}

int virt_acl_has_cap(uint32_t cid, uint32_t cap)
{
	uint32_t caps = virt_acl_get_caps(cid);
	return (caps & cap) == cap;
}

uint32_t virt_acl_get_caps(uint32_t cid)
{
	int i;
	uint32_t caps = AMBA_VIRT_ROLE_STANDARD;

	if (!g_acl_initialized)
		virt_acl_init();

	pthread_mutex_lock(&g_acl_mutex);
	for (i = 0; i < VIRT_ACL_MAX_RULES; i++) {
		if (g_acl_entries[i].in_use && g_acl_entries[i].cid == cid) {
			caps = g_acl_entries[i].caps;
			pthread_mutex_unlock(&g_acl_mutex);
			return caps;
		}
	}
	pthread_mutex_unlock(&g_acl_mutex);

	/* Unregistered CIDs default to STANDARD role */
	return caps;
}

int virt_acl_set_rule(uint32_t cid, const char *name, uint32_t caps, uint32_t quota_mb, uint32_t priority)
{
	int i;
	int target_slot = -1;

	if (!g_acl_initialized)
		virt_acl_init();

	pthread_mutex_lock(&g_acl_mutex);
	for (i = 0; i < VIRT_ACL_MAX_RULES; i++) {
		if (g_acl_entries[i].in_use && g_acl_entries[i].cid == cid) {
			target_slot = i;
			break;
		}
	}

	if (target_slot < 0) {
		for (i = 0; i < VIRT_ACL_MAX_RULES; i++) {
			if (!g_acl_entries[i].in_use) {
				target_slot = i;
				break;
			}
		}
	}

	if (target_slot < 0) {
		pthread_mutex_unlock(&g_acl_mutex);
		return -ENOSPC;
	}

	g_acl_entries[target_slot].cid = cid;
	g_acl_entries[target_slot].caps = caps;
	g_acl_entries[target_slot].quota_mb = quota_mb;
	g_acl_entries[target_slot].priority = priority;
	if (name && strlen(name) > 0)
		snprintf(g_acl_entries[target_slot].tenant_name, sizeof(g_acl_entries[target_slot].tenant_name), "%s", name);
	else
		snprintf(g_acl_entries[target_slot].tenant_name, sizeof(g_acl_entries[target_slot].tenant_name), "guest-%u", cid);
	g_acl_entries[target_slot].in_use = 1;

	pthread_mutex_unlock(&g_acl_mutex);
	return 0;
}

int virt_acl_get_rule(uint32_t cid, struct virt_acl_entry *out_entry)
{
	int i;

	if (!out_entry)
		return -EINVAL;

	if (!g_acl_initialized)
		virt_acl_init();

	pthread_mutex_lock(&g_acl_mutex);
	for (i = 0; i < VIRT_ACL_MAX_RULES; i++) {
		if (g_acl_entries[i].in_use && g_acl_entries[i].cid == cid) {
			memcpy(out_entry, &g_acl_entries[i], sizeof(*out_entry));
			pthread_mutex_unlock(&g_acl_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_acl_mutex);

	/* Synthesize default entry if not explicitly registered */
	memset(out_entry, 0, sizeof(*out_entry));
	out_entry->cid = cid;
	out_entry->caps = AMBA_VIRT_ROLE_STANDARD;
	out_entry->quota_mb = 1024;
	out_entry->priority = VIRT_PRIORITY_NORMAL;
	snprintf(out_entry->tenant_name, sizeof(out_entry->tenant_name), "guest-%u", cid);
	out_entry->in_use = 1;
	return 0;
}

int virt_acl_remove_rule(uint32_t cid)
{
	int i;

	if (!g_acl_initialized)
		return -ENOENT;

	pthread_mutex_lock(&g_acl_mutex);
	for (i = 0; i < VIRT_ACL_MAX_RULES; i++) {
		if (g_acl_entries[i].in_use && g_acl_entries[i].cid == cid) {
			g_acl_entries[i].in_use = 0;
			pthread_mutex_unlock(&g_acl_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_acl_mutex);
	return -ENOENT;
}

int virt_acl_get_all_entries(struct virt_acl_entry *entries, int max_entries)
{
	int i, count = 0;

	if (!entries || max_entries <= 0)
		return 0;

	if (!g_acl_initialized)
		virt_acl_init();

	pthread_mutex_lock(&g_acl_mutex);
	for (i = 0; i < VIRT_ACL_MAX_RULES && count < max_entries; i++) {
		if (g_acl_entries[i].in_use) {
			memcpy(&entries[count], &g_acl_entries[i], sizeof(struct virt_acl_entry));
			count++;
		}
	}
	pthread_mutex_unlock(&g_acl_mutex);
	return count;
}

int virt_acl_save_policies(const char *path)
{
	FILE *fp;
	char dir[256];
	char *slash;
	int i;
	int count = 0;

	if (!path)
		path = VIRT_ACL_POLICY_FILE;

	/* Create parent directory if needed */
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0755);
	}

	fp = fopen(path, "w");
	if (!fp) {
		if (strcmp(path, VIRT_ACL_POLICY_FILE) == 0) {
			/* Fallback */
			path = VIRT_ACL_FALLBACK_FILE;
			snprintf(dir, sizeof(dir), "%s", path);
			slash = strrchr(dir, '/');
			if (slash) {
				*slash = '\0';
				mkdir(dir, 0755);
			}
			fp = fopen(path, "w");
		}
	}
	if (!fp)
		return -errno;

	fprintf(fp, "[\n");
	pthread_mutex_lock(&g_acl_mutex);
	for (i = 0; i < VIRT_ACL_MAX_RULES; i++) {
		if (g_acl_entries[i].in_use) {
			if (count > 0)
				fprintf(fp, ",\n");
			fprintf(fp, "  {\n");
			fprintf(fp, "    \"cid\": %u,\n", g_acl_entries[i].cid);
			fprintf(fp, "    \"tenant_name\": \"%s\",\n", g_acl_entries[i].tenant_name);
			fprintf(fp, "    \"quota_mb\": %u,\n", g_acl_entries[i].quota_mb);
			fprintf(fp, "    \"priority\": %u,\n", g_acl_entries[i].priority);
			fprintf(fp, "    \"caps\": %u\n", g_acl_entries[i].caps);
			fprintf(fp, "  }");
			count++;
		}
	}
	pthread_mutex_unlock(&g_acl_mutex);
	fprintf(fp, "\n]\n");
	fclose(fp);
	return 0;
}

int virt_acl_load_policies(const char *path)
{
	FILE *fp;
	char line[512];
	uint32_t cid = 0, quota = 1024, priority = 1, caps = AMBA_VIRT_ROLE_STANDARD;
	char name[64] = "guest";
	int in_obj = 0;

	if (!path)
		path = VIRT_ACL_POLICY_FILE;

	fp = fopen(path, "r");
	if (!fp) {
		fp = fopen(VIRT_ACL_FALLBACK_FILE, "r");
		if (!fp)
			return 0; /* No policy file present yet */
	}

	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		while (*p && isspace(*p)) p++;

		if (*p == '{') {
			in_obj = 1;
			cid = 0;
			quota = 1024;
			priority = 1;
			caps = AMBA_VIRT_ROLE_STANDARD;
			snprintf(name, sizeof(name), "guest");
			continue;
		}
		if (*p == '}') {
			if (in_obj && cid > 0) {
				virt_acl_set_rule(cid, name, caps, quota, priority);
			}
			in_obj = 0;
			continue;
		}

		if (strstr(p, "\"cid\"")) {
			sscanf(p, "\"cid\": %u", &cid);
		} else if (strstr(p, "\"tenant_name\"")) {
			char temp[64] = {0};
			if (sscanf(p, "\"tenant_name\": \"%63[^\"]\"", temp) == 1)
				snprintf(name, sizeof(name), "%s", temp);
		} else if (strstr(p, "\"quota_mb\"")) {
			sscanf(p, "\"quota_mb\": %u", &quota);
		} else if (strstr(p, "\"priority\"")) {
			sscanf(p, "\"priority\": %u", &priority);
		} else if (strstr(p, "\"caps\"")) {
			sscanf(p, "\"caps\": %u", &caps);
		}
	}

	fclose(fp);
	return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
