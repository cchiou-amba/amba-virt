/*
 * virt_iav_aperture.c
 *
 * The per-guest aux aperture: composition, node configuration and the table the broker reads. See
 * virt_iav_aperture.h.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <iav_ioctl.h>

#include "virt_iav_aperture.h"

#define PAGE_BYTES	4096ull

const uint32_t virt_iav_aperture_primary_pids[] = {
	IAV_PART_MASK, IAV_PART_WARP, IAV_PART_QPMATRIX, IAV_PART_CE_GO_MAP, IAV_PART_CANVAS_OVERLAY,
	IAV_PART_ARB_BLUR, IAV_PART_YUV_REMAP,
};
const unsigned int virt_iav_aperture_primary_count =
	sizeof(virt_iav_aperture_primary_pids) / sizeof(virt_iav_aperture_primary_pids[0]);

struct node_state {
	int attached;			/* the table below is what the guest of this index maps */
	struct amba_virt_iav_aux_cfg cfg;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;			/* sources registered with the node module */
static int g_init_rc = -ENODATA;	/* the last init result, for the status line */
static struct virt_iav_aperture_part g_parts[VIRT_IAV_APERTURE_MAX_PARTS];
static unsigned int g_nparts;
static uint64_t g_avoid[VIRT_IAV_APERTURE_MAX_AVOID];
static unsigned int g_navoid;
static struct node_state g_node[VIRT_IAV_APERTURE_NODES];

static int real_node_ioctl(void *ctx, uint32_t node, unsigned long cmd, void *arg)
{
	char path[64];
	int fd, rc = 0;

	(void)ctx;
	snprintf(path, sizeof(path), AMBA_VIRT_IAV_AUX_PATH_FMT, node);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (ioctl(fd, cmd, arg) != 0)
		rc = -errno;
	close(fd);
	return rc;
}

static struct virt_iav_aperture_ops g_ops = { NULL, real_node_ioctl };

void virt_iav_aperture_set_ops(const struct virt_iav_aperture_ops *ops)
{
	pthread_mutex_lock(&g_lock);
	if (ops && ops->node_ioctl)
		g_ops = *ops;
	else {
		g_ops.ctx = NULL;
		g_ops.node_ioctl = real_node_ioctl;
	}
	pthread_mutex_unlock(&g_lock);
}

void virt_iav_aperture_reset(void)
{
	pthread_mutex_lock(&g_lock);
	g_ready = 0;
	g_init_rc = -ENODATA;
	memset(g_parts, 0, sizeof(g_parts));
	g_nparts = 0;
	memset(g_avoid, 0, sizeof(g_avoid));
	g_navoid = 0;
	memset(g_node, 0, sizeof(g_node));
	g_ops.ctx = NULL;
	g_ops.node_ioctl = real_node_ioctl;
	pthread_mutex_unlock(&g_lock);
}

static int is_pow2(uint64_t v)
{
	return v && !(v & (v - 1));
}

static uint64_t round_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) / a * a;
}

int virt_iav_aperture_compose(const struct virt_iav_aperture_part *parts, unsigned int n, const uint64_t *avoid,
			      unsigned int navoid, uint64_t epoch, struct amba_virt_iav_aux_cfg *out)
{
	uint64_t off = 0, size, need;
	unsigned int k, i, j, count = 0;
	int again;

	if (!out || (n && !parts) || (navoid && !avoid) || epoch == 0)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	for (k = 0; k < virt_iav_aperture_primary_count; k++) {
		const struct virt_iav_aperture_part *p = NULL;

		for (i = 0; i < n; i++)
			if (parts[i].pid == virt_iav_aperture_primary_pids[k])
				p = &parts[i];
		if (!p || !p->len || p->base % PAGE_BYTES || p->len % PAGE_BYTES || p->base + p->len < p->base)
			continue;
		if (count >= AMBA_VIRT_IAV_AUX_MAX_RANGES)
			break;
		off = round_up(off, AMBA_VIRT_IAV_AUX_ALIGN);
		if (off > AMBA_VIRT_IAV_AUX_MAX_SIZE || p->len > AMBA_VIRT_IAV_AUX_MAX_SIZE - off)
			return -E2BIG;
		out->range[count].phys = p->base;
		out->range[count].len = p->len;
		out->range[count].offset = off;
		out->range[count].pid = p->pid;
		count++;
		off += p->len;
	}
	if (count == 0)
		return 0;
	need = off + PAGE_BYTES;	/* the header page after the last range */
	size = AMBA_VIRT_IAV_AUX_MIN_SIZE;
	while (size < need)
		size <<= 1;
	do {
		again = 0;
		for (j = 0; j < navoid; j++) {
			if (avoid[j] == size) {
				size <<= 1;
				again = 1;
			}
		}
	} while (again && size <= AMBA_VIRT_IAV_AUX_MAX_SIZE);
	if (size > AMBA_VIRT_IAV_AUX_MAX_SIZE || !is_pow2(size))
		return -E2BIG;
	out->size = size;
	out->epoch = epoch;
	out->count = count;
	return (int)count;
}

/* Same configuration: everything the node holds that the server chose (the mapping count is not a choice). */
static int same_cfg(const struct amba_virt_iav_aux_cfg *a, const struct amba_virt_iav_aux_cfg *b)
{
	return a->size == b->size && a->epoch == b->epoch && a->count == b->count &&
	       memcmp(a->range, b->range, sizeof(a->range)) == 0;
}

int virt_iav_aperture_init(const struct virt_iav_aperture_part *parts, unsigned int n, const uint64_t *avoid,
			   unsigned int navoid)
{
	struct amba_virt_iav_aux_sources src;
	unsigned int i;
	int rc;

	if (n > VIRT_IAV_APERTURE_MAX_PARTS || navoid > VIRT_IAV_APERTURE_MAX_AVOID || (n && !parts) ||
	    (navoid && !avoid))
		return -EINVAL;
	memset(&src, 0, sizeof(src));
	for (i = 0; i < n; i++) {
		src.src[i].phys = parts[i].base;
		src.src[i].len = parts[i].len;
		src.src[i].pid = parts[i].pid;
	}
	src.count = n;
	pthread_mutex_lock(&g_lock);
	rc = g_ops.node_ioctl(g_ops.ctx, 0, AMBA_VIRT_IAV_WIN_IOC_AUX_SOURCES, &src);
	g_init_rc = rc;
	g_ready = rc == 0;
	if (rc == 0) {
		memcpy(g_parts, parts, n * sizeof(*parts));
		g_nparts = n;
		if (navoid)
			memcpy(g_avoid, avoid, navoid * sizeof(*avoid));
		g_navoid = navoid;
	}
	pthread_mutex_unlock(&g_lock);
	return rc;
}

/* Clear node `index` when nothing maps it. Called with g_lock held. */
static void clear_if_idle(uint32_t index)
{
	struct amba_virt_iav_aux_cfg have;

	memset(&have, 0, sizeof(have));
	if (g_ops.node_ioctl(g_ops.ctx, index, AMBA_VIRT_IAV_WIN_IOC_AUX_GET, &have) != 0)
		return;
	if (have.size && have.maps == 0)
		g_ops.node_ioctl(g_ops.ctx, index, AMBA_VIRT_IAV_WIN_IOC_CLEAR, NULL);
}

int virt_iav_aperture_attach(uint32_t index, uint64_t epoch, int primary, uint64_t *size)
{
	struct amba_virt_iav_aux_cfg want, have;
	int rc, n;

	if (size)
		*size = 0;
	if (index >= VIRT_IAV_APERTURE_NODES || !size)
		return -EINVAL;
	pthread_mutex_lock(&g_lock);
	memset(&g_node[index], 0, sizeof(g_node[index]));
	if (!g_ready) {
		rc = -ENODEV;
		goto out;
	}
	if (!primary || epoch == 0 || epoch > 0xffffffffull) {
		/* a consumer (or a lease the wire cannot name) gets no aperture: leave nothing of another guest's */
		clear_if_idle(index);
		rc = 0;
		goto out;
	}
	n = virt_iav_aperture_compose(g_parts, g_nparts, g_avoid, g_navoid, epoch, &want);
	if (n <= 0) {
		clear_if_idle(index);
		rc = n;
		goto out;
	}
	memset(&have, 0, sizeof(have));
	rc = g_ops.node_ioctl(g_ops.ctx, index, AMBA_VIRT_IAV_WIN_IOC_AUX_GET, &have);
	if (rc != 0)
		goto out;
	if (!(have.size && same_cfg(&have, &want))) {
		if (have.maps) {
			/* another composition is still mapped: this guest runs without an aperture */
			rc = -EBUSY;
			goto out;
		}
		rc = g_ops.node_ioctl(g_ops.ctx, index, AMBA_VIRT_IAV_WIN_IOC_AUX_CONFIG, &want);
		if (rc != 0)
			goto out;
	}
	g_node[index].attached = 1;
	g_node[index].cfg = want;
	*size = want.size;
	rc = 0;
out:
	pthread_mutex_unlock(&g_lock);
	return rc;
}

void virt_iav_aperture_detach(uint32_t index)
{
	if (index >= VIRT_IAV_APERTURE_NODES)
		return;
	pthread_mutex_lock(&g_lock);
	memset(&g_node[index], 0, sizeof(g_node[index]));
	if (g_ready)
		clear_if_idle(index);
	pthread_mutex_unlock(&g_lock);
}

int virt_iav_aperture_adopt(uint32_t index, uint64_t epoch, int primary)
{
	struct amba_virt_iav_aux_cfg want, have;
	int adopted = 0, n;

	if (index >= VIRT_IAV_APERTURE_NODES)
		return 0;
	pthread_mutex_lock(&g_lock);
	memset(&g_node[index], 0, sizeof(g_node[index]));
	if (!g_ready)
		goto out;
	memset(&have, 0, sizeof(have));
	if (g_ops.node_ioctl(g_ops.ctx, index, AMBA_VIRT_IAV_WIN_IOC_AUX_GET, &have) != 0 || have.size == 0)
		goto out;
	n = (primary && epoch && epoch <= 0xffffffffull) ?
	    virt_iav_aperture_compose(g_parts, g_nparts, g_avoid, g_navoid, epoch, &want) : 0;
	if (n > 0 && same_cfg(&have, &want)) {
		g_node[index].attached = 1;
		g_node[index].cfg = want;
		adopted = 1;
	} else if (have.maps == 0) {
		g_ops.node_ioctl(g_ops.ctx, index, AMBA_VIRT_IAV_WIN_IOC_CLEAR, NULL);
	}
out:
	pthread_mutex_unlock(&g_lock);
	return adopted;
}

int virt_iav_aperture_get(uint32_t index, struct virt_iav_aperture_view *out)
{
	uint32_t i;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (index >= VIRT_IAV_APERTURE_NODES)
		return -EINVAL;
	pthread_mutex_lock(&g_lock);
	if (g_node[index].attached) {
		const struct amba_virt_iav_aux_cfg *c = &g_node[index].cfg;

		out->size = c->size;
		out->epoch = c->epoch;
		out->count = c->count;
		for (i = 0; i < c->count && i < VIRT_IAV_APERTURE_MAX_PARTS; i++) {
			out->e[i].pid = c->range[i].pid;
			out->e[i].base = c->range[i].phys;
			out->e[i].len = c->range[i].len;
			out->e[i].offset = c->range[i].offset;
		}
	}
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int virt_iav_aperture_format(char *buf, size_t len)
{
	unsigned int i;
	int n, m;

	if (!buf || len == 0)
		return -1;
	pthread_mutex_lock(&g_lock);
	n = snprintf(buf, len, "APERTURE state=%s sources=%u nodes=", g_ready ? "ready" : "off", g_nparts);
	for (i = 0; i < VIRT_IAV_APERTURE_NODES && n >= 0 && (size_t)n < len; i++) {
		if (!g_node[i].attached)
			continue;
		m = snprintf(buf + n, len - (size_t)n, "%s%u:0x%llx/%u", n && buf[n - 1] != '=' ? "," : "", i,
			     (unsigned long long)g_node[i].cfg.size, g_node[i].cfg.count);
		if (m < 0)
			n = -1;
		else
			n += m;
	}
	if (n >= 0 && (size_t)n < len) {
		m = snprintf(buf + n, len - (size_t)n, "%s init=%d\n", buf[n - 1] == '=' ? "-" : "", g_init_rc);
		n = m < 0 ? -1 : n + m;
	}
	pthread_mutex_unlock(&g_lock);
	if (n < 0 || (size_t)n >= len)
		return -1;
	return n;
}
