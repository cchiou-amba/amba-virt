/*
 * iav_virt.c
 *
 * Ambarella IAV Linux HVM Guest Frontend Driver
 *
 * Exposes /dev/iav to unmodified user runtimes (libiav and legacy bare-metal applications) and
 * carries each call to the IAV broker in the Dom0 amba-virt-server over the amba_virt vsock
 * channel. The broker opens the real /dev/iav, validates every argument, and runs the call. Bulk
 * data (frames, bitstream) is never copied through vsock: mmap maps the IAV window the host
 * attached to this guest as an ivshmem BAR, after the broker authorized the mapping.
 *
 * The ioctl table that says which ioctls exist and how big their arguments are is the one the
 * broker uses (tools/iav_ioctl_table.inc, generated from the IAV driver sources). The broker
 * decides which of them are allowed. This driver only shapes the message; a guest that bypasses
 * it gets the same answer from the broker.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/sort.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/version.h>

#include <amba_virt.h>
#include <amba_virt_iav.h>
#include <amba_virt_iav_table.h>
#include <amba_virt_iav_win.h>
#include "amba_virt_kernel.h"

#include <iav_ioctl.h>
#include <ambas_imgproc_ioctl_arch.h>

#define IVSHMEM_VENDOR		0x1af4
#define IVSHMEM_DEVICE		0x1110
#define IAV_MAX_WINDOWS		AMBA_VIRT_IAV_MAX_WINDOWS
#define IAV_POLL_IDLE_JIFFIES	(2 * HZ)

static unsigned int iav_rpc_timeout_ms = 5000;
module_param_named(rpc_timeout_ms, iav_rpc_timeout_ms, uint, 0644);
MODULE_PARM_DESC(rpc_timeout_ms, "Longest a call waits for the broker (default 5000)");

/* ---- the ioctl table ---- */

struct iav_ent {
	unsigned int cmd;
	u8 kind;
	u8 dir;
	u16 flags;
	u32 size;
};

#define IAV_IOCTL(name, magic, kind, dir, size, cls, flags) { name, kind, dir, flags, size },
static const struct iav_ent g_table[] = {
#include "iav_ioctl_table.inc"
};
#undef IAV_IOCTL

#define IAV_TABLE_COUNT ARRAY_SIZE(g_table)

static u16 *g_index;	/* indices into g_table, sorted by cmd */

static int iav_ent_cmp(const void *a, const void *b)
{
	unsigned int ca = g_table[*(const u16 *)a].cmd;
	unsigned int cb = g_table[*(const u16 *)b].cmd;

	return ca < cb ? -1 : ca > cb;
}

static const struct iav_ent *iav_lookup(unsigned int cmd)
{
	int lo = 0, hi = (int)IAV_TABLE_COUNT - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const struct iav_ent *e = &g_table[g_index[mid]];

		if (e->cmd == cmd)
			return e;
		if (e->cmd < cmd)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* ---- the channel to the broker ---- */

struct iav_reply {
	u8 *buf;			/* the response frame */
	struct amba_virt_iav_resp r;	/* a copy of the response header */
	const u8 *out;			/* the output bytes, inside buf */
	u32 out_len;
};

static atomic_t g_seq = ATOMIC_INIT(0);

static void iav_reply_free(struct iav_reply *rep)
{
	kfree(rep->buf);
	rep->buf = NULL;
	rep->out = NULL;
	rep->out_len = 0;
}

/*
 * One request and its response. Returns a negative errno when the channel failed or the answer
 * was not well-formed. On 0 the caller reads rep->r.status and the rest, then frees the reply.
 */
static int iav_call(u32 op, u64 handle, u32 cmd, u64 arg_value, u64 arg2, const void *arg,
		    u32 arg_len, struct iav_reply *rep, unsigned int timeout_ms)
{
	struct amba_virt_msg *msg, *rmsg;
	struct amba_virt_iav_req *req;
	struct amba_virt_iav_resp *resp;
	u8 *buf;
	u32 rlen = AMBA_VIRT_MAX_MSG;
	u32 hdr = sizeof(*msg) + sizeof(*resp);
	int ret;

	memset(rep, 0, sizeof(*rep));
	if (arg_len > AMBA_VIRT_IAV_MAX_ARG)
		return -EMSGSIZE;
	buf = kzalloc(2 * AMBA_VIRT_MAX_MSG, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	msg = (struct amba_virt_msg *)buf;
	req = (struct amba_virt_iav_req *)(buf + sizeof(*msg));
	msg->type = AMBA_VIRT_MSG_IAV_REQ;
	msg->seq = (u32)atomic_inc_return(&g_seq);
	msg->shm_len = sizeof(*req) + arg_len;
	req->proto = AMBA_VIRT_IAV_PROTO;
	req->op = op;
	req->handle = handle;
	req->cmd = cmd;
	req->arg_len = arg_len;
	req->arg_value = arg_value;
	req->arg2 = arg2;
	if (arg_len)
		memcpy((u8 *)(req + 1), arg, arg_len);

	ret = amba_virt_rpc_seq(buf, sizeof(*msg) + sizeof(*req) + arg_len, buf + AMBA_VIRT_MAX_MSG,
			    &rlen, timeout_ms ? timeout_ms : iav_rpc_timeout_ms);
	if (ret)
		goto fail;
	rmsg = (struct amba_virt_msg *)(buf + AMBA_VIRT_MAX_MSG);
	resp = (struct amba_virt_iav_resp *)(rmsg + 1);
	if (rlen < hdr || rmsg->type != AMBA_VIRT_MSG_IAV_RESP || rmsg->seq != msg->seq ||
	    resp->proto != AMBA_VIRT_IAV_PROTO || resp->out_len > rlen - hdr) {
		pr_info_ratelimited("iav_virt: bad response: op %u rlen %u type %u (want %u) seq %u (want %u) proto %u out_len %u\n",
				    op, rlen, rlen >= sizeof(*rmsg) ? rmsg->type : 0, AMBA_VIRT_MSG_IAV_RESP,
				    rlen >= sizeof(*rmsg) ? rmsg->seq : 0, msg->seq,
				    rlen >= hdr ? resp->proto : 0, rlen >= hdr ? resp->out_len : 0);
		ret = -EPROTO;
		goto fail;
	}
	rep->buf = buf;
	rep->r = *resp;
	rep->out = (const u8 *)(resp + 1);
	rep->out_len = resp->out_len;
	return 0;
fail:
	kfree(buf);
	return ret;
}

/* ---- the windows ---- */

struct iav_win_dev {
	struct pci_dev *pdev;
	int bar;
	phys_addr_t phys;
	size_t size;
	bool present;
};

static struct iav_win_dev g_wins[IAV_MAX_WINDOWS];
static DEFINE_MUTEX(g_win_lock);

static bool iav_has_claim(void __iomem *io, size_t len)
{
	struct amba_virt_slice_claim claim;

	if (len >= sizeof(claim)) {
		memcpy_fromio(&claim, io + len - sizeof(claim), sizeof(claim));
		if (claim.magic == AMBA_VIRT_SLICE_CLAIM_MAGIC)
			return true;
	}
	if (len >= PAGE_SIZE) {
		memcpy_fromio(&claim, io + len - PAGE_SIZE, sizeof(claim));
		if (claim.magic == AMBA_VIRT_SLICE_CLAIM_MAGIC)
			return true;
	}
	return false;
}

/*
 * Find the ivshmem devices that are IAV windows. A device amba_virt has bound is a data slice. A
 * window with padding says what it is in its last bytes; the one without (the DSP partition
 * when it is already a power of two) is the unbound device whose size matches the broker's DSP
 * window and that carries no slice claim.
 */
static void iav_scan_windows(const struct amba_virt_iav_window *info, unsigned int ninfo)
{
	struct pci_dev *pdev = NULL;
	unsigned int i;

	mutex_lock(&g_win_lock);
	while ((pdev = pci_get_device(IVSHMEM_VENDOR, IVSHMEM_DEVICE, pdev)) != NULL) {
		resource_size_t len = 0;
		int bar = -1, b, id = -1;
		void __iomem *io;
		bool known = false;

		if (pdev->driver)
			continue;
		for (i = 0; i < IAV_MAX_WINDOWS; i++)
			if (g_wins[i].present && g_wins[i].pdev == pdev)
				known = true;
		if (known)
			continue;
		for (b = 0; b < PCI_STD_NUM_BARS; b++) {
			if ((pci_resource_flags(pdev, b) & IORESOURCE_MEM) &&
			    pci_resource_len(pdev, b) > len) {
				len = pci_resource_len(pdev, b);
				bar = b;
			}
		}
		if (bar < 0 || len <= AMBA_VIRT_IAV_WIN_HDR_BYTES)
			continue;
		if (pci_enable_device(pdev))
			continue;
		io = pci_iomap_range(pdev, bar, len - PAGE_SIZE, PAGE_SIZE);
		if (io) {
			struct amba_virt_iav_win_hdr hdr;

			memcpy_fromio(&hdr, io + PAGE_SIZE - AMBA_VIRT_IAV_WIN_HDR_BYTES, sizeof(hdr));
			if (hdr.magic == AMBA_VIRT_IAV_WIN_HDR_MAGIC &&
			    hdr.version == AMBA_VIRT_IAV_WIN_HDR_VERSION &&
			    hdr.window_id < IAV_MAX_WINDOWS && hdr.size == len) {
				id = (int)hdr.window_id;
			} else if (!iav_has_claim(io, PAGE_SIZE)) {
				/* no header and no slice claim: the DSP window, if the sizes agree */
				for (i = 0; i < ninfo; i++)
					if (info[i].id == AMBA_VIRT_IAV_WIN_DSP && info[i].size == len)
						id = (int)AMBA_VIRT_IAV_WIN_DSP;
			}
			pci_iounmap(pdev, io);
		}
		if (id < 0 || g_wins[id].present || pci_request_region(pdev, bar, "iav_virt")) {
			pci_disable_device(pdev);
			continue;
		}
		g_wins[id].pdev = pci_dev_get(pdev);
		g_wins[id].bar = bar;
		g_wins[id].phys = pci_resource_start(pdev, bar);
		g_wins[id].size = (size_t)len;
		g_wins[id].present = true;
		dev_info(&pdev->dev, "iav_virt: window %d BAR%d phys 0x%llx size %zu\n", id, bar,
			 (unsigned long long)g_wins[id].phys, (size_t)len);
	}
	mutex_unlock(&g_win_lock);
}

static void iav_release_windows(void)
{
	unsigned int i;

	mutex_lock(&g_win_lock);
	for (i = 0; i < IAV_MAX_WINDOWS; i++) {
		if (!g_wins[i].present)
			continue;
		pci_release_region(g_wins[i].pdev, g_wins[i].bar);
		pci_disable_device(g_wins[i].pdev);
		pci_dev_put(g_wins[i].pdev);
		memset(&g_wins[i], 0, sizeof(g_wins[i]));
	}
	mutex_unlock(&g_win_lock);
}

/* ---- one open /dev/iav ---- */

struct iav_file {
	struct mutex lock;		/* serializes calls on this descriptor */
	u64 handle;			/* the broker's handle for the real fd, 0 if none */
	bool poll_cfg_valid;		/* replayed after the session is reopened */
	struct iav_poll_event_config poll_cfg;

	wait_queue_head_t wq;
	struct work_struct poll_work;
	atomic_t armed;			/* the poll worker is running */
	bool ready;			/* the last poll slice found an event */
	bool closing;
	unsigned long last_poll;
};

static int iav_status_error(const struct iav_reply *rep)
{
	return rep->r.status < 0 ? rep->r.status : 0;
}

static bool iav_session_lost(int status)
{
	return status == -ESTALE || status == -EBADF;
}

/* Open the real descriptor and restore what the application had configured on it. */
static int iav_open_session(struct iav_file *f)
{
	struct iav_reply rep;
	int ret;

	ret = iav_call(AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, NULL, 0, &rep, 0);
	if (ret)
		return ret;
	ret = iav_status_error(&rep);
	if (!ret && rep.r.handle == 0)
		ret = -EPROTO;
	if (!ret)
		WRITE_ONCE(f->handle, rep.r.handle);
	iav_reply_free(&rep);
	if (ret)
		return ret;

	if (f->poll_cfg_valid) {
		ret = iav_call(AMBA_VIRT_IAV_OP_IOCTL, f->handle, IAV_IOC_SET_POLL_EVENT_CFG, 0, 0,
			       &f->poll_cfg, sizeof(f->poll_cfg), &rep, 0);
		if (!ret)
			iav_reply_free(&rep);
	}
	return 0;
}

static void iav_close_session(struct iav_file *f)
{
	struct iav_reply rep;
	u64 h = READ_ONCE(f->handle);

	if (!h)
		return;
	WRITE_ONCE(f->handle, 0);
	if (!iav_call(AMBA_VIRT_IAV_OP_CLOSE, h, 0, 0, 0, NULL, 0, &rep, 2000))
		iav_reply_free(&rep);
}

static void iav_poll_work(struct work_struct *w);

static int iav_virt_open(struct inode *inode, struct file *filp)
{
	struct iav_file *f;
	int ret;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	mutex_init(&f->lock);
	init_waitqueue_head(&f->wq);
	INIT_WORK(&f->poll_work, iav_poll_work);
	atomic_set(&f->armed, 0);
	ret = iav_open_session(f);
	if (ret) {
		kfree(f);
		return ret;
	}
	filp->private_data = f;
	return 0;
}

/* ---- poll: a worker asks the broker in slices, so poll() never sleeps in the RPC ---- */

static void iav_poll_work(struct work_struct *w)
{
	struct iav_file *f = container_of(w, struct iav_file, poll_work);

	while (!READ_ONCE(f->closing) && atomic_read(&f->armed)) {
		struct iav_reply rep;
		u64 h = READ_ONCE(f->handle);
		int ret;

		if (time_after(jiffies, f->last_poll + IAV_POLL_IDLE_JIFFIES))
			break;
		if (!h) {
			msleep(100);
			continue;
		}
		ret = iav_call(AMBA_VIRT_IAV_OP_POLL, h, POLLIN | POLLRDNORM, AMBA_VIRT_IAV_POLL_SLICE_MS,
			       0, NULL, 0, &rep, AMBA_VIRT_IAV_POLL_SLICE_MS + 2000);
		if (ret) {
			msleep(100);
			continue;
		}
		if (iav_session_lost(rep.r.status)) {
			iav_reply_free(&rep);
			mutex_lock(&f->lock);
			if (!f->closing && READ_ONCE(f->handle) == h)
				iav_open_session(f);
			mutex_unlock(&f->lock);
			continue;
		}
		if (rep.r.status == 0 && rep.r.rc >= 0 && (rep.r.value & (POLLIN | POLLRDNORM))) {
			WRITE_ONCE(f->ready, true);
			wake_up_interruptible(&f->wq);
		} else {
			WRITE_ONCE(f->ready, false);
			if (rep.r.status < 0)
				msleep(100);
		}
		iav_reply_free(&rep);
	}
	WRITE_ONCE(f->ready, false);
	atomic_set(&f->armed, 0);
}

static __poll_t iav_virt_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct iav_file *f = filp->private_data;

	poll_wait(filp, &f->wq, wait);
	f->last_poll = jiffies;
	if (!atomic_xchg(&f->armed, 1)) {
		queue_work(system_unbound_wq, &f->poll_work);
	}
	return READ_ONCE(f->ready) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static int iav_virt_release(struct inode *inode, struct file *filp)
{
	struct iav_file *f = filp->private_data;

	f->closing = true;
	atomic_set(&f->armed, 0);
	wake_up_interruptible(&f->wq);
	cancel_work_sync(&f->poll_work);
	mutex_lock(&f->lock);
	iav_close_session(f);
	mutex_unlock(&f->lock);
	kfree(f);
	return 0;
}

/* ---- ioctl ---- */

static long iav_virt_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct iav_file *f = filp->private_data;
	const struct iav_ent *e = iav_lookup(cmd);
	u8 *argbuf = NULL;
	u32 arg_len = 0, out_len = 0;
	u64 arg_value = 0;
	long result;
	int attempt;

	if (!e)
		return -ENOTTY;
	switch (e->kind) {
	case IAV_ARG_NONE:
		break;
	case IAV_ARG_VALUE:
		arg_value = arg;
		break;
	case IAV_ARG_PTR:
		if (e->size == 0 || e->size > AMBA_VIRT_IAV_MAX_ARG)
			return -ENOTTY;
		argbuf = kzalloc(e->size, GFP_KERNEL);
		if (!argbuf)
			return -ENOMEM;
		if (e->dir & IAV_DIR_W) {
			arg_len = e->size;
			if (copy_from_user(argbuf, (void __user *)arg, arg_len)) {
				kfree(argbuf);
				return -EFAULT;
			}
		}
		if (e->dir & IAV_DIR_R)
			out_len = e->size;
		break;
	default:
		return -ENOTTY;
	}

	mutex_lock(&f->lock);
	WRITE_ONCE(f->ready, false);
	for (attempt = 0; ; attempt++) {
		struct iav_reply rep;
		int ret = iav_call(AMBA_VIRT_IAV_OP_IOCTL, READ_ONCE(f->handle), cmd, arg_value, 0,
				   arg_len ? argbuf : NULL, arg_len, &rep, 0);

		if (ret) {
			result = ret;
			break;
		}
		if (iav_session_lost(rep.r.status) && attempt == 0) {
			/* the server restarted or forgot this descriptor: reopen once and retry */
			iav_reply_free(&rep);
			ret = iav_open_session(f);
			if (ret) {
				result = ret;
				break;
			}
			continue;
		}
		if (rep.r.status < 0) {
			result = rep.r.status;
		} else if (rep.r.rc < 0) {
			result = -rep.r.err;
			if (!result)
				result = -EIO;
		} else {
			result = rep.r.rc;
			if (out_len && rep.out_len == out_len &&
			    copy_to_user((void __user *)arg, rep.out, out_len))
				result = -EFAULT;
			else if (out_len && rep.out_len != out_len)
				result = -EPROTO;
			if (e->cmd == IAV_IOC_SET_POLL_EVENT_CFG && result >= 0) {
				memcpy(&f->poll_cfg, argbuf, sizeof(f->poll_cfg));
				f->poll_cfg_valid = true;
			} else if (e->cmd == IAV_IOC_CLEAR_POLL_EVENT_CFG && result >= 0) {
				f->poll_cfg_valid = false;
			}
		}
		iav_reply_free(&rep);
		break;
	}
	mutex_unlock(&f->lock);
	kfree(argbuf);
	return result;
}

/* ---- mmap ---- */

static int iav_map_chunk(struct vm_area_struct *vma, unsigned long uaddr, phys_addr_t phys,
			 u64 len, pgprot_t prot)
{
	return io_remap_pfn_range(vma, uaddr, PHYS_PFN(phys), len, prot);
}

/*
 * The application maps a partition at the address the driver reported for it, as on bare metal.
 * The broker authorizes the mapping and says which window and how much; this maps that window's
 * BAR. A bitstream ring asks for twice the window, which maps the same BAR twice in a row.
 */
static int iav_virt_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct iav_file *f = filp->private_data;
	u64 offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 length = vma->vm_end - vma->vm_start;
	struct amba_virt_iav_window info[IAV_MAX_WINDOWS];
	struct amba_virt_iav_map map;
	struct iav_reply rep;
	unsigned int ninfo = 0, i, prot = 0;
	u64 part_len = 0, first, second;
	pgprot_t pg;
	int ret;

	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	if (vma->vm_flags & VM_READ)
		prot |= 1;	/* PROT_READ */
	if (vma->vm_flags & VM_WRITE)
		prot |= 2;	/* PROT_WRITE */

	mutex_lock(&f->lock);
	ret = iav_call(AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, NULL, 0, &rep, 0);
	if (!ret) {
		ret = iav_status_error(&rep);
		if (!ret) {
			ninfo = rep.out_len / sizeof(info[0]);
			if (ninfo > IAV_MAX_WINDOWS || rep.out_len % sizeof(info[0]))
				ret = -EPROTO;
			else
				memcpy(info, rep.out, ninfo * sizeof(info[0]));
		}
		iav_reply_free(&rep);
	}
	if (ret)
		goto out;

	ret = iav_call(AMBA_VIRT_IAV_OP_MAP, READ_ONCE(f->handle), prot, offset, length, NULL, 0,
		       &rep, 0);
	if (!ret) {
		ret = iav_status_error(&rep);
		if (!ret && rep.out_len != sizeof(map))
			ret = -EPROTO;
		if (!ret)
			memcpy(&map, rep.out, sizeof(map));
		iav_reply_free(&rep);
	}
	if (ret)
		goto out;
	if ((prot & ~map.prot) || map.length != length) {
		ret = -EPERM;
		goto out;
	}
	for (i = 0; i < ninfo; i++)
		if (info[i].id == map.window)
			part_len = info[i].part_len;
	if (!part_len || map.window >= IAV_MAX_WINDOWS) {
		ret = -EPROTO;
		goto out;
	}

	iav_scan_windows(info, ninfo);
	mutex_lock(&g_win_lock);
	if (!g_wins[map.window].present || map.window_offset > g_wins[map.window].size ||
	    part_len > g_wins[map.window].size - map.window_offset) {
		mutex_unlock(&g_win_lock);
		ret = -ENODEV;
		goto out;
	}
	first = min(length, part_len);
	second = length - first;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif
	pg = pgprot_writecombine(vma->vm_page_prot);
	ret = iav_map_chunk(vma, vma->vm_start, g_wins[map.window].phys + map.window_offset,
			    first, pg);
	if (!ret && second)
		ret = iav_map_chunk(vma, vma->vm_start + first,
				    g_wins[map.window].phys + map.window_offset, second, pg);
	mutex_unlock(&g_win_lock);
out:
	mutex_unlock(&f->lock);
	return ret;
}

static const struct file_operations iav_virt_fops = {
	.owner = THIS_MODULE,
	.open = iav_virt_open,
	.release = iav_virt_release,
	.unlocked_ioctl = iav_virt_ioctl,
	.compat_ioctl = iav_virt_ioctl,
	.mmap = iav_virt_mmap,
	.poll = iav_virt_poll,
	.llseek = no_llseek,
};

static struct miscdevice iav_virt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "iav",
	.fops = &iav_virt_fops,
	.mode = 0660,
};

static int __init iav_virt_init(void)
{
	unsigned int i;
	int ret;

	g_index = kmalloc_array(IAV_TABLE_COUNT, sizeof(*g_index), GFP_KERNEL);
	if (!g_index)
		return -ENOMEM;
	for (i = 0; i < IAV_TABLE_COUNT; i++)
		g_index[i] = (u16)i;
	sort(g_index, IAV_TABLE_COUNT, sizeof(*g_index), iav_ent_cmp, NULL);
	ret = misc_register(&iav_virt_misc);
	if (ret) {
		kfree(g_index);
		return ret;
	}
	pr_info("iav_virt: /dev/iav registered, %zu ioctls in the table\n",
		(size_t)IAV_TABLE_COUNT);
	return 0;
}

static void __exit iav_virt_exit(void)
{
	misc_deregister(&iav_virt_misc);
	iav_release_windows();
	kfree(g_index);
}

module_init(iav_virt_init);
module_exit(iav_virt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Ambarella IAV guest frontend (virtual /dev/iav over amba_virt)");
MODULE_SOFTDEP("pre: amba_virt");
