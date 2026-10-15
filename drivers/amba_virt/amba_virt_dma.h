/*
 * amba_virt_dma.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMBA_VIRT_DMA_H
#define AMBA_VIRT_DMA_H

#ifdef BUILD_HOST_UNIT_TEST
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "amba_virt.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
typedef int64_t  ktime_t;

#ifndef U32_MAX
#define U32_MAX 0xffffffffU
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif
#ifndef __iomem
#define __iomem
#endif

struct mutex {
	pthread_mutex_t m;
};
static inline void mutex_init(struct mutex *lock) { pthread_mutex_init(&lock->m, NULL); }
static inline void mutex_lock(struct mutex *lock) { pthread_mutex_lock(&lock->m); }
static inline void mutex_unlock(struct mutex *lock) { pthread_mutex_unlock(&lock->m); }

struct completion {
	int done;
};
static inline void init_completion(struct completion *c) { c->done = 0; }
static inline void reinit_completion(struct completion *c) { c->done = 0; }
static inline void complete(struct completion *c) { c->done = 1; }
static inline void wait_for_completion(struct completion *c) { (void)c; }
static inline unsigned long wait_for_completion_timeout(struct completion *c, unsigned long t)
{
	(void)c;
	return t;
}

struct timer_list {
	void (*fn)(struct timer_list *);
	unsigned long expires;
};
#define timer_setup(t, cb, flags) do { (t)->fn = (cb); (t)->expires = 0; } while (0)
#define from_timer(var, cb_timer, timer_fieldname) \
	((typeof(var))((char *)(cb_timer) - offsetof(typeof(*var), timer_fieldname)))
static inline int mod_timer(struct timer_list *t, unsigned long exp) { t->expires = exp; return 0; }
static inline int del_timer(struct timer_list *t) { t->expires = 0; return 0; }
static inline int del_timer_sync(struct timer_list *t) { t->expires = 0; return 0; }
#define jiffies 0UL
static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }

static inline ktime_t ktime_get(void) { return 0; }
static inline ktime_t ktime_sub(ktime_t a, ktime_t b) { return a - b; }
static inline int64_t ktime_to_us(ktime_t k) { return k; }
static inline int64_t ktime_to_ms(ktime_t k) { return k; }

enum dma_transfer_direction {
	DMA_MEM_TO_MEM = 0,
	DMA_MEM_TO_DEV = 1,
	DMA_DEV_TO_MEM = 2,
};
enum dma_slave_buswidth {
	DMA_SLAVE_BUSWIDTH_UNDEFINED = 0,
	DMA_SLAVE_BUSWIDTH_1_BYTE    = 1,
	DMA_SLAVE_BUSWIDTH_2_BYTES   = 2,
	DMA_SLAVE_BUSWIDTH_4_BYTES   = 4,
};
#define DMA_PREP_INTERRUPT (1 << 0)
#define DMA_CTRL_ACK       (1 << 1)

struct dma_slave_config {
	enum dma_transfer_direction direction;
	phys_addr_t src_addr;
	phys_addr_t dst_addr;
	enum dma_slave_buswidth src_addr_width;
	enum dma_slave_buswidth dst_addr_width;
	u32 src_maxburst;
	u32 dst_maxburst;
};

struct dma_chan {
	const char *name;
	u32 dreq;
};

struct dma_async_tx_descriptor {
	void (*callback)(void *param);
	void *callback_param;
};

struct cdev { int dummy; };
struct device { int dummy; };
struct vm_area_struct { unsigned long vm_start; unsigned long vm_end; unsigned long vm_page_prot; };

#define pr_info(fmt, ...)  do { } while (0)
#define pr_warn(fmt, ...)  do { } while (0)
#define pr_err(fmt, ...)   do { } while (0)

void amba_dma_host_test_reset(void);
u32 amba_dma_host_test_last_configured_dreq(void);
u32 amba_dma_host_test_config_count(void);
#else
#include <linux/types.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/completion.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <amba_virt.h>
#endif

#define AMBA_DMA_MAX_CHAINS             16
#define AMBA_DMA_WATCHDOG_MS            500
#define AMBA_DMA_DRAIN_TIMEOUT_MS       2000

struct amba_dma_endpoint_policy {
	u32 endpoint_id;
	const char *name;
	const char *of_node_path;
	u32 tx_dreq;
	u32 rx_dreq;
	u32 fifo_offset;
	phys_addr_t fifo_phys;
	dma_addr_t fifo_dma;
	u32 addr_width;
	u32 max_burst;
	struct dma_chan *tx_chan;
	struct dma_chan *rx_chan;
	int owner_lease; /* -1 = unreserved */
	struct mutex lock;
};

struct amba_dma_lease {
	u32 lease_id;
	u32 state;
	u32 bound_endpoint_id;
	u64 epoch;
	u64 capability;
	u8  hmac_key[32];
	u8  vm_uuid[16];
	u32 boot_generation;
	u32 vsock_cid;

	phys_addr_t slice_phys;
	size_t slice_size;
	dma_addr_t slice_dma;

	struct cdev cdev;
	struct device *dev;
	dev_t devt;
	struct mutex lock;

	/* Active transaction */
	bool in_flight;
	u64 active_cookie;
	u32 active_endpoint;
	u32 active_operation;
	struct completion xfer_done;
	struct timer_list watchdog;
	u32 active_timeout_ms;
	int xfer_status;
	u32 xfer_transferred;

	/* Replay protection: ring of recent completed cookies */
	u64 replay_cookies[32];
	u32 replay_idx;

	/* Telemetry & Auditing */
	struct amba_dma_lease_stats stats;
};

/* Module lifecycle */
int amba_virt_dma_init(struct device *parent);
void amba_virt_dma_exit(void);

/* Pillar control interface */
int amba_dma_alloc_lease(struct amba_dma_lease_alloc *alloc);
int amba_dma_drain_lease(u32 lease_id, u64 epoch, bool force);
int amba_dma_release_lease(u32 lease_id, u64 epoch);
int amba_dma_quarantine_release(u32 lease_id, u64 epoch);
int amba_dma_get_lease_stats(u32 lease_id, struct amba_dma_lease_stats *stats);

/* Request execution via lease-scoped FD */
int amba_dma_dispatch_request(u32 lease_id,
			      struct amba_virt_dma_request *req,
			      struct amba_virt_dma_response *resp);

/* Memory mapping for lease slice */
int amba_dma_mmap_slice(u32 lease_id, struct vm_area_struct *vma);

#endif /* AMBA_VIRT_DMA_H */
