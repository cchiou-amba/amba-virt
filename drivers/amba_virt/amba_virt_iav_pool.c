/*
 * amba_virt_iav_pool.c
 *
 * Dom0 nodes for the eight leased low-memory slices. A map may cover the
 * whole 64 MiB slice, which is the power-of-two size QEMU puts in the BAR.
 * The last page is the claim page. Guests are told the usable length is
 * one page shorter.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/version.h>

#include "iav_low_slice.h"

static unsigned long long pool_base;
module_param(pool_base, ullong, 0444);
MODULE_PARM_DESC(pool_base, "Pinned low-pool physical base, 64 MiB aligned");

static struct iav_low_slice g_slices[IAV_LOW_SLICE_COUNT];
static int g_ready;

struct pool_node {
	struct miscdevice misc;
	unsigned index;
	struct mutex lock;
};

static struct pool_node g_nodes[IAV_LOW_SLICE_COUNT];
static char g_names[IAV_LOW_SLICE_COUNT][32];

static int pool_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct pool_node *node = filp->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	int ret = 0;

	if (!g_ready)
		return -ENODEV;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (size == 0 || size > IAV_LOW_SLICE_BYTES)
		return -EINVAL;
	mutex_lock(&node->lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif
	/* The slice is no-map memory: Normal-NC at stage 2, as for the IAV windows (amba_virt_iav_win.c). */
#ifdef VM_ALLOW_ANY_UNCACHED
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED);
#else
	vma->vm_flags |= VM_ALLOW_ANY_UNCACHED;
#endif
#endif
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	ret = remap_pfn_range(vma, vma->vm_start,
			      PHYS_PFN(g_slices[node->index].phys),
			      size, vma->vm_page_prot);
	mutex_unlock(&node->lock);
	return ret;
}

static int pool_open(struct inode *inode, struct file *filp)
{
	struct pool_node *node = container_of(filp->private_data, struct pool_node, misc);

	filp->private_data = node;
	return 0;
}

static const struct file_operations pool_fops = {
	.owner = THIS_MODULE,
	.open = pool_open,
	.mmap = pool_mmap,
	.llseek = no_llseek,
};

static int __init pool_init(void)
{
	unsigned i;
	int ret;

	if (pool_base) {
		ret = iav_low_slice_init(g_slices, IAV_LOW_SLICE_COUNT,
					 pool_base, IAV_LOW_SLICE_COUNT * IAV_LOW_SLICE_BYTES);
		if (ret)
			return ret;
		g_ready = 1;
	}
	for (i = 0; i < IAV_LOW_SLICE_COUNT; i++) {
		snprintf(g_names[i], sizeof(g_names[i]), "amba_virt_iav_slice%u", i);
		mutex_init(&g_nodes[i].lock);
		g_nodes[i].index = i;
		g_nodes[i].misc.minor = MISC_DYNAMIC_MINOR;
		g_nodes[i].misc.name = g_names[i];
		g_nodes[i].misc.fops = &pool_fops;
		g_nodes[i].misc.mode = 0600;
		ret = misc_register(&g_nodes[i].misc);
		if (ret) {
			while (i-- > 0)
				misc_deregister(&g_nodes[i].misc);
			return ret;
		}
	}
	return 0;
}

static void __exit pool_exit(void)
{
	unsigned i;

	for (i = 0; i < IAV_LOW_SLICE_COUNT; i++)
		misc_deregister(&g_nodes[i].misc);
}

module_init(pool_init);
module_exit(pool_exit);
MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Dom0 nodes for leased IAV low-memory slices");
MODULE_LICENSE("GPL");
