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

#define IAV_BROKER_MAX_SESSIONS		64
#define IAV_BROKER_MAX_CIDS		16
#define IAV_BROKER_DEFAULT_PER_CID	16u
#define IAV_BROKER_DEFAULT_BLOCK_MS	2000u

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
};

struct iav_broker_window {
	uint32_t id;
	uint64_t base;		/* the address the real driver reports for the partition */
	uint64_t part_len;	/* the partition's size */
	uint64_t size;		/* the window and its backing device: a power of two >= part_len */
	unsigned int ring;	/* nonzero: the partition may be mapped twice in a row (a ring) */
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
