/*
 * amba_cavalry_hvm.c
 *
 * Ambarella Cavalry Linux HVM Guest Frontend Driver
 * Exposes /dev/cavalry (mode 0600) to unmodified user runtimes (nnctrl),
 * translating operations into VSOCK RPCs over amba_virt.
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
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/io.h>

#include <amba_virt.h>
#include "amba_virt_kernel.h"
#include <cavalry_ioctl.h>
#include "cavalry_ioctl_path_b.h"

#define CAVALRY_POOL_BASE        0x02000000U    /* 32 MiB */
#define CAVALRY_POOL_SIZE        0x3E000000U    /* 992 MiB (extends to 1 GiB) */
#define CAVALRY_RPC_ARENA_OFFSET 0x01F00000U    /* 31 MiB (1 MiB control arena) */
#define CAVALRY_RPC_ARENA_SIZE   0x00100000U    /* 1 MiB */

static unsigned int cavalry_rpc_timeout_ms = 5000;

static unsigned int g_pool_size = CAVALRY_POOL_SIZE;
module_param_named(pool_size, g_pool_size, uint, 0444);
MODULE_PARM_DESC(pool_size, "Cavalry memory pool size in bytes (default 992 MiB)");

static unsigned int g_rpc_arena_size = CAVALRY_RPC_ARENA_SIZE;
module_param_named(rpc_arena_size, g_rpc_arena_size, uint, 0444);
MODULE_PARM_DESC(rpc_arena_size, "Cavalry RPC arena size in bytes (default 1 MiB)");

static unsigned int g_preferred_offset = AMBA_VIRT_OFFSET_AUTO;
module_param_named(preferred_offset, g_preferred_offset, uint, 0444);
MODULE_PARM_DESC(preferred_offset, "Preferred BAR base offset, or 0xFFFFFFFF for AUTO");

static int g_force_replace = 0;
module_param_named(force_replace, g_force_replace, int, 0444);
MODULE_PARM_DESC(force_replace, "Force replacement of existing Cavalry registration (default 0)");

struct amba_cavalry_dev {
	struct miscdevice misc;
	struct mutex arena_mutex;
	phys_addr_t bar_phys;
	void __iomem *bar_iomem;
	size_t bar_size;
	atomic_t sequence;
	u32 pool_base;
	u32 pool_size;
	u32 rpc_arena_offset;
	u32 rpc_arena_size;
	bool online;
};

static struct amba_cavalry_dev g_cav;

static int amba_cavalry_negotiate_bounds(struct amba_cavalry_dev *cav)
{
	struct {
		struct amba_virt_msg msg;
		struct amba_virt_dev_bounds_req req;
	} req_pkt;
	struct {
		struct amba_virt_msg msg;
		struct amba_virt_dev_bounds_resp resp;
	} resp_pkt;
	u32 resp_len = sizeof(resp_pkt);
	int ret;
	int retries = 1;

	memset(&req_pkt, 0, sizeof(req_pkt));
	req_pkt.msg.type = AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ;
	req_pkt.msg.seq = (u32)atomic_inc_return(&cav->sequence);
	req_pkt.req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req_pkt.req.requested_size = g_pool_size;
	req_pkt.req.preferred_offset = g_preferred_offset;
	req_pkt.req.rpc_arena_size = g_rpc_arena_size;
	req_pkt.req.flags = AMBA_VIRT_DEV_F_EXACT;
	if (g_force_replace)
		req_pkt.req.flags |= AMBA_VIRT_DEV_F_REPLACE;

retry:
	dma_wmb();
	ret = amba_virt_rpc(&req_pkt, sizeof(req_pkt), &resp_pkt, &resp_len, cavalry_rpc_timeout_ms);
	if (ret) {
		pr_err("amba_cavalry: vsock RPC transport failure or timeout (%d)\n", ret);
		return ret;
	}
	dma_rmb();

	if (resp_len != sizeof(resp_pkt) ||
	    resp_pkt.msg.type != AMBA_VIRT_MSG_DEV_SET_BOUNDS_RESP)
		return -EPROTO;

	if (resp_pkt.resp.status == 0) {
		cav->pool_base = resp_pkt.resp.granted_offset;
		cav->pool_size = resp_pkt.resp.granted_size;
		cav->rpc_arena_offset = resp_pkt.resp.rpc_arena_offset;
		cav->rpc_arena_size = g_rpc_arena_size;
		pr_info("amba_cavalry: registered at BAR offset 0x%08x (%u MB), arena at 0x%08x\n",
			cav->pool_base, cav->pool_size / (1024 * 1024), cav->rpc_arena_offset);
		return 0;
	}

	switch (resp_pkt.resp.status) {
	case -EEXIST:
		pr_warn("amba_cavalry: offset 0x%08x collides with active dev %u\n",
			req_pkt.req.preferred_offset, resp_pkt.resp.colliding_dev);
		if (retries > 0 && req_pkt.req.preferred_offset != AMBA_VIRT_OFFSET_AUTO &&
		    resp_pkt.resp.suggested_offset != 0) {
			pr_info("amba_cavalry: auto-retrying at suggested offset 0x%08x\n",
				resp_pkt.resp.suggested_offset);
			req_pkt.req.preferred_offset = resp_pkt.resp.suggested_offset;
			retries--;
			goto retry;
		}
		break;
	case -ENOMEM:
		pr_err("amba_cavalry: requested %u MB exceeds available quota (%u MB available)\n",
		       req_pkt.req.requested_size / (1024 * 1024),
		       resp_pkt.resp.max_avail_size / (1024 * 1024));
		break;
	case -EPERM:
		pr_err("amba_cavalry: host ACL denied registration (err_code=%u). Check tenant permissions\n",
		       resp_pkt.resp.err_code);
		break;
	case -EBUSY:
		pr_err("amba_cavalry: Cavalry device already registered for this VM. Pass 'force_replace=1' to overwrite\n");
		break;
	case -ERANGE:
		pr_err("amba_cavalry: requested range exceeds hypervisor BAR window (err_code=%u)\n",
		       resp_pkt.resp.err_code);
		break;
	default:
		pr_err("amba_cavalry: boundary negotiation failed (status=%d, err_code=%u)\n",
		       resp_pkt.resp.status, resp_pkt.resp.err_code);
		break;
	}

	return resp_pkt.resp.status;
}

static void amba_cavalry_release_bounds(struct amba_cavalry_dev *cav)
{
	struct {
		struct amba_virt_msg msg;
		struct amba_virt_dev_bounds_req req;
	} req_pkt;
	struct {
		struct amba_virt_msg msg;
		struct amba_virt_dev_bounds_resp resp;
	} resp_pkt;
	u32 resp_len = sizeof(resp_pkt);

	memset(&req_pkt, 0, sizeof(req_pkt));
	req_pkt.msg.type = AMBA_VIRT_MSG_DEV_RELEASE_BOUNDS_REQ;
	req_pkt.msg.seq = (u32)atomic_inc_return(&cav->sequence);
	req_pkt.req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;

	dma_wmb();
	amba_virt_rpc(&req_pkt, sizeof(req_pkt), &resp_pkt, &resp_len, cavalry_rpc_timeout_ms);
	dma_rmb();
}

static int amba_cavalry_send_rpc(struct amba_virt_cavalry_rpc *rpc)
{
	struct {
		struct amba_virt_msg msg;
		struct amba_virt_cavalry_rpc rpc;
	} req, resp;
	u32 resp_len = sizeof(resp);
	int ret;

	if (!g_cav.online)
		return -ENODEV;

	memset(&req, 0, sizeof(req));
	req.msg.type = AMBA_VIRT_MSG_CAVALRY_REQ;
	req.msg.seq = (u32)atomic_inc_return(&g_cav.sequence);
	req.rpc = *rpc;

	dma_wmb();
	ret = amba_virt_rpc(&req, sizeof(req), &resp, &resp_len, cavalry_rpc_timeout_ms);
	if (ret)
		return ret;
	dma_rmb();

	if (resp_len != sizeof(resp) ||
	    resp.msg.type != AMBA_VIRT_MSG_CAVALRY_RESP ||
	    resp.msg.seq != req.msg.seq)
		return -EPROTO;

	*rpc = resp.rpc;
	return rpc->status;
}

struct cavalry_session {
	u32 session_id;
};

static atomic_t g_cavalry_session_seq = ATOMIC_INIT(0);

static int amba_cavalry_open(struct inode *inode, struct file *filp)
{
	struct cavalry_session *sess;

	if (!g_cav.online)
		return -ENODEV;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess)
		return -ENOMEM;

	sess->session_id = (u32)atomic_inc_return(&g_cavalry_session_seq);
	filp->private_data = sess;
	pr_info("amba_cavalry: open filp=%p sess=%u\n", filp, sess->session_id);
	return 0;
}

struct cavalry_close_work {
	struct work_struct work;
	u32 session_id;
};

static void cavalry_close_session_work(struct work_struct *work)
{
	struct cavalry_close_work *cw = container_of(work, struct cavalry_close_work, work);
	struct amba_virt_cavalry_rpc rpc;
	int ret;

	if (!g_cav.online) {
		kfree(cw);
		return;
	}

	memset(&rpc, 0, sizeof(rpc));
	rpc.opcode = VCAV_OP_CLOSE_SESSION;
	rpc.session_id = cw->session_id;
	pr_info("amba_cavalry: kworker closing session %u over vsock\n", cw->session_id);
	ret = amba_cavalry_send_rpc(&rpc);
	pr_info("amba_cavalry: kworker close session %u returned %d\n", cw->session_id, ret);
	kfree(cw);
}

static int amba_cavalry_release(struct inode *inode, struct file *filp)
{
	struct cavalry_session *sess = filp->private_data;

	pr_info("amba_cavalry: release called filp=%p sess=%p\n", filp, sess);
	if (sess) {
		if (sess->session_id && g_cav.online) {
			struct cavalry_close_work *cw = kzalloc(sizeof(*cw), GFP_ATOMIC);
			if (cw) {
				cw->session_id = sess->session_id;
				INIT_WORK(&cw->work, cavalry_close_session_work);
				schedule_work(&cw->work);
			}
		}
		kfree(sess);
		filp->private_data = NULL;
	}
	return 0;
}

static int amba_cavalry_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long offset;
	unsigned long len;

	if (!g_cav.online || !g_cav.bar_phys || !g_cav.bar_size)
		return -ENODEV;

	u32 pool_base = g_cav.pool_base ? g_cav.pool_base : CAVALRY_POOL_BASE;
	u32 pool_size = g_cav.pool_size ? g_cav.pool_size : CAVALRY_POOL_SIZE;

	offset = vma->vm_pgoff << PAGE_SHIFT;
	len = vma->vm_end - vma->vm_start;

	/* Strictly reject any offset below pool base */
	if (offset < pool_base) {
		pr_warn_ratelimited("amba_cavalry: rejected mmap at offset 0x%lx (protected low window)\n", offset);
		return -EINVAL;
	}

	if (offset + len > (unsigned long)pool_base + pool_size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);

	return remap_pfn_range(vma, vma->vm_start,
			       PFN_DOWN(g_cav.bar_phys) + vma->vm_pgoff,
			       len, vma->vm_page_prot);
}

static long amba_cavalry_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct cavalry_session *sess = filp->private_data;
	u32 session_id = sess ? sess->session_id : 0;
	int ret = 0;

	if (!g_cav.online)
		return -ENODEV;

	switch (cmd) {
	case CAVALRY_GET_DRIVER_VERSION: {
		struct amba_virt_cavalry_rpc rpc = {
			.opcode = VCAV_OP_GET_VERSION,
			.session_id = session_id,
		};
		struct cavalry_driver_version ver;
		ret = amba_cavalry_send_rpc(&rpc);
		if (ret)
			return ret;
		memset(&ver, 0, sizeof(ver));
		ver.major = 3;
		ver.minor = 0;
		if (copy_to_user((void __user *)arg, &ver, sizeof(ver)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_GET_CV_CHIP_ID: {
		struct amba_virt_cavalry_rpc rpc = {
			.opcode = VCAV_OP_GET_CHIP_ID,
			.session_id = session_id,
		};
		ret = amba_cavalry_send_rpc(&rpc);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &rpc.chip_id, sizeof(rpc.chip_id)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_GET_CAVALRY_STATUS: {
		struct amba_virt_cavalry_rpc rpc = {
			.opcode = VCAV_OP_GET_STATUS,
			.session_id = session_id,
		};
		struct cavalry_status status = { 0 };
		ret = amba_cavalry_send_rpc(&rpc);
		pr_info("amba_cavalry: GET_STATUS rpc ret=%d, status=%d, rval=%u\n",
			ret, rpc.status, rpc.rval);
		if (ret < 0)
			return ret;
		status.is_cavalry_started = rpc.rval ? 1 : 0;
		if (copy_to_user((void __user *)arg, &status, sizeof(status)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_QUERY_BUF: {
		struct cavalry_querybuf q;
		if (copy_from_user(&q, (void __user *)arg, sizeof(q)))
			return -EFAULT;
		if (q.buf == CAVALRY_MEM_USER) {
			q.offset = g_cav.pool_base ? g_cav.pool_base : CAVALRY_POOL_BASE;
			q.length = g_cav.pool_size ? g_cav.pool_size : CAVALRY_POOL_SIZE;
		} else {
			q.offset = 0;
			q.length = 0;
		}
		if (copy_to_user((void __user *)arg, &q, sizeof(q)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_QUERY_UCODE_CMD_SIZE: {
		struct cavalry_ucode_cmd_size ucmd;
		struct amba_virt_cavalry_rpc rpc;
		if (copy_from_user(&ucmd, (void __user *)arg, sizeof(ucmd)))
			return -EFAULT;
		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_QUERY_UCODE_CMD_SIZE;
		rpc.session_id = session_id;
		rpc.bar_offset = ucmd.cmd_id;
		rpc.size = ucmd.dag_cnt;
		ret = amba_cavalry_send_rpc(&rpc);
		if (ret < 0)
			return ret;
		ucmd.cmd_size = rpc.rval;
		if (copy_to_user((void __user *)arg, &ucmd, sizeof(ucmd)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_ALLOC_MEM: {
		struct cavalry_mem alloc;
		struct amba_virt_cavalry_rpc rpc;
		if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc)))
			return -EFAULT;
		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_ALLOC_MEM;
		rpc.session_id = session_id;
		rpc.size = alloc.length;
		ret = amba_cavalry_send_rpc(&rpc);
		if (ret)
			return ret;
		alloc.offset = rpc.bar_offset;
		if (copy_to_user((void __user *)arg, &alloc, sizeof(alloc)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_FREE_MEM: {
		struct cavalry_mem f;
		struct amba_virt_cavalry_rpc rpc;
		if (copy_from_user(&f, (void __user *)arg, sizeof(f)))
			return -EFAULT;
		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_FREE_MEM;
		rpc.session_id = session_id;
		rpc.bar_offset = f.offset;
		return amba_cavalry_send_rpc(&rpc);
	}

	case CAVALRY_SYNC_CACHE_MEM:
		/* Normal-NC / WC memory; flush CPU store buffers to point of coherency */
#ifdef CONFIG_ARM64
		asm volatile("dsb sy" ::: "memory");
#endif
		return 0;

	case CAVALRY_START_VP: {
		struct amba_virt_cavalry_rpc rpc = { .opcode = VCAV_OP_START_VP };
		return amba_cavalry_send_rpc(&rpc);
	}

	case CAVALRY_STOP_VP:
		/* Host-owned shared engine; guest stop is ignored */
		return 0;

	case CAVALRY_GET_AUDIO_CLK: {
		uint64_t clk = 24000000ULL;
		if (copy_to_user((void __user *)arg, &clk, sizeof(clk)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_GET_DRAM_CFG: {
		struct cavalry_dram_cfg cfg = { .burst_size = 64 };
		if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_QUERY_MEM_ATTR: {
		struct cavalry_mem_attr attr = { .cache_en = 0 };
		if (copy_to_user((void __user *)arg, &attr, sizeof(attr)))
			return -EFAULT;
		return 0;
	}

	case CAVALRY_RUN_DAGS: {
		struct cavalry_run_dags run_hdr;
		struct amba_virt_cavalry_rpc rpc;
		size_t req_size;
		void *kbuf;

		if (copy_from_user(&run_hdr, (void __user *)arg, sizeof(run_hdr)))
			return -EFAULT;

		u32 arena_off = g_cav.rpc_arena_offset ? g_cav.rpc_arena_offset : CAVALRY_RPC_ARENA_OFFSET;
		u32 arena_sz = g_cav.rpc_arena_size ? g_cav.rpc_arena_size : CAVALRY_RPC_ARENA_SIZE;

		if (run_hdr.dag_cnt == 0 || run_hdr.dag_cnt > 128)
			return -EINVAL;

		req_size = sizeof(struct cavalry_run_dags) +
			run_hdr.dag_cnt * sizeof(struct cavalry_dag_desc);
		if (req_size > arena_sz)
			return -EMSGSIZE;

		kbuf = vmalloc(req_size);
		if (!kbuf)
			return -ENOMEM;

		if (copy_from_user(kbuf, (void __user *)arg, req_size)) {
			vfree(kbuf);
			return -EFAULT;
		}

		/* Lock arena mutex across write -> RPC -> read results */
		mutex_lock(&g_cav.arena_mutex);

		memcpy_toio(g_cav.bar_iomem + arena_off, kbuf, req_size);
#ifdef CONFIG_ARM64
		asm volatile("dsb st" ::: "memory");
#endif

		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_RUN_DAGS;
		rpc.session_id = session_id;
		rpc.bar_offset = arena_off;
		rpc.arena_len = req_size;

		ret = amba_cavalry_send_rpc(&rpc);
		if (ret == 0) {
			run_hdr.rval = rpc.rval;
			run_hdr.exec_total_ticks = rpc.exec_ticks;
			run_hdr.finish_dags = run_hdr.dag_cnt;
			if (copy_to_user((void __user *)arg, &run_hdr, sizeof(run_hdr)))
				ret = -EFAULT;
		}

		mutex_unlock(&g_cav.arena_mutex);
		vfree(kbuf);
		return ret;
	}

	case CAVALRY_DMA_COPY:
		/* Unconditionally denied for security */
		pr_warn_ratelimited("amba_cavalry: CAVALRY_DMA_COPY (0x28) is denied\n");
		return -EPERM;

	case CAVALRY_IOC_REGISTER_DAG: {
		struct cavalry_reg_dag_user reg_u;
		struct amba_virt_cavalry_reg_dag_desc rdesc;
		struct amba_virt_cavalry_rpc rpc;
		void *kbuf = NULL;
		size_t total_payload;

		if (copy_from_user(&reg_u, (void __user *)arg, sizeof(reg_u)))
			return -EFAULT;

		u32 arena_off = g_cav.rpc_arena_offset ? g_cav.rpc_arena_offset : CAVALRY_RPC_ARENA_OFFSET;
		u32 arena_sz = g_cav.rpc_arena_size ? g_cav.rpc_arena_size : CAVALRY_RPC_ARENA_SIZE;

		if (reg_u.run_dags_bytes == 0 ||
		    reg_u.run_dags_bytes > (arena_sz - sizeof(rdesc)))
			return -EINVAL;

		kbuf = vmalloc(reg_u.run_dags_bytes);
		if (!kbuf)
			return -ENOMEM;

		if (copy_from_user(kbuf, (void __user *)(uintptr_t)reg_u.run_dags_ptr, reg_u.run_dags_bytes)) {
			vfree(kbuf);
			return -EFAULT;
		}

		memset(&rdesc, 0, sizeof(rdesc));
		rdesc.staging_bar_offset = reg_u.staging_bar_offset;
		rdesc.staging_size = reg_u.staging_size;
		rdesc.dvi_offset_in_slice = reg_u.dvi_offset_in_slice;
		rdesc.extra_dag_list_offset_in_slice = reg_u.extra_dag_list_offset_in_slice;
		rdesc.extra_dag_common_offset_in_slice = reg_u.extra_dag_common_offset_in_slice;
		rdesc.extra_poke_list_offset_in_slice = reg_u.extra_poke_list_offset_in_slice;
		memcpy(rdesc.sha256, reg_u.sha256, sizeof(rdesc.sha256));
		rdesc.run_dags_bytes = reg_u.run_dags_bytes;

		total_payload = sizeof(rdesc) + reg_u.run_dags_bytes;

		/* Lock arena mutex across write -> RPC -> read results */
		mutex_lock(&g_cav.arena_mutex);

		memcpy_toio(g_cav.bar_iomem + arena_off, &rdesc, sizeof(rdesc));
		memcpy_toio(g_cav.bar_iomem + arena_off + sizeof(rdesc), kbuf, reg_u.run_dags_bytes);
#ifdef CONFIG_ARM64
		asm volatile("dsb st" ::: "memory");
#endif

		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_REGISTER_DAG;
		rpc.session_id = session_id;
		rpc.bar_offset = arena_off;
		rpc.arena_len = total_payload;

		ret = amba_cavalry_send_rpc(&rpc);
		if (ret == 0) {
			reg_u.dag_id = rpc.dag_id;
			if (copy_to_user((void __user *)arg, &reg_u, sizeof(reg_u)))
				ret = -EFAULT;
		}

		mutex_unlock(&g_cav.arena_mutex);
		vfree(kbuf);
		return ret;
	}

	case CAVALRY_IOC_UNREGISTER_DAG: {
		struct cavalry_unreg_dag_user unreg;
		struct amba_virt_cavalry_rpc rpc;

		if (copy_from_user(&unreg, (void __user *)arg, sizeof(unreg)))
			return -EFAULT;

		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_UNREGISTER_DAG;
		rpc.session_id = session_id;
		rpc.dag_id = unreg.dag_id;

		return amba_cavalry_send_rpc(&rpc);
	}

	case CAVALRY_IOC_ALLOC_HANDLE: {
		struct cavalry_alloc_handle_user alloc_h;
		struct amba_virt_cavalry_rpc rpc;

		if (copy_from_user(&alloc_h, (void __user *)arg, sizeof(alloc_h)))
			return -EFAULT;

		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_ALLOC_HANDLE;
		rpc.session_id = session_id;
		rpc.size = alloc_h.size;

		ret = amba_cavalry_send_rpc(&rpc);
		if (ret == 0) {
			alloc_h.handle_id = rpc.dag_id;
			alloc_h.bar_offset = rpc.bar_offset;
			if (copy_to_user((void __user *)arg, &alloc_h, sizeof(alloc_h)))
				ret = -EFAULT;
		}
		return ret;
	}

	case CAVALRY_IOC_FREE_HANDLE: {
		struct cavalry_free_handle_user free_h;
		struct amba_virt_cavalry_rpc rpc;

		if (copy_from_user(&free_h, (void __user *)arg, sizeof(free_h)))
			return -EFAULT;

		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_FREE_HANDLE;
		rpc.session_id = session_id;
		rpc.dag_id = free_h.handle_id;

		return amba_cavalry_send_rpc(&rpc);
	}

	case CAVALRY_IOC_RUN_REGISTERED_DAG: {
		struct cavalry_run_reg_user run_u;
		struct amba_virt_cavalry_run_reg_desc rrun;
		struct amba_virt_cavalry_rpc rpc;

		if (copy_from_user(&run_u, (void __user *)arg, sizeof(run_u)))
			return -EFAULT;

		if (run_u.port_cnt > CAVALRY_MAX_PORTS)
			return -EINVAL;

		memset(&rrun, 0, sizeof(rrun));
		rrun.dag_id = run_u.dag_id;
		rrun.port_cnt = run_u.port_cnt;
		memcpy(rrun.ports, run_u.ports, sizeof(run_u.ports));

		u32 arena_off = g_cav.rpc_arena_offset ? g_cav.rpc_arena_offset : CAVALRY_RPC_ARENA_OFFSET;

		/* Lock arena mutex across write -> RPC -> read results */
		mutex_lock(&g_cav.arena_mutex);

		memcpy_toio(g_cav.bar_iomem + arena_off, &rrun, sizeof(rrun));
#ifdef CONFIG_ARM64
		asm volatile("dsb st" ::: "memory");
#endif

		memset(&rpc, 0, sizeof(rpc));
		rpc.opcode = VCAV_OP_RUN_REGISTERED_DAG;
		rpc.session_id = session_id;
		rpc.dag_id = run_u.dag_id;
		rpc.bar_offset = arena_off;
		rpc.arena_len = sizeof(rrun);

		ret = amba_cavalry_send_rpc(&rpc);
		if (ret == 0) {
			run_u.rval = rpc.rval;
			run_u.exec_ticks = rpc.exec_ticks;
			if (copy_to_user((void __user *)arg, &run_u, sizeof(run_u)))
				ret = -EFAULT;
		}

		mutex_unlock(&g_cav.arena_mutex);
		return ret;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations amba_cavalry_fops = {
	.owner = THIS_MODULE,
	.open = amba_cavalry_open,
	.release = amba_cavalry_release,
	.mmap = amba_cavalry_mmap,
	.unlocked_ioctl = amba_cavalry_ioctl,
	.compat_ioctl = amba_cavalry_ioctl,
	.llseek = no_llseek,
};

static int __init amba_cavalry_init(void)
{
	int ret;

	memset(&g_cav, 0, sizeof(g_cav));
	mutex_init(&g_cav.arena_mutex);
	atomic_set(&g_cav.sequence, 0);

	ret = amba_virt_get_window(&g_cav.bar_phys, &g_cav.bar_iomem, &g_cav.bar_size);
	if (ret) {
		pr_err("amba_cavalry: amba_virt window not available (%d)\n", ret);
		return ret;
	}

	ret = amba_cavalry_negotiate_bounds(&g_cav);
	if (ret) {
		pr_err("amba_cavalry: boundary negotiation failed (%d)\n", ret);
		return ret;
	}

	g_cav.misc.minor = MISC_DYNAMIC_MINOR;
	g_cav.misc.name = "cavalry";
	g_cav.misc.fops = &amba_cavalry_fops;
	g_cav.misc.mode = 0600;

	ret = misc_register(&g_cav.misc);
	if (ret) {
		amba_cavalry_release_bounds(&g_cav);
		pr_err("amba_cavalry: failed to register misc device /dev/cavalry (%d)\n", ret);
		return ret;
	}

	g_cav.online = true;
	pr_info("amba_cavalry: guest frontend registered /dev/cavalry (bar %pa, size %zu)\n",
		&g_cav.bar_phys, g_cav.bar_size);
	return 0;
}

static void __exit amba_cavalry_exit(void)
{
	g_cav.online = false;
	misc_deregister(&g_cav.misc);
	amba_cavalry_release_bounds(&g_cav);
	pr_info("amba_cavalry: unregistered\n");
}

module_init(amba_cavalry_init);
module_exit(amba_cavalry_exit);

MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Ambarella Cavalry Linux HVM Guest Frontend Driver");
MODULE_LICENSE("GPL");

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
