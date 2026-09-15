/*
 * test_gdma.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>

#include <amba_gdma_window.h>
#include <soc/ambarella/gdma.h>

#define TEST_SIZE		4096U
#define STRESS_LARGE_SIZE	(7U * 1024U * 1024U)

#define PITCH_1080P_WIDTH	1920U
#define PITCH_1080P_PITCH	2048U
#define PITCH_1080P_HEIGHT	1080U
#define PITCH_1080P_TOTAL	(PITCH_1080P_PITCH * PITCH_1080P_HEIGHT)

#define NUM_WORKERS		4
#define WORKER_BUF_SIZE		65536U
#define WORKER_ITERS		25

static bool run;
module_param(run, bool, 0444);
MODULE_PARM_DESC(run, "Run guest GDMA validation during module load");

static bool stress;
module_param(stress, bool, 0444);
MODULE_PARM_DESC(stress, "Run Phase 4 high-volume GDMA stress suite");

static unsigned int iterations = 100;
module_param(iterations, uint, 0444);
MODULE_PARM_DESC(iterations, "Number of endurance iterations (default: 100)");

static int test_window_copy(void)
{
	void __iomem *src;
	void __iomem *dst;
	phys_addr_t src_phys;
	phys_addr_t dst_phys;
	u8 *expected;
	u8 *actual;
	unsigned int i;
	int ret = -ENOMEM;

	src = gdma_window_alloc(&src_phys, TEST_SIZE);
	dst = gdma_window_alloc(&dst_phys, TEST_SIZE);
	expected = kmalloc(TEST_SIZE, GFP_KERNEL);
	actual = kmalloc(TEST_SIZE, GFP_KERNEL);
	if (!src || !dst || !expected || !actual)
		goto out;
	for (i = 0; i < TEST_SIZE; i++)
		expected[i] = (u8)(i * 73U + 19U);
	memcpy_toio(src, expected, TEST_SIZE);
	memset_io(dst, 0xa5, TEST_SIZE);
	mb();

	ret = dma_noncache_memcpy((u8 *)(uintptr_t)dst_phys,
				  (u8 *)(uintptr_t)src_phys, TEST_SIZE);
	if (ret) {
		pr_err("testGDMA: dma_noncache_memcpy returned %d\n", ret);
		goto out;
	}
	mb();
	memcpy_fromio(actual, dst, TEST_SIZE);
	if (memcmp(expected, actual, TEST_SIZE)) {
		pr_err("testGDMA: memcmp mismatch: exp=%02x %02x %02x %02x, act=%02x %02x %02x %02x (src_phys=0x%llx, dst_phys=0x%llx)\n",
		       expected[0], expected[1], expected[2], expected[3],
		       actual[0], actual[1], actual[2], actual[3],
		       (unsigned long long)src_phys, (unsigned long long)dst_phys);
		ret = -EIO;
	}
out:
	kfree(actual);
	kfree(expected);
	if (dst)
		gdma_window_free(dst, TEST_SIZE);
	if (src)
		gdma_window_free(src, TEST_SIZE);
	return ret;
}

static int test_system_ram_copy(void)
{
	u8 *src;
	u8 *dst;
	unsigned int i;
	int ret = -ENOMEM;

	src = (u8 *)__get_free_page(GFP_KERNEL);
	dst = (u8 *)__get_free_page(GFP_KERNEL);
	if (!src || !dst)
		goto out;
	for (i = 0; i < PAGE_SIZE; i++)
		src[i] = (u8)(i * 31U + 7U);
	memset(dst, 0xa5, PAGE_SIZE);

	ret = dma_memcpy((u8 *)(uintptr_t)virt_to_phys(dst),
			 (u8 *)(uintptr_t)virt_to_phys(src), PAGE_SIZE);
	if (!ret && memcmp(src, dst, PAGE_SIZE))
		ret = -EIO;
out:
	if (dst)
		free_page((unsigned long)dst);
	if (src)
		free_page((unsigned long)src);
	return ret;
}

static int test_pitch_copy(void)
{
	struct gdma_param param = { 0 };
	void __iomem *src;
	void __iomem *dst;
	phys_addr_t src_phys;
	phys_addr_t dst_phys;
	u8 expected[63];
	u8 actual[63];
	unsigned int row;
	unsigned int i;
	int ret = -ENOMEM;

	src = gdma_window_alloc(&src_phys, TEST_SIZE);
	dst = gdma_window_alloc(&dst_phys, TEST_SIZE);
	if (!src || !dst)
		goto out;
	for (i = 0; i < sizeof(expected); i++)
		expected[i] = (u8)(i + 0x31);
	for (row = 0; row < 16; row++) {
		memcpy_toio(src + row * 128, expected, sizeof(expected));
		memset_io(dst + row * 128, 0xa5, 128);
	}

	param.src_addr = src_phys;
	param.dest_addr = dst_phys;
	param.src_non_cached = 1;
	param.dest_non_cached = 1;
	param.src_pitch = 128;
	param.dest_pitch = 128;
	param.width = sizeof(expected);
	param.height = 16;
	ret = dma_pitch_memcpy(&param);
	if (ret)
		goto out;
	for (row = 0; row < 16; row++) {
		memcpy_fromio(actual, dst + row * 128, sizeof(actual));
		if (memcmp(expected, actual, sizeof(expected))) {
			ret = -EIO;
			break;
		}
	}
out:
	if (dst)
		gdma_window_free(dst, TEST_SIZE);
	if (src)
		gdma_window_free(src, TEST_SIZE);
	return ret;
}

static int test_rejected_copies(void)
{
	void __iomem *buf;
	phys_addr_t phys;
	int ret = -ENOMEM;

	buf = gdma_window_alloc(&phys, TEST_SIZE);
	if (!buf)
		return ret;
	if (dma_noncache_memcpy((u8 *)(uintptr_t)(phys + 2),
				(u8 *)(uintptr_t)phys, 4) >= 0) {
		pr_err("testGDMA: overlapping copy was accepted\n");
		ret = -EINVAL;
		goto out;
	}
	if (dma_noncache_memcpy(
			(u8 *)(uintptr_t)((phys_addr_t)~0ULL - 1),
			(u8 *)(uintptr_t)((phys_addr_t)~0ULL - 3), 2) >= 0) {
		pr_err("testGDMA: out-of-window copy was accepted\n");
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
out:
	gdma_window_free(buf, TEST_SIZE);
	return ret;
}

/* Phase 4 Stress 1: 7 MiB linear transfer across BAR window */
static int test_large_transfer(void)
{
	void __iomem *src;
	void __iomem *dst;
	phys_addr_t src_phys;
	phys_addr_t dst_phys;
	u64 i;
	int ret = -ENOMEM;

	pr_info("testGDMA: starting 7 MiB transfer test...\n");
	src = gdma_window_alloc(&src_phys, STRESS_LARGE_SIZE);
	dst = gdma_window_alloc(&dst_phys, STRESS_LARGE_SIZE);
	if (!src || !dst) {
		pr_err("testGDMA: failed to allocate 7 MiB buffers in window\n");
		goto out;
	}

	for (i = 0; i < (STRESS_LARGE_SIZE / sizeof(u64)); i++) {
		u64 val = i ^ 0x5a5a5a5a12345678ULL;
		writeq_relaxed(val, src + i * sizeof(u64));
		writeq_relaxed(0, dst + i * sizeof(u64));
	}
	mb();

	ret = dma_noncache_memcpy((u8 *)(uintptr_t)dst_phys,
				  (u8 *)(uintptr_t)src_phys, STRESS_LARGE_SIZE);
	if (ret) {
		pr_err("testGDMA: 7 MiB dma_noncache_memcpy failed: %d\n", ret);
		goto out;
	}
	mb();

	for (i = 0; i < (STRESS_LARGE_SIZE / sizeof(u64)); i++) {
		u64 exp = i ^ 0x5a5a5a5a12345678ULL;
		u64 act = readq_relaxed(dst + i * sizeof(u64));
		if (act != exp) {
			pr_err("testGDMA: 7 MiB mismatch at word %llu (offset 0x%llx): exp 0x%016llx, got 0x%016llx\n",
			       (unsigned long long)i,
			       (unsigned long long)(i * sizeof(u64)),
			       (unsigned long long)exp,
			       (unsigned long long)act);
			ret = -EIO;
			goto out;
		}
	}
	pr_info("testGDMA: 7 MiB transfer passed (100%% byte identity verified)\n");
	ret = 0;

out:
	if (dst)
		gdma_window_free(dst, STRESS_LARGE_SIZE);
	if (src)
		gdma_window_free(src, STRESS_LARGE_SIZE);
	return ret;
}

/* Phase 4 Stress 2: 1080p pitch copy oracle with padding integrity checks */
static int test_1080p_pitch(void)
{
	struct gdma_param param = { 0 };
	void __iomem *src;
	void __iomem *dst;
	phys_addr_t src_phys;
	phys_addr_t dst_phys;
	unsigned int y, x;
	int ret = -ENOMEM;

	pr_info("testGDMA: starting 1080p pitch copy oracle (1920x1080, pitch 2048)...\n");
	src = gdma_window_alloc(&src_phys, PITCH_1080P_TOTAL);
	dst = gdma_window_alloc(&dst_phys, PITCH_1080P_TOTAL);
	if (!src || !dst) {
		pr_err("testGDMA: failed to allocate 1080p pitch buffers\n");
		goto out;
	}

	for (y = 0; y < PITCH_1080P_HEIGHT; y++) {
		void __iomem *line = src + y * PITCH_1080P_PITCH;
		for (x = 0; x < PITCH_1080P_WIDTH; x++)
			writeb_relaxed((u8)((x ^ (y * 37U)) & 0xff), line + x);
		for (x = PITCH_1080P_WIDTH; x < PITCH_1080P_PITCH; x++)
			writeb_relaxed(0xee, line + x); /* Poison stride gap */
	}
	memset_io(dst, 0xa5, PITCH_1080P_TOTAL);
	mb();

	param.src_addr = src_phys;
	param.dest_addr = dst_phys;
	param.src_non_cached = 1;
	param.dest_non_cached = 1;
	param.src_pitch = PITCH_1080P_PITCH;
	param.dest_pitch = PITCH_1080P_PITCH;
	param.width = PITCH_1080P_WIDTH;
	param.height = PITCH_1080P_HEIGHT;

	ret = dma_pitch_memcpy(&param);
	if (ret) {
		pr_err("testGDMA: 1080p dma_pitch_memcpy returned %d\n", ret);
		goto out;
	}
	mb();

	for (y = 0; y < PITCH_1080P_HEIGHT; y++) {
		void __iomem *line = dst + y * PITCH_1080P_PITCH;
		for (x = 0; x < PITCH_1080P_WIDTH; x++) {
			u8 exp = (u8)((x ^ (y * 37U)) & 0xff);
			u8 act = readb_relaxed(line + x);
			if (act != exp) {
				pr_err("testGDMA: 1080p mismatch at y=%u, x=%u: exp 0x%02x, got 0x%02x\n",
				       y, x, exp, act);
				ret = -EIO;
				goto out;
			}
		}
		for (x = PITCH_1080P_WIDTH; x < PITCH_1080P_PITCH; x++) {
			u8 act = readb_relaxed(line + x);
			if (act != 0xa5) {
				pr_err("testGDMA: 1080p stride gap corrupt at y=%u, x=%u: exp 0xa5, got 0x%02x\n",
				       y, x, act);
				ret = -EIO;
				goto out;
			}
		}
	}
	pr_info("testGDMA: 1080p pitch copy oracle passed (2,073,600 active bytes + 138,240 stride bytes verified)\n");
	ret = 0;

out:
	if (dst)
		gdma_window_free(dst, PITCH_1080P_TOTAL);
	if (src)
		gdma_window_free(src, PITCH_1080P_TOTAL);
	return ret;
}

/* Phase 4 Stress 3: 4 concurrent worker threads */
struct worker_data {
	int id;
	int ret;
	struct completion done;
};

static int gdma_worker_thread(void *arg)
{
	struct worker_data *w = arg;
	void __iomem *src = NULL;
	void __iomem *dst = NULL;
	phys_addr_t src_phys, dst_phys;
	u32 *pat = NULL;
	unsigned int iter, i;
	int ret = 0;

	src = gdma_window_alloc(&src_phys, WORKER_BUF_SIZE);
	dst = gdma_window_alloc(&dst_phys, WORKER_BUF_SIZE);
	pat = kmalloc(WORKER_BUF_SIZE, GFP_KERNEL);
	if (!src || !dst || !pat) {
		ret = -ENOMEM;
		goto out;
	}

	for (iter = 0; iter < WORKER_ITERS; iter++) {
		u32 seed = (u32)(w->id * 1000 + iter);
		for (i = 0; i < (WORKER_BUF_SIZE / sizeof(u32)); i++)
			pat[i] = seed ^ (i * 0x01010101U);
		memcpy_toio(src, pat, WORKER_BUF_SIZE);
		memset_io(dst, 0, WORKER_BUF_SIZE);
		mb();

		ret = dma_noncache_memcpy((u8 *)(uintptr_t)dst_phys,
					  (u8 *)(uintptr_t)src_phys, WORKER_BUF_SIZE);
		if (ret) {
			pr_err("testGDMA: worker %d iter %u dma failed: %d\n", w->id, iter, ret);
			break;
		}
		mb();

		for (i = 0; i < (WORKER_BUF_SIZE / sizeof(u32)); i++) {
			u32 act = readl_relaxed(dst + i * sizeof(u32));
			if (act != pat[i]) {
				pr_err("testGDMA: worker %d iter %u mismatch at word %u: exp 0x%08x got 0x%08x\n",
				       w->id, iter, i, pat[i], act);
				ret = -EIO;
				break;
			}
		}
		if (ret)
			break;
	}

out:
	kfree(pat);
	if (dst)
		gdma_window_free(dst, WORKER_BUF_SIZE);
	if (src)
		gdma_window_free(src, WORKER_BUF_SIZE);
	w->ret = ret;
	complete(&w->done);
	return ret;
}

static int test_concurrent_workers(void)
{
	struct worker_data workers[NUM_WORKERS];
	struct task_struct *tasks[NUM_WORKERS];
	int i, ret = 0;

	pr_info("testGDMA: starting %d concurrent worker threads (%d iters x %u KB)...\n",
		NUM_WORKERS, WORKER_ITERS, WORKER_BUF_SIZE / 1024);

	for (i = 0; i < NUM_WORKERS; i++) {
		workers[i].id = i;
		workers[i].ret = 0;
		init_completion(&workers[i].done);
		tasks[i] = kthread_run(gdma_worker_thread, &workers[i], "gdma_w%d", i);
		if (IS_ERR(tasks[i])) {
			pr_err("testGDMA: failed to create kthread %d\n", i);
			workers[i].ret = PTR_ERR(tasks[i]);
			complete(&workers[i].done);
		}
	}

	for (i = 0; i < NUM_WORKERS; i++) {
		wait_for_completion(&workers[i].done);
		if (workers[i].ret) {
			pr_err("testGDMA: worker %d returned %d\n", i, workers[i].ret);
			ret = workers[i].ret;
		}
	}
	if (!ret)
		pr_info("testGDMA: %d concurrent worker threads passed (%d simultaneous transfers total)\n",
			NUM_WORKERS, NUM_WORKERS * WORKER_ITERS);
	return ret;
}

/* Phase 4 Stress 4: 100 iterations endurance loop */
static int test_endurance_loop(unsigned int count)
{
	unsigned int i;
	int ret = 0;

	pr_info("testGDMA: starting endurance stress loop (%u iterations)...\n", count);
	for (i = 1; i <= count; i++) {
		ret = test_window_copy();
		if (ret) {
			pr_err("testGDMA: endurance fail at iter %u (window copy): %d\n", i, ret);
			return ret;
		}
		ret = test_pitch_copy();
		if (ret) {
			pr_err("testGDMA: endurance fail at iter %u (pitch copy): %d\n", i, ret);
			return ret;
		}
		ret = test_system_ram_copy();
		if (ret) {
			pr_err("testGDMA: endurance fail at iter %u (system ram copy): %d\n", i, ret);
			return ret;
		}
		if (i % 25 == 0 || i == count)
			pr_info("testGDMA: endurance progress: %u/%u iterations OK\n", i, count);
	}
	pr_info("testGDMA: endurance stress loop passed (%u/%u iterations)\n", count, count);
	return 0;
}

static int run_tests(void)
{
	int ret;

	ret = test_window_copy();
	if (ret) {
		pr_err("testGDMA: guest NC copy failed: %d\n", ret);
		return ret;
	}
	ret = test_system_ram_copy();
	if (ret) {
		pr_err("testGDMA: guest System-RAM copy failed: %d\n", ret);
		return ret;
	}
	ret = test_pitch_copy();
	if (ret) {
		pr_err("testGDMA: guest pitch copy failed: %d\n", ret);
		return ret;
	}
	ret = test_rejected_copies();
	if (ret)
		return ret;
	if (dma_noncache_memcpy((u8 *)(uintptr_t)2,
				(u8 *)(uintptr_t)0, 1) >= 0) {
		pr_err("testGDMA: odd copy was accepted\n");
		return -EINVAL;
	}
	pr_info("testGDMA: guest 6/6 basic cases passed\n");

	if (stress) {
		ret = test_large_transfer();
		if (ret)
			return ret;
		ret = test_1080p_pitch();
		if (ret)
			return ret;
		ret = test_concurrent_workers();
		if (ret)
			return ret;
		ret = test_endurance_loop(iterations);
		if (ret)
			return ret;
		pr_info("testGDMA: all Phase 4 high-volume stress cases passed!\n");
	}

	return 0;
}

static int __init test_gdma_init(void)
{
	if (!run) {
		pr_info("testGDMA: loaded; reload with run=1 to execute\n");
		return 0;
	}
	return run_tests();
}

static void __exit test_gdma_exit(void)
{
}

module_init(test_gdma_init);
module_exit(test_gdma_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ambarella GDMA Linux HVM validation suite");
MODULE_AUTHOR("Ambarella International LLC");
MODULE_SOFTDEP("pre: ambarella-gdma");

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
