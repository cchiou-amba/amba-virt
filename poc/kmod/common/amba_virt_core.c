/*
 * amba_virt_core.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/vm_sockets.h>
#include <net/sock.h>

#include <amba_virt.h>
#include "amba_virt_core.h"

static int recv_exact(struct socket *sock, void *buf, size_t len, long timeout_jiffies)
{
	size_t done = 0;

	if (sock && sock->sk)
		sock->sk->sk_rcvtimeo = timeout_jiffies;

	while (done < len) {
		struct kvec iov = {
			.iov_base = (u8 *)buf + done,
			.iov_len = len - done,
		};
		struct msghdr msg = { 0 };
		int n;

		n = kernel_recvmsg(sock, &msg, &iov, 1, iov.iov_len, 0);
		if (n <= 0) {
			if (n == -EAGAIN || n == -EWOULDBLOCK)
				return -ETIMEDOUT;
			return n < 0 ? n : -ECONNRESET;
		}
		done += n;
	}
	return 0;
}

static int send_exact(struct socket *sock, const void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		struct kvec iov = {
			.iov_base = (void *)((const u8 *)buf + done),
			.iov_len = len - done,
		};
		struct msghdr msg = { 0 };
		int n;

		n = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
		if (n <= 0)
			return n < 0 ? n : -EIO;
		done += n;
	}
	return 0;
}

static int frame_send(struct socket *sock, const void *payload, u32 len)
{
	__le32 hdr;
	int ret;

	if (len == 0 || len > AMBA_VIRT_MAX_MSG)
		return -EINVAL;
	hdr = cpu_to_le32(len);
	ret = send_exact(sock, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	return send_exact(sock, payload, len);
}

static int frame_recv(struct socket *sock, void *payload, u32 *len, long timeout_jiffies)
{
	__le32 hdr;
	u32 n;
	int ret;

	ret = recv_exact(sock, &hdr, sizeof(hdr), timeout_jiffies);
	if (ret)
		return ret;
	n = le32_to_cpu(hdr);
	if (n == 0 || n > AMBA_VIRT_MAX_MSG)
		return -EPROTO;
	ret = recv_exact(sock, payload, n, timeout_jiffies);
	if (ret)
		return ret;
	*len = n;
	return 0;
}

void amba_virt_vsock_disconnect(struct amba_virt_dev *dev)
{
	mutex_lock(&dev->sock_lock);
	if (dev->conn_sock) {
		kernel_sock_shutdown(dev->conn_sock, SHUT_RDWR);
		sock_release(dev->conn_sock);
		dev->conn_sock = NULL;
	}
	mutex_unlock(&dev->sock_lock);
}

int amba_virt_vsock_connect(struct amba_virt_dev *dev)
{
	struct socket *sock = NULL;
	struct sockaddr_vm addr = { 0 };
	int ret;

	if (dev->is_host)
		return -EOPNOTSUPP;

	mutex_lock(&dev->sock_lock);
	if (dev->conn_sock) {
		mutex_unlock(&dev->sock_lock);
		return 0;
	}

	ret = sock_create_kern(&init_net, AF_VSOCK, SOCK_STREAM, 0, &sock);
	if (ret)
		goto out;

	addr.svm_family = AF_VSOCK;
	addr.svm_cid = dev->vsock_cid;
	addr.svm_port = dev->vsock_port;
	ret = kernel_connect(sock, (struct sockaddr *)&addr, sizeof(addr), 0);
	if (ret) {
		sock_release(sock);
		goto out;
	}
	dev->conn_sock = sock;
	sock = NULL;
out:
	mutex_unlock(&dev->sock_lock);
	return ret;
}

static int accept_one(void *data)
{
	struct amba_virt_dev *dev = data;

	while (!kthread_should_stop()) {
		struct socket *newsock = NULL;
		int ret;

		if (!dev->listen_sock) {
			msleep(100);
			continue;
		}
		ret = kernel_accept(dev->listen_sock, &newsock, O_NONBLOCK);
		if (ret) {
			if (kthread_should_stop())
				break;
			if (ret == -EAGAIN || ret == -EWOULDBLOCK ||
			    ret == -EINTR || ret == -ERESTARTSYS) {
				msleep_interruptible(100);
				continue;
			}
			msleep_interruptible(200);
			continue;
		}
		mutex_lock(&dev->sock_lock);
		if (dev->conn_sock) {
			kernel_sock_shutdown(dev->conn_sock, SHUT_RDWR);
			sock_release(dev->conn_sock);
		}
		dev->conn_sock = newsock;
		mutex_unlock(&dev->sock_lock);
		pr_info("amba_virt: accepted vsock connection\n");
	}
	return 0;
}

int amba_virt_vsock_listen(struct amba_virt_dev *dev)
{
	struct socket *sock = NULL;
	struct sockaddr_vm addr = { 0 };
	int ret;

	ret = sock_create_kern(&init_net, AF_VSOCK, SOCK_STREAM, 0, &sock);
	if (ret)
		return ret;

	addr.svm_family = AF_VSOCK;
	addr.svm_cid = VMADDR_CID_ANY;
	addr.svm_port = dev->vsock_port;
	ret = kernel_bind(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret)
		goto err;
	ret = kernel_listen(sock, 4);
	if (ret)
		goto err;

	dev->listen_sock = sock;
	dev->accept_thread = kthread_run(accept_one, dev, "amba_virt_acc");
	if (IS_ERR(dev->accept_thread)) {
		ret = PTR_ERR(dev->accept_thread);
		dev->accept_thread = NULL;
		goto err;
	}
	pr_info("amba_virt: listening vsock *:%u\n", dev->vsock_port);
	return 0;
err:
	if (sock)
		sock_release(sock);
	dev->listen_sock = NULL;
	return ret;
}

/*
 * Attach the host backing file.
 *
 * The file is created and sized by the hypervisor when the HVM domain starts,
 * which is well after this module loads: the module has to be resident early so
 * that /dev/amba_virt exists before the NOHYPER container is created, but the
 * window itself only shows up once the peer VM is running. So attach on first
 * use rather than at load, and re-read the size each time in case the window
 * was grown by a model change and an app restart.
 *
 * Missing file is not an error: the control plane stays usable and only mmap
 * is refused until the window appears.
 */
int amba_virt_attach_shm(struct amba_virt_dev *dev)
{
	struct file *f;
	loff_t size;
	int ret = 0;

	if (!dev || !dev->is_host || !dev->shm_path)
		return 0;

	mutex_lock(&dev->shm_lock);
	if (dev->shm_file) {
		size = i_size_read(file_inode(dev->shm_file));
		if (size > 0)
			dev->shm_size = (size_t)size;
		goto out;
	}
	f = filp_open(dev->shm_path, O_RDWR, 0);
	if (IS_ERR(f)) {
		ret = PTR_ERR(f);
		pr_debug("amba_virt: %s not available yet (%d)\n",
			 dev->shm_path, ret);
		goto out;
	}
	size = i_size_read(file_inode(f));
	if (size <= 0) {
		filp_close(f, NULL);
		pr_err("amba_virt: %s has size %lld\n",
		       dev->shm_path, (long long)size);
		ret = -EINVAL;
		goto out;
	}
	dev->shm_file = f;
	dev->shm_size = (size_t)size;
	pr_info("amba_virt: attached %s size %zu\n",
		dev->shm_path, dev->shm_size);
out:
	mutex_unlock(&dev->shm_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(amba_virt_attach_shm);

static int amba_virt_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct amba_virt_dev *dev = filp->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!dev)
		return -ENODEV;
	/* The window may have appeared since open. */
	amba_virt_attach_shm(dev);
	if (!dev->shm_size)
		return -ENODEV;
	if (size > dev->shm_size)
		return -EINVAL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif

	if (dev->shm_file && dev->shm_file->f_op && dev->shm_file->f_op->mmap) {
		if (vma->vm_file)
			fput(vma->vm_file);
		vma->vm_file = get_file(dev->shm_file);
		vma->vm_pgoff = 0;
		return dev->shm_file->f_op->mmap(dev->shm_file, vma);
	}
	if (dev->shm_phys) {
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return remap_pfn_range(vma, vma->vm_start,
				       PHYS_PFN(dev->shm_phys), size,
				       vma->vm_page_prot);
	}
	return -ENODEV;
}

static int amba_virt_open(struct inode *inode, struct file *filp)
{
	struct amba_virt_dev *dev;

	dev = container_of(inode->i_cdev, struct amba_virt_dev, cdev);
	filp->private_data = dev;
	/*
	 * Best effort. Opening has to succeed even without a window so the
	 * control plane and AMBA_VIRT_IOC_GET_INFO stay reachable; GET_INFO
	 * reports shm_size 0 and mmap returns -ENODEV until it attaches.
	 */
	amba_virt_attach_shm(dev);
	return 0;
}

static long amba_virt_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct amba_virt_dev *dev = filp->private_data;
	struct amba_virt_info info;
	struct amba_virt_xfer *xfer;
	int ret = 0;

	switch (cmd) {
	case AMBA_VIRT_IOC_GET_INFO:
		memset(&info, 0, sizeof(info));
		info.proto = AMBA_VIRT_PROTO;
		info.role = dev->is_host ? AMBA_VIRT_ROLE_HOST : AMBA_VIRT_ROLE_GUEST;
		info.shm_size = (u32)dev->shm_size;
		info.vsock_cid = dev->vsock_cid;
		info.vsock_port = dev->vsock_port;
		mutex_lock(&dev->sock_lock);
		info.connected = dev->conn_sock ? 1 : 0;
		mutex_unlock(&dev->sock_lock);
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;

	case AMBA_VIRT_IOC_CONNECT:
		if (dev->is_host)
			return 0;
		return amba_virt_vsock_connect(dev);

	case AMBA_VIRT_IOC_SEND:
	case AMBA_VIRT_IOC_RECV:
		break;
	default:
		return -ENOTTY;
	}

	xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return -ENOMEM;

	if (copy_from_user(xfer, (void __user *)arg, sizeof(*xfer))) {
		ret = -EFAULT;
		goto out_xfer;
	}

	if (cmd == AMBA_VIRT_IOC_SEND) {
		struct socket *sock = NULL;

		mutex_lock(&dev->send_lock);
		if (!dev->is_host) {
			ret = amba_virt_vsock_connect(dev);
			if (ret) {
				mutex_unlock(&dev->send_lock);
				goto out_xfer;
			}
		}
		mutex_lock(&dev->sock_lock);
		sock = dev->conn_sock;
		if (sock && sock->sk)
			sock_hold(sock->sk);
		mutex_unlock(&dev->sock_lock);

		if (!sock) {
			mutex_unlock(&dev->send_lock);
			ret = -ENOTCONN;
			goto out_xfer;
		}

		ret = frame_send(sock, xfer->data, xfer->len);
		if (ret < 0 && ret != -EINVAL && ret != -ETIMEDOUT) {
			amba_virt_vsock_disconnect(dev);
		}
		if (sock->sk)
			sock_put(sock->sk);
		mutex_unlock(&dev->send_lock);
		goto out_xfer;
	}

	/* RECV */
	{
		struct socket *sock = NULL;
		long timeout = MAX_SCHEDULE_TIMEOUT;
		u32 n = 0;

		if (xfer->timeout_ms > 0)
			timeout = msecs_to_jiffies(xfer->timeout_ms);
		else if (xfer->timeout_ms == 0)
			timeout = msecs_to_jiffies(5000);

		mutex_lock(&dev->recv_lock);
		mutex_lock(&dev->sock_lock);
		sock = dev->conn_sock;
		if (!sock && !dev->is_host) {
			mutex_unlock(&dev->sock_lock);
			ret = amba_virt_vsock_connect(dev);
			if (ret) {
				mutex_unlock(&dev->recv_lock);
				goto out_xfer;
			}
			mutex_lock(&dev->sock_lock);
			sock = dev->conn_sock;
		}
		if (sock && sock->sk)
			sock_hold(sock->sk);
		mutex_unlock(&dev->sock_lock);

		if (!sock) {
			mutex_unlock(&dev->recv_lock);
			ret = -ENOTCONN;
			goto out_xfer;
		}

		ret = frame_recv(sock, xfer->data, &n, timeout);
		if (ret < 0 && ret != -ETIMEDOUT) {
			amba_virt_vsock_disconnect(dev);
		}
		if (sock->sk)
			sock_put(sock->sk);
		mutex_unlock(&dev->recv_lock);

		if (ret)
			goto out_xfer;
		xfer->len = n;
		if (copy_to_user((void __user *)arg, xfer, sizeof(*xfer)))
			ret = -EFAULT;
	}

out_xfer:
	kfree(xfer);
	return ret;
}

static int amba_virt_release(struct inode *inode, struct file *filp)
{
	return 0;
}

const struct file_operations amba_virt_fops = {
	.owner = THIS_MODULE,
	.open = amba_virt_open,
	.release = amba_virt_release,
	.unlocked_ioctl = amba_virt_ioctl,
	.compat_ioctl = amba_virt_ioctl,
	.mmap = amba_virt_mmap,
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
static char *amba_virt_devnode(struct device *dev, umode_t *mode)
#else
static char *amba_virt_devnode(const struct device *dev, umode_t *mode)
#endif
{
	if (mode)
		*mode = 0666;
	return NULL;
}

int amba_virt_core_init(struct amba_virt_dev *dev, bool is_host)
{
	int ret;

	dev->is_host = is_host;
	dev->vsock_cid = AMBA_VIRT_VSOCK_CID;
	dev->vsock_port = AMBA_VIRT_VSOCK_PORT;
	mutex_init(&dev->sock_lock);
	mutex_init(&dev->send_lock);
	mutex_init(&dev->recv_lock);
	mutex_init(&dev->shm_lock);

	ret = alloc_chrdev_region(&dev->devt, 0, 1, AMBA_VIRT_DEV_NAME);
	if (ret)
		return ret;
	cdev_init(&dev->cdev, &amba_virt_fops);
	dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dev->cdev, dev->devt, 1);
	if (ret)
		goto err_unreg;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	dev->class = class_create(THIS_MODULE, AMBA_VIRT_DEV_NAME);
#else
	dev->class = class_create(AMBA_VIRT_DEV_NAME);
#endif
	if (IS_ERR(dev->class)) {
		ret = PTR_ERR(dev->class);
		dev->class = NULL;
		goto err_cdev;
	}
	dev->class->devnode = amba_virt_devnode;
	dev->device = device_create(dev->class, NULL, dev->devt, NULL,
				    AMBA_VIRT_DEV_NAME);
	if (IS_ERR(dev->device)) {
		ret = PTR_ERR(dev->device);
		dev->device = NULL;
		goto err_class;
	}
	return 0;

err_class:
	class_destroy(dev->class);
	dev->class = NULL;
err_cdev:
	cdev_del(&dev->cdev);
err_unreg:
	unregister_chrdev_region(dev->devt, 1);
	return ret;
}

void amba_virt_core_exit(struct amba_virt_dev *dev)
{
	if (dev->accept_thread) {
		kthread_stop(dev->accept_thread);
		dev->accept_thread = NULL;
	}
	amba_virt_vsock_disconnect(dev);
	if (dev->listen_sock) {
		kernel_sock_shutdown(dev->listen_sock, SHUT_RDWR);
		sock_release(dev->listen_sock);
		dev->listen_sock = NULL;
	}
	if (dev->device) {
		device_destroy(dev->class, dev->devt);
		dev->device = NULL;
	}
	if (dev->class) {
		class_destroy(dev->class);
		dev->class = NULL;
	}
	if (dev->devt) {
		cdev_del(&dev->cdev);
		unregister_chrdev_region(dev->devt, 1);
		dev->devt = 0;
	}
	if (dev->shm_file) {
		filp_close(dev->shm_file, NULL);
		dev->shm_file = NULL;
	}
	if (dev->shm_iomem) {
		iounmap(dev->shm_iomem);
		dev->shm_iomem = NULL;
	}
}
