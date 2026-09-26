/*
 * amba_virt_dma.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMBA_VIRT_DMA_H
#define AMBA_VIRT_DMA_H

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
	u64 epoch;
	u64 capability;
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
