/*
 * drivers/amba_virt/amba_virt_slice.c
 *
 * Pure shared-memory window geometry and slice computation.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include "amba_virt_slice.h"

int amba_virt_slice_compute_geometry(uint64_t window_phys,
				     uint64_t window_size,
				     uint32_t page_size,
				     uint32_t slice_count,
				     struct amba_virt_slice_list *out_list)
{
	uint64_t slice_size;
	uint64_t total_product;
	uint32_t usable_size;
	uint32_t i;

	if (!out_list)
		return -1;

	out_list->count = 0;

	if (window_size == 0 || page_size == 0 || slice_count == 0 || slice_count > 8)
		return -1;

	/* Window size must be page-aligned */
	if ((window_size % page_size) != 0)
		return -1;

	slice_size = window_size / slice_count;
	if (slice_size == 0)
		return -1;

	/* Slice size must be page-aligned */
	if ((slice_size % page_size) != 0)
		return -1;

	/* 64-bit overflow check and exact divisibility check */
	total_product = slice_size * (uint64_t)slice_count;
	if (total_product != window_size)
		return -1;

	/* Usable size is slice_size minus the last claim page */
	if (slice_size <= page_size)
		return -1; /* usable_size would be 0 */

	usable_size = (uint32_t)(slice_size - page_size);

	for (i = 0; i < slice_count; i++) {
		uint64_t offset = (uint64_t)i * slice_size;
		uint64_t phys = window_phys + offset;

		out_list->slices[i].index = i;
		out_list->slices[i].usable_size = usable_size;
		out_list->slices[i].offset = offset;
		out_list->slices[i].phys = phys;
		out_list->slices[i].slice_size = slice_size;
	}

	out_list->count = slice_count;
	return 0;
}

int amba_virt_slice_compare_list(const struct amba_virt_slice_list *a,
				 const struct amba_virt_slice_list *b)
{
	uint32_t i;

	if (!a || !b)
		return 0;

	if (a->count == 0 || b->count == 0 || a->count != b->count)
		return 0;

	for (i = 0; i < a->count; i++) {
		if (a->slices[i].index != b->slices[i].index ||
		    a->slices[i].usable_size != b->slices[i].usable_size ||
		    a->slices[i].offset != b->slices[i].offset ||
		    a->slices[i].phys != b->slices[i].phys ||
		    a->slices[i].slice_size != b->slices[i].slice_size)
			return 0;
	}

	return 1;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
