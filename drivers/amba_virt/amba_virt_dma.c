/*
 * amba_virt_dma.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/random.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/mm.h>

#include "amba_virt_dma.h"

#define DRV_NAME "amba_virt_dma"

static dev_t dma_devt_base;
static struct class *dma_class;
static struct cdev ctl_cdev;
static struct device *ctl_device;

static struct amba_dma_lease leases[AMBA_VIRT_DMA32_NUM_SLICES];
static struct mutex global_lock;

/* Endpoint policy table */
static struct amba_dma_endpoint_policy endpoint_uart2 = {
	.endpoint_id  = AMBA_DMA_ENDPOINT_UART2,
	.name         = "UART2",
	.of_node_path = "/ahb@ffe0000000/uart@e0018000",
	.tx_dreq      = 13,
	.rx_dreq      = 14,
	.fifo_offset  = 0x40,
	.fifo_phys    = 0xe0018040ULL,
	.addr_width   = DMA_SLAVE_BUSWIDTH_1_BYTE,
	.max_burst    = 8,
	.owner_lease  = -1,
};

static struct amba_dma_endpoint_policy endpoint_uart3 = {
	.endpoint_id  = AMBA_DMA_ENDPOINT_UART3,
	.name         = "UART3",
	.of_node_path = "/ahb@ffe0000000/uart@e0019000",
	.tx_dreq      = 15,
	.rx_dreq      = 16,
	.fifo_offset  = 0x40,
	.fifo_phys    = 0xe0019040ULL,
	.addr_width   = DMA_SLAVE_BUSWIDTH_1_BYTE,
	.max_burst    = 8,
	.owner_lease  = -1,
};

static void amba_dma_watchdog_fired(struct timer_list *t)
{
	struct amba_dma_lease *lease = from_timer(lease, t, watchdog);

	pr_warn("%s: lease %u DMA transfer watchdog timed out (%u ms)\n",
		DRV_NAME, lease->lease_id, lease->active_timeout_ms);

	lease->xfer_status = -ETIMEDOUT;
	lease->stats.err_dma_watchdog++;

	if (endpoint_uart2.owner_lease == (int)lease->lease_id) {
		if (lease->active_operation == AMBA_DMA_OP_MEM_TO_DEV && endpoint_uart2.tx_chan)
			dmaengine_terminate_sync(endpoint_uart2.tx_chan);
		else if (lease->active_operation == AMBA_DMA_OP_DEV_TO_MEM && endpoint_uart2.rx_chan)
			dmaengine_terminate_sync(endpoint_uart2.rx_chan);
	}
	if (endpoint_uart3.owner_lease == (int)lease->lease_id) {
		if (lease->active_operation == AMBA_DMA_OP_MEM_TO_DEV && endpoint_uart3.tx_chan)
			dmaengine_terminate_sync(endpoint_uart3.tx_chan);
		else if (lease->active_operation == AMBA_DMA_OP_DEV_TO_MEM && endpoint_uart3.rx_chan)
			dmaengine_terminate_sync(endpoint_uart3.rx_chan);
	}

	complete(&lease->xfer_done);
}

static void amba_dma_callback(void *data)
{
	struct amba_dma_lease *lease = (struct amba_dma_lease *)data;

	del_timer(&lease->watchdog);
	lease->xfer_status = 0;
	complete(&lease->xfer_done);
}

static int amba_dma_sanitize_slice(struct amba_dma_lease *lease)
{
	void __iomem *vaddr;
	ktime_t t_start, t_end;

	t_start = ktime_get();

	vaddr = ioremap_wc(lease->slice_phys, lease->slice_size);
	if (!vaddr) {
		pr_err("%s: failed to remap slice %u for sanitization\n",
		       DRV_NAME, lease->lease_id);
		return -ENOMEM;
	}

	memset_io(vaddr, 0, lease->slice_size);
	wmb();
	iounmap(vaddr);

	t_end = ktime_get();
	lease->stats.sanitize_duration_us = (u32)ktime_to_us(ktime_sub(t_end, t_start));

	pr_info("%s: lease %u slice sanitized in %u us\n",
		DRV_NAME, lease->lease_id, lease->stats.sanitize_duration_us);

	return 0;
}

static void amba_dma_write_bootstrap_page(struct amba_dma_lease *lease)
{
	void __iomem *vaddr;
	struct amba_dma_bootstrap boot;

	vaddr = ioremap_wc(lease->slice_phys, AMBA_DMA_BOOTSTRAP_SIZE);
	if (!vaddr) {
		pr_err("%s: failed to map bootstrap page for lease %u\n",
		       DRV_NAME, lease->lease_id);
		return;
	}

	memset_io(vaddr, 0, AMBA_DMA_BOOTSTRAP_SIZE);
	memset(&boot, 0, sizeof(boot));
	boot.magic = AMBA_DMA_BOOTSTRAP_MAGIC;
	boot.version = AMBA_DMA_BOOTSTRAP_VERSION;
	boot.lease_id = lease->lease_id;
	boot.reserved0 = 0;
	boot.epoch = lease->epoch;
	memcpy(boot.hmac_key, lease->hmac_key, sizeof(boot.hmac_key));

	memcpy_toio(vaddr, &boot, sizeof(boot));
	wmb();
	iounmap(vaddr);
}

int amba_dma_alloc_lease(struct amba_dma_lease_alloc *alloc)
{
	struct amba_dma_lease *lease;
	u32 id;
	int ret = -ENOSPC;

	if (!alloc)
		return -EINVAL;

	mutex_lock(&global_lock);

	/* 1. If vsock_cid already has an allocated lease, return it */
	if (alloc->vsock_cid != 0) {
		for (id = 0; id < AMBA_VIRT_DMA32_NUM_SLICES; id++) {
			lease = &leases[id];
			mutex_lock(&lease->lock);
			if ((lease->state == AMBA_DMA_LEASE_ACTIVE ||
			     lease->state == AMBA_DMA_LEASE_STARTING) &&
			    lease->vsock_cid == alloc->vsock_cid) {
				alloc->lease_id = id;
				alloc->capability = lease->capability;
				alloc->epoch = lease->epoch;
				mutex_unlock(&lease->lock);
				mutex_unlock(&global_lock);
				return 0;
			}
			mutex_unlock(&lease->lock);
		}
	}

	for (id = 0; id < AMBA_VIRT_DMA32_NUM_SLICES; id++) {
		lease = &leases[id];
		mutex_lock(&lease->lock);
		if (lease->state == AMBA_DMA_LEASE_FREE) {
			lease->state = AMBA_DMA_LEASE_STARTING;
			lease->boot_generation = alloc->boot_generation;
			lease->vsock_cid = alloc->vsock_cid;
			memcpy(lease->vm_uuid, alloc->vm_uuid, 16);

			get_random_bytes(&lease->capability, sizeof(lease->capability));
			get_random_bytes(lease->hmac_key, sizeof(lease->hmac_key));
			lease->epoch++;

			amba_dma_write_bootstrap_page(lease);

			alloc->lease_id = id;
			alloc->capability = lease->capability;
			alloc->epoch = lease->epoch;

			pr_info("%s: allocated lease %u for CID %u (epoch %llu)\n",
				DRV_NAME, id, lease->vsock_cid, lease->epoch);

			mutex_unlock(&lease->lock);
			ret = 0;
			break;
		}
		mutex_unlock(&lease->lock);
	}

	mutex_unlock(&global_lock);
	return ret;
}

int amba_dma_drain_lease(u32 lease_id, u64 epoch, bool force)
{
	struct amba_dma_lease *lease;
	ktime_t t_start, t_end;

	if (lease_id >= AMBA_VIRT_DMA32_NUM_SLICES)
		return -EINVAL;

	lease = &leases[lease_id];
	mutex_lock(&lease->lock);

	if (epoch != 0 && lease->epoch != epoch) {
		mutex_unlock(&lease->lock);
		return -ESTALE;
	}

	if (lease->state != AMBA_DMA_LEASE_ACTIVE && lease->state != AMBA_DMA_LEASE_STARTING) {
		mutex_unlock(&lease->lock);
		return -EBUSY;
	}

	t_start = ktime_get();
	lease->state = AMBA_DMA_LEASE_DRAINING;

	/* If a transfer is currently in flight, wait or terminate */
	if (lease->in_flight) {
		if (force) {
			del_timer(&lease->watchdog);
			if (endpoint_uart2.owner_lease == (int)lease->lease_id) {
				if (endpoint_uart2.tx_chan)
					dmaengine_terminate_sync(endpoint_uart2.tx_chan);
				if (endpoint_uart2.rx_chan)
					dmaengine_terminate_sync(endpoint_uart2.rx_chan);
			}
			if (endpoint_uart3.owner_lease == (int)lease->lease_id) {
				if (endpoint_uart3.tx_chan)
					dmaengine_terminate_sync(endpoint_uart3.tx_chan);
				if (endpoint_uart3.rx_chan)
					dmaengine_terminate_sync(endpoint_uart3.rx_chan);
			}
			complete(&lease->xfer_done);
		} else {
			mutex_unlock(&lease->lock);
			wait_for_completion_timeout(&lease->xfer_done,
						    msecs_to_jiffies(AMBA_DMA_DRAIN_TIMEOUT_MS));
			mutex_lock(&lease->lock);
		}
	}

	t_end = ktime_get();
	lease->stats.teardown_drain_ms = (u32)ktime_to_ms(ktime_sub(t_end, t_start));

	pr_info("%s: lease %u drained in %u ms\n",
		DRV_NAME, lease_id, lease->stats.teardown_drain_ms);

	mutex_unlock(&lease->lock);
	return 0;
}

int amba_dma_release_lease(u32 lease_id, u64 epoch)
{
	struct amba_dma_lease *lease;
	int ret;

	if (lease_id >= AMBA_VIRT_DMA32_NUM_SLICES)
		return -EINVAL;

	lease = &leases[lease_id];
	mutex_lock(&lease->lock);

	if (epoch != 0 && lease->epoch != epoch) {
		mutex_unlock(&lease->lock);
		return -ESTALE;
	}

	lease->state = AMBA_DMA_LEASE_SANITIZING;

	/* Release endpoint ownership if reserved */
	mutex_lock(&endpoint_uart2.lock);
	if (endpoint_uart2.owner_lease == (int)lease_id)
		endpoint_uart2.owner_lease = -1;
	mutex_unlock(&endpoint_uart2.lock);

	mutex_lock(&endpoint_uart3.lock);
	if (endpoint_uart3.owner_lease == (int)lease_id)
		endpoint_uart3.owner_lease = -1;
	mutex_unlock(&endpoint_uart3.lock);

	ret = amba_dma_sanitize_slice(lease);
	if (ret) {
		lease->state = AMBA_DMA_LEASE_QUARANTINED;
		mutex_unlock(&lease->lock);
		return ret;
	}

	/* Reset identity and state */
	lease->capability = 0;
	memset(lease->hmac_key, 0, sizeof(lease->hmac_key));
	lease->vsock_cid = 0;
	lease->boot_generation = 0;
	memset(lease->vm_uuid, 0, 16);
	lease->in_flight = false;
	lease->active_cookie = 0;
	memset(lease->replay_cookies, 0, sizeof(lease->replay_cookies));
	lease->replay_idx = 0;
	lease->state = AMBA_DMA_LEASE_FREE;

	pr_info("%s: lease %u successfully released and returned to FREE\n",
		DRV_NAME, lease_id);

	mutex_unlock(&lease->lock);
	return 0;
}

int amba_dma_quarantine_release(u32 lease_id, u64 epoch)
{
	struct amba_dma_lease *lease;

	if (lease_id >= AMBA_VIRT_DMA32_NUM_SLICES)
		return -EINVAL;

	lease = &leases[lease_id];
	mutex_lock(&lease->lock);

	if (lease->state != AMBA_DMA_LEASE_QUARANTINED) {
		mutex_unlock(&lease->lock);
		return -EINVAL;
	}

	mutex_lock(&endpoint_uart2.lock);
	if (endpoint_uart2.owner_lease == (int)lease_id)
		endpoint_uart2.owner_lease = -1;
	mutex_unlock(&endpoint_uart2.lock);

	mutex_lock(&endpoint_uart3.lock);
	if (endpoint_uart3.owner_lease == (int)lease_id)
		endpoint_uart3.owner_lease = -1;
	mutex_unlock(&endpoint_uart3.lock);

	amba_dma_sanitize_slice(lease);
	lease->state = AMBA_DMA_LEASE_FREE;
	lease->epoch++;
	memset(lease->hmac_key, 0, sizeof(lease->hmac_key));

	pr_info("%s: lease %u quarantine cleared by administrator\n",
		DRV_NAME, lease_id);

	mutex_unlock(&lease->lock);
	return 0;
}

int amba_dma_get_lease_stats(u32 lease_id, struct amba_dma_lease_stats *stats)
{
	struct amba_dma_lease *lease;

	if (lease_id >= AMBA_VIRT_DMA32_NUM_SLICES || !stats)
		return -EINVAL;

	lease = &leases[lease_id];
	mutex_lock(&lease->lock);
	memcpy(stats, &lease->stats, sizeof(*stats));
	stats->lease_id = lease_id;
	stats->state = lease->state;
	stats->epoch = lease->epoch;
	mutex_unlock(&lease->lock);

	return 0;
}

static bool amba_dma_cookie_is_replayed(struct amba_dma_lease *lease, u64 cookie)
{
	u32 i;

	if (cookie == 0)
		return false;

	if (lease->in_flight && lease->active_cookie == cookie)
		return true;

	for (i = 0; i < ARRAY_SIZE(lease->replay_cookies); i++) {
		if (lease->replay_cookies[i] != 0 && lease->replay_cookies[i] == cookie)
			return true;
	}

	return false;
}

static void amba_dma_record_cookie(struct amba_dma_lease *lease, u64 cookie)
{
	lease->replay_cookies[lease->replay_idx] = cookie;
	lease->replay_idx = (lease->replay_idx + 1) % ARRAY_SIZE(lease->replay_cookies);
}

int amba_dma_dispatch_request(u32 lease_id,
			      struct amba_virt_dma_request *req,
			      struct amba_virt_dma_response *resp)
{
	struct amba_dma_lease *lease;
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *tx_desc;
	struct dma_slave_config cfg;
	dma_addr_t buf_dma;
	enum dma_transfer_direction dir;
	int ret;

	if (lease_id >= AMBA_VIRT_DMA32_NUM_SLICES || !req || !resp)
		return -EINVAL;

	lease = &leases[lease_id];
	mutex_lock(&lease->lock);

	/* 1. State and capability validation */
	if (lease->state != AMBA_DMA_LEASE_ACTIVE && lease->state != AMBA_DMA_LEASE_STARTING) {
		mutex_unlock(&lease->lock);
		return -EPERM;
	}

	if (lease->state == AMBA_DMA_LEASE_STARTING)
		lease->state = AMBA_DMA_LEASE_ACTIVE;

	if (req->capability != lease->capability) {
		lease->stats.err_invalid_capability++;
		mutex_unlock(&lease->lock);
		return -EACCES;
	}

	if (req->epoch != lease->epoch) {
		lease->stats.err_invalid_epoch++;
		mutex_unlock(&lease->lock);
		return -ESTALE;
	}

	/* 2. Replay check */
	if (amba_dma_cookie_is_replayed(lease, req->cookie)) {
		lease->stats.err_invalid_epoch++;
		mutex_unlock(&lease->lock);
		return -EALREADY;
	}

	/* 3. Concurrency check (single-flight) */
	if (lease->in_flight) {
		mutex_unlock(&lease->lock);
		return -EBUSY;
	}

	/* 4. Arithmetic & bounds checks (overflow-safe) */
	if (req->length == 0 || req->length > (4 * 1024 * 1024)) {
		lease->stats.err_invalid_bounds++;
		mutex_unlock(&lease->lock);
		return -EINVAL;
	}

	if (req->offset < AMBA_DMA_BOOTSTRAP_SIZE ||
	    req->offset >= lease->slice_size ||
	    req->offset > (lease->slice_size - req->length)) {
		lease->stats.err_invalid_bounds++;
		mutex_unlock(&lease->lock);
		return -ERANGE;
	}

	buf_dma = lease->slice_dma + req->offset;
	if (buf_dma + req->length - 1 > U32_MAX) {
		lease->stats.err_invalid_bounds++;
		mutex_unlock(&lease->lock);
		return -EFBIG;
	}

	/* 5. Endpoint policy check */
	struct amba_dma_endpoint_policy *ep;
	if (req->endpoint_id == AMBA_DMA_ENDPOINT_UART2) {
		ep = &endpoint_uart2;
	} else if (req->endpoint_id == AMBA_DMA_ENDPOINT_UART3) {
		ep = &endpoint_uart3;
	} else {
		mutex_unlock(&lease->lock);
		return -ENODEV;
	}

	/* Negative constraint 9: One lease binds one endpoint */
	if ((ep == &endpoint_uart2 && endpoint_uart3.owner_lease == (int)lease_id) ||
	    (ep == &endpoint_uart3 && endpoint_uart2.owner_lease == (int)lease_id)) {
		mutex_unlock(&lease->lock);
		return -EBUSY;
	}

	/* Verification: A UART3 request is rejected until UART3 channels exist */
	if (!ep->tx_chan || !ep->rx_chan) {
		mutex_unlock(&lease->lock);
		return -ENODEV;
	}

	mutex_lock(&ep->lock);
	if (ep->owner_lease != -1 && ep->owner_lease != (int)lease_id) {
		mutex_unlock(&ep->lock);
		mutex_unlock(&lease->lock);
		return -EBUSY; /* Exclusive endpoint already owned by another lease */
	}
	ep->owner_lease = lease_id;
	mutex_unlock(&ep->lock);

	/* 6. Channel & direction configuration */
	memset(&cfg, 0, sizeof(cfg));
	if (req->operation == AMBA_DMA_OP_MEM_TO_DEV) {
		dir = DMA_MEM_TO_DEV;
		chan = ep->tx_chan;
		cfg.direction = DMA_MEM_TO_DEV;
		cfg.dst_addr = ep->fifo_dma;
		cfg.dst_addr_width = ep->addr_width;
		cfg.dst_maxburst = ep->max_burst;
	} else if (req->operation == AMBA_DMA_OP_DEV_TO_MEM) {
		dir = DMA_DEV_TO_MEM;
		chan = ep->rx_chan;
		cfg.direction = DMA_DEV_TO_MEM;
		cfg.src_addr = ep->fifo_dma;
		cfg.src_addr_width = ep->addr_width;
		cfg.src_maxburst = ep->max_burst;
	} else {
		mutex_unlock(&lease->lock);
		return -EINVAL;
	}

	if (!chan) {
		mutex_unlock(&lease->lock);
		return -ENODEV;
	}

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		mutex_unlock(&lease->lock);
		return ret;
	}

	tx_desc = dmaengine_prep_slave_single(chan, buf_dma, req->length, dir,
					      DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx_desc) {
		mutex_unlock(&lease->lock);
		return -EIO;
	}

	pr_info("%s: lease %u %s DMA submitted: chan=%s dreq=%u addr=0x%llx len=%u cookie=%llu\n",
		DRV_NAME, lease_id, ep->name, dma_chan_name(chan),
		req->operation == AMBA_DMA_OP_MEM_TO_DEV ? ep->tx_dreq : ep->rx_dreq,
		(unsigned long long)buf_dma, req->length, (unsigned long long)req->cookie);

	/* 7. Setup in-flight transaction */
	lease->in_flight = true;
	lease->active_cookie = req->cookie;
	lease->active_endpoint = req->endpoint_id;
	lease->active_operation = req->operation;
	lease->xfer_status = -EINPROGRESS;
	lease->xfer_transferred = 0;
	reinit_completion(&lease->xfer_done);

	tx_desc->callback = amba_dma_callback;
	tx_desc->callback_param = lease;

	/* Arm dynamic line-time watchdog: line time + 2000 ms margin */
	{
		u32 timeout_ms = ((uint64_t)req->length * 10 * 1000) / 115200 + 2000;
		lease->active_timeout_ms = timeout_ms;
		mod_timer(&lease->watchdog, jiffies + msecs_to_jiffies(timeout_ms));
	}

	dmaengine_submit(tx_desc);
	dma_async_issue_pending(chan);

	mutex_unlock(&lease->lock);

	/* Synchronous wait for completion or watchdog */
	wait_for_completion(&lease->xfer_done);

	mutex_lock(&lease->lock);
	lease->in_flight = false;
	amba_dma_record_cookie(lease, req->cookie);

	resp->cookie = req->cookie;
	resp->status = lease->xfer_status;
	if (lease->xfer_status == 0) {
		resp->transferred = req->length;
		lease->stats.rx_requests_total++;
		lease->stats.rx_bytes_total += req->length;
	} else {
		resp->transferred = 0;
	}

	mutex_unlock(&lease->lock);
	return 0;
}

int amba_dma_mmap_slice(u32 lease_id, struct vm_area_struct *vma)
{
	struct amba_dma_lease *lease;
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long pfn;

	if (lease_id >= AMBA_VIRT_DMA32_NUM_SLICES)
		return -EINVAL;

	lease = &leases[lease_id];
	if (size > lease->slice_size)
		return -EINVAL;

	pfn = lease->slice_phys >> PAGE_SHIFT;
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
}

/* Character device file operations for /dev/amba_dma_ctl */
static long amba_dma_ctl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case AMBA_DMA_IOC_LEASE_ALLOC: {
		struct amba_dma_lease_alloc alloc;
		if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc)))
			return -EFAULT;
		if (amba_dma_alloc_lease(&alloc))
			return -ENOSPC;
		if (copy_to_user((void __user *)arg, &alloc, sizeof(alloc)))
			return -EFAULT;
		return 0;
	}
	case AMBA_DMA_IOC_LEASE_CTRL: {
		struct amba_dma_lease_control ctrl;
		int ret = 0;
		if (copy_from_user(&ctrl, (void __user *)arg, sizeof(ctrl)))
			return -EFAULT;
		if (ctrl.command == AMBA_DMA_LEASE_CMD_DRAIN)
			ret = amba_dma_drain_lease(ctrl.lease_id, ctrl.epoch, false);
		else if (ctrl.command == AMBA_DMA_LEASE_CMD_FORCE_DRAIN)
			ret = amba_dma_drain_lease(ctrl.lease_id, ctrl.epoch, true);
		else if (ctrl.command == AMBA_DMA_LEASE_CMD_RELEASE)
			ret = amba_dma_release_lease(ctrl.lease_id, ctrl.epoch);
		else if (ctrl.command == AMBA_DMA_LEASE_CMD_QUARANTINE_RELEASE)
			ret = amba_dma_quarantine_release(ctrl.lease_id, ctrl.epoch);
		else
			ret = -EINVAL;
		ctrl.status = ret;
		if (copy_to_user((void __user *)arg, &ctrl, sizeof(ctrl)))
			return -EFAULT;
		return ret;
	}
	case AMBA_DMA_IOC_LEASE_STATS: {
		struct amba_dma_lease_stats stats;
		if (copy_from_user(&stats, (void __user *)arg, sizeof(stats)))
			return -EFAULT;
		if (amba_dma_get_lease_stats(stats.lease_id, &stats))
			return -EINVAL;
		if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations amba_dma_ctl_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = amba_dma_ctl_ioctl,
};

/* Character device file operations for /dev/amba_dma_lease<N> */
static int amba_dma_lease_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	if (minor == 0 || minor > AMBA_VIRT_DMA32_NUM_SLICES)
		return -ENODEV;
	file->private_data = (void *)(unsigned long)(minor - 1);
	return 0;
}

static int amba_dma_lease_mmap(struct file *file, struct vm_area_struct *vma)
{
	u32 lease_id = (u32)(unsigned long)file->private_data;
	return amba_dma_mmap_slice(lease_id, vma);
}

static long amba_dma_lease_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	u32 lease_id = (u32)(unsigned long)file->private_data;

	if (cmd == AMBA_DMA_IOC_REQUEST) {
		struct amba_virt_dma_request req;
		struct amba_virt_dma_response resp;
		int ret;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		memset(&resp, 0, sizeof(resp));
		ret = amba_dma_dispatch_request(lease_id, &req, &resp);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &resp, sizeof(resp)))
			return -EFAULT;
		return 0;
	}

	if (cmd == AMBA_DMA_IOC_LEASE_GET_INFO) {
		struct amba_dma_lease_info info;
		struct amba_dma_lease *lease;

		if (lease_id >= AMBA_VIRT_DMA32_NUM_SLICES)
			return -EINVAL;
		lease = &leases[lease_id];
		mutex_lock(&lease->lock);
		memset(&info, 0, sizeof(info));
		info.lease_id = lease->lease_id;
		info.state = lease->state;
		info.capability = lease->capability;
		info.epoch = lease->epoch;
		info.vsock_cid = lease->vsock_cid;
		memcpy(info.hmac_key, lease->hmac_key, sizeof(info.hmac_key));
		mutex_unlock(&lease->lock);

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	return -ENOTTY;
}

static const struct file_operations amba_dma_lease_fops = {
	.owner          = THIS_MODULE,
	.open           = amba_dma_lease_open,
	.mmap           = amba_dma_lease_mmap,
	.unlocked_ioctl = amba_dma_lease_ioctl,
};

static int amba_dma_acquire_endpoint(struct device *parent, struct amba_dma_endpoint_policy *ep)
{
	struct device_node *of_node;
	struct device *dma_dev = NULL;

	ep->tx_chan = NULL;
	ep->rx_chan = NULL;
	ep->fifo_dma = 0;

	of_node = of_find_node_by_path(ep->of_node_path);
	if (!of_node) {
		pr_warn("%s: OF node %s not found in host DT\n", DRV_NAME, ep->of_node_path);
		return -ENODEV;
	}

	ep->tx_chan = of_dma_request_slave_channel(of_node, "tx");
	ep->rx_chan = of_dma_request_slave_channel(of_node, "rx");
	of_node_put(of_node);

	if (!ep->tx_chan || !ep->rx_chan) {
		pr_warn("%s: warning: could not acquire slave DMA channels for %s (TX=%p, RX=%p)\n",
			DRV_NAME, ep->name, ep->tx_chan, ep->rx_chan);
		return -ENODEV;
	}

	pr_info("%s: acquired slave DMA channels for %s (TX=%s, RX=%s)\n",
		DRV_NAME, ep->name,
		dma_chan_name(ep->tx_chan),
		dma_chan_name(ep->rx_chan));

	if (ep->tx_chan && ep->tx_chan->device)
		dma_dev = ep->tx_chan->device->dev;
	else if (parent)
		dma_dev = parent;

	if (dma_dev) {
		ep->fifo_dma = dma_map_resource(dma_dev, ep->fifo_phys, 0x1000,
						 DMA_BIDIRECTIONAL, 0);
		if (dma_mapping_error(dma_dev, ep->fifo_dma)) {
			pr_err("%s: failed to map FIFO resource for %s\n", DRV_NAME, ep->name);
			ep->fifo_dma = 0;
			return -EIO;
		}
	}

	return 0;
}

static void amba_dma_release_endpoint(struct device *parent, struct amba_dma_endpoint_policy *ep)
{
	struct device *dma_dev = NULL;

	mutex_lock(&ep->lock);
	if (ep->tx_chan && ep->tx_chan->device)
		dma_dev = ep->tx_chan->device->dev;
	else if (parent)
		dma_dev = parent;

	if (dma_dev && ep->fifo_dma) {
		dma_unmap_resource(dma_dev, ep->fifo_dma, 0x1000, DMA_BIDIRECTIONAL, 0);
		ep->fifo_dma = 0;
	}

	if (ep->tx_chan) {
		dmaengine_terminate_sync(ep->tx_chan);
		dma_release_channel(ep->tx_chan);
		ep->tx_chan = NULL;
	}
	if (ep->rx_chan) {
		dmaengine_terminate_sync(ep->rx_chan);
		dma_release_channel(ep->rx_chan);
		ep->rx_chan = NULL;
	}
	ep->owner_lease = -1;
	mutex_unlock(&ep->lock);
}

int amba_virt_dma_init(struct device *parent)
{
	int ret, i;
	dev_t devt;
	struct device *dma_dev = NULL;

	mutex_init(&global_lock);
	mutex_init(&endpoint_uart2.lock);
	mutex_init(&endpoint_uart3.lock);

	amba_dma_acquire_endpoint(parent, &endpoint_uart2);
	amba_dma_acquire_endpoint(parent, &endpoint_uart3);

	if (endpoint_uart2.tx_chan && endpoint_uart2.tx_chan->device)
		dma_dev = endpoint_uart2.tx_chan->device->dev;
	else if (endpoint_uart3.tx_chan && endpoint_uart3.tx_chan->device)
		dma_dev = endpoint_uart3.tx_chan->device->dev;
	else
		dma_dev = parent;

	ret = alloc_chrdev_region(&dma_devt_base, 0, AMBA_VIRT_DMA32_NUM_SLICES + 1, "amba_dma");
	if (ret) {
		pr_err("%s: failed to allocate chrdev region\n", DRV_NAME);
		return ret;
	}

	dma_class = class_create(THIS_MODULE, "amba_dma");
	if (IS_ERR(dma_class)) {
		unregister_chrdev_region(dma_devt_base, AMBA_VIRT_DMA32_NUM_SLICES + 1);
		return PTR_ERR(dma_class);
	}

	/* 1. Register master control node: /dev/amba_dma_ctl */
	cdev_init(&ctl_cdev, &amba_dma_ctl_fops);
	ctl_cdev.owner = THIS_MODULE;
	ret = cdev_add(&ctl_cdev, dma_devt_base, 1);
	if (ret)
		goto err_cdev_ctl;

	ctl_device = device_create(dma_class, parent, dma_devt_base, NULL, "amba_dma_ctl");
	if (IS_ERR(ctl_device)) {
		ret = PTR_ERR(ctl_device);
		goto err_dev_ctl;
	}

	/* 2. Register per-slice lease devices: /dev/amba_dma_lease<0..3> */
	for (i = 0; i < AMBA_VIRT_DMA32_NUM_SLICES; i++) {
		struct amba_dma_lease *lease = &leases[i];

		lease->lease_id = i;
		lease->state = AMBA_DMA_LEASE_FREE;
		lease->epoch = 1;
		lease->slice_phys = AMBA_VIRT_DMA32_POOL_BASE + (i * AMBA_VIRT_DMA32_SLICE_SIZE);
		lease->slice_size = AMBA_VIRT_DMA32_SLICE_SIZE;
		if (dma_dev) {
			lease->slice_dma = dma_map_resource(dma_dev, lease->slice_phys,
							    lease->slice_size, DMA_BIDIRECTIONAL, 0);
			if (dma_mapping_error(dma_dev, lease->slice_dma)) {
				pr_err("%s: failed to map slice %d resource\n", DRV_NAME, i);
				lease->slice_dma = 0;
			}
		} else {
			lease->slice_dma = 0;
		}

		mutex_init(&lease->lock);
		init_completion(&lease->xfer_done);
		timer_setup(&lease->watchdog, amba_dma_watchdog_fired, 0);

		devt = MKDEV(MAJOR(dma_devt_base), i + 1);
		cdev_init(&lease->cdev, &amba_dma_lease_fops);
		lease->cdev.owner = THIS_MODULE;
		ret = cdev_add(&lease->cdev, devt, 1);
		if (ret)
			goto err_cdev_lease;

		lease->dev = device_create(dma_class, parent, devt, NULL, "amba_dma_lease%d", i);
		if (IS_ERR(lease->dev)) {
			ret = PTR_ERR(lease->dev);
			cdev_del(&lease->cdev);
			goto err_cdev_lease;
		}

		pr_info("%s: initialized lease %d (phys=0x%llx, size=16 MiB)\n",
			DRV_NAME, i, (u64)lease->slice_phys);
	}

	pr_info("%s: kernel DMA reference monitor initialized\n", DRV_NAME);
	return 0;

err_cdev_lease:
	while (--i >= 0) {
		if (dma_dev && leases[i].slice_dma)
			dma_unmap_resource(dma_dev, leases[i].slice_dma, leases[i].slice_size,
					   DMA_BIDIRECTIONAL, 0);
		device_destroy(dma_class, MKDEV(MAJOR(dma_devt_base), i + 1));
		cdev_del(&leases[i].cdev);
	}
	device_destroy(dma_class, dma_devt_base);
err_dev_ctl:
	cdev_del(&ctl_cdev);
err_cdev_ctl:
	class_destroy(dma_class);
	unregister_chrdev_region(dma_devt_base, AMBA_VIRT_DMA32_NUM_SLICES + 1);
	return ret;
}

void amba_virt_dma_exit(void)
{
	int i;
	struct device *dma_dev = NULL;

	if (endpoint_uart2.tx_chan && endpoint_uart2.tx_chan->device)
		dma_dev = endpoint_uart2.tx_chan->device->dev;
	else if (endpoint_uart3.tx_chan && endpoint_uart3.tx_chan->device)
		dma_dev = endpoint_uart3.tx_chan->device->dev;

	amba_dma_release_endpoint(NULL, &endpoint_uart2);
	amba_dma_release_endpoint(NULL, &endpoint_uart3);

	for (i = 0; i < AMBA_VIRT_DMA32_NUM_SLICES; i++) {
		struct amba_dma_lease *lease = &leases[i];
		del_timer_sync(&lease->watchdog);
		if (dma_dev && lease->slice_dma)
			dma_unmap_resource(dma_dev, lease->slice_dma, lease->slice_size,
					   DMA_BIDIRECTIONAL, 0);
		device_destroy(dma_class, MKDEV(MAJOR(dma_devt_base), i + 1));
		cdev_del(&lease->cdev);
	}

	device_destroy(dma_class, dma_devt_base);
	cdev_del(&ctl_cdev);
	class_destroy(dma_class);
	unregister_chrdev_region(dma_devt_base, AMBA_VIRT_DMA32_NUM_SLICES + 1);

	pr_info("%s: kernel DMA reference monitor exited\n", DRV_NAME);
}

static int __init amba_dma_module_init(void)
{
	return amba_virt_dma_init(NULL);
}

static void __exit amba_dma_module_exit(void)
{
	amba_virt_dma_exit();
}

module_init(amba_dma_module_init);
module_exit(amba_dma_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Charles Chiou <cchiou@ambarella.com>");
MODULE_DESCRIPTION("Ambarella Virtual DMA Authority and Lease Reference Monitor");

