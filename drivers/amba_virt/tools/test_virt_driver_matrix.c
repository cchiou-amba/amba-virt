/*
 * test_virt_driver_matrix.c
 *
 * Formalized Unit Tests for Ambarella Virtualization Driver Dependency Matrix.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virt_driver_matrix.h"

static void test_matrix_non_empty(void)
{
	printf("[TEST] Checking driver matrix non-empty...\n");
	assert(DRIVER_MATRIX_COUNT > 0);
	printf("[PASS] Matrix contains %zu registered modules\n", DRIVER_MATRIX_COUNT);
}

static void test_matrix_unique_masks(void)
{
	printf("[TEST] Checking module bitmasks are unique and powers of 2...\n");
	uint32_t seen_masks = 0;
	for (size_t i = 0; i < DRIVER_MATRIX_COUNT; i++) {
		uint32_t m = g_driver_matrix[i].module_mask;
		assert(m != 0);
		/* Must be a power of 2 */
		assert((m & (m - 1)) == 0);
		/* Must not be duplicate */
		assert((seen_masks & m) == 0);
		seen_masks |= m;
	}
	printf("[PASS] All module bitmasks are unique power-of-two flags\n");
}

static void test_prerequisite_dependencies(void)
{
	printf("[TEST] Validating prerequisite dependency hierarchy...\n");
	for (size_t i = 0; i < DRIVER_MATRIX_COUNT; i++) {
		const struct virt_module_dep *dep = &g_driver_matrix[i];
		/* Self-dependency is strictly forbidden */
		assert((dep->prerequisite_mask & dep->module_mask) == 0);

		/* If cavalry.ko, must depend on ambcma.ko */
		if (strcmp(dep->module_name, "cavalry.ko") == 0) {
			assert(dep->prerequisite_mask & HOST_MOD_AMBCMA);
			assert(dep->virt_dev_id == 1);
		}

		/* If iav.ko, must depend on dsp.ko and imgproc.ko */
		if (strcmp(dep->module_name, "iav.ko") == 0) {
			assert(dep->prerequisite_mask & HOST_MOD_DSP);
			assert(dep->prerequisite_mask & HOST_MOD_IMGPROC);
		}
	}
	printf("[PASS] Prerequisite dependency hierarchy validated\n");
}

static void test_pipeline_preset_masks(void)
{
	printf("[TEST] Checking pipeline preset composite masks...\n");
	assert((PIPELINE_MASK_NPU & HOST_MOD_AMBCMA) && (PIPELINE_MASK_NPU & HOST_MOD_CAVALRY));
	assert(PIPELINE_MASK_CAMERA & HOST_MOD_IAV);
	assert(PIPELINE_MASK_CAMERA & HOST_MOD_DSP);
	assert(PIPELINE_MASK_CAMERA & HOST_MOD_IMGPROC);
	printf("[PASS] Pipeline preset masks validated\n");
}

int main(void)
{
	printf("=== Running Virt Driver Matrix Test Suite ===\n");
	test_matrix_non_empty();
	test_matrix_unique_masks();
	test_prerequisite_dependencies();
	test_pipeline_preset_masks();
	printf("=== All Driver Matrix Tests Passed ===\n\n");
	return 0;
}
