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

static int amba_virt_shm_open(struct inode *inode, struct file *filp)
{
	i_size_write(file_inode(filp), gdev.shm_size);
	return 0;
}

static int amba_virt_shm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return amba_virt_mmap_window(&gdev, vma);
}

static const struct file_operations amba_virt_shm_fops = {
	.owner = THIS_MODULE,
	.open = amba_virt_shm_open,
	.mmap = amba_virt_shm_mmap,
	.llseek = no_llseek,
};

static struct miscdevice amba_virt_shm_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AMBA_VIRT_SHM_DEV_NAME,
	.fops = &amba_virt_shm_fops,
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
	}

	ret = amba_virt_vsock_listen(&gdev);
	if (ret) {
		pr_err("amba_virt: vsock listen failed %d\n", ret);
		if (gdev.shm_phys)
			misc_deregister(&amba_virt_shm_miscdev);
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
	if (gdev.shm_phys)
		misc_deregister(&amba_virt_shm_miscdev);
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
