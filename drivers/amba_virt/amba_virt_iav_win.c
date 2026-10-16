/*
 * amba_virt_iav_win.c
 *
 * Exposes the real IAV driver's DSP partition, bitstream buffer, decode
 * partition and overlay partition as mmap-able nodes for the hypervisor to map into a guest (ivshmem BARs), and
 * eight aux aperture nodes, one per Cavalry lease index, that compose several partitions into one BAR.
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
#include <linux/slab.h>
#include <linux/highmem.h>

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

#define IAV_WIN_NODES	(AMBA_VIRT_IAV_WIN_KINDS * AMBA_VIRT_IAV_WIN_SETS)

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
	hdr.window_id = (u32)((w - g_win) % AMBA_VIRT_IAV_WIN_KINDS);	/* nodes run DSP, BSB, DEC, OVL in each set */
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
#ifdef VM_ALLOW_ANY_UNCACHED
		if (VM_ALLOW_ANY_UNCACHED != VM_NONE)
			c.flags |= AMBA_VIRT_IAV_WIN_F_NORMAL_NC;
#endif
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
	 * The real range is no-map memory, which KVM maps Device at stage 2 unless the VMA allows any uncached
	 * attribute. Device memory makes an unaligned access or DC ZVA from the guest an abort KVM cannot handle,
	 * which destroys the VM. With the flag KVM maps it Normal-NC, the attribute bare metal uses for these
	 * partitions. The padding is RAM, which KVM maps cacheable whatever the flag says.
	 */
#ifdef VM_ALLOW_ANY_UNCACHED
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED);
#else
	vma->vm_flags |= VM_ALLOW_ANY_UNCACHED;
#endif
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

/* ---- the per-guest aux apertures (plan_iav_usb_yolo Envelope F) ---- */

/*
 * One node per Cavalry lease index. The server composes the node for the guest holding that lease: real ranges of the
 * partitions its role may map at 64 KiB-aligned offsets, one zeroed scratch page behind every other page, and the
 * identity header in the last bytes of the last page. Every aux node and the module-wide list of sources are guarded by
 * one mutex: configuration is rare and a node's mapping count, ranges and the sources are checked together.
 */
struct iav_aux {
	struct miscdevice misc;
	unsigned int index;
	char name[32];
	bool configured;
	int maps;
	struct amba_virt_iav_aux_cfg cfg;
	struct page *scratch;	/* every page of the BAR that is neither a range nor the header page */
	struct page *hdr;	/* the last page of the BAR */
};

static struct iav_aux g_aux[AMBA_VIRT_IAV_AUX_NODES];
static DEFINE_MUTEX(g_aux_lock);
static struct amba_virt_iav_aux_sources g_sources;

static bool iav_aux_phys_ok(u64 phys, u64 len)
{
	u64 end;

	if (!len || !PAGE_ALIGNED(phys) || !PAGE_ALIGNED(len))
		return false;
	if (check_add_overflow(phys, len, &end))
		return false;
	return phys >= win_min && end <= win_max;
}

/* Called with g_aux_lock held. */
static bool iav_aux_is_source(const struct amba_virt_iav_aux_sources *s, u64 phys, u64 len, u32 pid)
{
	u32 i;

	for (i = 0; i < s->count && i < AMBA_VIRT_IAV_AUX_MAX_SOURCES; i++)
		if (s->src[i].phys == phys && s->src[i].len == len && s->src[i].pid == pid)
			return true;
	return false;
}

static bool iav_aux_zero(const void *p, size_t n)
{
	const u8 *b = p;
	size_t i;

	for (i = 0; i < n; i++)
		if (b[i])
			return false;
	return true;
}

/* Everything a configuration must satisfy, before the sources are consulted. Called with no lock. */
static int iav_aux_validate(const struct amba_virt_iav_aux_cfg *c)
{
	u32 i, j;

	if (c->flags || c->maps || c->reserved || !c->epoch)
		return -EINVAL;
	if (!is_power_of_2(c->size) || c->size < AMBA_VIRT_IAV_AUX_MIN_SIZE || c->size > AMBA_VIRT_IAV_AUX_MAX_SIZE)
		return -EINVAL;
	if (!c->count || c->count > AMBA_VIRT_IAV_AUX_MAX_RANGES)
		return -EINVAL;
	for (i = 0; i < AMBA_VIRT_IAV_AUX_MAX_RANGES; i++) {
		const struct amba_virt_iav_aux_range *r = &c->range[i];
		u64 end;

		if (i >= c->count) {
			if (!iav_aux_zero(r, sizeof(*r)))
				return -EINVAL;
			continue;
		}
		if (r->flags)
			return -EINVAL;	/* read-only ranges are reserved (phase 3) */
		if (!iav_aux_phys_ok(r->phys, r->len))
			return -ERANGE;
		if (r->offset % AMBA_VIRT_IAV_AUX_ALIGN || check_add_overflow(r->offset, r->len, &end) ||
		    end > c->size - PAGE_SIZE)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			const struct amba_virt_iav_aux_range *o = &c->range[j];

			if (r->offset < o->offset + o->len && o->offset < r->offset + r->len)
				return -EINVAL;	/* two ranges at one place in the BAR */
			if (r->phys < o->phys + o->len && o->phys < r->phys + r->len)
				return -EINVAL;	/* one memory twice */
		}
	}
	return 0;
}

static void iav_aux_write_header(struct iav_aux *x)
{
	struct amba_virt_iav_aux_hdr hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = AMBA_VIRT_IAV_AUX_HDR_MAGIC;
	hdr.version = AMBA_VIRT_IAV_AUX_HDR_VERSION;
	hdr.node = x->index;
	hdr.count = x->cfg.count;
	hdr.epoch = x->cfg.epoch;
	hdr.size = x->cfg.size;
	memcpy((u8 *)page_address(x->hdr) + PAGE_SIZE - AMBA_VIRT_IAV_WIN_HDR_BYTES, &hdr, sizeof(hdr));
}

static int iav_aux_config(struct iav_aux *x, const struct amba_virt_iav_aux_cfg *c)
{
	u32 i;
	int ret;

	ret = iav_aux_validate(c);
	if (ret)
		return ret;
	mutex_lock(&g_aux_lock);
	if (x->maps) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < c->count; i++) {
		if (!iav_aux_is_source(&g_sources, c->range[i].phys, c->range[i].len, c->range[i].pid)) {
			ret = -EPERM;
			goto out;
		}
	}
	if (!x->scratch)
		x->scratch = alloc_page(GFP_KERNEL);
	if (!x->hdr)
		x->hdr = alloc_page(GFP_KERNEL);
	if (!x->scratch || !x->hdr) {
		ret = -ENOMEM;
		goto out;
	}
	/* What the previous guest of this node wrote into its scratch and header pages is not the next one's to read. */
	clear_highpage(x->scratch);
	clear_highpage(x->hdr);
	x->cfg = *c;
	iav_aux_write_header(x);
	x->configured = true;
out:
	mutex_unlock(&g_aux_lock);
	return ret;
}

static int iav_aux_set_sources(const struct amba_virt_iav_aux_sources *s)
{
	u32 i, n;
	int ret = 0;

	if (s->reserved || s->count > AMBA_VIRT_IAV_AUX_MAX_SOURCES)
		return -EINVAL;
	for (i = 0; i < AMBA_VIRT_IAV_AUX_MAX_SOURCES; i++) {
		if (i >= s->count) {
			if (!iav_aux_zero(&s->src[i], sizeof(s->src[i])))
				return -EINVAL;
			continue;
		}
		if (s->src[i].reserved || !iav_aux_phys_ok(s->src[i].phys, s->src[i].len))
			return -EINVAL;
	}
	mutex_lock(&g_aux_lock);
	/* A configured node keeps only ranges the new list still names. */
	for (n = 0; n < AMBA_VIRT_IAV_AUX_NODES && !ret; n++) {
		if (!g_aux[n].configured)
			continue;
		for (i = 0; i < g_aux[n].cfg.count; i++) {
			const struct amba_virt_iav_aux_range *r = &g_aux[n].cfg.range[i];

			if (!iav_aux_is_source(s, r->phys, r->len, r->pid)) {
				ret = -EBUSY;
				break;
			}
		}
	}
	if (!ret)
		g_sources = *s;
	mutex_unlock(&g_aux_lock);
	return ret;
}

static long iav_aux_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct iav_aux *x = filp->private_data;
	long ret = 0;

	switch (cmd) {
	case AMBA_VIRT_IAV_WIN_IOC_AUX_GET: {
		struct amba_virt_iav_aux_cfg c;

		mutex_lock(&g_aux_lock);
		if (x->configured)
			c = x->cfg;
		else
			memset(&c, 0, sizeof(c));
		c.maps = (u32)x->maps;
		c.flags = 0;
#ifdef VM_ALLOW_ANY_UNCACHED
		if (VM_ALLOW_ANY_UNCACHED != VM_NONE)
			c.flags |= AMBA_VIRT_IAV_WIN_F_NORMAL_NC;
#endif
		mutex_unlock(&g_aux_lock);
		return copy_to_user((void __user *)arg, &c, sizeof(c)) ? -EFAULT : 0;
	}
	case AMBA_VIRT_IAV_WIN_IOC_AUX_CONFIG: {
		struct amba_virt_iav_aux_cfg *c;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		c = kmalloc(sizeof(*c), GFP_KERNEL);
		if (!c)
			return -ENOMEM;
		if (copy_from_user(c, (void __user *)arg, sizeof(*c)))
			ret = -EFAULT;
		else
			ret = iav_aux_config(x, c);
		kfree(c);
		return ret;
	}
	case AMBA_VIRT_IAV_WIN_IOC_AUX_SOURCES: {
		struct amba_virt_iav_aux_sources *s;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		s = kmalloc(sizeof(*s), GFP_KERNEL);
		if (!s)
			return -ENOMEM;
		if (copy_from_user(s, (void __user *)arg, sizeof(*s)))
			ret = -EFAULT;
		else
			ret = iav_aux_set_sources(s);
		kfree(s);
		return ret;
	}
	case AMBA_VIRT_IAV_WIN_IOC_CLEAR:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		mutex_lock(&g_aux_lock);
		if (x->maps) {
			ret = -EBUSY;
		} else {
			x->configured = false;
			memset(&x->cfg, 0, sizeof(x->cfg));
		}
		mutex_unlock(&g_aux_lock);
		return ret;
	default:
		return -ENOTTY;
	}
}

static int iav_aux_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *m = filp->private_data;

	filp->private_data = container_of(m, struct iav_aux, misc);
	return 0;
}

static void iav_aux_vma_open(struct vm_area_struct *vma)
{
	struct iav_aux *x = vma->vm_private_data;

	mutex_lock(&g_aux_lock);
	x->maps++;
	mutex_unlock(&g_aux_lock);
}

static void iav_aux_vma_close(struct vm_area_struct *vma)
{
	struct iav_aux *x = vma->vm_private_data;

	mutex_lock(&g_aux_lock);
	x->maps--;
	mutex_unlock(&g_aux_lock);
}

static const struct vm_operations_struct iav_aux_vm_ops = {
	.open = iav_aux_vma_open,
	.close = iav_aux_vma_close,
};

/* The range covering BAR offset off, or NULL. Called with g_aux_lock held. */
static const struct amba_virt_iav_aux_range *iav_aux_range_at(const struct iav_aux *x, u64 off)
{
	u32 i;

	for (i = 0; i < x->cfg.count; i++)
		if (off >= x->cfg.range[i].offset && off - x->cfg.range[i].offset < x->cfg.range[i].len)
			return &x->cfg.range[i];
	return NULL;
}

static int iav_aux_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct iav_aux *x = filp->private_data;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 size = vma->vm_end - vma->vm_start;
	unsigned long uaddr = vma->vm_start;
	pgprot_t ram_prot, real_prot;
	u64 end;
	int ret = 0;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (check_add_overflow(off, size, &end))
		return -EINVAL;

	mutex_lock(&g_aux_lock);
	if (!x->configured) {
		ret = -ENODEV;
		goto out;
	}
	if (end > x->cfg.size) {
		ret = -EINVAL;
		goto out;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif
	/* As a window: the real ranges are no-map memory, Normal-NC at stage 2 with this flag; the pages are RAM. */
#ifdef VM_ALLOW_ANY_UNCACHED
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED);
#else
	vma->vm_flags |= VM_ALLOW_ANY_UNCACHED;
#endif
#endif
	ram_prot = vma->vm_page_prot;
	real_prot = pgprot_writecombine(vma->vm_page_prot);

	while (off < end && !ret) {
		const struct amba_virt_iav_aux_range *r = iav_aux_range_at(x, off);
		u64 n;

		if (r) {
			n = min(end, r->offset + r->len) - off;
			ret = remap_pfn_range(vma, uaddr, PHYS_PFN(r->phys + (off - r->offset)), n, real_prot);
		} else {
			struct page *pg = off == x->cfg.size - PAGE_SIZE ? x->hdr : x->scratch;

			n = PAGE_SIZE;
			ret = remap_pfn_range(vma, uaddr, page_to_pfn(pg), n, ram_prot);
		}
		uaddr += n;
		off += n;
	}
	if (ret)
		goto out;
	vma->vm_private_data = x;
	vma->vm_ops = &iav_aux_vm_ops;
	x->maps++;
out:
	mutex_unlock(&g_aux_lock);
	return ret;
}

static const struct file_operations iav_aux_fops = {
	.owner = THIS_MODULE,
	.open = iav_aux_open,
	.mmap = iav_aux_mmap,
	.unlocked_ioctl = iav_aux_ioctl,
	.compat_ioctl = iav_aux_ioctl,
	.llseek = no_llseek,
};

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
	AMBA_VIRT_IAV_WIN_DEC_NAME,
	AMBA_VIRT_IAV_WIN_OVL_NAME,
};

static void iav_aux_unregister(int n)
{
	while (--n >= 0) {
		misc_deregister(&g_aux[n].misc);
		if (g_aux[n].scratch)
			__free_page(g_aux[n].scratch);
		if (g_aux[n].hdr)
			__free_page(g_aux[n].hdr);
		g_aux[n].scratch = g_aux[n].hdr = NULL;
	}
}

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
	for (i = 0; i < (int)AMBA_VIRT_IAV_AUX_NODES; i++) {
		struct iav_aux *x = &g_aux[i];

		x->index = (unsigned int)i;
		snprintf(x->name, sizeof(x->name), AMBA_VIRT_IAV_AUX_NAME_FMT, (unsigned int)i);
		x->misc.minor = MISC_DYNAMIC_MINOR;
		x->misc.name = x->name;
		x->misc.fops = &iav_aux_fops;
		x->misc.mode = 0600;
		ret = misc_register(&x->misc);
		if (ret) {
			iav_aux_unregister(i);
			for (i = 0; i < IAV_WIN_NODES; i++)
				misc_deregister(&g_win[i].misc);
			return ret;
		}
	}
	pr_info("amba_virt_iav_win: one window set (DSP, BSB, DEC and OVL) and %u aux apertures registered\n",
		AMBA_VIRT_IAV_AUX_NODES);
	return 0;
}

static void __exit iav_win_exit(void)
{
	int i;

	iav_aux_unregister((int)AMBA_VIRT_IAV_AUX_NODES);
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
