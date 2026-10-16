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
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/sysfs.h>
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
#include <linux/refcount.h>
#include <linux/log2.h>

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

static inline unsigned int iav_rpc_timeout_ms_get(void)
{
	return iav_rpc_timeout_ms;
}

/* ---- the ioctl table ---- */

struct iav_ent {
	unsigned int cmd;
	u8 kind;
	u8 dir;
	u16 flags;
	u32 size;	/* what the ioctl header names */
	u32 wire;	/* what the handler copies when that differs, else 0 */
};

#define IAV_IOCTL(name, magic, kind, dir, size, cls, flags, wire) { name, kind, dir, flags, size, wire },
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

/* ---- the negotiated protocol (OP_HELLO) ---- */

#define IAV_NEG_UNKNOWN		0	/* not asked yet, or the channel failed: ask again */
#define IAV_NEG_DONE		1	/* the broker granted features */
#define IAV_NEG_LEGACY		2	/* an older broker (-ENOSYS): the legacy protocol */

#define IAV_OFFERED_FEATURES	(AMBA_VIRT_IAV_F_NC_WINDOWS | AMBA_VIRT_IAV_F_SIZE_OVERRIDE | AMBA_VIRT_IAV_F_EXT_INFO | \
				 AMBA_VIRT_IAV_F_ARENA | AMBA_VIRT_IAV_F_APERTURE | AMBA_VIRT_IAV_F_INLINE)

struct iav_neg {
	int state;
	u32 revision;		/* the broker's */
	u32 role;		/* AMBA_VIRT_IAV_ROLE_* when it answered */
	u64 features;		/* granted */
	u32 max_window_id;
	u64 table_hash;		/* the broker's */
};

static DEFINE_MUTEX(g_neg_lock);
static struct iav_neg g_neg;
static u64 g_table_hash;

static u64 iav_features(void)
{
	return READ_ONCE(g_neg.features);
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

	/*
	 * The windows are Normal-NC: stores the application made to them may still be buffered. The DSP must see a
	 * JPEG, an overlay or a table before the request that names it reaches the driver.
	 */
	wmb();
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
	/* What the DSP wrote before the answer is read after it. */
	rmb();
	rep->buf = buf;
	rep->r = *resp;
	rep->out = (const u8 *)(resp + 1);
	rep->out_len = resp->out_len;
	return 0;
fail:
	kfree(buf);
	return ret;
}

/*
 * Ask the broker what it supports. force: ask again even if the answer is known (the broker restarted and forgot
 * this guest). An older broker answers -ENOSYS and the module runs the legacy protocol; a channel failure leaves the
 * state unknown so the next open asks again.
 */
static void iav_negotiate(bool force)
{
	struct amba_virt_iav_hello hl;
	struct amba_virt_iav_hello_resp out;
	struct iav_reply rep;
	int ret;

	mutex_lock(&g_neg_lock);
	if (!force && g_neg.state != IAV_NEG_UNKNOWN) {
		mutex_unlock(&g_neg_lock);
		return;
	}
	memset(&hl, 0, sizeof(hl));
	hl.revision = AMBA_VIRT_IAV_REVISION;
	hl.page_size = PAGE_SIZE;
	hl.features = IAV_OFFERED_FEATURES;
	hl.max_window_id = IAV_MAX_WINDOWS - 1;
	hl.table_rows = IAV_TABLE_COUNT;
	hl.table_hash = g_table_hash;
	ret = iav_call(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, &hl, sizeof(hl), &rep, 0);
	if (ret) {
		/* the channel failed: after a broker restart nothing negotiated holds; ask again later */
		if (force)
			memset(&g_neg, 0, sizeof(g_neg));
		mutex_unlock(&g_neg_lock);
		return;
	}
	memset(&g_neg, 0, sizeof(g_neg));
	if (rep.r.status == 0 && rep.out_len == sizeof(out)) {
		memcpy(&out, rep.out, sizeof(out));
		g_neg.state = IAV_NEG_DONE;
		g_neg.revision = out.revision;
		g_neg.role = out.role;
		/* never more than offered, whatever the answer says */
		WRITE_ONCE(g_neg.features, out.features & IAV_OFFERED_FEATURES);
		g_neg.max_window_id = min_t(u32, out.max_window_id, IAV_MAX_WINDOWS - 1);
		g_neg.table_hash = out.table_hash;
		pr_info("iav_virt: negotiated revision %u features 0x%llx role %u%s\n", out.revision,
			(unsigned long long)g_neg.features, out.role,
			out.table_hash == g_table_hash ? "" : " (the broker's ioctl table differs from this module's)");
	} else {
		g_neg.state = IAV_NEG_LEGACY;
		pr_info("iav_virt: the broker does not negotiate (%d): legacy protocol\n", rep.r.status);
	}
	iav_reply_free(&rep);
	mutex_unlock(&g_neg_lock);
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
 * window with padding says what it is in its last bytes; one without (a partition that is already a
 * power of two: the DSP partition, the overlay partition) is the unbound device whose size matches
 * exactly one of the broker's unpadded windows and that carries no slice claim.
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
			} else if (hdr.magic == AMBA_VIRT_IAV_WIN_HDR_MAGIC && hdr.window_id >= AMBA_VIRT_IAV_WIN_ID_SLICE) {
				/* a private slice (the arena's backing store): never a window */
				id = -1;
			} else if (hdr.magic == AMBA_VIRT_IAV_AUX_HDR_MAGIC) {
				/* the aux aperture: never a window */
				id = -1;
			} else if (!iav_has_claim(io, PAGE_SIZE)) {
				/*
				 * no header and no slice claim: a window with no padding (the DSP partition, the
				 * overlay partition), if exactly one such window has this size
				 */
				unsigned int matches = 0;

				for (i = 0; i < ninfo; i++) {
					if (info[i].id < IAV_MAX_WINDOWS && info[i].size == len &&
					    info[i].part_len == len) {
						id = (int)info[i].id;
						matches++;
					}
				}
				if (matches != 1)
					id = -1;
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

/* ---- the arena: anonymous partitions in the private slice (plan_iav_usb_yolo, Envelope E) ---- */

/*
 * The allocator is plain C shared with its host unit test (iav_arena.h). The guest's private slice is attached by the
 * host as an ivshmem BAR whose claim page carries the slice header (AMBA_VIRT_IAV_WIN_ID_SLICE + index); OP_INFO says,
 * to a primary guest that negotiated AMBA_VIRT_IAV_F_ARENA, where the slice starts in the IAV address space. An
 * application sees what the bare-metal driver gives it: ALLOC_ANON_MEM_PART returns an anonymous id and the address of
 * the block (the slice's address plus the block's offset), which it passes to mmap; the buffer calls name the block
 * by its id and offsets in it, and the broker places them in this guest's slice.
 */
#include "iav_arena.c"

struct iav_arena_dev {
	bool present;
	bool broken;		/* the broker reported another slice while blocks exist: no new allocation */
	struct pci_dev *pdev;
	int bar;
	phys_addr_t phys;	/* the BAR in the guest's physical address space */
	u64 size;		/* the BAR (the slice) */
	u32 index;		/* the slice */
	u64 base;		/* the slice's address in the IAV address space, as QUERY_MEMBLOCK addresses are */
	u64 usable;		/* the arena: the slice less its claim page */
	u32 pid;		/* the pool partition behind it */
};

static DEFINE_MUTEX(g_arena_lock);
static struct iav_arena g_arena;
static struct iav_arena_dev g_adev;

/* A mapping of a block, shared by the copies of its VMA (fork, split). */
struct iav_arena_vma {
	struct iav_arena_blk *blk;
	const void *file;
	refcount_t refs;
};

static void *iav_arena_zalloc(size_t n)
{
	return kzalloc(n, GFP_KERNEL);
}

static void iav_arena_kfree(void *p)
{
	kfree(p);
}

static bool iav_arena_ready(void)
{
	return (iav_features() & AMBA_VIRT_IAV_F_ARENA) && READ_ONCE(g_adev.present);
}

static void iav_arena_drop_device_locked(void)
{
	if (!g_adev.present)
		return;
	iav_arena_destroy(&g_arena);
	pci_release_region(g_adev.pdev, g_adev.bar);
	pci_disable_device(g_adev.pdev);
	pci_dev_put(g_adev.pdev);
	memset(&g_adev, 0, sizeof(g_adev));
}

/* The unbound ivshmem device whose claim page names this slice. Called with g_arena_lock held. */
static int iav_arena_claim_device_locked(const struct amba_virt_iav_entry *ent)
{
	struct pci_dev *pdev = NULL;

	while ((pdev = pci_get_device(IVSHMEM_VENDOR, IVSHMEM_DEVICE, pdev)) != NULL) {
		struct amba_virt_iav_win_hdr hdr;
		resource_size_t len = 0;
		void __iomem *io;
		int bar = -1, b;

		if (pdev->driver)
			continue;
		for (b = 0; b < PCI_STD_NUM_BARS; b++) {
			if ((pci_resource_flags(pdev, b) & IORESOURCE_MEM) && pci_resource_len(pdev, b) > len) {
				len = pci_resource_len(pdev, b);
				bar = b;
			}
		}
		if (bar < 0 || len != ent->size)
			continue;
		if (pci_enable_device(pdev))
			continue;
		io = pci_iomap_range(pdev, bar, len - AMBA_VIRT_IAV_WIN_HDR_BYTES, AMBA_VIRT_IAV_WIN_HDR_BYTES);
		if (!io) {
			pci_disable_device(pdev);
			continue;
		}
		memcpy_fromio(&hdr, io, sizeof(hdr));
		pci_iounmap(pdev, io);
		if (hdr.magic != AMBA_VIRT_IAV_WIN_HDR_MAGIC || hdr.version != AMBA_VIRT_IAV_WIN_HDR_VERSION ||
		    hdr.window_id != AMBA_VIRT_IAV_WIN_ID_SLICE + ent->reserved || hdr.size != len ||
		    hdr.part_len != ent->part_len || pci_request_region(pdev, bar, "iav_virt_arena")) {
			pci_disable_device(pdev);
			continue;
		}
		g_adev.pdev = pdev;	/* the reference pci_get_device took */
		g_adev.bar = bar;
		g_adev.phys = pci_resource_start(pdev, bar);
		g_adev.size = len;
		return 0;
	}
	return -ENODEV;
}

/*
 * After a negotiation that granted the arena: find the arena entry in OP_INFO and the slice BAR it describes. The slice
 * of a guest does not change while it runs; if the broker reports another one while blocks exist, the arena stops
 * allocating (the blocks already handed out stay mapped where they are).
 */
static void iav_arena_discover(void)
{
	struct amba_virt_iav_entry ent, found;
	struct iav_reply rep;
	bool have = false;
	u32 i, n;

	if (!(iav_features() & AMBA_VIRT_IAV_F_ARENA))
		return;
	if (iav_call(AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, NULL, 0, &rep, 0))
		return;
	n = rep.r.value;
	if (rep.r.status == 0 && n && n <= AMBA_VIRT_IAV_MAX_OUT / sizeof(ent) && rep.out_len == n * sizeof(ent)) {
		for (i = 0; i < n; i++) {
			memcpy(&ent, rep.out + (size_t)i * sizeof(ent), sizeof(ent));
			if (ent.id == AMBA_VIRT_IAV_ENT_ID_ARENA && (ent.flags & AMBA_VIRT_IAV_ENT_ARENA)) {
				found = ent;
				have = true;
			}
		}
	}
	iav_reply_free(&rep);
	if (!have || !is_power_of_2(found.size) || found.part_len < IAV_ARENA_ALIGN || found.part_len >= found.size ||
	    (found.base & (PAGE_SIZE - 1)) || found.reserved > 0xffu)
		return;

	mutex_lock(&g_arena_lock);
	if (g_adev.present) {
		if (g_adev.index == found.reserved && g_adev.base == found.base && g_adev.usable == found.part_len &&
		    g_adev.size == found.size) {
			mutex_unlock(&g_arena_lock);
			return;
		}
		if (g_arena.nblk) {
			if (!g_adev.broken)
				pr_warn("iav_virt: the broker reports slice %u, this guest's blocks are in slice %u: no new allocation\n",
					(u32)found.reserved, g_adev.index);
			g_adev.broken = true;
			mutex_unlock(&g_arena_lock);
			return;
		}
		iav_arena_drop_device_locked();
	}
	if (iav_arena_claim_device_locked(&found) == 0) {
		if (iav_arena_init(&g_arena, found.part_len, PAGE_SIZE, iav_arena_zalloc, iav_arena_kfree) == 0) {
			g_adev.index = found.reserved;
			g_adev.base = found.base;
			g_adev.usable = found.part_len;
			g_adev.pid = found.pid;
			g_adev.broken = false;
			WRITE_ONCE(g_adev.present, true);
			dev_info(&g_adev.pdev->dev, "iav_virt: arena in slice %u: BAR%d phys 0x%llx, 0x%llx bytes at 0x%llx\n",
				 g_adev.index, g_adev.bar, (unsigned long long)g_adev.phys,
				 (unsigned long long)g_adev.usable, (unsigned long long)g_adev.base);
		} else {
			pci_release_region(g_adev.pdev, g_adev.bar);
			pci_disable_device(g_adev.pdev);
			pci_dev_put(g_adev.pdev);
			memset(&g_adev, 0, sizeof(g_adev));
		}
	} else {
		pr_info("iav_virt: the broker granted an arena in slice %u, but no BAR carries its header\n",
			(u32)found.reserved);
	}
	mutex_unlock(&g_arena_lock);
}

/*
 * The anonymous-partition calls, answered here as the bare-metal driver answers them. Returns true when the call was
 * handled (*result is its return value); false for a call this guest's broker must see (a named partition).
 */
static bool iav_arena_local(void *file, unsigned int cmd, unsigned long arg, long *result)
{
	void __user *up = (void __user *)arg;

	switch (cmd) {
	case IAV_IOC_ALLOC_ANON_MEM_PART: {
		struct iav_alloc_mem_part cfg;
		u64 off = 0, len = 0;
		u32 vid = 0;
		int rc;

		if (copy_from_user(&cfg, up, sizeof(cfg))) {
			*result = -EFAULT;
			return true;
		}
		mutex_lock(&g_arena_lock);
		if (g_adev.broken || !g_adev.present)
			rc = -EIO;
		else
			rc = iav_arena_alloc(&g_arena, file, cfg.length, &vid, &off, &len);
		if (rc == 0) {
			cfg.pid = vid;
			cfg.length = len;
			cfg.offset = g_adev.base + off;
		}
		mutex_unlock(&g_arena_lock);
		/* as the driver: nothing is written back when the allocation failed */
		*result = rc ? rc : (copy_to_user(up, &cfg, sizeof(cfg)) ? -EFAULT : 0);
		return true;
	}
	case IAV_IOC_FREE_MEM_PART: {
		struct iav_free_mem_part fr;

		if (copy_from_user(&fr, up, sizeof(fr))) {
			*result = -EFAULT;
			return true;
		}
		if (!iav_arena_is_anon(fr.pid))
			return false;	/* a named partition: the broker's answer */
		mutex_lock(&g_arena_lock);
		*result = iav_arena_free(&g_arena, fr.pid);
		mutex_unlock(&g_arena_lock);
		return true;
	}
	case IAV_IOC_REGISTER_ANON_MEM_PART:
	case IAV_IOC_UNREGISTER_ANON_MEM_PART: {
		struct iav_anon_mem_part mp;
		int rc;

		if (copy_from_user(&mp, up, sizeof(mp))) {
			*result = -EFAULT;
			return true;
		}
		mutex_lock(&g_arena_lock);
		rc = cmd == IAV_IOC_REGISTER_ANON_MEM_PART ? iav_arena_register(&g_arena, mp.pid, file) :
							     iav_arena_unregister(&g_arena, mp.pid, file);
		mutex_unlock(&g_arena_lock);
		/* the driver's handlers answer every failure of these two with EINVAL (iav_enc_api.c) */
		*result = rc ? -EINVAL : 0;
		return true;
	}
	case IAV_IOC_QUERY_MEMBLOCK: {
		struct iav_querymem *q;
		struct iav_arena_blk *b;
		int mid;

		/* the query is large (its union holds the canvas table): not on the kernel stack */
		q = kmalloc(sizeof(*q), GFP_KERNEL);
		if (!q) {
			*result = -ENOMEM;
			return true;
		}
		if (copy_from_user(q, up, sizeof(*q))) {
			kfree(q);
			*result = -EFAULT;
			return true;
		}
		memcpy(&mid, &q->mid, sizeof(mid));
		if (mid != (int)IAV_MEM_PARTITION || !iav_arena_is_anon(q->arg.partition.pid)) {
			kfree(q);
			return false;
		}
		mutex_lock(&g_arena_lock);
		b = iav_arena_find(&g_arena, q->arg.partition.pid);
		if (b) {
			q->arg.partition.mem.addr = g_adev.base + b->off;
			q->arg.partition.mem.length = b->len;
			q->arg.partition.mem.offset = 0;
		}
		mutex_unlock(&g_arena_lock);
		*result = !b ? -EINVAL : (copy_to_user(up, q, sizeof(*q)) ? -EFAULT : 0);
		kfree(q);
		return true;
	}
	default:
		return false;
	}
}

/* Add the block named by an anonymous id to the OP_ARENA header (once). 0, or -EINVAL for an id with no block. */
static int iav_arena_declare(struct amba_virt_iav_arena_req *h, u32 vid)
{
	struct iav_arena_blk *b;
	u32 k;

	for (k = 0; k < h->count; k++)
		if (h->blk[k].vid == vid)
			return 0;
	b = iav_arena_find(&g_arena, vid);
	if (!b || h->count >= AMBA_VIRT_IAV_ARENA_MAX_BLKS)
		return -EINVAL;
	h->blk[h->count].vid = vid;
	h->blk[h->count].offset = b->off;
	h->blk[h->count].length = b->len;
	h->count++;
	return 0;
}

/*
 * A buffer call (SET_IMG_SCALE, GDMA_COPY, SYNC_CACHE_MEM_PART) that names blocks of the arena: the OP_ARENA header
 * that places them. 1 with the header, 0 when the call names no block, -EINVAL for an anonymous id this guest does not
 * hold (a freed id, another guest's id: the driver's answer to an unknown partition).
 */
static int iav_arena_header(unsigned int cmd, const void *argbuf, struct amba_virt_iav_arena_req *h)
{
	int rc = 0;

	memset(h, 0, sizeof(*h));
	mutex_lock(&g_arena_lock);
	if (cmd == IAV_IOC_SET_IMG_SCALE) {
		const struct iav_img_scale *s = argbuf;
		const struct iav_scale_buf_info *bufs[2] = { &s->input_buf, &s->output_buf };
		int i;

		for (i = 0; i < 2 && rc == 0; i++)
			if (!bufs[i]->use_phys && !bufs[i]->use_dma_buf_fd && iav_arena_is_anon(bufs[i]->pid))
				rc = iav_arena_declare(h, bufs[i]->pid);
	} else if (cmd == IAV_IOC_GDMA_COPY) {
		const struct iav_gdma_copy *g = argbuf;

		if (!g->src_use_phys && !g->src_use_dma_buf_fd && iav_arena_is_anon(g->src_mmap_type))
			rc = iav_arena_declare(h, g->src_mmap_type);
		if (rc == 0 && !g->dst_use_phys && !g->dst_use_dma_buf_fd && iav_arena_is_anon(g->dst_mmap_type))
			rc = iav_arena_declare(h, g->dst_mmap_type);
	} else if (cmd == IAV_IOC_SYNC_CACHE_MEM_PART) {
		const struct iav_sync_cache_mem_part *c = argbuf;

		if (iav_arena_is_anon(c->pid))
			rc = iav_arena_declare(h, c->pid);
	}
	mutex_unlock(&g_arena_lock);
	if (rc)
		return rc;
	return h->count ? 1 : 0;
}

static void iav_arena_vm_open(struct vm_area_struct *vma)
{
	struct iav_arena_vma *m = vma->vm_private_data;

	mutex_lock(&g_arena_lock);
	refcount_inc(&m->refs);
	iav_arena_map(&g_arena, m->blk, m->file);
	mutex_unlock(&g_arena_lock);
}

static void iav_arena_vm_close(struct vm_area_struct *vma)
{
	struct iav_arena_vma *m = vma->vm_private_data;

	mutex_lock(&g_arena_lock);
	iav_arena_unmap(&g_arena, m->blk, m->file);
	if (refcount_dec_and_test(&m->refs))
		kfree(m);
	mutex_unlock(&g_arena_lock);
}

static const struct vm_operations_struct iav_arena_vm_ops = {
	.open = iav_arena_vm_open,
	.close = iav_arena_vm_close,
};

/*
 * mmap of an address in the arena: as the bare-metal driver, the address must be the start of a block and the length
 * at most twice the block (a ring maps it twice in a row); shorter maps are accepted. Returns true when the address
 * is the arena's (*result is the answer).
 */
static bool iav_arena_mmap(void *file, struct vm_area_struct *vma, u64 offset, u64 length, int *result)
{
	struct iav_arena_blk *b;
	struct iav_arena_vma *m;
	u64 first, second;
	phys_addr_t at;
	pgprot_t pg;
	int ret;

	mutex_lock(&g_arena_lock);
	if (!g_adev.present || offset < g_adev.base || offset - g_adev.base >= g_adev.usable) {
		mutex_unlock(&g_arena_lock);
		return false;
	}
	b = iav_arena_find_at(&g_arena, offset - g_adev.base);
	if (!b || length == 0 || length > 2 * b->len) {
		mutex_unlock(&g_arena_lock);
		*result = -EINVAL;
		return true;
	}
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m || iav_arena_map(&g_arena, b, file) != 0) {
		mutex_unlock(&g_arena_lock);
		kfree(m);
		*result = -EINVAL;
		return true;
	}
	m->blk = b;
	m->file = file;
	refcount_set(&m->refs, 1);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif
	pg = pgprot_writecombine(vma->vm_page_prot);
	at = g_adev.phys + b->off;
	first = min(length, b->len);
	second = length - first;
	ret = io_remap_pfn_range(vma, vma->vm_start, PHYS_PFN(at), first, pg);
	if (!ret && second)
		ret = io_remap_pfn_range(vma, vma->vm_start + first, PHYS_PFN(at), second, pg);
	if (ret) {
		iav_arena_unmap(&g_arena, b, file);
		mutex_unlock(&g_arena_lock);
		kfree(m);
		*result = ret;
		return true;
	}
	vma->vm_private_data = m;
	vma->vm_ops = &iav_arena_vm_ops;
	mutex_unlock(&g_arena_lock);
	*result = 0;
	return true;
}

/* ---- the aux aperture (plan_iav_usb_yolo, Envelope F) ---- */

/*
 * One ivshmem BAR per guest that composes the partitions this guest's role may map beyond the legacy windows (MASK,
 * WARP, QPMATRIX, CE_GO_MAP, CANVAS_OVERLAY, ARB_BLUR, YUV_REMAP for the primary). OP_INFO describes it to a guest that
 * negotiated AMBA_VIRT_IAV_F_APERTURE: a descriptor (the BAR's size and the epoch of its header) and one entry per
 * partition (its address as QUERY_MEMBLOCK reports it, its length, its offset in the BAR). The BAR is the unbound
 * ivshmem device whose last 64 bytes carry the AMAP header with that size and epoch. An application maps a partition
 * at its address, as on bare metal; the broker authorizes it and answers the entry, which is mapped from the BAR.
 */
#define IAV_AUX_MAX	AMBA_VIRT_IAV_AUX_MAX_RANGES

struct iav_aux_ent {
	u32 id;
	u32 pid;
	u64 base;
	u64 part_len;
	u64 offset;
};

struct iav_aperture_dev {
	bool present;
	struct pci_dev *pdev;
	int bar;
	phys_addr_t phys;
	u64 size;
	u32 epoch;
	u32 count;
	struct iav_aux_ent ent[IAV_AUX_MAX];
};

static DEFINE_MUTEX(g_ap_lock);
static struct iav_aperture_dev g_ap;

static void iav_aperture_drop_locked(void)
{
	if (!g_ap.present)
		return;
	pci_release_region(g_ap.pdev, g_ap.bar);
	pci_disable_device(g_ap.pdev);
	pci_dev_put(g_ap.pdev);
	memset(&g_ap, 0, sizeof(g_ap));
}

/* The unbound ivshmem device whose AMAP header names this size, epoch and entry count. Called with g_ap_lock held. */
static int iav_aperture_claim_locked(u64 size, u32 epoch, u32 count)
{
	struct pci_dev *pdev = NULL;

	while ((pdev = pci_get_device(IVSHMEM_VENDOR, IVSHMEM_DEVICE, pdev)) != NULL) {
		struct amba_virt_iav_aux_hdr hdr;
		resource_size_t len = 0;
		void __iomem *io;
		int bar = -1, b;

		if (pdev->driver)
			continue;
		for (b = 0; b < PCI_STD_NUM_BARS; b++) {
			if ((pci_resource_flags(pdev, b) & IORESOURCE_MEM) && pci_resource_len(pdev, b) > len) {
				len = pci_resource_len(pdev, b);
				bar = b;
			}
		}
		if (bar < 0 || len != size)
			continue;
		if (pci_enable_device(pdev))
			continue;
		io = pci_iomap_range(pdev, bar, len - AMBA_VIRT_IAV_WIN_HDR_BYTES, AMBA_VIRT_IAV_WIN_HDR_BYTES);
		if (!io) {
			pci_disable_device(pdev);
			continue;
		}
		memcpy_fromio(&hdr, io, sizeof(hdr));
		pci_iounmap(pdev, io);
		if (hdr.magic != AMBA_VIRT_IAV_AUX_HDR_MAGIC || hdr.version != AMBA_VIRT_IAV_AUX_HDR_VERSION ||
		    hdr.epoch != epoch || hdr.size != len || hdr.count != count ||
		    pci_request_region(pdev, bar, "iav_virt_aux")) {
			pci_disable_device(pdev);
			continue;
		}
		g_ap.pdev = pdev;	/* the reference pci_get_device took */
		g_ap.bar = bar;
		g_ap.phys = pci_resource_start(pdev, bar);
		return 0;
	}
	return -ENODEV;
}

/*
 * After a negotiation that granted the aperture: read its descriptor and entries from OP_INFO, check them (inside the
 * BAR, below the header page, page aligned, not overlapping), and claim the BAR. The aperture of a running guest does
 * not change; a different description drops the old claim only when nothing of it is mapped (a mapping keeps its pages).
 */
static void iav_aperture_discover(void)
{
	struct amba_virt_iav_entry ent, desc;
	struct iav_aux_ent found[IAV_AUX_MAX];
	struct iav_reply rep;
	bool have = false, bad = false;
	u32 i, j, n, count = 0;

	if (!(iav_features() & AMBA_VIRT_IAV_F_APERTURE))
		return;
	if (iav_call(AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, NULL, 0, &rep, 0))
		return;
	memset(&desc, 0, sizeof(desc));
	memset(found, 0, sizeof(found));
	n = rep.r.value;
	if (rep.r.status == 0 && n && n <= AMBA_VIRT_IAV_MAX_OUT / sizeof(ent) && rep.out_len == n * sizeof(ent)) {
		for (i = 0; i < n; i++) {
			memcpy(&ent, rep.out + (size_t)i * sizeof(ent), sizeof(ent));
			if (!(ent.flags & AMBA_VIRT_IAV_ENT_APERTURE))
				continue;
			if (ent.id == AMBA_VIRT_IAV_ENT_ID_APERTURE) {
				desc = ent;
				have = true;
			} else if (ent.id >= AMBA_VIRT_IAV_ENT_ID_AUX && ent.id < AMBA_VIRT_IAV_ENT_ID_AUX + 0x100u) {
				if (count >= IAV_AUX_MAX) {
					bad = true;
					break;
				}
				found[count].id = ent.id;
				found[count].pid = ent.pid;
				found[count].base = ent.base;
				found[count].part_len = ent.part_len;
				found[count].offset = ent.offset;
				count++;
			}
		}
	}
	iav_reply_free(&rep);
	if (!have || bad || !count || !is_power_of_2(desc.size) || desc.size < 2 * PAGE_SIZE ||
	    desc.offset != desc.size - PAGE_SIZE)
		return;
	for (i = 0; i < count; i++) {
		const struct iav_aux_ent *e = &found[i];

		if (!e->part_len || (e->base & (PAGE_SIZE - 1)) || (e->part_len & (PAGE_SIZE - 1)) ||
		    (e->offset & (PAGE_SIZE - 1)) || e->offset > desc.size - PAGE_SIZE ||
		    e->part_len > desc.size - PAGE_SIZE - e->offset || e->id != AMBA_VIRT_IAV_ENT_ID_AUX + e->pid)
			return;
		for (j = 0; j < i; j++)
			if (found[j].id == e->id ||
			    (e->offset < found[j].offset + found[j].part_len && found[j].offset < e->offset + e->part_len))
				return;
	}

	mutex_lock(&g_ap_lock);
	if (g_ap.present) {
		if (g_ap.size == desc.size && g_ap.epoch == desc.reserved && g_ap.count == count &&
		    !memcmp(g_ap.ent, found, sizeof(found))) {
			mutex_unlock(&g_ap_lock);
			return;
		}
		iav_aperture_drop_locked();
	}
	if (iav_aperture_claim_locked(desc.size, desc.reserved, count) == 0) {
		g_ap.size = desc.size;
		g_ap.epoch = desc.reserved;
		g_ap.count = count;
		memcpy(g_ap.ent, found, sizeof(found));
		WRITE_ONCE(g_ap.present, true);
		dev_info(&g_ap.pdev->dev, "iav_virt: aux aperture BAR%d phys 0x%llx size 0x%llx, %u entries, epoch %u\n",
			 g_ap.bar, (unsigned long long)g_ap.phys, (unsigned long long)g_ap.size, count, g_ap.epoch);
	} else {
		pr_info("iav_virt: the broker granted an aux aperture (epoch %u), but no BAR carries its header\n",
			(u32)desc.reserved);
	}
	mutex_unlock(&g_ap_lock);
}

/* ---- the inline marshal of IMG_GET_MISC_INFO (AMBA_VIRT_IAV_F_INLINE) ---- */

struct iav_file;
static long iav_call_ioctl_simple(struct iav_file *f, unsigned int cmd, void *arg, u32 len);

/*
 * The pipeline's channel count, which bounds the entries the driver reads and writes (imgproc_statis.c): from
 * GET_SYSTEM_RESOURCE of the current mode. At most AMBA_VIRT_IAV_INLINE_MISC_CHANS.
 */
static int iav_misc_chan_count(struct iav_file *f, u32 *count)
{
	const struct iav_ent *e = iav_lookup(IAV_IOC_GET_SYSTEM_RESOURCE);
	struct iav_system_resource *res;
	long rc;

	if (!e || e->size < sizeof(*res))
		return -ENOTTY;
	res = kzalloc(e->size, GFP_KERNEL);
	if (!res)
		return -ENOMEM;
	res->encode_mode = 0xff;	/* the current mode */
	rc = iav_call_ioctl_simple(f, IAV_IOC_GET_SYSTEM_RESOURCE, res, e->size);
	if (rc >= 0)
		*count = min_t(u32, res->chan_num, AMBA_VIRT_IAV_INLINE_MISC_CHANS);
	kfree(res);
	return rc < 0 ? (int)rc : 0;
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

	/*
	 * Every new session asks again: the broker may have restarted, or been replaced by one of another revision,
	 * since the last answer, and a session opened now would not see the -ESTALE that tells an old one so. It
	 * costs one round trip per open().
	 */
	iav_negotiate(true);
	iav_arena_discover();
	iav_aperture_discover();
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
	/* as the bare-metal driver at release: this descriptor leaves every block; blocks nobody holds are freed */
	mutex_lock(&g_arena_lock);
	if (g_adev.present)
		iav_arena_release_file(&g_arena, f);
	mutex_unlock(&g_arena_lock);
	kfree(f);
	return 0;
}

/* ---- ioctl ---- */

/* One OP_IOCTL with a kernel buffer of len bytes, in and out; the session is reopened once if the broker forgot it. */
static long iav_call_ioctl_simple(struct iav_file *f, unsigned int cmd, void *arg, u32 len)
{
	long result = -EIO;
	int attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		struct iav_reply rep;
		int ret = iav_call(AMBA_VIRT_IAV_OP_IOCTL, READ_ONCE(f->handle), cmd, 0, 0, arg, len, &rep, 0);

		if (ret)
			return ret;
		if (iav_session_lost(rep.r.status) && attempt == 0) {
			iav_reply_free(&rep);
			ret = iav_open_session(f);
			if (ret)
				return ret;
			continue;
		}
		if (rep.r.status < 0)
			result = rep.r.status;
		else if (rep.r.rc < 0)
			result = rep.r.err ? -rep.r.err : -EIO;
		else if (rep.out_len != len)
			result = -EPROTO;
		else {
			memcpy(arg, rep.out, len);
			result = rep.r.rc;
		}
		iav_reply_free(&rep);
		break;
	}
	return result;
}

/*
 * IMG_GET_MISC_INFO with its three user pointers (struct img_misc_info, ambas_imgproc_arch.h). The pointers travel as
 * presence flags; the query bytes the driver will read go after the struct, and the valid flags and extra-raw entries
 * it wrote come back after it (amba_virt_iav.h, struct amba_virt_iav_inline_misc). As on bare metal, the application's
 * buffers are touched only when it asked for the extra-raw query, for the pipeline's channel count of entries.
 */
static long iav_misc_info(struct iav_file *f, unsigned long arg)
{
	struct {
		struct img_misc_info m;
		struct amba_virt_iav_inline_misc t;
	} *w;
	struct img_misc_info user;
	void __user *up = (void __user *)arg;
	u32 count = 0;
	long rc;

	BUILD_BUG_ON(sizeof(struct img_extra_raw_info) != AMBA_VIRT_IAV_INLINE_MISC_EXTRA_BYTES);
	BUILD_BUG_ON(IMG_MAX_VIN_CHANNEL_NUM != AMBA_VIRT_IAV_INLINE_MISC_CHANS);
	if (copy_from_user(&user, up, sizeof(user)))
		return -EFAULT;
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	mutex_lock(&f->lock);
	if (user.query_extra_raw) {
		rc = iav_misc_chan_count(f, &count);
		if (rc < 0)
			goto out;
		if (count && copy_from_user(w->t.query, (const void __user *)user.query_extra_raw, count)) {
			rc = -EFAULT;
			goto out;
		}
	}
	w->m = user;
	w->m.query_extra_raw = user.query_extra_raw ? (u8 *)1 : NULL;
	w->m.extra_raw_valid = user.extra_raw_valid ? (u8 *)1 : NULL;
	w->m.extra_raw = user.extra_raw ? (void *)1 : NULL;
	w->t.count = count;
	rc = iav_call_ioctl_simple(f, IAV_IOC_IMG_GET_MISC_INFO, w, sizeof(*w));
	if (rc < 0)
		goto out;
	/* the struct as the driver wrote it, with the application's own pointers */
	w->m.query_extra_raw = user.query_extra_raw;
	w->m.extra_raw_valid = user.extra_raw_valid;
	w->m.extra_raw = user.extra_raw;
	if (user.query_extra_raw && count &&
	    (copy_to_user((void __user *)user.extra_raw_valid, w->t.valid, count) ||
	     copy_to_user((void __user *)user.extra_raw, w->t.extra, (size_t)count * AMBA_VIRT_IAV_INLINE_MISC_EXTRA_BYTES))) {
		rc = -EFAULT;
		goto out;
	}
	if (copy_to_user(up, &w->m, sizeof(w->m)))
		rc = -EFAULT;
out:
	mutex_unlock(&f->lock);
	kfree(w);
	return rc;
}

static long iav_virt_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct iav_file *f = filp->private_data;
	const struct iav_ent *e = iav_lookup(cmd);
	u8 *argbuf = NULL, *msg = NULL;
	u32 op = AMBA_VIRT_IAV_OP_IOCTL;
	u32 arg_len = 0, out_len = 0, size;
	u64 arg_value = 0;
	long result;
	int attempt;

	if (!e)
		return -ENOTTY;
	if (e->kind == IAV_ARG_PTR && iav_arena_ready() && iav_arena_local(f, cmd, arg, &result))
		return result;
	if (cmd == IAV_IOC_IMG_GET_MISC_INFO && (iav_features() & AMBA_VIRT_IAV_F_INLINE))
		return iav_misc_info(f, arg);
	switch (e->kind) {
	case IAV_ARG_NONE:
		break;
	case IAV_ARG_VALUE:
		arg_value = arg;
		break;
	case IAV_ARG_PTR:
		/* The bytes the driver's handler really copies, once the broker agreed to carry them. */
		size = (e->wire && (iav_features() & AMBA_VIRT_IAV_F_SIZE_OVERRIDE)) ? e->wire : e->size;
		if (size == 0 || size > AMBA_VIRT_IAV_MAX_ARG)
			return -ENOTTY;
		argbuf = kzalloc(size, GFP_KERNEL);
		if (!argbuf)
			return -ENOMEM;
		if (e->dir & IAV_DIR_W) {
			arg_len = size;
			if (copy_from_user(argbuf, (void __user *)arg, arg_len)) {
				kfree(argbuf);
				return -EFAULT;
			}
		}
		if (e->dir & IAV_DIR_R)
			out_len = size;
		break;
	default:
		return -ENOTTY;
	}

	/* A buffer call that names blocks of the arena goes as OP_ARENA: the header places them in this guest's slice. */
	if (argbuf && arg_len && iav_arena_ready() &&
	    (cmd == IAV_IOC_SET_IMG_SCALE || cmd == IAV_IOC_GDMA_COPY || cmd == IAV_IOC_SYNC_CACHE_MEM_PART)) {
		struct amba_virt_iav_arena_req h;
		int x = iav_arena_header(cmd, argbuf, &h);

		if (x < 0) {
			kfree(argbuf);
			return x;
		}
		if (x > 0) {
			if (sizeof(h) + arg_len > AMBA_VIRT_IAV_MAX_ARG) {
				kfree(argbuf);
				return -EMSGSIZE;
			}
			msg = kmalloc(sizeof(h) + arg_len, GFP_KERNEL);
			if (!msg) {
				kfree(argbuf);
				return -ENOMEM;
			}
			memcpy(msg, &h, sizeof(h));
			memcpy(msg + sizeof(h), argbuf, arg_len);
			op = AMBA_VIRT_IAV_OP_ARENA;
		}
	}

	mutex_lock(&f->lock);
	WRITE_ONCE(f->ready, false);
	for (attempt = 0; ; attempt++) {
		struct iav_reply rep;
		/*
		 * A call the real driver may block (the decode waits take up to five seconds) must not give up
		 * before the driver does, or the guest drops the connection while the answer is still coming:
		 * such a call waits three seconds longer than the ordinary deadline.
		 */
		int ret = iav_call(op, READ_ONCE(f->handle), cmd, arg_value, 0,
				   msg ? msg : (arg_len ? argbuf : NULL), msg ? (u32)(sizeof(struct amba_virt_iav_arena_req) + arg_len) : arg_len,
				   &rep, (e->flags & IAV_F_BLOCKING) ? iav_rpc_timeout_ms_get() + 3000 : 0);

		if (ret) {
			result = ret;
			break;
		}
		if (iav_session_lost(rep.r.status) && attempt == 0) {
			/* the server restarted or forgot this descriptor: reopen (and negotiate) once and retry */
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
				/* nothing is polled any more: the worker stops after its current slice (F13) */
				atomic_set(&f->armed, 0);
				WRITE_ONCE(f->ready, false);
			}
		}
		iav_reply_free(&rep);
		break;
	}
	mutex_unlock(&f->lock);
	kfree(msg);
	kfree(argbuf);
	return result;
}

/*
 * The OP_INFO answer as legacy windows. A negotiated broker sends extended entries (only those this guest may map,
 * up to the window id it declared); an older one, or one this module did not negotiate with, the legacy entry.
 * Either way no more than IAV_MAX_WINDOWS legacy windows come out.
 */
static int iav_parse_info(const struct iav_reply *rep, struct amba_virt_iav_window *info, unsigned int *ninfo)
{
	unsigned int i, n;

	*ninfo = 0;
	/*
	 * resp.value is the number of entries, so the entry size follows from the length: an extended answer is
	 * recognized as such even if the broker changed since this module negotiated.
	 */
	if (rep->r.value && rep->r.value <= AMBA_VIRT_IAV_MAX_OUT / sizeof(struct amba_virt_iav_entry) &&
	    rep->out_len == rep->r.value * sizeof(struct amba_virt_iav_entry)) {
		const struct amba_virt_iav_entry *ent = (const struct amba_virt_iav_entry *)rep->out;

		n = rep->out_len / sizeof(*ent);
		for (i = 0; i < n; i++) {
			struct amba_virt_iav_entry e;

			memcpy(&e, &ent[i], sizeof(e));
			if (e.id >= IAV_MAX_WINDOWS || e.offset != 0)
				continue;	/* not a legacy window: nothing in this module maps it yet */
			if (*ninfo >= IAV_MAX_WINDOWS)
				return -EPROTO;
			memset(&info[*ninfo], 0, sizeof(info[0]));
			info[*ninfo].id = e.id;
			info[*ninfo].base = e.base;
			info[*ninfo].size = e.size;
			info[*ninfo].part_len = e.part_len;
			(*ninfo)++;
		}
		return 0;
	}
	n = rep->out_len / sizeof(info[0]);
	if (n > IAV_MAX_WINDOWS || rep->out_len % sizeof(info[0]))
		return -EPROTO;
	memcpy(info, rep->out, n * sizeof(info[0]));
	*ninfo = n;
	return 0;
}

/* ---- mmap ---- */

static int iav_map_chunk(struct vm_area_struct *vma, unsigned long uaddr, phys_addr_t phys,
			 u64 len, pgprot_t prot)
{
	return io_remap_pfn_range(vma, uaddr, PHYS_PFN(phys), len, prot);
}

/*
 * An entry of the aux aperture the broker authorized: mapped from the aperture BAR at the entry's offset, the entry
 * twice in a row for a ring map (as a legacy window), never past the entry.
 */
static int iav_aperture_map(struct vm_area_struct *vma, const struct amba_virt_iav_map *map, u64 addr, u64 length)
{
	const struct iav_aux_ent *e = NULL;
	u64 first, second;
	pgprot_t pg;
	u32 i;
	int ret;

	mutex_lock(&g_ap_lock);
	for (i = 0; g_ap.present && i < g_ap.count; i++)
		if (g_ap.ent[i].id == map->window)
			e = &g_ap.ent[i];
	if (!e || e->base != addr || map->window_offset != e->offset || length > 2 * e->part_len) {
		mutex_unlock(&g_ap_lock);
		return e ? -EPROTO : -ENODEV;
	}
	first = min(length, e->part_len);
	second = length - first;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
#endif
	pg = pgprot_writecombine(vma->vm_page_prot);
	ret = iav_map_chunk(vma, vma->vm_start, g_ap.phys + e->offset, first, pg);
	if (!ret && second)
		ret = iav_map_chunk(vma, vma->vm_start + first, g_ap.phys + e->offset, second, pg);
	mutex_unlock(&g_ap_lock);
	return ret;
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
	/* an arena block is mapped from this guest's own slice BAR: the guest owns it, the broker need not be asked */
	if (iav_arena_ready() && iav_arena_mmap(f, vma, offset, length, &ret))
		return ret;
	if (vma->vm_flags & VM_READ)
		prot |= 1;	/* PROT_READ */
	if (vma->vm_flags & VM_WRITE)
		prot |= 2;	/* PROT_WRITE */

	mutex_lock(&f->lock);
	ret = iav_call(AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, NULL, 0, &rep, 0);
	if (!ret) {
		ret = iav_status_error(&rep);
		if (!ret)
			ret = iav_parse_info(&rep, info, &ninfo);
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
	if (map.window >= AMBA_VIRT_IAV_ENT_ID_AUX) {
		ret = iav_aperture_map(vma, &map, offset, length);
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
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	.llseek = no_llseek,
#endif
};

/*
 * /sys/class/misc/iav/virtualized reads 1. The attribute exists only on this virtual device: on bare metal
 * the real IAV driver has none, so an application that needs to tell the two apart (to report it, not to
 * change behaviour) reads this file. The real /dev/iav ioctl structures carry no such flag.
 */
static ssize_t virtualized_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "1\n");
}
static DEVICE_ATTR_RO(virtualized);

/*
 * What this module negotiated with the broker, for reports and tests (an application never needs it): protocol is
 * the broker's revision (0: legacy or not yet asked), features the granted AMBA_VIRT_IAV_F_* bits, role the role
 * the broker held for this guest when it answered.
 */
static ssize_t protocol_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u32 rev;
	int st;

	mutex_lock(&g_neg_lock);
	rev = g_neg.revision;
	st = g_neg.state;
	mutex_unlock(&g_neg_lock);
	return sysfs_emit(buf, "%u %s\n", rev, st == IAV_NEG_DONE ? "negotiated" : st == IAV_NEG_LEGACY ? "legacy" : "unknown");
}
static DEVICE_ATTR_RO(protocol);

static ssize_t features_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u64 f = iav_features();

	return sysfs_emit(buf, "0x%llx%s%s%s%s%s%s\n", (unsigned long long)f,
			  (f & AMBA_VIRT_IAV_F_NC_WINDOWS) ? " nc_windows" : "",
			  (f & AMBA_VIRT_IAV_F_SIZE_OVERRIDE) ? " size_override" : "",
			  (f & AMBA_VIRT_IAV_F_EXT_INFO) ? " ext_info" : "",
			  (f & AMBA_VIRT_IAV_F_APERTURE) ? " aperture" : "",
			  (f & AMBA_VIRT_IAV_F_ARENA) ? " arena" : "",
			  (f & AMBA_VIRT_IAV_F_INLINE) ? " inline" : "");
}
static DEVICE_ATTR_RO(features);

/* The arena, for reports and tests: "slice=<n> base=<addr> usable=<bytes> blocks=<n> used=<bytes>", or "none". */
static ssize_t arena_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;

	mutex_lock(&g_arena_lock);
	if (!g_adev.present)
		n = sysfs_emit(buf, "none\n");
	else
		n = sysfs_emit(buf, "slice=%u base=0x%llx usable=0x%llx blocks=%u used=0x%llx%s\n", g_adev.index,
			       (unsigned long long)g_adev.base, (unsigned long long)g_adev.usable, g_arena.nblk,
			       (unsigned long long)g_arena.used, g_adev.broken ? " broken" : "");
	mutex_unlock(&g_arena_lock);
	return n;
}
static DEVICE_ATTR_RO(arena);

/* The aux aperture, for reports and tests: "size=<bytes> epoch=<n> entries=<pid>@<offset>+<len>,...", or "none". */
static ssize_t aperture_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;
	u32 i;

	mutex_lock(&g_ap_lock);
	if (!g_ap.present) {
		n = sysfs_emit(buf, "none\n");
	} else {
		n = sysfs_emit(buf, "size=0x%llx epoch=%u entries=", (unsigned long long)g_ap.size, g_ap.epoch);
		for (i = 0; i < g_ap.count; i++)
			n += sysfs_emit_at(buf, n, "%s%u@0x%llx+0x%llx", i ? "," : "", g_ap.ent[i].pid,
					   (unsigned long long)g_ap.ent[i].offset, (unsigned long long)g_ap.ent[i].part_len);
		n += sysfs_emit_at(buf, n, "\n");
	}
	mutex_unlock(&g_ap_lock);
	return n;
}
static DEVICE_ATTR_RO(aperture);

static ssize_t role_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u32 role;
	int st;

	mutex_lock(&g_neg_lock);
	role = g_neg.role;
	st = g_neg.state;
	mutex_unlock(&g_neg_lock);
	if (st != IAV_NEG_DONE)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", role == AMBA_VIRT_IAV_ROLE_PRIMARY ? "primary" :
			  role == AMBA_VIRT_IAV_ROLE_QUALIFY ? "qualification" : "consumer");
}
static DEVICE_ATTR_RO(role);

static struct attribute *iav_virt_attrs[] = {
	&dev_attr_virtualized.attr,
	&dev_attr_protocol.attr,
	&dev_attr_features.attr,
	&dev_attr_role.attr,
	&dev_attr_arena.attr,
	&dev_attr_aperture.attr,
	NULL,
};
ATTRIBUTE_GROUPS(iav_virt);

static struct miscdevice iav_virt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "iav",
	.fops = &iav_virt_fops,
	.groups = iav_virt_groups,
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
	g_table_hash = IAV_TABLE_HASH_INIT;
	for (i = 0; i < IAV_TABLE_COUNT; i++) {
		const struct iav_ent *e = &g_table[g_index[i]];

		g_table_hash = iav_table_hash_u32(g_table_hash, e->cmd);
		g_table_hash = iav_table_hash_u32(g_table_hash, e->kind);
		g_table_hash = iav_table_hash_u32(g_table_hash, e->dir);
		g_table_hash = iav_table_hash_u32(g_table_hash, e->size);
		g_table_hash = iav_table_hash_u32(g_table_hash, e->wire);
	}
	ret = misc_register(&iav_virt_misc);
	if (ret) {
		kfree(g_index);
		return ret;
	}
	pr_info("iav_virt: /dev/iav registered, %zu ioctls in the table\n",
		(size_t)IAV_TABLE_COUNT);
	{
		u32 window = 0, usable = 0;
		int qret = amba_virt_iav_slice_query(&window, &usable);

		if (qret)
			pr_info("iav_virt: private slice not leased (%d)\n", qret);
		else
			pr_info("iav_virt: private slice %u usable %u\n", window, usable);
	}
	iav_negotiate(false);
	iav_arena_discover();
	iav_aperture_discover();
	return 0;
}

static void __exit iav_virt_exit(void)
{
	misc_deregister(&iav_virt_misc);
	mutex_lock(&g_arena_lock);
	iav_arena_drop_device_locked();
	mutex_unlock(&g_arena_lock);
	mutex_lock(&g_ap_lock);
	iav_aperture_drop_locked();
	mutex_unlock(&g_ap_lock);
	iav_release_windows();
	kfree(g_index);
}

module_init(iav_virt_init);
module_exit(iav_virt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Ambarella IAV guest frontend (virtual /dev/iav over amba_virt)");
MODULE_SOFTDEP("pre: amba_virt");
