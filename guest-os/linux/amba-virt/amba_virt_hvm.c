/*
 * amba_virt_guest.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>

#include <amba_virt.h>
#include "amba_virt_core.h"

#define IVSHMEM_VENDOR	0x1af4
#define IVSHMEM_DEVICE	0x1110

static int shm_bar = 2;
module_param(shm_bar, int, 0644);
MODULE_PARM_DESC(shm_bar, "PCI BAR index of ivshmem shared memory (default 2)");

static struct amba_virt_dev gdev;

static int amba_virt_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	resource_size_t start, len;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	start = pci_resource_start(pdev, shm_bar);
	len = pci_resource_len(pdev, shm_bar);
	if (!start || !len) {
		dev_err(&pdev->dev, "ivshmem BAR%u missing\n", shm_bar);
		return -ENODEV;
	}

	memset(&gdev, 0, sizeof(gdev));
	gdev.shm_phys = start;
	gdev.shm_size = (size_t)len;
	gdev.shm_iomem = pcim_iomap(pdev, shm_bar, 0);

	ret = amba_virt_core_init(&gdev, false);
	if (ret)
		return ret;

	pci_set_drvdata(pdev, &gdev);
	dev_info(&pdev->dev, "amba_virt guest: shm phys 0x%llx size %zu\n",
		 (unsigned long long)start, (size_t)len);
	return 0;
}

static void amba_virt_pci_remove(struct pci_dev *pdev)
{
	amba_virt_core_exit(&gdev);
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
};

module_pci_driver(amba_virt_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("amba_virt guest (ivshmem PCI + vsock)");
MODULE_AUTHOR("amba-virt");
