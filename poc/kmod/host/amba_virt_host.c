/*
 * amba_virt_host.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>

#include <amba_virt.h>
#include "amba_virt_core.h"

static char *shm_path = "/dev/shm/amba-virt";
module_param(shm_path, charp, 0644);
MODULE_PARM_DESC(shm_path, "ivshmem memory-backend file (must exist, share=on)");

static unsigned int vsock_port = AMBA_VIRT_VSOCK_PORT;
module_param(vsock_port, uint, 0644);
MODULE_PARM_DESC(vsock_port, "vsock listen port (default 5555; do not use 2000)");

static struct amba_virt_dev gdev;

static int __init amba_virt_host_init(void)
{
	struct file *f;
	loff_t size;
	int ret;

	if (vsock_port == 2000) {
		pr_err("amba_virt: port 2000 is EVE VComLink; refusing\n");
		return -EINVAL;
	}

	f = filp_open(shm_path, O_RDWR, 0);
	if (IS_ERR(f)) {
		pr_err("amba_virt: open %s failed (%ld); create/truncate the file first\n",
		       shm_path, PTR_ERR(f));
		return PTR_ERR(f);
	}
	size = i_size_read(file_inode(f));
	if (size <= 0) {
		filp_close(f, NULL);
		pr_err("amba_virt: %s has size %lld\n", shm_path, (long long)size);
		return -EINVAL;
	}

	memset(&gdev, 0, sizeof(gdev));
	gdev.shm_file = f;
	gdev.shm_size = (size_t)size;

	ret = amba_virt_core_init(&gdev, true);
	if (ret) {
		filp_close(f, NULL);
		gdev.shm_file = NULL;
		return ret;
	}
	gdev.vsock_port = vsock_port;

	ret = amba_virt_vsock_listen(&gdev);
	if (ret) {
		pr_err("amba_virt: vsock listen failed %d\n", ret);
		amba_virt_core_exit(&gdev);
		return ret;
	}
	pr_info("amba_virt host: shm %s size %zu, vsock port %u\n",
		shm_path, gdev.shm_size, vsock_port);
	return 0;
}

static void __exit amba_virt_host_exit(void)
{
	amba_virt_core_exit(&gdev);
}

module_init(amba_virt_host_init);
module_exit(amba_virt_host_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("amba_virt host (shm file + vsock listen)");
MODULE_AUTHOR("amba-virt");
