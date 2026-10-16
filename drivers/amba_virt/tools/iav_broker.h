/*
 * iav_broker.h
 *
 * The IAV broker: serves a guest's virtual /dev/iav from the real IAV driver. It runs inside
 * amba-virt-server, which is a single point of failure by design, so every value that arrives from
 * a guest is checked before the real driver sees it. The broker never trusts, dereferences or
 * forwards a guest pointer. Handles are opaque, bound to the caller's kernel-reported CID, and
 * carry a generation and a broker epoch so a stale handle and a foreign handle fail differently.
 *
 * The real driver, the clock and the randomness source are reached through iav_broker_ops, so
 * the broker is tested on a build host against a mock driver that checks what it is given.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAV_BROKER_H
#define IAV_BROKER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "amba_virt_iav.h"
#include "iav_policy.h"
#include "virt_iav_guest_slice.h"

#define IAV_BROKER_MAX_SESSIONS		64
#define IAV_BROKER_MAX_CIDS		16
#define IAV_BROKER_DEFAULT_PER_CID	16u
#define IAV_BROKER_DEFAULT_BLOCK_MS	2000u

/* One partition in a guest's aux aperture: where it is, how much of it the guest maps, and where in the BAR. */
#define IAV_BROKER_AUX_MAX	16u

struct iav_broker_aux_entry {
	uint32_t pid;		/* the IAV partition */
	uint64_t base;		/* the address QUERY_MEMBLOCK reports for it */
	uint64_t len;		/* the bytes the entry covers (what QUERY_MEMBLOCK reports) */
	uint64_t offset;	/* its offset in the aperture BAR */
};

struct iav_broker_aperture {
	uint64_t size;		/* the BAR */
	uint64_t epoch;		/* what the aperture's header carries */
	uint32_t count;
	struct iav_broker_aux_entry e[IAV_BROKER_AUX_MAX];
};

/* The real driver and the environment, replaceable for tests. */
struct iav_broker_ops {
	void *ctx;
	/* Open a new fd on the real driver: a descriptor, or -1 with errno set. */
	int (*open)(void *ctx);
	int (*close)(void *ctx, int fd);
	/* An ioctl as the system call does: -1 with errno set on failure. */
	int (*ioctl)(void *ctx, int fd, unsigned long cmd, unsigned long arg);
	/* poll() on one fd: the count of ready fds (0 or 1), or -1 with errno set. */
	int (*poll)(void *ctx, int fd, int events, int timeout_ms, int *revents);
	void (*sleep_ms)(void *ctx, unsigned int ms);
	uint32_t (*random32)(void *ctx);
	uint64_t (*now_ms)(void *ctx);
	/*
	 * Optional. Nonzero if the guest with this kernel-reported id is the primary client, the one
	 * that gets IAV_PROFILE_PRIMARY. Asked on every call, so the answer follows the guest's current
	 * binding and never a value the guest sent. NULL: no guest is primary.
	 */
	int (*is_primary)(void *ctx, uint32_t cid);
	/* The slice EVE leased to this CID. 0 on success. NULL refuses every private submission. */
	int (*slice_of)(void *ctx, uint32_t cid, uint32_t *window);
	/*
	 * Optional. An arena call (OP_ARENA) is about to name slice index in a DSP or GDMA job: 0 lets it run and counts
	 * it as in flight, a negative errno refuses it (the slice is quarantined: its lease was released). arena_end is
	 * called once after every arena_begin that returned 0, when the real ioctl returned. The server keeps a released
	 * slice quarantined until no call is in flight and no job a returned call queued can still run, then scrubs it.
	 * NULL: no accounting (host tests).
	 */
	int (*arena_begin)(void *ctx, uint32_t index);
	void (*arena_end)(void *ctx, uint32_t index);
	/*
	 * Optional. The aux aperture the host attached to the guest with this kernel-reported id (Envelope F of
	 * plan_iav_usb_yolo): 0 with *out filled (count 0: none), or a negative errno. Asked on every call that needs it,
	 * so the answer follows the guest's current lease. NULL: no guest has an aperture.
	 */
	int (*aperture_of)(void *ctx, uint32_t cid, struct iav_broker_aperture *out);
	/*
	 * Optional. Read len bytes at offset off of the decode partition as the host sees them (the guest's writes
	 * included): 0, or a negative errno. DECODE_JPEG reads the JPEG's header with it before the DSP does. NULL: no
	 * JPEG is admitted.
	 */
	int (*read_dec)(void *ctx, uint64_t off, void *buf, size_t len);
};

struct iav_broker_window {
	uint32_t id;
	uint64_t base;		/* the address the real driver reports for the partition */
	uint64_t part_len;	/* the partition's size */
	uint64_t size;		/* the window and its backing device: a power of two >= part_len */
	unsigned int ring;	/* kept for callers; every window may be mapped twice in a row (G8) */
	uint32_t pid;		/* the IAV partition behind it, re-queried for pinning; 0 with base 0: not checked */
};

/* What a guest negotiated with OP_HELLO. */
struct iav_broker_hello {
	int done;			/* 0: the guest runs the legacy protocol */
	uint32_t revision;		/* the module's revision */
	uint64_t features;		/* granted AMBA_VIRT_IAV_F_* */
	uint32_t max_window_id;		/* the largest id OP_INFO reports to it */
	uint32_t page_size;
	uint64_t table_hash;		/* the module's table hash */
};

struct iav_broker_cfg {
	enum iav_profile profile;
	unsigned int max_sessions_per_cid;	/* 0: the default */
	unsigned int block_max_ms;		/* 0: the default */
	unsigned int rate_per_sec;		/* requests per second per CID; 0: unlimited */
	unsigned int rate_burst;		/* bucket size; 0: same as rate_per_sec */
};

struct iav_session {
	int in_use;
	int closing;
	uint16_t gen;
	uint32_t cid;
	int fd;
	int refs;
	uint64_t last_used_ms;
	pthread_mutex_t lock;	/* one operation at a time on the real fd */
};

struct iav_cid_state {
	int used;
	uint32_t cid;
	unsigned int sessions;
	double tokens;
	uint64_t last_ms;
	struct iav_broker_hello hello;
};

struct iav_broker {
	struct iav_broker_ops ops;
	struct iav_broker_cfg cfg;
	pthread_mutex_t lock;
	uint32_t epoch;
	struct iav_session sess[IAV_BROKER_MAX_SESSIONS];
	struct iav_cid_state cids[IAV_BROKER_MAX_CIDS];
	struct iav_broker_window windows[AMBA_VIRT_IAV_MAX_WINDOWS];
	unsigned int nwindows;
	/* Counters, for the admin view and for tests. */
	uint64_t n_requests;
	uint64_t n_refused;
	uint64_t n_hello;		/* OP_HELLO answered */
	uint64_t n_pinned;		/* calls refused because they would move a windowed partition */
	uint64_t n_stale_refused;	/* window-backed calls refused while the windows are stale */
	uint64_t n_rechecks;		/* window re-queries run */
	uint64_t n_arena;		/* OP_ARENA calls that reached the driver */
	uint64_t n_arena_refused;	/* OP_ARENA calls refused before the driver */
	uint64_t n_aux_maps;		/* aperture entries authorized for mapping */
	/* The partitions behind every aux aperture (the server's sources), re-queried with the windows (pinning). */
	unsigned int naux_src;
	struct iav_broker_aux_entry aux_src[IAV_BROKER_AUX_MAX];
	int guest_pool_set;
	struct virt_iav_guest_pool guest_pool;
	uint64_t host_features;		/* AMBA_VIRT_IAV_F_* the host provides (NC windows) */
	uint64_t table_hash;		/* this broker's table hash */
	uint32_t table_rows;
	/*
	 * Nonzero when a re-query found a windowed partition at another address or length than its window: every
	 * window-backed call and every map is refused until the partitions match the windows again.
	 */
	int windows_stale;
};

/* Returns 0 or a negative errno. */
int iav_broker_init(struct iav_broker *b, const struct iav_broker_ops *ops,
		    const struct iav_broker_cfg *cfg);
/* Closes every session. */
void iav_broker_destroy(struct iav_broker *b);

/*
 * Set the windows the guests may map. Every size must be a power of two, every base and size
 * page aligned, ranges disjoint, ids unique and below AMBA_VIRT_IAV_MAX_WINDOWS.
 */
int iav_broker_set_windows(struct iav_broker *b, const struct iav_broker_window *w, unsigned int n);
void iav_broker_set_guest_pool(struct iav_broker *b, const struct virt_iav_guest_pool *pool);
/* The features the host provides, granted to a guest that offers them (AMBA_VIRT_IAV_F_NC_WINDOWS). */
void iav_broker_set_host_features(struct iav_broker *b, uint64_t features);
/*
 * The partitions the aux apertures are built from (pid, base, len; offset unused), re-queried with the windows: a
 * moved one makes the windows stale. At most IAV_BROKER_AUX_MAX, every base and length page aligned. 0 or -EINVAL.
 */
int iav_broker_set_aux_sources(struct iav_broker *b, const struct iav_broker_aux_entry *src, unsigned int n);

/*
 * Window pinning: query every windowed partition on a new descriptor of the real driver and compare it with its
 * window. Returns 1 if the windows are stale (refusing window-backed calls), 0 if they match, or a negative errno
 * if the driver could not be opened (the state is then unchanged).
 */
int iav_broker_recheck_windows(struct iav_broker *b);
int iav_broker_windows_stale(struct iav_broker *b);

/* What cid negotiated; zeroed when it did not. */
void iav_broker_cid_hello(struct iav_broker *b, uint32_t cid, struct iav_broker_hello *out);

/* One line for the admin view: counters, the stale flag and the negotiated guests. Returns bytes written or -1. */
int iav_broker_format_status(struct iav_broker *b, char *buf, size_t len);

/* FNV-1a 64 over the table rows in ascending cmd order (amba_virt_iav_table.h). */
uint64_t iav_broker_table_hash(uint32_t *rows);

/*
 * Handle one request from the guest with kernel-reported id cid. req is the payload after the
 * struct amba_virt_msg header. Always builds a response in resp when it can: a refused request
 * is a response with a negative status. Returns 0, or -1 if resp is too small for even an
 * error response.
 */
int iav_broker_handle(struct iav_broker *b, uint32_t cid, const void *req, size_t req_len,
		      void *resp, size_t resp_cap, size_t *resp_len);

/* The guest is gone: release everything it held. */
void iav_broker_client_gone(struct iav_broker *b, uint32_t cid);
/* Release sessions unused for idle_ms. Returns how many were released. */
unsigned int iav_broker_reap_idle(struct iav_broker *b, uint64_t idle_ms);
/* The distinct guest ids that hold a session, up to max. Returns how many were stored. */
unsigned int iav_broker_list_cids(struct iav_broker *b, uint32_t *out, unsigned int max);
/* True if cid holds a session. For the liveness check. */
int iav_broker_cid_has_sessions(struct iav_broker *b, uint32_t cid);
unsigned int iav_broker_session_count(struct iav_broker *b);
unsigned int iav_broker_cid_sessions(struct iav_broker *b, uint32_t cid);

#endif /* IAV_BROKER_H */
