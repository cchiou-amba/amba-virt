/*
 * amba_virt_nohyper.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/overflow.h>
#include <linux/fdtable.h>
#include <linux/uaccess.h>
#include <linux/dmaengine.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/completion.h>

#include <amba_virt.h>
#include <soc/ambarella/gdma.h>
#include "amba_virt_core.h"

extern int cavalry_user_window_get(phys_addr_t *phys, size_t *size);

static char *shm_path = "/dev/shm/amba-virt";
module_param(shm_path, charp, 0644);
MODULE_PARM_DESC(shm_path, "ivshmem memory-backend file (must exist, share=on)");

static unsigned int vsock_port = AMBA_VIRT_VSOCK_PORT;
module_param(vsock_port, uint, 0644);
MODULE_PARM_DESC(vsock_port, "vsock listen port (default 5555; do not use 2000)");

static unsigned long long shm_phys;
module_param(shm_phys, ullong, 0444);
MODULE_PARM_DESC(shm_phys, "Physical base of a no-map shared window");

static unsigned long shm_size;
module_param(shm_size, ulong, 0444);
MODULE_PARM_DESC(shm_size, "Size in bytes of the physical shared window");

static struct amba_virt_dev gdev;
static bool cavalry_window_held;

static int amba_virt_host_gdma_copy(struct amba_virt_dev *dev,
				    struct amba_virt_gdma_copy *copy)
{
	u64 src_end;
	u64 dst_end;
	int ret;

	if (!dev->shm_phys || !dev->shm_size)
		return -ENODEV;
	if (copy->flags & ~AMBA_VIRT_GDMA_F_PITCH)
		return -EINVAL;

	if (copy->flags & AMBA_VIRT_GDMA_F_PITCH) {
		struct gdma_param param = { 0 };

		if (!copy->height || !copy->width || copy->width > 4096 ||
		    copy->src_pitch < copy->width ||
		    copy->dst_pitch < copy->width ||
		    ((u32)copy->src_pitch * copy->height) & 1)
			return -EINVAL;
		src_end = (u64)copy->src_off +
			(u64)(copy->height - 1) * copy->src_pitch +
			copy->width;
		dst_end = (u64)copy->dst_off +
			(u64)(copy->height - 1) * copy->dst_pitch +
			copy->width;
		if (src_end > dev->shm_size || dst_end > dev->shm_size)
			return -ERANGE;
		if ((u64)copy->src_off < dst_end &&
		    (u64)copy->dst_off < src_end)
			return -EINVAL;

		param.src_addr = dev->shm_phys + copy->src_off;
		param.dest_addr = dev->shm_phys + copy->dst_off;
		param.src_non_cached = 1;
		param.dest_non_cached = 1;
		param.src_pitch = copy->src_pitch;
		param.dest_pitch = copy->dst_pitch;
		param.width = copy->width;
		param.height = copy->height;
		dma_wmb();
		ret = dma_pitch_memcpy(&param);
		dma_rmb();
		return ret;
	}

	if (!copy->len || copy->len & 1)
		return -EINVAL;
	src_end = (u64)copy->src_off + copy->len;
	dst_end = (u64)copy->dst_off + copy->len;
	if (src_end > dev->shm_size || dst_end > dev->shm_size)
		return -ERANGE;
	if ((u64)copy->src_off < dst_end && (u64)copy->dst_off < src_end)
		return -EINVAL;

	dma_wmb();
	ret = dma_noncache_memcpy(
		(u8 *)(uintptr_t)(dev->shm_phys + copy->dst_off),
		(u8 *)(uintptr_t)(dev->shm_phys + copy->src_off),
		copy->len);
	dma_rmb();
	return ret;
}

static DEFINE_MUTEX(g_slave_dma_lock);
static struct dma_chan *g_slave_chans[32];
static void *g_slave_bounce_virt;
static dma_addr_t g_slave_bounce_phys;
static struct device *g_slave_bounce_dev;

static void amba_virt_dma_complete_cb(void *param)
{
	struct completion *done = param;

	if (done)
		complete(done);
}

static struct dma_chan *amba_virt_get_slave_chan(u32 channel)
{
	const char *dt_path = NULL;
	const char *dma_name = NULL;
	struct device_node *np;
	struct dma_chan *chan;

	if (channel >= ARRAY_SIZE(g_slave_chans))
		return NULL;

	if (g_slave_chans[channel])
		return g_slave_chans[channel];

	switch (channel) {
	case 11:
		dt_path = "/ahb@ffe0000000/uart@e0017000";
		dma_name = "tx";
		break;
	case 12:
		dt_path = "/ahb@ffe0000000/uart@e0017000";
		dma_name = "rx";
		break;
	case 13:
		dt_path = "/ahb@ffe0000000/uart@e0018000";
		dma_name = "tx";
		break;
	case 14:
		dt_path = "/ahb@ffe0000000/uart@e0018000";
		dma_name = "rx";
		break;
	case 15:
		dt_path = "/ahb@ffe0000000/uart@e0019000";
		dma_name = "tx";
		break;
	case 16:
		dt_path = "/ahb@ffe0000000/uart@e0019000";
		dma_name = "rx";
		break;
	default:
		return NULL;
	}

	np = of_find_node_by_path(dt_path);
	if (!np) {
		pr_err("amba_virt: DT node %s not found\n", dt_path);
		return NULL;
	}

	chan = of_dma_request_slave_channel(np, dma_name);
	of_node_put(np);
	if (IS_ERR_OR_NULL(chan)) {
		pr_err("amba_virt: failed to get slave channel for %s:%s\n",
		       dt_path, dma_name);
		return NULL;
	}

	g_slave_chans[channel] = chan;
	pr_info("amba_virt: acquired physical slave DMA channel for %s:%s (chan_id=%d)\n",
		dt_path, dma_name, chan->chan_id);
	return chan;
}

static int amba_virt_host_dma_slave(struct amba_virt_dev *dev,
				    struct amba_virt_dma_slave_xfer *xfer)
{
	struct dma_chan *chan;
	struct dma_slave_config cfg;
	struct dma_async_tx_descriptor *desc;
	struct completion done;
	dma_cookie_t cookie;
	phys_addr_t fifo_addr = 0;
	dma_addr_t dma_addr;
	void __iomem *shm_map = NULL;
	u64 buf_end;
	unsigned long timeout;
	int ret;

	if (!dev->shm_phys || !dev->shm_size)
		return -ENODEV;
	if (!xfer->buf_len || xfer->buf_len > (64 * 1024))
		return -EINVAL;
	if (xfer->direction != AMBA_VIRT_DMA_DIR_MEM_TO_DEV &&
	    xfer->direction != AMBA_VIRT_DMA_DIR_DEV_TO_MEM)
		return -EINVAL;

	/* Validate buffer range within host ivshmem extent */
	if (xfer->buf_phys < dev->shm_phys)
		return -ERANGE;
	buf_end = (u64)xfer->buf_phys + xfer->buf_len;
	if (buf_end > (u64)dev->shm_phys + dev->shm_size)
		return -ERANGE;

	switch (xfer->channel) {
	case 11:
	case 12:
		fifo_addr = 0xffe0017040ULL;
		break;
	case 13:
	case 14:
		fifo_addr = 0xffe0018040ULL;
		break;
	case 15:
	case 16:
		fifo_addr = 0xffe0019040ULL;
		break;
	default:
		return -ENXIO;
	}

	mutex_lock(&g_slave_dma_lock);
	chan = amba_virt_get_slave_chan(xfer->channel);
	if (!chan) {
		mutex_unlock(&g_slave_dma_lock);
		return -ENODEV;
	}

	/*
	 * Generic-DMA1 hardware registers are strictly 32-bit (DMA_BIT_MASK(32)).
	 * If the buffer physical address is >= 4 GiB, bounce it through a 32-bit
	 * DMA coherent buffer allocated from the DMA controller device.
	 */
	dma_addr = (dma_addr_t)xfer->buf_phys;
	if (xfer->buf_phys >= 0x100000000ULL) {
		if (!g_slave_bounce_virt) {
			g_slave_bounce_dev = chan->device->dev;
			g_slave_bounce_virt = dma_alloc_coherent(g_slave_bounce_dev,
								 65536,
								 &g_slave_bounce_phys,
								 GFP_KERNEL);
			if (!g_slave_bounce_virt) {
				mutex_unlock(&g_slave_dma_lock);
				return -ENOMEM;
			}
		}

		shm_map = ioremap_wc(xfer->buf_phys, xfer->buf_len);
		if (!shm_map) {
			mutex_unlock(&g_slave_dma_lock);
			return -ENOMEM;
		}

		if (xfer->direction == AMBA_VIRT_DMA_DIR_MEM_TO_DEV)
			memcpy_fromio(g_slave_bounce_virt, shm_map, xfer->buf_len);

		dma_addr = g_slave_bounce_phys;
	}

	memset(&cfg, 0, sizeof(cfg));
	if (xfer->direction == AMBA_VIRT_DMA_DIR_MEM_TO_DEV) {
		cfg.direction = DMA_MEM_TO_DEV;
		cfg.dst_addr = fifo_addr;
		cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
		cfg.dst_maxburst = 8;
	} else {
		cfg.direction = DMA_DEV_TO_MEM;
		cfg.src_addr = fifo_addr;
		cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
		cfg.src_maxburst = 8;
	}

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		if (shm_map)
			iounmap(shm_map);
		mutex_unlock(&g_slave_dma_lock);
		return ret;
	}

	init_completion(&done);
	dma_wmb();

	desc = dmaengine_prep_slave_single(chan, dma_addr,
					   xfer->buf_len,
					   (xfer->direction == AMBA_VIRT_DMA_DIR_MEM_TO_DEV) ?
					   DMA_MEM_TO_DEV : DMA_DEV_TO_MEM,
					   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		if (shm_map)
			iounmap(shm_map);
		mutex_unlock(&g_slave_dma_lock);
		return -EIO;
	}

	desc->callback = amba_virt_dma_complete_cb;
	desc->callback_param = &done;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		if (shm_map)
			iounmap(shm_map);
		mutex_unlock(&g_slave_dma_lock);
		return -EIO;
	}

	dma_async_issue_pending(chan);

	timeout = msecs_to_jiffies(xfer->timeout_ms ? xfer->timeout_ms : 1000);
	if (!wait_for_completion_timeout(&done, timeout)) {
		pr_err("amba_virt: DMA slave timeout on channel %u\n", xfer->channel);
		dmaengine_terminate_sync(chan);
		if (shm_map)
			iounmap(shm_map);
		mutex_unlock(&g_slave_dma_lock);
		return -ETIMEDOUT;
	}

	dma_rmb();

	if (shm_map) {
		if (xfer->direction == AMBA_VIRT_DMA_DIR_DEV_TO_MEM)
			memcpy_toio(shm_map, g_slave_bounce_virt, xfer->buf_len);
		iounmap(shm_map);
	}

	xfer->transferred = xfer->buf_len;
	mutex_unlock(&g_slave_dma_lock);
	return 0;
}

static int amba_virt_shm_open(struct inode *inode, struct file *filp)
{
	filp->private_data = (void *)0;
	i_size_write(file_inode(filp), 0x40000000ULL);
	return 0;
}

static int amba_virt_shm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return amba_virt_mmap_slice(&gdev, vma, 0);
}

static long amba_virt_shm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int dmabuf_fd = -1;
	int ret;

	switch (cmd) {
	case AMBA_VIRT_IOC_EXPORT_DMABUF:
		ret = amba_virt_export_dmabuf_slice(&gdev, 0, 0, 0x40000000ULL, &dmabuf_fd);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &dmabuf_fd, sizeof(dmabuf_fd))) {
			close_fd(dmabuf_fd);
			return -EFAULT;
		}
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations amba_virt_shm_fops = {
	.owner = THIS_MODULE,
	.open = amba_virt_shm_open,
	.mmap = amba_virt_shm_mmap,
	.unlocked_ioctl = amba_virt_shm_ioctl,
	.compat_ioctl = amba_virt_shm_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice amba_virt_shm_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AMBA_VIRT_SHM_DEV_NAME,
	.fops = &amba_virt_shm_fops,
	.mode = 0600,
};

static int amba_virt_shm0_open(struct inode *inode, struct file *filp)
{
	filp->private_data = (void *)0;
	i_size_write(file_inode(filp), 0x40000000ULL);
	return 0;
}

static int amba_virt_shm0_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return amba_virt_mmap_slice(&gdev, vma, 0);
}

static long amba_virt_shm0_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int dmabuf_fd = -1;
	int ret;

	switch (cmd) {
	case AMBA_VIRT_IOC_EXPORT_DMABUF:
		ret = amba_virt_export_dmabuf_slice(&gdev, 0, 0, 0x40000000ULL, &dmabuf_fd);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &dmabuf_fd, sizeof(dmabuf_fd))) {
			close_fd(dmabuf_fd);
			return -EFAULT;
		}
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations amba_virt_shm0_fops = {
	.owner = THIS_MODULE,
	.open = amba_virt_shm0_open,
	.mmap = amba_virt_shm0_mmap,
	.unlocked_ioctl = amba_virt_shm0_ioctl,
	.compat_ioctl = amba_virt_shm0_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice amba_virt_shm0_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "amba_virt_shm0",
	.fops = &amba_virt_shm0_fops,
	.mode = 0600,
};

static int amba_virt_shm1_open(struct inode *inode, struct file *filp)
{
	filp->private_data = (void *)1;
	i_size_write(file_inode(filp), 0x40000000ULL);
	return 0;
}

static int amba_virt_shm1_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return amba_virt_mmap_slice(&gdev, vma, 1);
}

static long amba_virt_shm1_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int dmabuf_fd = -1;
	int ret;

	switch (cmd) {
	case AMBA_VIRT_IOC_EXPORT_DMABUF:
		ret = amba_virt_export_dmabuf_slice(&gdev, 1, 0x40000000ULL, 0x40000000ULL, &dmabuf_fd);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &dmabuf_fd, sizeof(dmabuf_fd))) {
			close_fd(dmabuf_fd);
			return -EFAULT;
		}
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations amba_virt_shm1_fops = {
	.owner = THIS_MODULE,
	.open = amba_virt_shm1_open,
	.mmap = amba_virt_shm1_mmap,
	.unlocked_ioctl = amba_virt_shm1_ioctl,
	.compat_ioctl = amba_virt_shm1_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice amba_virt_shm1_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "amba_virt_shm1",
	.fops = &amba_virt_shm1_fops,
	.mode = 0600,
};

static int __init amba_virt_host_init(void)
{
	int (*window_get)(phys_addr_t *phys, size_t *size);
	phys_addr_t window_end;
	int ret;

	if (vsock_port == 2000) {
		pr_err("amba_virt: port 2000 is EVE VComLink; refusing\n");
		return -EINVAL;
	}

	memset(&gdev, 0, sizeof(gdev));
	if (!shm_phys) {
		window_get = symbol_get(cavalry_user_window_get);
		if (window_get) {
			ret = window_get(&gdev.shm_phys, &gdev.shm_size);
			if (!ret)
				cavalry_window_held = true;
			else
				symbol_put(cavalry_user_window_get);
		}
	}
	if (shm_phys) {
		gdev.shm_phys = (phys_addr_t)shm_phys;
		gdev.shm_size = shm_size;
	}
	if (gdev.shm_phys &&
	    (!gdev.shm_size || gdev.shm_size > U32_MAX ||
	     !PAGE_ALIGNED(gdev.shm_phys) ||
	     !PAGE_ALIGNED(gdev.shm_size) ||
	     check_add_overflow(gdev.shm_phys,
				(phys_addr_t)gdev.shm_size, &window_end))) {
		pr_err("amba_virt: invalid physical shared window\n");
		if (cavalry_window_held) {
			symbol_put(cavalry_user_window_get);
			cavalry_window_held = false;
		}
		return -EINVAL;
	}
	if (gdev.shm_phys) {
		gdev.gdma_copy = amba_virt_host_gdma_copy;
		gdev.dma_slave_xfer = amba_virt_host_dma_slave;
	} else {
		gdev.shm_path = shm_path;
	}

	ret = amba_virt_core_init(&gdev, true);
	if (ret) {
		if (cavalry_window_held) {
			symbol_put(cavalry_user_window_get);
			cavalry_window_held = false;
		}
		return ret;
	}
	gdev.vsock_port = vsock_port;

	if (gdev.shm_phys) {
		ret = misc_register(&amba_virt_shm_miscdev);
		if (ret) {
			pr_err("amba_virt: shared-window device failed %d\n", ret);
			amba_virt_core_exit(&gdev);
			if (cavalry_window_held) {
				symbol_put(cavalry_user_window_get);
				cavalry_window_held = false;
			}
			return ret;
		}
		ret = misc_register(&amba_virt_shm0_miscdev);
		if (ret)
			pr_warn("amba_virt: register shm0 failed %d (continuing)\n", ret);
		ret = misc_register(&amba_virt_shm1_miscdev);
		if (ret)
			pr_warn("amba_virt: register shm1 failed %d (continuing)\n", ret);
	}

	ret = amba_virt_vsock_listen(&gdev);
	if (ret) {
		pr_err("amba_virt: vsock listen failed %d\n", ret);
		if (gdev.shm_phys) {
			misc_deregister(&amba_virt_shm1_miscdev);
			misc_deregister(&amba_virt_shm0_miscdev);
			misc_deregister(&amba_virt_shm_miscdev);
		}
		amba_virt_core_exit(&gdev);
		if (cavalry_window_held) {
			symbol_put(cavalry_user_window_get);
			cavalry_window_held = false;
		}
		return ret;
	}

	/*
	 * The backing file is created by the hypervisor when the HVM domain
	 * starts, so at boot it usually does not exist yet. Loading must still
	 * succeed: /dev/amba_virt has to be present before the NOHYPER
	 * container is created, otherwise EVE injects nothing and the app comes
	 * up silently missing the device. The window is picked up on first use.
	 */
	amba_virt_attach_shm(&gdev);
	if (gdev.shm_phys)
		pr_info("amba_virt host: physical shm %pa size %zu, vsock port %u\n",
			&gdev.shm_phys, gdev.shm_size, vsock_port);
	else
		pr_info("amba_virt host: shm %s (%s), vsock port %u\n",
			shm_path, gdev.shm_file ? "attached" : "pending",
			vsock_port);
	return 0;
}

static void __exit amba_virt_host_exit(void)
{
	int i;

	mutex_lock(&g_slave_dma_lock);
	for (i = 0; i < ARRAY_SIZE(g_slave_chans); i++) {
		if (g_slave_chans[i]) {
			dma_release_channel(g_slave_chans[i]);
			g_slave_chans[i] = NULL;
		}
	}
	if (g_slave_bounce_virt && g_slave_bounce_dev) {
		dma_free_coherent(g_slave_bounce_dev, 65536,
				  g_slave_bounce_virt, g_slave_bounce_phys);
		g_slave_bounce_virt = NULL;
		g_slave_bounce_dev = NULL;
	}
	mutex_unlock(&g_slave_dma_lock);

	if (gdev.shm_phys) {
		misc_deregister(&amba_virt_shm1_miscdev);
		misc_deregister(&amba_virt_shm0_miscdev);
		misc_deregister(&amba_virt_shm_miscdev);
	}
	amba_virt_core_exit(&gdev);
	if (cavalry_window_held)
		symbol_put(cavalry_user_window_get);
}

module_init(amba_virt_host_init);
module_exit(amba_virt_host_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("amba_virt host (shm file + vsock listen)");
MODULE_AUTHOR("amba-virt");
MODULE_SOFTDEP("pre: cavalry");
