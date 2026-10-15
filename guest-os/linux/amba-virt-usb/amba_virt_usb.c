/*
 * amba_virt_usb.c
 *
 * Ubuntu HVM xHCI driver for the passed-through Cadence host controller.
 * The 32 MiB ivshmem BAR is the virt_usb_dma carveout. Ring allocations
 * return host physical addresses in the AVUD rings window. URB buffers
 * come from hcd->localmem_pool in the URB window. dma_alloc_pages users
 * (uvcvideo, through dma_alloc_noncontiguous) get ZONE_DEVICE pages
 * from the same URB window.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/dma-map-ops.h>
#include <linux/genalloc.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/version.h>

#include "amba_virt_usb_dma.h"

/*
 * First two fields of struct xhci_hcd. They are stable from the guest
 * 6.8 xhci_hcd through the 6.12 header: main_hcd, then shared_hcd.
 * usb_create_shared_hcd does not store the secondary HCD there; the
 * glue driver does, before usb_add_hcd.
 */
struct amba_xhci_link {
	struct usb_hcd *main_hcd;
	struct usb_hcd *shared_hcd;
};

struct usb_tt;

/*
 * Guest 6.8 xhci_init_driver reads this layout. hub_control was added
 * in 6.2 and is the last field through 6.12. It stays NULL.
 */
struct xhci_driver_overrides {
	size_t extra_priv_size;
	int (*reset)(struct usb_hcd *hcd);
	int (*start)(struct usb_hcd *hcd);
	int (*add_endpoint)(struct usb_hcd *hcd, struct usb_device *udev,
			    struct usb_host_endpoint *ep);
	int (*drop_endpoint)(struct usb_hcd *hcd, struct usb_device *udev,
			     struct usb_host_endpoint *ep);
	int (*check_bandwidth)(struct usb_hcd *hcd, struct usb_device *udev);
	void (*reset_bandwidth)(struct usb_hcd *hcd, struct usb_device *udev);
	int (*update_hub_device)(struct usb_hcd *hcd, struct usb_device *hdev,
				 struct usb_tt *tt, gfp_t mem_flags);
	int (*hub_control)(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
			   u16 wIndex, char *buf, u16 wLength);
};

void xhci_init_driver(struct hc_driver *drv,
		      const struct xhci_driver_overrides *over);
int xhci_gen_setup(struct usb_hcd *hcd,
		   void (*get_quirks)(struct device *dev, void *xhci));
int xhci_run(struct usb_hcd *hcd);

/*
 * Same registers xhci_cdns3_plat_start() programs in
 * eve-kernel/drivers/usb/cdns3/host.c. Both bits are set. Bit 15
 * is not cleared.
 */
#define XECP_PORT_CAP_REG		0x8000
#define XECP_AUX_CTRL_REG1		0x8120
#define CFG_RXDET_P3_EN			BIT(15)
#define LPM_2_STB_SWITCH_EN		BIT(25)
#define XHCI_USBCMD_EU3S		BIT(11)

#define AMBA_IVSHMEM_DEVICE		0x1110
#define AMBA_XHCI_MMIO_BYTES		0x10000
#define AMBA_RING_ALIGN_MIN		64

struct amba_stream_map {
	struct list_head node;
	dma_addr_t dma;
	void *bounce;
	struct page *page;
	unsigned long offset;
	size_t size;
	enum dma_data_direction dir;
};

struct amba_virt_usb {
	struct device *dev;
	void *bar;
	phys_addr_t bar_phys;
	struct dev_pagemap urb_pgmap;
	struct pci_dev *pci;
	int bar_index;
	struct gen_pool *rings;
	struct gen_pool *urbs;
	void __iomem *regs;
	struct usb_hcd *hcd;
	struct usb_hcd *shared;
	spinlock_t map_lock;
	struct list_head maps;
	bool dma_ops_set;
};

static struct hc_driver amba_xhci_hc_driver;

/*
 * usb_create_hcd stores the HCD in dev->driver_data, so the DMA ops
 * cannot recover this object from the device. One AMBA0002 exists.
 */
static struct amba_virt_usb *amba_active;

static struct amba_virt_usb *amba_from_dev(struct device *dev)
{
	if (amba_active && amba_active->dev == dev)
		return amba_active;
	return NULL;
}

static void amba_copy_span(struct page *page, unsigned long offset,
			   void *bounce, size_t size, bool to_bounce)
{
	while (size) {
		size_t page_off = offset & (PAGE_SIZE - 1);
		size_t chunk = min_t(size_t, size, PAGE_SIZE - page_off);
		struct page *pg = page + (offset >> PAGE_SHIFT);
		void *addr = kmap_local_page(pg);

		if (to_bounce)
			memcpy(bounce, addr + page_off, chunk);
		else
			memcpy(addr + page_off, bounce, chunk);
		kunmap_local(addr);
		bounce += chunk;
		size -= chunk;
		offset += chunk;
	}
}

static void *amba_ring_alloc(struct amba_virt_usb *usb, size_t size,
			     dma_addr_t *dma)
{
	int align = roundup_pow_of_two(size);

	if (align < AMBA_RING_ALIGN_MIN)
		align = AMBA_RING_ALIGN_MIN;
	return gen_pool_dma_zalloc_align(usb->rings, size, dma, align);
}

static void *amba_dma_alloc(struct device *dev, size_t size,
			    dma_addr_t *dma_handle, gfp_t gfp,
			    unsigned long attrs)
{
	struct amba_virt_usb *usb = amba_from_dev(dev);
	void *virt;

	if (!usb || !size)
		return NULL;
	virt = amba_ring_alloc(usb, size, dma_handle);
	if (!virt)
		return NULL;
	wmb();
	return virt;
}

static void amba_dma_free(struct device *dev, size_t size, void *vaddr,
			  dma_addr_t dma_handle, unsigned long attrs)
{
	struct amba_virt_usb *usb = amba_from_dev(dev);

	if (!usb || !vaddr)
		return;
	gen_pool_free(usb->rings, (unsigned long)vaddr, size);
}

/*
 * xhci-ring bounce buffers and xhci_map_temp_buffer call dma_map_single
 * on the HCD sysdev. usbcore URB maps do not: those use localmem_pool.
 * The bounce lives in the URB window so the controller never sees
 * ordinary guest RAM.
 */
static dma_addr_t amba_dma_map_page(struct device *dev, struct page *page,
				    unsigned long offset, size_t size,
				    enum dma_data_direction dir,
				    unsigned long attrs)
{
	struct amba_virt_usb *usb = amba_from_dev(dev);
	struct amba_stream_map *map;
	dma_addr_t dma;
	void *bounce;
	unsigned long flags;

	if (!usb || !page || !size || dir == DMA_NONE)
		return DMA_MAPPING_ERROR;
	bounce = gen_pool_dma_alloc(usb->urbs, size, &dma);
	if (!bounce)
		return DMA_MAPPING_ERROR;
	map = kmalloc(sizeof(*map), GFP_ATOMIC);
	if (!map) {
		gen_pool_free(usb->urbs, (unsigned long)bounce, size);
		return DMA_MAPPING_ERROR;
	}
	if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL)
		amba_copy_span(page, offset, bounce, size, true);
	wmb();
	map->dma = dma;
	map->bounce = bounce;
	map->page = page;
	map->offset = offset;
	map->size = size;
	map->dir = dir;
	spin_lock_irqsave(&usb->map_lock, flags);
	list_add(&map->node, &usb->maps);
	spin_unlock_irqrestore(&usb->map_lock, flags);
	return dma;
}

static void amba_dma_unmap_page(struct device *dev, dma_addr_t dma_handle,
				size_t size, enum dma_data_direction dir,
				unsigned long attrs)
{
	struct amba_virt_usb *usb = amba_from_dev(dev);
	struct amba_stream_map *map, *found = NULL;
	unsigned long flags;

	if (!usb)
		return;
	spin_lock_irqsave(&usb->map_lock, flags);
	list_for_each_entry(map, &usb->maps, node) {
		if (map->dma == dma_handle && map->size == size) {
			list_del(&map->node);
			found = map;
			break;
		}
	}
	spin_unlock_irqrestore(&usb->map_lock, flags);
	if (!found) {
		dev_err(dev, "unmap of unknown dma address %pad size %zu\n",
			&dma_handle, size);
		return;
	}
	if (found->dir == DMA_FROM_DEVICE || found->dir == DMA_BIDIRECTIONAL) {
		rmb();
		amba_copy_span(found->page, found->offset, found->bounce,
			       found->size, false);
	}
	gen_pool_free(usb->urbs, (unsigned long)found->bounce, found->size);
	kfree(found);
}

/*
 * uvcvideo allocates its isochronous URB buffers with
 * dma_alloc_noncontiguous() on the HCD sysdev, not usb_alloc_coherent().
 * Without alloc_noncontiguous that becomes one dma_alloc_pages() call,
 * and the CPU address is page_address(), so the URB window carries
 * ZONE_DEVICE struct pages. The guest linear alias is Normal WB at
 * stage 1, but the host maps the carveout Normal-NC at stage 2, so the
 * access stays uncached.
 */
static struct page *amba_dma_alloc_pages(struct device *dev, size_t size,
					 dma_addr_t *dma_handle,
					 enum dma_data_direction dir,
					 gfp_t gfp)
{
	struct amba_virt_usb *usb = amba_from_dev(dev);
	unsigned long base, virt;

	if (!usb || !size)
		return NULL;
	size = PAGE_ALIGN(size);
	virt = (unsigned long)gen_pool_dma_alloc_align(usb->urbs, size,
						       dma_handle, PAGE_SIZE);
	if (!virt)
		return NULL;
	base = (unsigned long)usb->bar + AMBA_VIRT_USB_DMA_URB_OFFSET;
	return pfn_to_page(PHYS_PFN(usb->bar_phys +
				    AMBA_VIRT_USB_DMA_URB_OFFSET +
				    (virt - base)));
}

static void amba_dma_free_pages(struct device *dev, size_t size,
				struct page *page, dma_addr_t dma_handle,
				enum dma_data_direction dir)
{
	struct amba_virt_usb *usb = amba_from_dev(dev);
	phys_addr_t base, pa;

	if (!usb || !page)
		return;
	base = usb->bar_phys + AMBA_VIRT_USB_DMA_URB_OFFSET;
	pa = page_to_phys(page);
	if (pa < base || pa - base >= AMBA_VIRT_USB_DMA_URB_SIZE) {
		dev_err(dev, "free of page %pa outside the URB window\n", &pa);
		return;
	}
	gen_pool_free(usb->urbs,
		      (unsigned long)usb->bar + AMBA_VIRT_USB_DMA_URB_OFFSET +
		      (pa - base), PAGE_ALIGN(size));
}

static const struct dma_map_ops amba_dma_ops = {
	.alloc = amba_dma_alloc,
	.free = amba_dma_free,
	.alloc_pages = amba_dma_alloc_pages,
	.free_pages = amba_dma_free_pages,
	.map_page = amba_dma_map_page,
	.unmap_page = amba_dma_unmap_page,
};

static void amba_dma_ops_install(struct amba_virt_usb *usb)
{
	/*
	 * dev->dma_mem is not assigned because dma_declare_coherent_memory
	 * is not exported. The dev-coherent check in dma_alloc_attrs()
	 * therefore stays unused, and ops->alloc serves the rings window.
	 */
	set_dma_ops(usb->dev, &amba_dma_ops);
	usb->dma_ops_set = true;
}

static void amba_dma_ops_clear(struct amba_virt_usb *usb)
{
	struct amba_stream_map *map, *next;

	if (usb->dma_ops_set) {
		set_dma_ops(usb->dev, NULL);
		usb->dma_ops_set = false;
	}
	list_for_each_entry_safe(map, next, &usb->maps, node) {
		list_del(&map->node);
		gen_pool_free(usb->urbs, (unsigned long)map->bounce, map->size);
		kfree(map);
	}
}

/*
 * xhci_hc_driver.reset is NULL. xhci_init_driver copies that NULL
 * unless this override supplies a reset. usb_add_hcd then skips
 * xhci_gen_setup, and xhci_run loads xhci->interrupters[0] while it
 * is still NULL.
 */
static int amba_xhci_setup(struct usb_hcd *hcd)
{
	int ret;

	ret = xhci_gen_setup(hcd, NULL);
	if (ret)
		return ret;
	/*
	 * xhci_gen_setup advertises scatter-gather. With localmem_pool set,
	 * usb_hcd_map_urb_for_dma bounces transfer_buffer and never reads
	 * the SG list, so an SG URB (NULL buffer) fails in
	 * hcd_alloc_coherent. Without SG, usb_sg_init sends one buffer URB
	 * per entry.
	 */
	hcd->self.sg_tablesize = 0;
	hcd->self.no_sg_constraint = 0;
	return 0;
}

static int amba_xhci_start(struct usb_hcd *hcd)
{
	u32 caplength, value;
	void __iomem *op;

	if (hcd->regs) {
		caplength = readl(hcd->regs) & 0xff;
		op = hcd->regs + caplength;
		value = readl(op);
		value |= XHCI_USBCMD_EU3S;
		writel(value, op);

		value = readl(hcd->regs + XECP_AUX_CTRL_REG1);
		value |= CFG_RXDET_P3_EN;
		writel(value, hcd->regs + XECP_AUX_CTRL_REG1);

		value = readl(hcd->regs + XECP_PORT_CAP_REG);
		value |= LPM_2_STB_SWITCH_EN;
		writel(value, hcd->regs + XECP_PORT_CAP_REG);
	}
	return xhci_run(hcd);
}

static const struct xhci_driver_overrides amba_xhci_overrides = {
	.reset = amba_xhci_setup,
	.start = amba_xhci_start,
};

static bool amba_bar_is_avud(void *bar, resource_size_t len)
{
	struct amba_virt_usb_dma_hdr *hdr = bar;

	if (hdr->magic != 0x41565544)
		return false;
	if (hdr->version != AMBA_VIRT_USB_DMA_VERSION)
		return false;
	if (hdr->phys_base != AMBA_VIRT_USB_DMA_PHYS_BASE)
		return false;
	if (hdr->total_size != len || len != AMBA_VIRT_USB_DMA_TOTAL_SIZE)
		return false;
	if (hdr->rings_offset != AMBA_VIRT_USB_DMA_RINGS_OFFSET ||
	    hdr->rings_size != AMBA_VIRT_USB_DMA_RINGS_SIZE)
		return false;
	if (hdr->urb_offset != AMBA_VIRT_USB_DMA_URB_OFFSET ||
	    hdr->urb_size != AMBA_VIRT_USB_DMA_URB_SIZE)
		return false;
	return true;
}

static int amba_find_dma_bar(struct amba_virt_usb *usb)
{
	struct pci_dev *pdev = NULL;

	while ((pdev = pci_get_device(PCI_VENDOR_ID_REDHAT_QUMRANET,
				      AMBA_IVSHMEM_DEVICE, pdev))) {
		int bar;

		if (pci_enable_device(pdev))
			continue;
		for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
			resource_size_t len = pci_resource_len(pdev, bar);
			void *map;

			if (len != AMBA_VIRT_USB_DMA_TOTAL_SIZE)
				continue;
			if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))
				continue;
			if (pci_request_region(pdev, bar, "amba_virt_usb"))
				continue;
			map = memremap(pci_resource_start(pdev, bar), len,
				       MEMREMAP_WC);
			if (!map) {
				pci_release_region(pdev, bar);
				continue;
			}
			if (!amba_bar_is_avud(map, len)) {
				memunmap(map);
				pci_release_region(pdev, bar);
				continue;
			}
			usb->pci = pdev;
			usb->bar_index = bar;
			usb->bar = map;
			usb->bar_phys = pci_resource_start(pdev, bar);
			return 0;
		}
		pci_disable_device(pdev);
	}
	return -ENODEV;
}

static int amba_pools_create(struct amba_virt_usb *usb)
{
	phys_addr_t rings_phys = AMBA_VIRT_USB_DMA_PHYS_BASE +
				 AMBA_VIRT_USB_DMA_RINGS_OFFSET;
	phys_addr_t urb_phys = AMBA_VIRT_USB_DMA_PHYS_BASE +
			       AMBA_VIRT_USB_DMA_URB_OFFSET;
	int ret;

	usb->rings = gen_pool_create(6, -1);
	usb->urbs = gen_pool_create(6, -1);
	if (!usb->rings || !usb->urbs)
		return -ENOMEM;
	ret = gen_pool_add_virt(usb->rings,
				(unsigned long)usb->bar +
				AMBA_VIRT_USB_DMA_RINGS_OFFSET,
				rings_phys, AMBA_VIRT_USB_DMA_RINGS_SIZE, -1);
	if (ret)
		return ret;
	ret = gen_pool_add_virt(usb->urbs,
				(unsigned long)usb->bar +
				AMBA_VIRT_USB_DMA_URB_OFFSET,
				urb_phys, AMBA_VIRT_USB_DMA_URB_SIZE, -1);
	return ret;
}

/*
 * Released by devres after remove(), when usb_remove_hcd has freed every
 * URB buffer. The BAR is naturally aligned, so the window is 2 MiB
 * subsection aligned.
 */
static int amba_urb_pages_create(struct amba_virt_usb *usb)
{
	void *addr;

	usb->urb_pgmap.type = MEMORY_DEVICE_GENERIC;
	usb->urb_pgmap.range.start = usb->bar_phys +
				     AMBA_VIRT_USB_DMA_URB_OFFSET;
	usb->urb_pgmap.range.end = usb->urb_pgmap.range.start +
				   AMBA_VIRT_USB_DMA_URB_SIZE - 1;
	usb->urb_pgmap.nr_range = 1;
	addr = devm_memremap_pages(usb->dev, &usb->urb_pgmap);
	if (IS_ERR(addr))
		return PTR_ERR(addr);
	return 0;
}

static void amba_pools_destroy(struct amba_virt_usb *usb)
{
	if (usb->rings)
		gen_pool_destroy(usb->rings);
	if (usb->urbs)
		gen_pool_destroy(usb->urbs);
	usb->rings = NULL;
	usb->urbs = NULL;
}

static void amba_hcd_drop(struct amba_virt_usb *usb)
{
	if (usb->shared) {
		if (usb->shared->rh_registered)
			usb_remove_hcd(usb->shared);
		usb_put_hcd(usb->shared);
		usb->shared = NULL;
	}
	if (usb->hcd) {
		if (usb->hcd->rh_registered)
			usb_remove_hcd(usb->hcd);
		usb_put_hcd(usb->hcd);
		usb->hcd = NULL;
	}
}

static int amba_virt_usb_probe(struct platform_device *pdev)
{
	struct amba_virt_usb *usb;
	struct amba_xhci_link *link;
	struct resource *mmio;
	int irq, ret;

	if (amba_active)
		return -EBUSY;
	usb = devm_kzalloc(&pdev->dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->dev = &pdev->dev;
	amba_active = usb;
	usb->bar_index = -1;
	spin_lock_init(&usb->map_lock);
	INIT_LIST_HEAD(&usb->maps);
	platform_set_drvdata(pdev, usb);

	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto err_active;

	ret = amba_find_dma_bar(usb);
	if (ret) {
		dev_err(&pdev->dev, "no 32 MiB ivshmem BAR with AVUD magic\n");
		goto err_active;
	}
	ret = amba_pools_create(usb);
	if (ret)
		goto err_bar;
	ret = amba_urb_pages_create(usb);
	if (ret) {
		dev_err(&pdev->dev, "URB window struct pages: %d\n", ret);
		goto err_pools;
	}

	amba_dma_ops_install(usb);

	mmio = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mmio || resource_size(mmio) != AMBA_XHCI_MMIO_BYTES) {
		dev_err(&pdev->dev, "ACPI MMIO region is not 64 KiB\n");
		ret = -ENODEV;
		goto err_pools;
	}
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_pools;
	}
	usb->regs = devm_ioremap(&pdev->dev, mmio->start, resource_size(mmio));
	if (!usb->regs) {
		ret = -ENOMEM;
		goto err_pools;
	}

	usb->hcd = usb_create_hcd(&amba_xhci_hc_driver, &pdev->dev,
				  dev_name(&pdev->dev));
	if (!usb->hcd) {
		ret = -ENOMEM;
		goto err_pools;
	}
	usb->hcd->regs = usb->regs;
	usb->hcd->rsrc_start = mmio->start;
	usb->hcd->rsrc_len = resource_size(mmio);
	usb->hcd->skip_phy_initialization = 1;
	usb->hcd->localmem_pool = usb->urbs;

	usb->shared = usb_create_shared_hcd(&amba_xhci_hc_driver, &pdev->dev,
					    dev_name(&pdev->dev), usb->hcd);
	if (!usb->shared) {
		ret = -ENOMEM;
		goto err_hcd;
	}
	usb->shared->regs = usb->hcd->regs;
	usb->shared->rsrc_start = usb->hcd->rsrc_start;
	usb->shared->rsrc_len = usb->hcd->rsrc_len;
	usb->shared->skip_phy_initialization = 1;
	usb->shared->localmem_pool = usb->urbs;

	link = (struct amba_xhci_link *)usb->hcd->hcd_priv;
	link->shared_hcd = usb->shared;

	ret = usb_add_hcd(usb->hcd, irq, IRQF_SHARED);
	if (ret)
		goto err_hcd;
	ret = usb_add_hcd(usb->shared, irq, IRQF_SHARED);
	if (ret)
		goto err_hcd;

	dev_info(&pdev->dev, "xHCI bound to AMBA0002\n");
	return 0;

err_hcd:
	amba_hcd_drop(usb);
err_pools:
	amba_dma_ops_clear(usb);
	amba_pools_destroy(usb);
err_bar:
	if (usb->bar)
		memunmap(usb->bar);
	if (usb->pci) {
		if (usb->bar_index >= 0)
			pci_release_region(usb->pci, usb->bar_index);
		pci_dev_put(usb->pci);
	}
	usb->bar = NULL;
	usb->pci = NULL;
	if (amba_active == usb)
		amba_active = NULL;
err_active:
	if (amba_active == usb)
		amba_active = NULL;
	return ret;
}

static int amba_virt_usb_remove_int(struct platform_device *pdev)
{
	struct amba_virt_usb *usb = amba_active;

	if (!usb || usb->dev != &pdev->dev)
		return 0;
	amba_hcd_drop(usb);
	amba_dma_ops_clear(usb);
	amba_pools_destroy(usb);
	if (usb->bar)
		memunmap(usb->bar);
	if (usb->pci) {
		if (usb->bar_index >= 0)
			pci_release_region(usb->pci, usb->bar_index);
		pci_dev_put(usb->pci);
	}
	if (amba_active == usb)
		amba_active = NULL;
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void amba_virt_usb_remove(struct platform_device *pdev)
{
	amba_virt_usb_remove_int(pdev);
}
#else
#define amba_virt_usb_remove amba_virt_usb_remove_int
#endif

static const struct acpi_device_id amba_virt_usb_acpi_ids[] = {
	{ "AMBA0002", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, amba_virt_usb_acpi_ids);

static struct platform_driver amba_virt_usb_driver = {
	.probe = amba_virt_usb_probe,
	.remove = amba_virt_usb_remove,
	.driver = {
		.name = "amba_virt_usb",
		.acpi_match_table = ACPI_PTR(amba_virt_usb_acpi_ids),
	},
};

static int __init amba_virt_usb_init(void)
{
	xhci_init_driver(&amba_xhci_hc_driver, &amba_xhci_overrides);
	return platform_driver_register(&amba_virt_usb_driver);
}

static void __exit amba_virt_usb_exit(void)
{
	platform_driver_unregister(&amba_virt_usb_driver);
}

module_init(amba_virt_usb_init);
module_exit(amba_virt_usb_exit);

MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Ambarella HVM physical xHCI passthrough");
MODULE_LICENSE("GPL");
