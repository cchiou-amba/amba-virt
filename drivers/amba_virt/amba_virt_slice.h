/*
 * drivers/amba_virt/amba_virt_slice.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _AMBA_VIRT_SLICE_H
#define _AMBA_VIRT_SLICE_H

#if defined(__KERNEL__)
#include <linux/types.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif

#include "amba_virt.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure geometry function:
 * window_phys: base physical address of the attached window
 * window_size: total byte size of the attached window
 * page_size: page size (e.g. 4096)
 * slice_count: number of distinct slice indices (e.g. 2)
 * out_list: output slice list
 *
 * Returns 0 on success (out_list->count > 0), negative error or 0 count on reject.
 */
int amba_virt_slice_compute_geometry(uint64_t window_phys,
                                     uint64_t window_size,
                                     uint32_t page_size,
                                     uint32_t slice_count,
                                     struct amba_virt_slice_list *out_list);

/*
 * Pure slice-list compare function:
 * Compares an enumerated list against an expected list.
 * Returns 1 if match, 0 if reject/mismatch.
 */
int amba_virt_slice_compare_list(const struct amba_virt_slice_list *a,
                                 const struct amba_virt_slice_list *b);

#ifdef __cplusplus
}
#endif

#endif /* _AMBA_VIRT_SLICE_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
