/*
 * amba_virt_usb_dma.c
 *
 * Host-side owner of the 32 MiB virt_usb_dma@15c00000 carveout. Exposes it
 * as /dev/amba_virt_usb_dma so the hypervisor can map it into a guest as an
 * ivshmem BAR next to the passed-through xHCI controller. The first page
 * carries the "AVUD" descriptor the guest driver reads.
 *
 * On the last close the pool is scrubbed so a following guest does not see
 * the previous guest's rings. This path must not read or write the xHCI
 * register block: on the N1-655 that access raises an asynchronous SError
 * and panics Dom0. The guest driver resets the controller when it probes.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "amba_virt_usb_dma.h"

#define DRV_NAME		"amba_virt_usb_dma"

#define SCRUB_CHUNK		(1ULL << 20)

static DEFINE_MUTEX(usb_dma_lock);
static unsigned int usb_dma_opens;

/* Zero everything behind the descriptor page. */
static int usb_dma_scrub_pool(void)
{
	u64 off = AMBA_VIRT_USB_DMA_HDR_BYTES;

	while (off < AMBA_VIRT_USB_DMA_TOTAL_SIZE) {
		u64 n = min(SCRUB_CHUNK, AMBA_VIRT_USB_DMA_TOTAL_SIZE - off);
		void __iomem *va;

		va = ioremap_wc(AMBA_VIRT_USB_DMA_PHYS_BASE + off, n);
		if (!va)
			return -ENOMEM;
		memset_io(va, 0, n);
		wmb();
		iounmap(va);
		off += n;
	}
	return 0;
}

static int usb_dma_write_header(void)
{
	struct amba_virt_usb_dma_hdr *hdr;
	void __iomem *va;

	hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	va = ioremap_wc(AMBA_VIRT_USB_DMA_PHYS_BASE, AMBA_VIRT_USB_DMA_HDR_BYTES);
	if (!va) {
		kfree(hdr);
		return -ENOMEM;
	}

	hdr->magic = AMBA_VIRT_USB_DMA_MAGIC;
	hdr->version = AMBA_VIRT_USB_DMA_VERSION;
	hdr->phys_base = AMBA_VIRT_USB_DMA_PHYS_BASE;
	hdr->total_size = AMBA_VIRT_USB_DMA_TOTAL_SIZE;
	hdr->rings_offset = AMBA_VIRT_USB_DMA_RINGS_OFFSET;
	hdr->rings_size = AMBA_VIRT_USB_DMA_RINGS_SIZE;
	hdr->urb_offset = AMBA_VIRT_USB_DMA_URB_OFFSET;
	hdr->urb_size = AMBA_VIRT_USB_DMA_URB_SIZE;

	memcpy_toio(va, hdr, sizeof(*hdr));
	wmb();
	iounmap(va);
	kfree(hdr);
	return 0;
}

static int usb_dma_open(struct inode *inode, struct file *filp)
{
	mutex_lock(&usb_dma_lock);
	usb_dma_opens++;
	mutex_unlock(&usb_dma_lock);
	return 0;
}

static int usb_dma_release(struct inode *inode, struct file *filp)
{
	mutex_lock(&usb_dma_lock);
	if (--usb_dma_opens == 0) {
		if (usb_dma_scrub_pool())
			pr_err("%s: pool scrub failed\n", DRV_NAME);
	}
	mutex_unlock(&usb_dma_lock);
	return 0;
}

static loff_t usb_dma_llseek(struct file *filp, loff_t off, int whence)
{
	return fixed_size_llseek(filp, off, whence, AMBA_VIRT_USB_DMA_TOTAL_SIZE);
}

static int usb_dma_mmap(struct file *filp, struct vm_area_struct *vma)
{
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 size = vma->vm_end - vma->vm_start;
	u64 end;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (check_add_overflow(off, size, &end) ||
	    end > AMBA_VIRT_USB_DMA_TOTAL_SIZE)
		return -EINVAL;

	/*
	 * The carveout is no-map, so KVM would map it Device at stage 2.
	 * Guest memset uses DC ZVA and usbcore/uvcvideo copy at any
	 * alignment; both raise an alignment fault KVM cannot handle on
	 * Device memory. Normal-NC keeps it uncached for the xHCI DMA engine.
	 */
#ifdef VM_ALLOW_ANY_UNCACHED
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED);
#else
	vma->vm_flags |= VM_ALLOW_ANY_UNCACHED;
#endif
#endif
	/* The xHCI DMA engine writes here; the CPU must not hold stale lines. */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			       PHYS_PFN(AMBA_VIRT_USB_DMA_PHYS_BASE + off),
			       size, vma->vm_page_prot);
}

static const struct file_operations usb_dma_fops = {
	.owner = THIS_MODULE,
	.open = usb_dma_open,
	.release = usb_dma_release,
	.mmap = usb_dma_mmap,
	.llseek = usb_dma_llseek,
};

static struct miscdevice usb_dma_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AMBA_VIRT_USB_DMA_DEV_NAME,
	.fops = &usb_dma_fops,
	.mode = 0600,
};

static int __init usb_dma_init(void)
{
	struct device_node *np;
	struct resource res;
	int ret;

	np = of_find_node_by_path(AMBA_VIRT_USB_DMA_OF_PATH);
	if (!np) {
		pr_err("%s: %s not found in the device tree\n", DRV_NAME,
		       AMBA_VIRT_USB_DMA_OF_PATH);
		return -ENODEV;
	}
	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret) {
		pr_err("%s: cannot read the carveout address: %d\n", DRV_NAME, ret);
		return ret;
	}
	if (res.start != AMBA_VIRT_USB_DMA_PHYS_BASE ||
	    resource_size(&res) != AMBA_VIRT_USB_DMA_TOTAL_SIZE) {
		pr_err("%s: carveout is %pR, expected base 0x%llx size 0x%llx\n",
		       DRV_NAME, &res, AMBA_VIRT_USB_DMA_PHYS_BASE,
		       AMBA_VIRT_USB_DMA_TOTAL_SIZE);
		return -EINVAL;
	}

	ret = usb_dma_write_header();
	if (ret) {
		pr_err("%s: cannot write the AVUD header: %d\n", DRV_NAME, ret);
		return ret;
	}

	ret = misc_register(&usb_dma_misc);
	if (ret) {
		pr_err("%s: misc_register failed: %d\n", DRV_NAME, ret);
		return ret;
	}

	pr_info("%s: /dev/%s serving %pR\n", DRV_NAME,
		AMBA_VIRT_USB_DMA_DEV_NAME, &res);
	return 0;
}

static void __exit usb_dma_exit(void)
{
	misc_deregister(&usb_dma_misc);
}

module_init(usb_dma_init);
module_exit(usb_dma_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Host USB DMA carveout for the passed-through xHCI controller");
