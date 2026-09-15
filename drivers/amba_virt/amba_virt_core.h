/*
 * amba_virt_core.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMBA_VIRT_CORE_H
#define AMBA_VIRT_CORE_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/wait.h>

struct amba_virt_gdma_copy;

struct amba_virt_dev {
	struct cdev cdev;
	struct class *class;
	struct device *device;
	dev_t devt;

	bool is_host;
	size_t shm_size;
	phys_addr_t shm_phys;	/* guest BAR; 0 on host */
	void __iomem *shm_iomem; /* guest optional */
	struct file *shm_file;	/* host backing file */
	const char *shm_path;	/* host: attached lazily, see amba_virt_attach_shm */
	struct mutex shm_lock;	/* serialises the lazy attach */
	int (*gdma_copy)(struct amba_virt_dev *dev,
			 struct amba_virt_gdma_copy *copy);

	struct socket *listen_sock;
	struct socket *conn_sock;
	struct mutex sock_lock;
	struct mutex send_lock;
	struct mutex recv_lock;
	struct mutex rpc_lock;
	struct task_struct *accept_thread;

	unsigned int vsock_cid;
	unsigned int vsock_port;
};

int amba_virt_core_init(struct amba_virt_dev *dev, bool is_host);
void amba_virt_core_exit(struct amba_virt_dev *dev);

int amba_virt_attach_shm(struct amba_virt_dev *dev);
int amba_virt_mmap_window(struct amba_virt_dev *dev,
			  struct vm_area_struct *vma);
int amba_virt_export_dmabuf(struct amba_virt_dev *dev, int *out_fd);

int amba_virt_vsock_listen(struct amba_virt_dev *dev);
int amba_virt_vsock_connect(struct amba_virt_dev *dev);
void amba_virt_vsock_disconnect(struct amba_virt_dev *dev);
int amba_virt_rpc_dev(struct amba_virt_dev *dev, const void *request,
		      u32 request_len, void *response, u32 *response_len,
		      unsigned int timeout_ms);

extern const struct file_operations amba_virt_fops;

#endif
