/*
 * amba_gdma_hvm.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include <amba_virt.h>
#include <amba_virt_kernel.h>
#include <amba_gdma_window.h>
#include <soc/ambarella/gdma.h>

#define GDMA_RPC_TIMEOUT_MS	2000
#define GDMA_PITCH_TIMEOUT_MS	10000
#define GDMA_STAGE_SIZE		(8U << 20)
#define GDMA_MAX_PITCH_WIDTH	4096U

struct amba_gdma {
	phys_addr_t window_phys;
	void __iomem *window_iomem;
	size_t window_size;
	unsigned long *window_bitmap;
	u32 *alloc_pages;
	unsigned long window_pages;
	void __iomem *stage_src;
	void __iomem *stage_dst;
	phys_addr_t stage_src_phys;
	phys_addr_t stage_dst_phys;
	struct mutex alloc_lock;
	struct mutex stage_lock;
	atomic_t sequence;
	bool online;
};

static struct amba_gdma gdma;

static int window_range(phys_addr_t phys, size_t len, u32 *offset)
{
	u64 off;

	if (!len || phys < gdma.window_phys)
		return -EINVAL;
	off = phys - gdma.window_phys;
	if (off > gdma.window_size || len > gdma.window_size - off)
		return -ERANGE;
	if (off > U32_MAX)
		return -ERANGE;
	*offset = (u32)off;
	return 0;
}

static bool overlaps_staging(u32 offset, u64 len)
{
	u64 end = (u64)offset + len;
	u64 stage_src = gdma.stage_src_phys - gdma.window_phys;
	u64 stage_dst = gdma.stage_dst_phys - gdma.window_phys;

	return ((u64)offset < stage_src + GDMA_STAGE_SIZE &&
		stage_src < end) ||
	       ((u64)offset < stage_dst + GDMA_STAGE_SIZE &&
		stage_dst < end);
}

void __iomem *gdma_window_alloc(phys_addr_t *phys, size_t len)
{
	unsigned long pages;
	unsigned long page;
	void __iomem *addr = NULL;

	if (!phys || !len)
		return NULL;
	if (len > gdma.window_size)
		return NULL;
	pages = DIV_ROUND_UP(len, PAGE_SIZE);

	mutex_lock(&gdma.alloc_lock);
	if (!gdma.online) {
		mutex_unlock(&gdma.alloc_lock);
		return NULL;
	}
	page = bitmap_find_next_zero_area(gdma.window_bitmap,
		gdma.window_pages, 0, pages, 0);
	if (page < gdma.window_pages) {
		bitmap_set(gdma.window_bitmap, page, pages);
		gdma.alloc_pages[page] = pages;
		*phys = gdma.window_phys + ((phys_addr_t)page << PAGE_SHIFT);
		addr = gdma.window_iomem + (page << PAGE_SHIFT);
	}
	mutex_unlock(&gdma.alloc_lock);
	return addr;
}
EXPORT_SYMBOL_GPL(gdma_window_alloc);

void gdma_window_free(void __iomem *addr, size_t len)
{
	unsigned long offset;
	unsigned long page;
	unsigned long pages;

	if (!addr || !len || addr < gdma.window_iomem)
		return;
	offset = addr - gdma.window_iomem;
	if (!PAGE_ALIGNED(offset) || offset >= gdma.window_size)
		return;
	if (len > gdma.window_size)
		return;
	pages = DIV_ROUND_UP(len, PAGE_SIZE);
	page = offset >> PAGE_SHIFT;
	if (pages > gdma.window_pages - page)
		return;

	mutex_lock(&gdma.alloc_lock);
	if (gdma.online && gdma.alloc_pages[page] == pages) {
		gdma.alloc_pages[page] = 0;
		bitmap_clear(gdma.window_bitmap, page, pages);
	}
	mutex_unlock(&gdma.alloc_lock);
}
EXPORT_SYMBOL_GPL(gdma_window_free);

static int gdma_rpc_copy(struct amba_virt_gdma_copy *copy)
{
	struct {
		struct amba_virt_msg msg;
		struct amba_virt_gdma_copy copy;
	} request, response;
	u32 response_len = sizeof(response);
	int ret;

	memset(&request, 0, sizeof(request));
	request.msg.type = copy->flags & AMBA_VIRT_GDMA_F_PITCH ?
		AMBA_VIRT_MSG_GDMA_PITCH_REQ : AMBA_VIRT_MSG_GDMA_COPY_REQ;
	request.msg.seq = (u32)atomic_inc_return(&gdma.sequence);
	request.copy = *copy;

	dma_wmb();
	ret = amba_virt_rpc(&request, sizeof(request), &response,
			    &response_len,
			    copy->flags & AMBA_VIRT_GDMA_F_PITCH ?
			    GDMA_PITCH_TIMEOUT_MS : GDMA_RPC_TIMEOUT_MS);
	if (ret)
		return ret;
	dma_rmb();

	if (response_len != sizeof(response) ||
	    response.msg.type !=
		(request.msg.type == AMBA_VIRT_MSG_GDMA_PITCH_REQ ?
		 AMBA_VIRT_MSG_GDMA_PITCH_RESP :
		 AMBA_VIRT_MSG_GDMA_COPY_RESP) ||
	    response.msg.seq != request.msg.seq)
		return -EPROTO;
	if (response.copy.src_off != copy->src_off ||
	    response.copy.dst_off != copy->dst_off ||
	    response.copy.len != copy->len ||
	    response.copy.flags != copy->flags)
		return -EPROTO;
	return response.copy.status;
}

int dma_noncache_memcpy(u8 *dest_addr, u8 *src_addr, u32 size)
{
	struct amba_virt_gdma_copy copy = { 0 };
	u32 dst_off;
	u32 src_off;
	u64 src_end;
	u64 dst_end;
	u32 done = 0;
	int ret;

	if (!size || size & 1)
		return -EINVAL;
	ret = window_range((phys_addr_t)(uintptr_t)src_addr, size, &src_off);
	if (ret)
		return ret;
	ret = window_range((phys_addr_t)(uintptr_t)dest_addr, size, &dst_off);
	if (ret)
		return ret;
	src_end = (u64)src_off + size;
	dst_end = (u64)dst_off + size;
	if ((u64)src_off < dst_end && (u64)dst_off < src_end)
		return -EINVAL;
	if (overlaps_staging(src_off, size) ||
	    overlaps_staging(dst_off, size))
		return -EBUSY;

	while (done < size) {
		copy.src_off = src_off + done;
		copy.dst_off = dst_off + done;
		copy.len = min_t(u32, size - done, GDMA_STAGE_SIZE);
		ret = gdma_rpc_copy(&copy);
		if (ret)
			return ret;
		done += copy.len;
	}
	return 0;
}
EXPORT_SYMBOL(dma_noncache_memcpy);

static int system_ram_range(phys_addr_t phys, size_t len)
{
	phys_addr_t end;
	unsigned long first;
	unsigned long last;
	unsigned long pfn;

	if (!len || check_add_overflow(phys, len - 1, &end))
		return -EINVAL;
	first = PHYS_PFN(phys);
	last = PHYS_PFN(end);
	for (pfn = first; ; pfn++) {
		if (!pfn_valid(pfn))
			return -EFAULT;
		if (pfn == last)
			break;
	}
	return 0;
}

int dma_memcpy(u8 *dest_addr, u8 *src_addr, u32 size)
{
	phys_addr_t dst_phys = (phys_addr_t)(uintptr_t)dest_addr;
	phys_addr_t src_phys = (phys_addr_t)(uintptr_t)src_addr;
	phys_addr_t dst_end;
	phys_addr_t src_end;
	u8 *dst;
	u8 *src;
	u32 done = 0;
	int ret;

	if (!size || size & 1)
		return -EINVAL;
	if (check_add_overflow(src_phys, (phys_addr_t)size, &src_end) ||
	    check_add_overflow(dst_phys, (phys_addr_t)size, &dst_end))
		return -ERANGE;
	if (src_phys < dst_end && dst_phys < src_end)
		return -EINVAL;
	ret = system_ram_range(src_phys, size);
	if (ret)
		return ret;
	ret = system_ram_range(dst_phys, size);
	if (ret)
		return ret;
	src = phys_to_virt(src_phys);
	dst = phys_to_virt(dst_phys);

	mutex_lock(&gdma.stage_lock);
	while (done < size) {
		struct amba_virt_gdma_copy copy = { 0 };
		u32 chunk = min_t(u32, size - done, GDMA_STAGE_SIZE);

		memcpy_toio(gdma.stage_src, src + done, chunk);
		copy.src_off = gdma.stage_src_phys - gdma.window_phys;
		copy.dst_off = gdma.stage_dst_phys - gdma.window_phys;
		copy.len = chunk;
		ret = gdma_rpc_copy(&copy);
		if (ret)
			break;
		memcpy_fromio(dst + done, gdma.stage_dst, chunk);
		done += chunk;
	}
	mutex_unlock(&gdma.stage_lock);
	return ret;
}
EXPORT_SYMBOL(dma_memcpy);

int dma_pitch_memcpy(struct gdma_param *params)
{
	struct amba_virt_gdma_copy copy = { 0 };
	u64 src_len;
	u64 dst_len;
	u32 src_off;
	u32 dst_off;
	int ret;

	if (!params || !params->height || !params->width ||
	    params->width > GDMA_MAX_PITCH_WIDTH ||
	    params->src_pitch < params->width ||
	    params->dest_pitch < params->width ||
	    ((u32)params->src_pitch * params->height) & 1 ||
	    !params->src_non_cached || !params->dest_non_cached)
		return -EINVAL;

	src_len = (u64)(params->height - 1) * params->src_pitch +
		params->width;
	dst_len = (u64)(params->height - 1) * params->dest_pitch +
		params->width;
	if (src_len > SIZE_MAX || dst_len > SIZE_MAX)
		return -ERANGE;
	ret = window_range(params->src_addr, (size_t)src_len, &src_off);
	if (ret)
		return ret;
	ret = window_range(params->dest_addr, (size_t)dst_len, &dst_off);
	if (ret)
		return ret;
	if ((u64)src_off < (u64)dst_off + dst_len &&
	    (u64)dst_off < (u64)src_off + src_len)
		return -EINVAL;
	if (overlaps_staging(src_off, src_len) ||
	    overlaps_staging(dst_off, dst_len))
		return -EBUSY;

	copy.src_off = src_off;
	copy.dst_off = dst_off;
	copy.flags = AMBA_VIRT_GDMA_F_PITCH;
	copy.src_pitch = params->src_pitch;
	copy.dst_pitch = params->dest_pitch;
	copy.width = params->width;
	copy.height = params->height;
	return gdma_rpc_copy(&copy);
}
EXPORT_SYMBOL(dma_pitch_memcpy);

static int __init amba_gdma_init(void)
{
	int ret;

	memset(&gdma, 0, sizeof(gdma));
	mutex_init(&gdma.alloc_lock);
	mutex_init(&gdma.stage_lock);
	atomic_set(&gdma.sequence, 0);

	ret = amba_virt_get_window(&gdma.window_phys, &gdma.window_iomem,
				   &gdma.window_size);
	if (ret)
		return ret;
#define AMBA_GDMA_MAX_WINDOW_SIZE  (31UL * 1024 * 1024)

	if (gdma.window_size > AMBA_GDMA_MAX_WINDOW_SIZE)
		gdma.window_size = AMBA_GDMA_MAX_WINDOW_SIZE;

	gdma.window_pages = gdma.window_size >> PAGE_SHIFT;
	gdma.window_bitmap = bitmap_zalloc(gdma.window_pages, GFP_KERNEL);
	if (!gdma.window_bitmap)
		return -ENOMEM;
	gdma.alloc_pages = kvcalloc(gdma.window_pages,
				    sizeof(*gdma.alloc_pages), GFP_KERNEL);
	if (!gdma.alloc_pages) {
		ret = -ENOMEM;
		goto err_bitmap;
	}
	gdma.online = true;

	gdma.stage_src = gdma_window_alloc(&gdma.stage_src_phys,
					   GDMA_STAGE_SIZE);
	gdma.stage_dst = gdma_window_alloc(&gdma.stage_dst_phys,
					   GDMA_STAGE_SIZE);
	if (!gdma.stage_src || !gdma.stage_dst) {
		ret = -ENOMEM;
		goto err_staging;
	}

	pr_info("ambarella-gdma: proxy ready, window %pa size %zu\n",
		&gdma.window_phys, gdma.window_size);
	return 0;

err_staging:
	if (gdma.stage_dst)
		gdma_window_free(gdma.stage_dst, GDMA_STAGE_SIZE);
	if (gdma.stage_src)
		gdma_window_free(gdma.stage_src, GDMA_STAGE_SIZE);
	gdma.online = false;
	kvfree(gdma.alloc_pages);
	gdma.alloc_pages = NULL;
err_bitmap:
	bitmap_free(gdma.window_bitmap);
	gdma.window_bitmap = NULL;
	return ret;
}

static void __exit amba_gdma_exit(void)
{
	gdma_window_free(gdma.stage_dst, GDMA_STAGE_SIZE);
	gdma_window_free(gdma.stage_src, GDMA_STAGE_SIZE);
	mutex_lock(&gdma.alloc_lock);
	gdma.online = false;
	mutex_unlock(&gdma.alloc_lock);
	kvfree(gdma.alloc_pages);
	gdma.alloc_pages = NULL;
	bitmap_free(gdma.window_bitmap);
	gdma.window_bitmap = NULL;
}

module_init(amba_gdma_init);
module_exit(amba_gdma_exit);

MODULE_SOFTDEP("pre: amba_virt");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ambarella GDMA proxy for Linux HVM guests");
MODULE_AUTHOR("Ambarella International LLC");
