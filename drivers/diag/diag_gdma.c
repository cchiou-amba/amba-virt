/*
 * diag_gdma.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>

#include <soc/ambarella/gdma.h>
extern int cavalry_user_window_get(phys_addr_t *phys, size_t *size);

#define TEST_REGION_SIZE	(2U << 20)
#define TEST_SRC_OFFSET		0
#define TEST_DST_OFFSET		(1U << 20)

static bool run;
module_param(run, bool, 0444);
MODULE_PARM_DESC(run, "Run the GDMA validation cases during module load");

static int test_linear(void __iomem *window, phys_addr_t phys, u32 size,
		       bool reverse)
{
	u32 src_offset = reverse ? TEST_DST_OFFSET : TEST_SRC_OFFSET;
	u32 dst_offset = reverse ? TEST_SRC_OFFSET : TEST_DST_OFFSET;
	u8 *expected;
	u8 *actual;
	u32 i;
	int ret = -ENOMEM;

	expected = kmalloc(size, GFP_KERNEL);
	actual = kmalloc(size, GFP_KERNEL);
	if (!expected || !actual)
		goto out;
	for (i = 0; i < size; i++)
		expected[i] = (u8)(i * 131U + size);

	memcpy_toio(window + src_offset, expected, size);
	memset_io(window + dst_offset, 0xa5, size);
	ret = dma_noncache_memcpy(
		(u8 *)(uintptr_t)(phys + dst_offset),
		(u8 *)(uintptr_t)(phys + src_offset), size);
	if (ret)
		goto out;
	memcpy_fromio(actual, window + dst_offset, size);
	if (memcmp(expected, actual, size))
		ret = -EIO;
out:
	kfree(actual);
	kfree(expected);
	return ret;
}

static int test_pitch(void __iomem *window, phys_addr_t phys)
{
	struct gdma_param param = { 0 };
	u8 expected[64];
	u8 actual[64];
	unsigned int row;
	unsigned int i;
	int ret;

	for (i = 0; i < sizeof(expected); i++)
		expected[i] = (u8)(0x40 + i);
	for (row = 0; row < 16; row++) {
		memcpy_toio(window + TEST_SRC_OFFSET + row * 128,
			    expected, sizeof(expected));
		memset_io(window + TEST_DST_OFFSET + row * 128, 0xa5, 128);
	}

	param.src_addr = phys + TEST_SRC_OFFSET;
	param.dest_addr = phys + TEST_DST_OFFSET;
	param.src_non_cached = 1;
	param.dest_non_cached = 1;
	param.src_pitch = 128;
	param.dest_pitch = 128;
	param.width = sizeof(expected);
	param.height = 16;
	ret = dma_pitch_memcpy(&param);
	if (ret)
		return ret;

	for (row = 0; row < 16; row++) {
		memcpy_fromio(actual, window + TEST_DST_OFFSET + row * 128,
			      sizeof(actual));
		if (memcmp(expected, actual, sizeof(expected)))
			return -EIO;
	}
	return 0;
}

static int run_tests(void)
{
	static const u32 sizes[] = { 2, 64, 4094, 4096, 4098, 1U << 20 };
	void __iomem *window;
	void __iomem *edge_window;
	phys_addr_t phys;
	phys_addr_t edge_phys;
	size_t size;
	unsigned int i;
	unsigned int passed = 0;
	int ret;

	ret = cavalry_user_window_get(&phys, &size);
	if (ret)
		return ret;
	if (size < TEST_REGION_SIZE)
		return -ENOSPC;
	window = ioremap_wc(phys, TEST_REGION_SIZE);
	if (!window)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		ret = test_linear(window, phys, sizes[i], false);
		if (ret) {
			pr_err("diag_gdma: linear size %u failed: %d\n",
			       sizes[i], ret);
			goto out;
		}
		passed++;
	}
	ret = test_linear(window, phys, 4096, true);
	if (ret) {
		pr_err("diag_gdma: reverse copy failed: %d\n", ret);
		goto out;
	}
	passed++;

	edge_phys = phys + size - TEST_REGION_SIZE;
	edge_window = ioremap_wc(edge_phys, TEST_REGION_SIZE);
	if (!edge_window) {
		ret = -ENOMEM;
		goto out;
	}
	ret = test_linear(edge_window, edge_phys, 4096, false);
	iounmap(edge_window);
	if (ret) {
		pr_err("diag_gdma: end-of-window copy failed: %d\n", ret);
		goto out;
	}
	passed++;

	ret = test_pitch(window, phys);
	if (ret) {
		pr_err("diag_gdma: pitch failed: %d\n", ret);
		goto out;
	}
	passed++;

	if (dma_noncache_memcpy((u8 *)(uintptr_t)(phys + TEST_DST_OFFSET),
				(u8 *)(uintptr_t)phys, 1) >= 0) {
		ret = -EINVAL;
		pr_err("diag_gdma: odd size was accepted\n");
		goto out;
	}
	passed++;
	pr_info("diag_gdma: %u/%u cases passed at USER phys %pa\n",
		passed, (unsigned int)ARRAY_SIZE(sizes) + 4, &phys);
	ret = 0;
out:
	iounmap(window);
	return ret;
}

static int __init diag_gdma_init(void)
{
	if (!run) {
		pr_info("diag_gdma: loaded; reload with run=1 to execute\n");
		return 0;
	}
	return run_tests();
}

static void __exit diag_gdma_exit(void)
{
}

module_init(diag_gdma_init);
module_exit(diag_gdma_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ambarella native GDMA diagnostic & validation");
MODULE_AUTHOR("Ambarella International LLC");

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
