/*
 * amba_virt_iav_win.c
 *
 * Exposes the real IAV driver's DSP partition and bitstream buffer as
 * mmap-able nodes for the hypervisor to map into a guest (ivshmem BARs).
 * Dom0 amba-virt-server configures each node once from the partition
 * table the real driver reports; nothing here talks to the IAV driver.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pfn.h>
#include <linux/log2.h>
#include <linux/capability.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include <amba_virt_iav_win.h>

/* The DSP is 32-bit. iav@1 starts at 0x28000000 on N1-655. */
static unsigned long long win_min = 0x28000000ULL;
module_param(win_min, ullong, 0444);
MODULE_PARM_DESC(win_min, "Lowest physical address a window may cover");

static unsigned long long win_max = 0x100000000ULL;
module_param(win_max, ullong, 0444);
MODULE_PARM_DESC(win_max, "End (exclusive) of the physical range a window may cover");

/*
 * Largest padding behind a window. It is allocated in chunks because the buddy allocator
 * cannot return one contiguous block of more than a few MiB.
 */
#define IAV_WIN_MAX_PAD		(64ULL << 20)
#define IAV_WIN_CHUNK		(2ULL << 20)
#define IAV_WIN_MAX_CHUNKS	(IAV_WIN_MAX_PAD / IAV_WIN_CHUNK)
#define IAV_WIN_MAX_SIZE	(4ULL << 30)

struct iav_win {
	struct miscdevice misc;
	struct mutex lock;
	bool configured;
	int maps;
	u64 phys;
	u64 len;
	u64 size;
	void *pad[IAV_WIN_MAX_CHUNKS];	/* zeroed pages behind the real range */
	u64 pad_len;
};

#define IAV_WIN_NODES	(2 * AMBA_VIRT_IAV_WIN_SETS)

static struct iav_win g_win[IAV_WIN_NODES];

static inline struct iav_win *iav_win_of(struct file *filp)
{
	return filp->private_data;
}

static int iav_win_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *m = filp->private_data;

	filp->private_data = container_of(m, struct iav_win, misc);
	return 0;
}

static int iav_win_validate(const struct amba_virt_iav_win_cfg *c)
{
	u64 end;

	if (c->flags || c->reserved)
		return -EINVAL;
	if (!c->len || !c->size)
		return -EINVAL;
	if (!PAGE_ALIGNED(c->phys) || !PAGE_ALIGNED(c->len) || !PAGE_ALIGNED(c->size))
		return -EINVAL;
	if (!is_power_of_2(c->size) || c->size > IAV_WIN_MAX_SIZE || c->size < c->len)
		return -EINVAL;
	if (check_add_overflow(c->phys, c->len, &end))
		return -EINVAL;
	if (c->phys < win_min || end > win_max)
		return -ERANGE;
	if (c->size - c->len > IAV_WIN_MAX_PAD)
		return -EINVAL;
	return 0;
}

static u64 iav_win_chunk_len(u64 pad_len, unsigned int i)
{
	u64 start = (u64)i * IAV_WIN_CHUNK;

	return min(IAV_WIN_CHUNK, pad_len - start);
}

static void iav_win_free_pad(void **pad, u64 pad_len)
{
	unsigned int i, n = DIV_ROUND_UP(pad_len, IAV_WIN_CHUNK);

	for (i = 0; i < n; i++) {
		if (pad[i])
			free_pages_exact(pad[i], iav_win_chunk_len(pad_len, i));
		pad[i] = NULL;
	}
}

static void iav_win_drop(struct iav_win *w)
{
	iav_win_free_pad(w->pad, w->pad_len);
	w->pad_len = 0;
	w->configured = false;
	w->phys = w->len = w->size = 0;
}

/* The identity header goes in the last bytes of the padding, when there is a page of it. */
static void iav_win_write_header(struct iav_win *w, const struct amba_virt_iav_win_cfg *c)
{
	struct amba_virt_iav_win_hdr hdr;
	unsigned int last;
	u64 last_len;

	if (w->pad_len < PAGE_SIZE)
		return;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = AMBA_VIRT_IAV_WIN_HDR_MAGIC;
	hdr.version = AMBA_VIRT_IAV_WIN_HDR_VERSION;
	hdr.window_id = (u32)((w - g_win) & 1);	/* nodes alternate DSP, BSB in each set */
	hdr.part_len = c->len;
	hdr.size = c->size;
	last = DIV_ROUND_UP(w->pad_len, IAV_WIN_CHUNK) - 1;
	last_len = iav_win_chunk_len(w->pad_len, last);
	memcpy((u8 *)w->pad[last] + last_len - AMBA_VIRT_IAV_WIN_HDR_BYTES, &hdr, sizeof(hdr));
}

static long iav_win_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct iav_win *w = iav_win_of(filp);
	struct amba_virt_iav_win_cfg c;
	long ret = 0;

	switch (cmd) {
	case AMBA_VIRT_IAV_WIN_IOC_GET:
		memset(&c, 0, sizeof(c));
		mutex_lock(&w->lock);
		c.phys = w->phys;
		c.len = w->len;
		c.size = w->size;
		mutex_unlock(&w->lock);
		return copy_to_user((void __user *)arg, &c, sizeof(c)) ? -EFAULT : 0;
	case AMBA_VIRT_IAV_WIN_IOC_CONFIG:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (copy_from_user(&c, (void __user *)arg, sizeof(c)))
			return -EFAULT;
		ret = iav_win_validate(&c);
		if (ret)
			return ret;
		mutex_lock(&w->lock);
		if (w->maps) {
			ret = -EBUSY;
		} else {
			void *pad[IAV_WIN_MAX_CHUNKS] = { NULL };
			u64 pad_len = c.size - c.len;
			unsigned int i, n = DIV_ROUND_UP(pad_len, IAV_WIN_CHUNK);

			for (i = 0; i < n && !ret; i++) {
				pad[i] = alloc_pages_exact(iav_win_chunk_len(pad_len, i),
							   GFP_KERNEL | __GFP_ZERO);
				if (!pad[i])
					ret = -ENOMEM;
			}
			if (ret) {
				iav_win_free_pad(pad, pad_len);
			} else {
				iav_win_drop(w);
				memcpy(w->pad, pad, sizeof(pad));
				w->pad_len = pad_len;
				iav_win_write_header(w, &c);
				w->phys = c.phys;
				w->len = c.len;
				w->size = c.size;
				w->configured = true;
			}
		}
		mutex_unlock(&w->lock);
		return ret;
	case AMBA_VIRT_IAV_WIN_IOC_CLEAR:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		mutex_lock(&w->lock);
		if (w->maps)
			ret = -EBUSY;
		else
			iav_win_drop(w);
		mutex_unlock(&w->lock);
		return ret;
	default:
		return -ENOTTY;
	}
}

static void iav_win_vma_open(struct vm_area_struct *vma)
{
	struct iav_win *w = vma->vm_private_data;

	mutex_lock(&w->lock);
	w->maps++;
	mutex_unlock(&w->lock);
}

static void iav_win_vma_close(struct vm_area_struct *vma)
{
	struct iav_win *w = vma->vm_private_data;

	mutex_lock(&w->lock);
	w->maps--;
	mutex_unlock(&w->lock);
}

static const struct vm_operations_struct iav_win_vm_ops = {
	.open = iav_win_vma_open,
	.close = iav_win_vma_close,
};

static int iav_win_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct iav_win *w = iav_win_of(filp);
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 size = vma->vm_end - vma->vm_start;
	u64 end, real_end, n;
	unsigned long uaddr = vma->vm_start;
	pgprot_t pad_prot;
	int ret = 0;

	/* The hypervisor maps the whole node shared. Nothing else is valid. */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (check_add_overflow(off, size, &end))
		return -EINVAL;

	mutex_lock(&w->lock);
	if (!w->configured) {
		ret = -ENODEV;
		goto out;
	}
	if (end > w->size) {
		ret = -EINVAL;
		goto out;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif
	/*
	 * The real range is DSP memory the CPU must see as the DSP wrote it, so it is mapped
	 * write-combined. The padding is ordinary RAM that the kernel zeroed through its cached
	 * linear map; a non-cached alias would read stale RAM instead of the zeros, so it keeps
	 * the default cacheable attributes.
	 */
	pad_prot = vma->vm_page_prot;
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	real_end = min(end, w->len);
	if (off < real_end) {
		n = real_end - off;
		ret = remap_pfn_range(vma, uaddr, PHYS_PFN(w->phys + off), n,
				      vma->vm_page_prot);
		if (ret)
			goto out;
		uaddr += n;
		off += n;
	}
	while (off < end) {
		u64 rel = off - w->len;
		unsigned int i = rel / IAV_WIN_CHUNK;
		u64 in_chunk = rel % IAV_WIN_CHUNK;

		n = min(end - off, iav_win_chunk_len(w->pad_len, i) - in_chunk);
		ret = remap_pfn_range(vma, uaddr,
				      page_to_pfn(virt_to_page(w->pad[i])) +
				      PHYS_PFN(in_chunk),
				      n, pad_prot);
		if (ret)
			goto out;
		uaddr += n;
		off += n;
	}
	vma->vm_private_data = w;
	vma->vm_ops = &iav_win_vm_ops;
	w->maps++;
out:
	mutex_unlock(&w->lock);
	return ret;
}

static const struct file_operations iav_win_fops = {
	.owner = THIS_MODULE,
	.open = iav_win_open,
	.mmap = iav_win_mmap,
	.unlocked_ioctl = iav_win_ioctl,
	.compat_ioctl = iav_win_ioctl,
	.llseek = no_llseek,
};

static const char * const iav_win_names[IAV_WIN_NODES] = {
	AMBA_VIRT_IAV_WIN_DSP_NAME,
	AMBA_VIRT_IAV_WIN_BSB_NAME,
	AMBA_VIRT_IAV_WIN_DSP1_NAME,
	AMBA_VIRT_IAV_WIN_BSB1_NAME,
};

static int __init iav_win_init(void)
{
	int i, ret;

	for (i = 0; i < IAV_WIN_NODES; i++) {
		struct iav_win *w = &g_win[i];

		mutex_init(&w->lock);
		w->misc.minor = MISC_DYNAMIC_MINOR;
		w->misc.name = iav_win_names[i];
		w->misc.fops = &iav_win_fops;
		w->misc.mode = 0600;
		ret = misc_register(&w->misc);
		if (ret) {
			while (--i >= 0)
				misc_deregister(&g_win[i].misc);
			return ret;
		}
	}
	pr_info("amba_virt_iav_win: %d window sets (DSP and BSB each) registered\n", AMBA_VIRT_IAV_WIN_SETS);
	return 0;
}

static void __exit iav_win_exit(void)
{
	int i;

	for (i = 0; i < IAV_WIN_NODES; i++) {
		misc_deregister(&g_win[i].misc);
		iav_win_drop(&g_win[i]);
	}
}

module_init(iav_win_init);
module_exit(iav_win_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Host IAV windows for guest ivshmem BARs");
