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

static int shm_bar = 0;
static struct amba_virt_dev gdev;

int amba_virt_get_window(phys_addr_t *phys, void __iomem **iomem,
			 size_t *size)
{
	if (!phys || !iomem || !size)
		return -EINVAL;
	if (!gdev.shm_phys || !gdev.shm_iomem || !gdev.shm_size)
		return -ENODEV;

	*phys = gdev.shm_phys;
	*iomem = gdev.shm_iomem;
	*size = gdev.shm_size;
	return 0;
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

	ret = pci_request_region(pdev, bar, "amba_virt");
	if (ret) {
		dev_err(&pdev->dev, "cannot request ivshmem BAR%d\n", bar);
		pci_disable_device(pdev);
		return ret;
	}

	memset(&gdev, 0, sizeof(gdev));
	gdev.shm_phys = start;
	gdev.shm_size = (size_t)len;
	gdev.shm_iomem = ioremap_wc(start, len);
	if (!gdev.shm_iomem) {
		dev_err(&pdev->dev, "cannot map ivshmem BAR%d\n", bar);
		pci_release_region(pdev, bar);
		pci_disable_device(pdev);
		return -ENOMEM;
	}

	ret = amba_virt_core_init(&gdev, false);
	if (ret) {
		iounmap(gdev.shm_iomem);
		gdev.shm_iomem = NULL;
		pci_release_region(pdev, bar);
		pci_disable_device(pdev);
		return ret;
	}

	shm_bar = bar;
	pci_set_drvdata(pdev, &gdev);
	dev_info(&pdev->dev, "amba_virt guest: BAR%d shm phys 0x%llx size %zu\n",
		 bar, (unsigned long long)start, (size_t)len);
	return 0;
}

static void amba_virt_pci_remove(struct pci_dev *pdev)
{
	amba_virt_core_exit(&gdev);
	if (gdev.shm_iomem) {
		iounmap(gdev.shm_iomem);
		gdev.shm_iomem = NULL;
	}
	pci_release_region(pdev, shm_bar);
	pci_disable_device(pdev);
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

module_pci_driver(amba_virt_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("amba_virt guest (ivshmem PCI + vsock)");
MODULE_AUTHOR("amba-virt");
