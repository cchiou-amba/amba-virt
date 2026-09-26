/*
 * amba_virt_hvm.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>

#include <amba_virt.h>
#include "amba_virt_kernel.h"
#include "amba_virt_core.h"

#define IVSHMEM_VENDOR	0x1af4
#define IVSHMEM_DEVICE	0x1110

#define AMBA_VIRT_DMA32_WINDOW_SIZE	0x1000000ULL /* 16 MiB */

struct amba_virt_pci_window {
	struct pci_dev		*pdev;
	int			bar;
	phys_addr_t		phys;
	void __iomem		*iomem;
	size_t			size;
};

static struct amba_virt_pci_window bulk_win;
static struct amba_virt_pci_window dma32_win;
static struct amba_virt_dev gdev;

int amba_virt_get_bulk_window(phys_addr_t *phys, void __iomem **iomem,
			      size_t *size)
{
	if (!phys || !iomem || !size)
		return -EINVAL;
	if (!bulk_win.phys || !bulk_win.iomem || !bulk_win.size)
		return -ENODEV;

	*phys = bulk_win.phys;
	*iomem = bulk_win.iomem;
	*size = bulk_win.size;
	return 0;
}
EXPORT_SYMBOL_GPL(amba_virt_get_bulk_window);

int amba_virt_get_dma32_window(phys_addr_t *phys, void __iomem **iomem,
			       size_t *size)
{
	if (!phys || !iomem || !size)
		return -EINVAL;
	if (!dma32_win.phys || !dma32_win.iomem || !dma32_win.size)
		return -ENODEV;

	*phys = dma32_win.phys;
	*iomem = dma32_win.iomem;
	*size = dma32_win.size;
	return 0;
}
EXPORT_SYMBOL_GPL(amba_virt_get_dma32_window);

int amba_virt_get_window(phys_addr_t *phys, void __iomem **iomem,
			 size_t *size)
{
	if (bulk_win.phys)
		return amba_virt_get_bulk_window(phys, iomem, size);
	return amba_virt_get_dma32_window(phys, iomem, size);
}
EXPORT_SYMBOL_GPL(amba_virt_get_window);

int amba_virt_rpc(const void *request, u32 request_len, void *response,
		  u32 *response_len, unsigned int timeout_ms)
{
	return amba_virt_rpc_dev(&gdev, request, request_len, response,
				 response_len, timeout_ms);
}
EXPORT_SYMBOL_GPL(amba_virt_rpc);

static int amba_virt_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	resource_size_t start, len = 0;
	int bar = 0, ret, i;
	bool is_dma32 = false;

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	/* Find the shared memory window: pick the largest memory BAR */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if ((pci_resource_flags(pdev, i) & IORESOURCE_MEM) &&
		    pci_resource_len(pdev, i) > len) {
			len = pci_resource_len(pdev, i);
			bar = i;
		}
	}

	start = pci_resource_start(pdev, bar);
	len = pci_resource_len(pdev, bar);
	if (!start || !len) {
		dev_err(&pdev->dev, "ivshmem memory BAR missing\n");
		pci_disable_device(pdev);
		return -ENODEV;
	}

	/* Classify window role by BAR size */
	if (len == AMBA_VIRT_DMA32_WINDOW_SIZE) {
		if (dma32_win.phys != 0) {
			dev_warn(&pdev->dev, "duplicate DMA32 window ignored\n");
			pci_disable_device(pdev);
			return -EBUSY;
		}
		is_dma32 = true;
	} else if (len > AMBA_VIRT_DMA32_WINDOW_SIZE) {
		if (bulk_win.phys != 0) {
			dev_warn(&pdev->dev, "duplicate bulk window ignored\n");
			pci_disable_device(pdev);
			return -EBUSY;
		}
		is_dma32 = false;
	} else {
		/* Aperture or unrecognized size */
		pci_disable_device(pdev);
		return -ENODEV;
	}

	ret = pci_request_region(pdev, bar, is_dma32 ? "amba_dma32" : "amba_virt");
	if (ret) {
		dev_err(&pdev->dev, "cannot request ivshmem BAR%d\n", bar);
		pci_disable_device(pdev);
		return ret;
	}

	if (is_dma32) {
		dma32_win.pdev = pdev;
		dma32_win.bar = bar;
		dma32_win.phys = start;
		dma32_win.size = (size_t)len;
		dma32_win.iomem = ioremap_wc(start, len);
		if (!dma32_win.iomem) {
			dev_err(&pdev->dev, "cannot map DMA32 BAR%d\n", bar);
			pci_release_region(pdev, bar);
			pci_disable_device(pdev);
			memset(&dma32_win, 0, sizeof(dma32_win));
			return -ENOMEM;
		}
		pci_set_drvdata(pdev, &dma32_win);
		dev_info(&pdev->dev, "amba_virt guest: DMA32 lease window BAR%d phys 0x%llx size %zu\n",
			 bar, (unsigned long long)start, (size_t)len);
		return 0;
	}

	/* Bulk window */
	bulk_win.pdev = pdev;
	bulk_win.bar = bar;
	bulk_win.phys = start;
	bulk_win.size = (size_t)len;
	bulk_win.iomem = ioremap_wc(start, len);
	if (!bulk_win.iomem) {
		dev_err(&pdev->dev, "cannot map bulk BAR%d\n", bar);
		pci_release_region(pdev, bar);
		pci_disable_device(pdev);
		memset(&bulk_win, 0, sizeof(bulk_win));
		return -ENOMEM;
	}

	memset(&gdev, 0, sizeof(gdev));
	gdev.shm_phys = start;
	gdev.shm_size = (size_t)len;
	gdev.shm_iomem = bulk_win.iomem;

	ret = amba_virt_core_init(&gdev, false);
	if (ret) {
		iounmap(bulk_win.iomem);
		bulk_win.iomem = NULL;
		pci_release_region(pdev, bar);
		pci_disable_device(pdev);
		memset(&bulk_win, 0, sizeof(bulk_win));
		return ret;
	}

	pci_set_drvdata(pdev, &bulk_win);
	dev_info(&pdev->dev, "amba_virt guest: bulk window BAR%d phys 0x%llx size %zu\n",
		 bar, (unsigned long long)start, (size_t)len);
	return 0;
}

static void amba_virt_pci_remove(struct pci_dev *pdev)
{
	void *drvdata = pci_get_drvdata(pdev);

	if (drvdata == &dma32_win) {
		if (dma32_win.iomem) {
			iounmap(dma32_win.iomem);
			dma32_win.iomem = NULL;
		}
		pci_release_region(pdev, dma32_win.bar);
		pci_disable_device(pdev);
		memset(&dma32_win, 0, sizeof(dma32_win));
	} else if (drvdata == &bulk_win) {
		amba_virt_core_exit(&gdev);
		if (bulk_win.iomem) {
			iounmap(bulk_win.iomem);
			bulk_win.iomem = NULL;
		}
		pci_release_region(pdev, bulk_win.bar);
		pci_disable_device(pdev);
		memset(&bulk_win, 0, sizeof(bulk_win));
		memset(&gdev, 0, sizeof(gdev));
	}
}

static const struct pci_device_id amba_virt_pci_ids[] = {
	{ PCI_DEVICE(IVSHMEM_VENDOR, IVSHMEM_DEVICE) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, amba_virt_pci_ids);

static struct pci_driver amba_virt_pci_driver = {
	.name = "amba_virt",
	.id_table = amba_virt_pci_ids,
	.probe = amba_virt_pci_probe,
	.remove = amba_virt_pci_remove,
	.driver = {
		.suppress_bind_attrs = true,
	},
};

static int __init amba_virt_guest_init(void)
{
	return pci_register_driver(&amba_virt_pci_driver);
}

static void __exit amba_virt_guest_exit(void)
{
	pci_unregister_driver(&amba_virt_pci_driver);
}

module_init(amba_virt_guest_init);
module_exit(amba_virt_guest_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("amba_virt guest (ivshmem PCI + vsock)");
MODULE_AUTHOR("amba-virt");
