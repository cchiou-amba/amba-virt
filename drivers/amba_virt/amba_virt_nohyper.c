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

#define AMBA_VIRT_MAX_MINORS 8

static unsigned int num_instances = 2;
module_param(num_instances, uint, 0644);
MODULE_PARM_DESC(num_instances, "Number of host amba_virt instances (default 2, max 8)");

static struct amba_virt_dev g_devs[AMBA_VIRT_MAX_MINORS];
static char g_shm_paths[AMBA_VIRT_MAX_MINORS][128];
static unsigned int g_active_instances = 0;
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

#include "amba_virt_slice.h"
#include <linux/random.h>
#include <linux/io.h>

struct amba_virt_slice_state {
	struct amba_virt_slice_desc desc;
	u64 nonce;
	u32 bound_cid;
	int open_count;
};

static struct amba_virt_slice_list g_slice_list;
static struct amba_virt_slice_state g_slice_states[8];
static DEFINE_MUTEX(g_slice_lock);

static void amba_virt_write_slice_claim(unsigned int slice_idx,
					const struct amba_virt_slice_desc *desc,
					u64 nonce)
{
	phys_addr_t claim_phys = desc->phys + desc->usable_size;
	void *va;

	va = memremap(claim_phys, PAGE_SIZE, MEMREMAP_WT);
	if (!va)
		va = memremap(claim_phys, PAGE_SIZE, MEMREMAP_WB);
	if (va) {
		struct amba_virt_slice_claim claim;
		memset(&claim, 0, sizeof(claim));
		claim.magic = AMBA_VIRT_SLICE_CLAIM_MAGIC;
		claim.index = slice_idx;
		claim.usable_size = desc->usable_size;
		claim.reserved = 0;
		claim.nonce = nonce;
		memcpy(va, &claim, sizeof(claim));
		if (PAGE_SIZE > sizeof(claim))
			memcpy((char *)va + PAGE_SIZE - sizeof(claim), &claim, sizeof(claim));
		dma_wmb();
		memunmap(va);
	} else {
		pr_err("amba_virt: failed to memremap claim page at 0x%llx\n",
		       (unsigned long long)claim_phys);
	}
}

int amba_virt_slice_get_list(struct amba_virt_slice_list *out_list)
{
	mutex_lock(&g_slice_lock);
	memcpy(out_list, &g_slice_list, sizeof(*out_list));
	mutex_unlock(&g_slice_lock);
	return 0;
}

int amba_virt_slice_get_desc(unsigned int slice_idx, struct amba_virt_slice_desc *out_desc)
{
	int ret = -ENOENT;

	mutex_lock(&g_slice_lock);
	if (slice_idx < g_slice_list.count) {
		*out_desc = g_slice_states[slice_idx].desc;
		ret = 0;
	}
	mutex_unlock(&g_slice_lock);
	return ret;
}

int amba_virt_slice_bind(u32 cid, u64 nonce, struct amba_virt_slice_desc *out_desc)
{
	int ret = -ENOENT;
	unsigned int i, j;

	if (cid <= 2)
		return -EINVAL;
	if (nonce == 0)
		return -ENOENT;

	mutex_lock(&g_slice_lock);
	for (i = 0; i < g_slice_list.count; i++) {
		if (g_slice_states[i].nonce == nonce) {
			if (g_slice_states[i].bound_cid != 0 && g_slice_states[i].bound_cid != cid) {
				ret = -EBUSY;
				goto out_unlock;
			}
			for (j = 0; j < g_slice_list.count; j++) {
				if (j != i && g_slice_states[j].bound_cid == cid) {
					ret = -EBUSY;
					goto out_unlock;
				}
			}
			g_slice_states[i].bound_cid = cid;
			if (out_desc)
				*out_desc = g_slice_states[i].desc;
			ret = 0;
			goto out_unlock;
		}
	}

out_unlock:
	mutex_unlock(&g_slice_lock);
	return ret;
}

int amba_virt_slice_get_bindings(struct amba_virt_binding_list *out_list)
{
	unsigned int i;

	memset(out_list, 0, sizeof(*out_list));
	mutex_lock(&g_slice_lock);
	for (i = 0; i < g_slice_list.count && out_list->count < 8; i++) {
		if (g_slice_states[i].bound_cid != 0) {
			struct amba_virt_slice_binding *entry;

			entry = &out_list->entries[out_list->count++];
			entry->cid = g_slice_states[i].bound_cid;
			entry->slice = g_slice_states[i].desc;
		}
	}
	mutex_unlock(&g_slice_lock);
	return 0;
}

void amba_virt_slice_unbind_cid(u32 cid)
{
	unsigned int i;

	if (cid == 0)
		return;

	mutex_lock(&g_slice_lock);
	for (i = 0; i < g_slice_list.count; i++) {
		if (g_slice_states[i].bound_cid == cid) {
			g_slice_states[i].bound_cid = 0;
		}
	}
	mutex_unlock(&g_slice_lock);
}

int amba_virt_slice_open(unsigned int slice_idx)
{
	int ret = 0;

	mutex_lock(&g_slice_lock);
	if (slice_idx < g_slice_list.count) {
		struct amba_virt_slice_state *s = &g_slice_states[slice_idx];
		s->open_count++;
		if (s->open_count == 1) {
			u64 nonce;
			do {
				nonce = get_random_u64();
			} while (nonce == 0);
			s->nonce = nonce;
			s->bound_cid = 0;
			amba_virt_write_slice_claim(slice_idx, &s->desc, nonce);
		}
	} else {
		ret = -ENODEV;
	}
	mutex_unlock(&g_slice_lock);
	return ret;
}

void amba_virt_slice_release(unsigned int slice_idx)
{
	mutex_lock(&g_slice_lock);
	if (slice_idx < g_slice_list.count) {
		struct amba_virt_slice_state *s = &g_slice_states[slice_idx];
		s->open_count--;
		if (s->open_count <= 0) {
			s->open_count = 0;
			s->nonce = 0;
			s->bound_cid = 0;
		}
	}
	mutex_unlock(&g_slice_lock);
}

static int amba_virt_shm_open_idx(struct file *filp, unsigned int slice_idx)
{
	struct amba_virt_slice_desc desc;
	int ret;

	filp->private_data = (void *)(uintptr_t)slice_idx;
	ret = amba_virt_slice_open(slice_idx);
	if (ret)
		return ret;
	if (amba_virt_slice_get_desc(slice_idx, &desc) == 0)
		i_size_write(file_inode(filp), desc.slice_size);
	return 0;
}

static int amba_virt_shm_release_idx(struct inode *inode, struct file *filp)
{
	unsigned int slice_idx = (unsigned int)(uintptr_t)filp->private_data;
	amba_virt_slice_release(slice_idx);
	return 0;
}

static int amba_virt_shm_open(struct inode *inode, struct file *filp)
{
	return amba_virt_shm_open_idx(filp, 0);
}

static int amba_virt_shm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return amba_virt_mmap_slice(&g_devs[0], vma, 0);
}

static long amba_virt_shm_ioctl_idx(struct file *filp, unsigned int cmd, unsigned long arg, unsigned int slice_idx)
{
	int dmabuf_fd = -1;
	struct amba_virt_slice_desc desc;
	int ret;

	ret = amba_virt_slice_get_desc(slice_idx, &desc);
	if (ret)
		return ret;

	switch (cmd) {
	case AMBA_VIRT_IOC_GET_SLICE:
		if (copy_to_user((void __user *)arg, &desc, sizeof(desc)))
			return -EFAULT;
		return 0;
	case AMBA_VIRT_IOC_EXPORT_DMABUF:
		ret = amba_virt_export_dmabuf_slice(&g_devs[0], slice_idx, desc.offset, desc.slice_size, &dmabuf_fd);
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

static long amba_virt_shm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return amba_virt_shm_ioctl_idx(filp, cmd, arg, 0);
}

static const struct file_operations amba_virt_shm_fops = {
	.owner = THIS_MODULE,
	.open = amba_virt_shm_open,
	.release = amba_virt_shm_release_idx,
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
	return amba_virt_shm_open_idx(filp, 0);
}

static int amba_virt_shm0_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return amba_virt_mmap_slice(&g_devs[0], vma, 0);
}

static long amba_virt_shm0_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return amba_virt_shm_ioctl_idx(filp, cmd, arg, 0);
}

static const struct file_operations amba_virt_shm0_fops = {
	.owner = THIS_MODULE,
	.open = amba_virt_shm0_open,
	.release = amba_virt_shm_release_idx,
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
	return amba_virt_shm_open_idx(filp, 1);
}

static int amba_virt_shm1_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return amba_virt_mmap_slice(&g_devs[0], vma, 1);
}

static long amba_virt_shm1_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return amba_virt_shm_ioctl_idx(filp, cmd, arg, 1);
}

static const struct file_operations amba_virt_shm1_fops = {
	.owner = THIS_MODULE,
	.open = amba_virt_shm1_open,
	.release = amba_virt_shm_release_idx,
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
	int ret = 0;
	unsigned int i;

	if (vsock_port == 2000) {
		pr_err("amba_virt: port 2000 is EVE VComLink; refusing\n");
		return -EINVAL;
	}

	memset(g_devs, 0, sizeof(g_devs));
	if (!shm_phys) {
		window_get = symbol_get(cavalry_user_window_get);
		if (window_get) {
			ret = window_get(&g_devs[0].shm_phys, &g_devs[0].shm_size);
			if (!ret)
				cavalry_window_held = true;
			else
				symbol_put(cavalry_user_window_get);
		}
	}
	if (shm_phys) {
		g_devs[0].shm_phys = (phys_addr_t)shm_phys;
		g_devs[0].shm_size = shm_size;
	}
	if (g_devs[0].shm_phys &&
	    (!g_devs[0].shm_size || g_devs[0].shm_size > U32_MAX ||
	     !PAGE_ALIGNED(g_devs[0].shm_phys) ||
	     !PAGE_ALIGNED(g_devs[0].shm_size) ||
	     check_add_overflow(g_devs[0].shm_phys,
				(phys_addr_t)g_devs[0].shm_size, &window_end))) {
		pr_err("amba_virt: invalid physical shared window\n");
		if (cavalry_window_held) {
			symbol_put(cavalry_user_window_get);
			cavalry_window_held = false;
		}
		return -EINVAL;
	}

	if (num_instances < 1) num_instances = 1;
	if (num_instances > AMBA_VIRT_MAX_MINORS) num_instances = AMBA_VIRT_MAX_MINORS;

	g_active_instances = 0;
	for (i = 0; i < num_instances; i++) {
		if (i == 0) {
			g_devs[i].shm_path = shm_path;
			if (g_devs[i].shm_phys)
				g_devs[i].gdma_copy = amba_virt_host_gdma_copy;
		} else {
			snprintf(g_shm_paths[i], sizeof(g_shm_paths[i]), "/dev/shm/amba-virt-%u", i);
			g_devs[i].shm_path = g_shm_paths[i];
		}

		ret = amba_virt_core_init_instance(&g_devs[i], true, i);
		if (ret) {
			pr_err("amba_virt: init instance %u failed %d\n", i, ret);
			break;
		}
		g_devs[i].vsock_port = vsock_port + i;
		ret = amba_virt_vsock_listen(&g_devs[i]);
		if (ret) {
			pr_warn("amba_virt: vsock listen for instance %u on port %u failed %d\n",
				i, g_devs[i].vsock_port, ret);
		}
		amba_virt_attach_shm(&g_devs[i]);
		g_active_instances++;
	}

	if (g_active_instances == 0) {
		if (cavalry_window_held) {
			symbol_put(cavalry_user_window_get);
			cavalry_window_held = false;
		}
		return ret;
	}

	if (g_devs[0].shm_phys) {
		unsigned int s_idx;
		memset(&g_slice_list, 0, sizeof(g_slice_list));
		memset(g_slice_states, 0, sizeof(g_slice_states));
		ret = amba_virt_slice_compute_geometry(g_devs[0].shm_phys,
						       g_devs[0].shm_size,
						       PAGE_SIZE, 2, &g_slice_list);
		if (!ret && g_slice_list.count > 0) {
			for (s_idx = 0; s_idx < g_slice_list.count; s_idx++) {
				g_slice_states[s_idx].desc = g_slice_list.slices[s_idx];
			}
			pr_info("amba_virt: published %u slices (slice_size=%llu, usable_size=%u)\n",
				g_slice_list.count,
				(unsigned long long)g_slice_list.slices[0].slice_size,
				g_slice_list.slices[0].usable_size);
		} else {
			pr_warn("amba_virt: slice geometry computation failed (%d)\n", ret);
		}

		ret = misc_register(&amba_virt_shm_miscdev);
		if (ret)
			pr_warn("amba_virt: register shm miscdev failed %d\n", ret);
		ret = misc_register(&amba_virt_shm0_miscdev);
		if (ret)
			pr_warn("amba_virt: register shm0 miscdev failed %d\n", ret);
		ret = misc_register(&amba_virt_shm1_miscdev);
		if (ret)
			pr_warn("amba_virt: register shm1 miscdev failed %d\n", ret);
	}

	pr_info("amba_virt host: %u instances active (/dev/amba_virt0..%u), vsock base port %u\n",
		g_active_instances, g_active_instances - 1, vsock_port);
	return 0;
}

static void __exit amba_virt_host_exit(void)
{
	unsigned int i;

	if (g_devs[0].shm_phys) {
		misc_deregister(&amba_virt_shm1_miscdev);
		misc_deregister(&amba_virt_shm0_miscdev);
		misc_deregister(&amba_virt_shm_miscdev);
	}
	for (i = 0; i < g_active_instances; i++) {
		amba_virt_core_exit_instance(&g_devs[i]);
	}
	if (cavalry_window_held)
		symbol_put(cavalry_user_window_get);
}

module_init(amba_virt_host_init);
module_exit(amba_virt_host_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("amba_virt host (shm file + vsock listen)");
MODULE_AUTHOR("amba-virt");
MODULE_SOFTDEP("pre: cavalry");
