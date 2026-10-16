/*
 * iav_broker_server.c
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include <iav_ioctl.h>

#include "amba_virt.h"
#include "amba_virt_iav_win.h"
#include "iav_broker_server.h"
#include "iav_slice_rpc.h"
#include "virt_camera_policy.h"
#include "virt_iav_aperture.h"
#include "virt_low_lease.h"
#include "virt_slice_arena.h"
#include "virt_slice_lease.h"

#define WORKERS			4
#define QUEUE_DEPTH		32
#define LIVENESS_PERIOD_MS	2000
#define IDLE_RELEASE_MS		300000ull
/* Window pinning: re-query the windowed partitions every this many liveness periods (every period while stale). */
#define RECHECK_PERIODS		15u

struct job {
	uint32_t cid;
	uint32_t seq;
	size_t len;
	uint8_t data[AMBA_VIRT_MAX_MSG];
};

static struct iav_broker g_broker;
static int g_active;
static int g_dev_fd = -1;
static int g_stop;
static pthread_t g_workers[WORKERS];
static pthread_t g_liveness;
static int g_threads_started;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qcond = PTHREAD_COND_INITIALIZER;
static struct job g_queue[QUEUE_DEPTH];
static unsigned int g_qhead, g_qcount;

/* ---- the real driver ---- */

static int real_open(void *ctx)
{
	(void)ctx;
	return open(IAV_BROKER_DEVICE_PATH, O_RDWR | O_CLOEXEC);
}

static int real_close(void *ctx, int fd)
{
	(void)ctx;
	return close(fd);
}

static int real_ioctl(void *ctx, int fd, unsigned long cmd, unsigned long arg)
{
	(void)ctx;
	return ioctl(fd, cmd, arg);
}

/*
 * The host's read of the decode partition for the JPEG header check: a read-only mapping on a descriptor of the
 * server's own, made once. The guest writes the window as Normal-NC or device memory, so the host's cached lines of
 * the range are invalidated (IAV_IOC_SYNC_CACHE_MEM_PART) before each read.
 */
static pthread_mutex_t g_rd_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_rd_fd = -1;
static const uint8_t *g_rd_map;
static uint64_t g_rd_len;

static int real_read_dec(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct iav_sync_cache_mem_part c;
	int rc = 0;

	(void)ctx;
	pthread_mutex_lock(&g_rd_lock);
	if (g_rd_map == NULL) {
		struct iav_querymem q;
		void *m;

		g_rd_fd = open(IAV_BROKER_DEVICE_PATH, O_RDWR | O_CLOEXEC);
		memset(&q, 0, sizeof(q));
		q.mid = IAV_MEM_PARTITION;
		q.arg.partition.pid = IAV_PART_DEC_BSB;
		if (g_rd_fd < 0 || ioctl(g_rd_fd, IAV_IOC_QUERY_MEMBLOCK, &q) != 0 || q.arg.partition.mem.length == 0) {
			rc = -EIO;
			goto out;
		}
		m = mmap(NULL, q.arg.partition.mem.length, PROT_READ, MAP_SHARED, g_rd_fd, (off_t)q.arg.partition.mem.addr);
		if (m == MAP_FAILED) {
			rc = -EIO;
			goto out;
		}
		g_rd_map = m;
		g_rd_len = q.arg.partition.mem.length;
	}
	if (off > g_rd_len || len > g_rd_len - off) {
		rc = -EINVAL;
		goto out;
	}
	memset(&c, 0, sizeof(c));
	c.pid = IAV_PART_DEC_BSB;
	c.opt = IAV_CACHE_INVALIDATE;
	c.offset = (unsigned long)off;
	c.length = (unsigned long)len;
	if (ioctl(g_rd_fd, IAV_IOC_SYNC_CACHE_MEM_PART, &c) != 0) {
		rc = -EIO;
		goto out;
	}
	memcpy(buf, g_rd_map + off, len);
out:
	pthread_mutex_unlock(&g_rd_lock);
	return rc;
}

static int real_poll(void *ctx, int fd, int events, int timeout_ms, int *revents)
{
	struct pollfd p;
	int n;

	(void)ctx;
	p.fd = fd;
	p.events = (short)events;
	p.revents = 0;
	n = poll(&p, 1, timeout_ms);
	*revents = p.revents;
	return n;
}

static void real_sleep_ms(void *ctx, unsigned int ms)
{
	struct timespec ts;

	(void)ctx;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

static uint32_t real_random32(void *ctx)
{
	uint32_t v = 0;

	(void)ctx;
	if (getrandom(&v, sizeof(v), 0) != (ssize_t)sizeof(v)) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		v = (uint32_t)(ts.tv_nsec ^ (ts.tv_sec << 7) ^ (uint32_t)getpid());
	}
	return v;
}

static uint64_t real_now_ms(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static uint64_t next_pow2(uint64_t v)
{
	uint64_t p = 4096;

	while (p < v && p != 0)
		p <<= 1;
	return p;
}

/*
 * Tell the host window module (amba_virt_iav_win) what each window covers, so the hypervisor can
 * map the node into a guest as an ivshmem BAR. Done before any guest VM starts. A node that is
 * already configured the same way is left alone (the server restarts while VMs hold the maps). A
 * window whose node cannot be configured is dropped, so no guest is told it exists.
 */
static int window_node_config(const struct iav_broker_window *w, const char *path, uint32_t *flags)
{
	struct amba_virt_iav_win_cfg want, have;
	int fd, rc = 0;

	*flags = 0;
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "iav_broker: window %u: cannot open %s: %s\n", w->id, path, strerror(errno));
		return -errno;
	}
	memset(&want, 0, sizeof(want));
	want.phys = w->base;
	want.len = w->part_len;
	want.size = w->size;
	memset(&have, 0, sizeof(have));
	if (ioctl(fd, AMBA_VIRT_IAV_WIN_IOC_GET, &have) == 0 && have.phys == want.phys &&
	    have.len == want.len && have.size == want.size) {
		*flags = have.flags;
		close(fd);
		return 0;
	}
	if (ioctl(fd, AMBA_VIRT_IAV_WIN_IOC_CONFIG, &want) != 0) {
		rc = -errno;
		fprintf(stderr, "iav_broker: window %u: cannot configure %s (phys 0x%llx len 0x%llx size 0x%llx): %s\n",
			w->id, path, (unsigned long long)want.phys, (unsigned long long)want.len,
			(unsigned long long)want.size, strerror(errno));
	} else {
		/* The module says how it maps the node (AMBA_VIRT_IAV_WIN_F_NORMAL_NC); an older one says nothing. */
		memset(&have, 0, sizeof(have));
		if (ioctl(fd, AMBA_VIRT_IAV_WIN_IOC_GET, &have) == 0)
			*flags = have.flags;
	}
	close(fd);
	return rc;
}

/* The one set of nodes every attached guest's QEMU maps. */
static int window_node_ready(const struct iav_broker_window *w, uint32_t *flags)
{
	const char *path;

	switch (w->id) {
	case AMBA_VIRT_IAV_WIN_DSP:
		path = AMBA_VIRT_IAV_WIN_DSP_PATH;
		break;
	case AMBA_VIRT_IAV_WIN_BSB:
		path = AMBA_VIRT_IAV_WIN_BSB_PATH;
		break;
	case AMBA_VIRT_IAV_WIN_DEC:
		path = AMBA_VIRT_IAV_WIN_DEC_PATH;
		break;
	case AMBA_VIRT_IAV_WIN_OVL:
		path = AMBA_VIRT_IAV_WIN_OVL_PATH;
		break;
	default:
		return -EINVAL;
	}
	return window_node_config(w, path, flags);
}

/* ---- the aux apertures (plan_iav_usb_yolo Envelope F) ---- */

static int aux_attach_hook(uint32_t index, uint64_t epoch, int primary, uint64_t *size)
{
	int rc = virt_iav_aperture_attach(index, epoch, primary, size);

	if (rc != 0 && primary)
		fprintf(stderr, "iav_broker: aux aperture %u not attached (%d); the guest runs without it\n", index, rc);
	return rc;
}

static void adopt_cb(uint32_t index, uint64_t epoch, int primary, int ticketed, void *ctx)
{
	unsigned int *adopted = ctx;

	if (ticketed && virt_iav_aperture_adopt(index, epoch, primary))
		(*adopted)++;
}

/*
 * The partitions the apertures carry (QUERY_MEMBLOCK now), registered with the window module as its sources and with the
 * broker for pinning; then what each node already holds for a running guest is adopted, and reserves from here on
 * attach apertures. A window module without aperture nodes (an older image) leaves every guest without one.
 */
static void setup_apertures(int fd, const struct iav_broker_window *w, unsigned int nw)
{
	struct virt_iav_aperture_part parts[VIRT_IAV_APERTURE_MAX_PARTS];
	struct iav_broker_aux_entry src[IAV_BROKER_AUX_MAX];
	uint64_t avoid[VIRT_IAV_APERTURE_MAX_AVOID];
	unsigned int n = 0, navoid = 0, i, adopted = 0;
	int rc;

	for (i = 0; i < nw && navoid < VIRT_IAV_APERTURE_MAX_AVOID; i++)
		if (w[i].size == w[i].part_len)
			avoid[navoid++] = w[i].size;	/* recognized by size alone by an older guest module */
	for (i = 0; i < virt_iav_aperture_primary_count && n < VIRT_IAV_APERTURE_MAX_PARTS; i++) {
		struct iav_querymem q;

		memset(&q, 0, sizeof(q));
		q.mid = IAV_MEM_PARTITION;
		q.arg.partition.pid = virt_iav_aperture_primary_pids[i];
		if (ioctl(fd, IAV_IOC_QUERY_MEMBLOCK, &q) != 0 || q.arg.partition.mem.length == 0)
			continue;
		if (q.arg.partition.mem.addr % 4096 || q.arg.partition.mem.length % 4096) {
			fprintf(stderr, "iav_broker: partition %u (0x%llx, 0x%llx) is not page aligned: no aperture entry\n",
				virt_iav_aperture_primary_pids[i], (unsigned long long)q.arg.partition.mem.addr,
				(unsigned long long)q.arg.partition.mem.length);
			continue;
		}
		parts[n].pid = virt_iav_aperture_primary_pids[i];
		parts[n].base = q.arg.partition.mem.addr;
		parts[n].len = q.arg.partition.mem.length;
		n++;
	}
	rc = virt_iav_aperture_init(parts, n, avoid, navoid);
	if (rc != 0) {
		printf("iav_broker: no aux apertures (%d): the window module has no aperture nodes or refused the sources\n", rc);
		return;
	}
	memset(src, 0, sizeof(src));
	for (i = 0; i < n && i < IAV_BROKER_AUX_MAX; i++) {
		src[i].pid = parts[i].pid;
		src[i].base = parts[i].base;
		src[i].len = parts[i].len;
	}
	if (iav_broker_set_aux_sources(&g_broker, src, n) != 0) {
		printf("iav_broker: the aperture sources are not usable for pinning: no aux apertures\n");
		virt_iav_aperture_reset();
		return;
	}
	virt_slice_lease_foreach(adopt_cb, &adopted);
	virt_slice_lease_set_aux_hooks(aux_attach_hook, virt_iav_aperture_detach);
	printf("iav_broker: aux apertures ready: %u partitions, %u adopted from running guests\n", n, adopted);
}

/*
 * The windows the guests may map: the DSP partition, the bitstream buffer, the decode partition and the overlay partition, as the driver
 * has them. The decode partition exists only when the pipeline has a decoder channel (the driver allocates it
 * when preview is enabled), and its window only when the module has the decode nodes; without either, decode is
 * refused for every guest and the other two windows are unaffected.
 */
static int set_windows_from_driver(void)
{
	struct iav_broker_window w[4];
	unsigned int n = 0;
	unsigned int pids[4] = { IAV_PART_DSP, IAV_PART_BSB, IAV_PART_DEC_BSB, IAV_PART_OVERLAY };
	unsigned int ids[4] = { AMBA_VIRT_IAV_WIN_DSP, AMBA_VIRT_IAV_WIN_BSB, AMBA_VIRT_IAV_WIN_DEC, AMBA_VIRT_IAV_WIN_OVL };
	int fd, i, rc;
	int all_nc = 1;

	fd = open(IAV_BROKER_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	memset(w, 0, sizeof(w));
	for (i = 0; i < 4; i++) {
		struct iav_querymem q;

		memset(&q, 0, sizeof(q));
		q.mid = IAV_MEM_PARTITION;
		q.arg.partition.pid = pids[i];
		if (ioctl(fd, IAV_IOC_QUERY_MEMBLOCK, &q) != 0)
			continue;
		if (q.arg.partition.mem.length == 0) {
			if (ids[i] == AMBA_VIRT_IAV_WIN_DEC)
				fprintf(stderr, "iav_broker: no decode partition, so no decode window (the pipeline has no decoder channel)\n");
			continue;
		}
		w[n].id = ids[i];
		w[n].base = q.arg.partition.mem.addr;
		w[n].part_len = q.arg.partition.mem.length;
		w[n].size = next_pow2(q.arg.partition.mem.length);
		w[n].ring = 1;
		w[n].pid = pids[i];
		{
			uint32_t flags = 0;

			if (window_node_ready(&w[n], &flags) == 0) {
				all_nc = all_nc && (flags & AMBA_VIRT_IAV_WIN_F_NORMAL_NC);
				n++;
			}
		}
	}
	/* Unaligned access is legal for a guest only when every window it may map is Normal-NC. */
	iav_broker_set_host_features(&g_broker, (n > 0 && all_nc) ? AMBA_VIRT_IAV_F_NC_WINDOWS : 0);
	printf("iav_broker: %u windows; the window nodes %s Normal-NC mappings\n", n,
	       (n > 0 && all_nc) ? "report" : "do not report");
	rc = iav_broker_set_windows(&g_broker, w, n);
	if (rc >= 0) {
		/*
		 * The nodes are configured and the broker knows the windows: tell the camera policy which
		 * windows exist and when, so a guest's reserve can attach them. This runs once per server
		 * run, before the server reports ready.
		 */
		struct virt_camera_window cw[4];
		unsigned int k;

		for (k = 0; k < n && k < 4; k++) {
			cw[k].id = w[k].id;
			cw[k].size = w[k].size;
		}
		virt_camera_policy_set_windows(cw, k, virt_camera_policy_now_ms());
		setup_apertures(fd, w, n);
	}
	close(fd);
	return rc;
}

/* ---- sending responses ---- */

static void send_to_guest(uint32_t cid, uint32_t seq, const void *payload, size_t len)
{
	struct amba_virt_xfer xfer;
	struct amba_virt_msg msg;

	if (len > AMBA_VIRT_MAX_MSG - sizeof(msg))
		return;
	memset(&xfer, 0, sizeof(xfer));
	memset(&msg, 0, sizeof(msg));
	msg.type = AMBA_VIRT_MSG_IAV_RESP;
	msg.seq = seq;
	memcpy(xfer.data, &msg, sizeof(msg));
	memcpy(xfer.data + sizeof(msg), payload, len);
	xfer.len = (uint32_t)(sizeof(msg) + len);
	xfer.client_cid = cid;
	if (ioctl(g_dev_fd, AMBA_VIRT_IOC_SEND, &xfer) < 0 && (errno == ENOTCONN || errno == EPIPE ||
								errno == ECONNRESET))
		iav_broker_client_gone(&g_broker, cid);
}

static void send_refusal(uint32_t cid, uint32_t seq, int err)
{
	struct amba_virt_iav_resp r;

	memset(&r, 0, sizeof(r));
	r.proto = AMBA_VIRT_IAV_PROTO;
	r.status = -err;
	r.rc = -1;
	send_to_guest(cid, seq, &r, sizeof(r));
}

/* ---- workers ---- */

static void *worker_main(void *arg)
{
	static __thread uint8_t resp[AMBA_VIRT_MAX_MSG];

	(void)arg;
	for (;;) {
		struct job *j;
		struct job local;
		size_t resp_len = 0;

		pthread_mutex_lock(&g_qlock);
		while (g_qcount == 0 && !g_stop)
			pthread_cond_wait(&g_qcond, &g_qlock);
		if (g_qcount == 0 && g_stop) {
			pthread_mutex_unlock(&g_qlock);
			return NULL;
		}
		j = &g_queue[g_qhead];
		local = *j;
		g_qhead = (g_qhead + 1) % QUEUE_DEPTH;
		g_qcount--;
		pthread_mutex_unlock(&g_qlock);

		if (iav_broker_handle(&g_broker, local.cid, local.data, local.len, resp,
				      sizeof(resp) - sizeof(struct amba_virt_msg), &resp_len) == 0)
			send_to_guest(local.cid, local.seq, resp, resp_len);
		else
			send_refusal(local.cid, local.seq, EMSGSIZE);
	}
}

/* Release the sessions of guests that are no longer connected, and idle ones. */
static void *liveness_main(void *arg)
{
	unsigned int periods = 0;

	(void)arg;
	while (!g_stop) {
		struct amba_virt_guest_list gl;
		uint32_t held[IAV_BROKER_MAX_CIDS];
		unsigned int n, i, j;

		real_sleep_ms(NULL, LIVENESS_PERIOD_MS);
		if (g_stop)
			break;
		memset(&gl, 0, sizeof(gl));
		if (ioctl(g_dev_fd, AMBA_VIRT_IOC_LIST_GUESTS, &gl) == 0) {
			n = iav_broker_list_cids(&g_broker, held, IAV_BROKER_MAX_CIDS);
			for (i = 0; i < n; i++) {
				int alive = 0;

				for (j = 0; j < gl.count && j < AMBA_VIRT_MAX_GUESTS; j++)
					alive = alive || gl.cids[j] == held[i];
				if (!alive)
					iav_broker_client_gone(&g_broker, held[i]);
			}
		}
		iav_broker_reap_idle(&g_broker, IDLE_RELEASE_MS);
		if (++periods >= RECHECK_PERIODS || iav_broker_windows_stale(&g_broker)) {
			int was = iav_broker_windows_stale(&g_broker);
			int now = iav_broker_recheck_windows(&g_broker);

			periods = 0;
			if (now >= 0 && now != was)
				fprintf(stderr, "iav_broker: windows %s\n", now ? "STALE: a windowed partition moved; "
					"window-backed calls are refused until it is back or the server restarts with the guests "
					"stopped" : "match their partitions again");
		}
	}
	return NULL;
}

/* ---- public ---- */

static int g_primary_slice = -1;

void iav_broker_server_set_primary_slice(int slice)
{
	g_primary_slice = slice;
}

static int real_slice_of(void *ctx, uint32_t cid, uint32_t *window)
{
	uint32_t usable = 0;

	(void)ctx;
	if (!window)
		return -EINVAL;
	return iav_slice_rpc_held(cid, window, &usable);
}

/* The guest's aux aperture: the one attached to its Cavalry lease index (kernel binding of its CID). */
static int real_aperture_of(void *ctx, uint32_t cid, struct iav_broker_aperture *out)
{
	struct amba_virt_binding_list bindings;
	struct virt_iav_aperture_view v;
	uint32_t i, k;

	(void)ctx;
	memset(out, 0, sizeof(*out));
	if (g_dev_fd < 0)
		return -ENODEV;
	memset(&bindings, 0, sizeof(bindings));
	if (ioctl(g_dev_fd, AMBA_VIRT_IOC_GET_BINDINGS, &bindings) < 0)
		return -errno;
	for (i = 0; i < bindings.count && i < 8; i++) {
		if (bindings.entries[i].cid != cid)
			continue;
		if (virt_iav_aperture_get(bindings.entries[i].slice.index, &v) != 0)
			return -ENOENT;
		out->size = v.size;
		out->epoch = v.epoch;
		out->count = v.count < IAV_BROKER_AUX_MAX ? v.count : IAV_BROKER_AUX_MAX;
		for (k = 0; k < out->count; k++) {
			out->e[k].pid = v.e[k].pid;
			out->e[k].base = v.e[k].base;
			out->e[k].len = v.e[k].len;
			out->e[k].offset = v.e[k].offset;
		}
		return 0;
	}
	return -ENOENT;
}

static int real_arena_begin(void *ctx, uint32_t index)
{
	(void)ctx;
	return virt_slice_arena_begin(index);
}

static void real_arena_end(void *ctx, uint32_t index)
{
	(void)ctx;
	virt_slice_arena_end(index);
}

static void apply_guest_pool(uint64_t base, uint32_t partition_id)
{
	struct virt_iav_guest_pool pool;

	memset(&pool, 0, sizeof(pool));
	pool.partition_id = partition_id;
	pool.parent_base = base;
	pool.parent_bytes = VIRT_LOW_LEASE_COUNT * VIRT_LOW_LEASE_BYTES;
	pool.slice_count = VIRT_LOW_LEASE_COUNT;
	pool.slice_bytes = VIRT_LOW_LEASE_BYTES;
	iav_broker_set_guest_pool(&g_broker, &pool);
}

void iav_broker_server_set_guest_pool(uint64_t base, uint32_t partition_id)
{
	if (!g_active || base == 0)
		return;
	apply_guest_pool(base, partition_id);
}

/*
 * The primary guest is decided by ticket (G19): the kernel's binding gives the caller's Cavalry index, the lease on
 * that index gives the ticket and the role it held at the guest's start, and the role table gives the role now. The
 * guest is primary only while both say primary and no other lease names the same ticket. A lease that carries no
 * ticket (reserved by RESERVE_SLICE, or recorded by a server that did not keep tickets, as when a newer server
 * takes over leases at run time) keeps the legacy rule: the guest bound to primary-slice.
 */
static int primary_by_ticket(uint32_t index, int *decided)
{
	char ticket[VIRT_CAMERA_TICKET_MAX + 1];
	int lease_primary = 0;

	*decided = 0;
	if (virt_slice_lease_ticket_of_index(index, ticket, sizeof(ticket), &lease_primary) != 0 || !ticket[0])
		return 0;
	*decided = 1;
	return lease_primary && virt_iav_role_of(ticket) == VIRT_IAV_ROLE_PRIMARY &&
	       virt_slice_lease_ticket_count(ticket) == 1;
}

static int real_is_primary(void *ctx, uint32_t cid)
{
	struct amba_virt_binding_list bindings;
	uint32_t i;

	(void)ctx;
	if (g_dev_fd < 0)
		return 0;
	memset(&bindings, 0, sizeof(bindings));
	if (ioctl(g_dev_fd, AMBA_VIRT_IOC_GET_BINDINGS, &bindings) < 0)
		return 0;
	for (i = 0; i < bindings.count && i < 8; i++) {
		if (bindings.entries[i].cid == cid) {
			int decided, primary = primary_by_ticket(bindings.entries[i].slice.index, &decided);

			if (decided)
				return primary;
			return g_primary_slice >= 0 && bindings.entries[i].slice.index == (uint32_t)g_primary_slice;
		}
	}
	return 0;
}

static int iav_status_line(char *buf, size_t len)
{
	int n, m;

	if (!g_active)
		return -1;
	n = iav_broker_format_status(&g_broker, buf, len);
	if (n < 0)
		return n;
	/* the private slices behind the arenas: quarantine, in-flight calls, scrubs */
	m = virt_slice_arena_format(buf + n, len - (size_t)n);
	if (m > 0)
		n += m;
	/* the aux apertures attached now */
	m = virt_iav_aperture_format(buf + n, len - (size_t)n);
	return m < 0 ? n : n + m;
}

int iav_broker_server_init(int dev_fd, enum iav_profile profile)
{
	struct iav_broker_ops ops;
	struct iav_broker_cfg cfg;
	int rc, i;

	if (g_active)
		return 0;
	if (access(IAV_BROKER_DEVICE_PATH, F_OK) != 0)
		return -ENODEV;

	memset(&ops, 0, sizeof(ops));
	ops.open = real_open;
	ops.close = real_close;
	ops.ioctl = real_ioctl;
	ops.poll = real_poll;
	ops.sleep_ms = real_sleep_ms;
	ops.random32 = real_random32;
	ops.now_ms = real_now_ms;
	ops.is_primary = real_is_primary;
	ops.slice_of = real_slice_of;
	ops.arena_begin = real_arena_begin;
	ops.arena_end = real_arena_end;
	ops.aperture_of = real_aperture_of;
	ops.read_dec = real_read_dec;
	memset(&cfg, 0, sizeof(cfg));
	cfg.profile = profile;
	cfg.rate_per_sec = 20000;
	cfg.rate_burst = 5000;

	rc = iav_broker_init(&g_broker, &ops, &cfg);
	if (rc < 0)
		return rc;
	rc = set_windows_from_driver();
	if (rc < 0)
		fprintf(stderr, "iav_broker: no usable windows from the driver (%d); mapping stays refused\n", rc);

	g_dev_fd = dev_fd;
	g_stop = 0;
	g_qhead = g_qcount = 0;
	for (i = 0; i < WORKERS; i++) {
		if (pthread_create(&g_workers[i], NULL, worker_main, NULL) != 0)
			break;
	}
	if (i != WORKERS) {
		g_stop = 1;
		pthread_cond_broadcast(&g_qcond);
		while (i-- > 0)
			pthread_join(g_workers[i], NULL);
		iav_broker_destroy(&g_broker);
		return -EAGAIN;
	}
	if (pthread_create(&g_liveness, NULL, liveness_main, NULL) != 0) {
		g_stop = 1;
		pthread_cond_broadcast(&g_qcond);
		for (i = 0; i < WORKERS; i++)
			pthread_join(g_workers[i], NULL);
		iav_broker_destroy(&g_broker);
		return -EAGAIN;
	}
	g_threads_started = 1;
	g_active = 1;
	virt_camera_policy_set_iav_status(iav_status_line);
	{
		uint64_t base = 0;
		uint32_t pid = 0;

		if (virt_low_lease_geometry(&base, &pid) == 0)
			apply_guest_pool(base, pid);
	}
	printf("iav_broker: started, profile %s\n",
	       profile == IAV_PROFILE_QUALIFICATION ? "qualification" : "consumer");
	return 0;
}

void iav_broker_server_shutdown(void)
{
	int i;

	if (!g_active)
		return;
	g_active = 0;
	virt_camera_policy_set_iav_status(NULL);
	pthread_mutex_lock(&g_qlock);
	g_stop = 1;
	pthread_cond_broadcast(&g_qcond);
	pthread_mutex_unlock(&g_qlock);
	if (g_threads_started) {
		for (i = 0; i < WORKERS; i++)
			pthread_join(g_workers[i], NULL);
		pthread_join(g_liveness, NULL);
		g_threads_started = 0;
	}
	iav_broker_destroy(&g_broker);
}

int iav_broker_server_active(void)
{
	return g_active;
}

int iav_broker_server_submit(uint32_t cid, uint32_t seq, const void *payload, size_t len)
{
	struct job *j;
	int queued = 0;

	if (!g_active)
		return -ENODEV;
	if (len > AMBA_VIRT_MAX_MSG)
		return -EMSGSIZE;
	pthread_mutex_lock(&g_qlock);
	if (g_qcount < QUEUE_DEPTH) {
		j = &g_queue[(g_qhead + g_qcount) % QUEUE_DEPTH];
		j->cid = cid;
		j->seq = seq;
		j->len = len;
		memcpy(j->data, payload, len);
		g_qcount++;
		queued = 1;
		pthread_cond_signal(&g_qcond);
	}
	pthread_mutex_unlock(&g_qlock);
	if (!queued)
		send_refusal(cid, seq, EBUSY);
	return 0;
}
