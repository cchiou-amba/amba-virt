/*
 * iav_new_parts.h
 *
 * The three IAV partitions amba-virt-server allocates at start (plan_iav_usb_yolo Envelope F, memory_adapter_proposal_v2
 * 3c): IAV_PART_BLUR, IAV_PART_ARB_BLUR and IAV_PART_YUV_REMAP, 0x2C9000 bytes in all, from the IAV private pool,
 * uncached. They exist only when someone allocates them (the build sizes them 0), and the driver keeps them until it is
 * removed or they are freed. A server that finds one already allocated with the expected length reuses it; one of
 * another length is left alone (it may be behind a guest's mapped aperture) and reported. The image's server never
 * frees them; only the qualification build may, on request, to return a board to the state of an image without them.
 * See f_design.md in the plan artifacts for the byte map.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef IAV_NEW_PARTS_H
#define IAV_NEW_PARTS_H

#include <stdint.h>

#define IAV_NEW_PARTS_COUNT	3u

struct iav_new_part {
	uint32_t pid;
	const char *name;
	uint64_t alloc_len;	/* what ALLOC_MEM_PART asks for: content plus the driver's reserve */
	uint64_t report_len;	/* what QUERY_MEMBLOCK reports once it exists: the content */
};

extern const struct iav_new_part iav_new_parts[IAV_NEW_PARTS_COUNT];

/* What became of each partition. */
#define IAV_NEW_PART_ABSENT	0	/* not there (an allocation failed, or nothing was tried) */
#define IAV_NEW_PART_ALLOCATED	1	/* allocated now */
#define IAV_NEW_PART_REUSED	2	/* found with the expected length */
#define IAV_NEW_PART_MISMATCH	3	/* found with another length: left alone, not used */
#define IAV_NEW_PART_FREED	4	/* freed (qualification build only) */

struct iav_new_parts_state {
	int state[IAV_NEW_PARTS_COUNT];
	int rc[IAV_NEW_PARTS_COUNT];		/* 0, or the negative errno of the step that failed */
	uint64_t addr[IAV_NEW_PARTS_COUNT];	/* as QUERY_MEMBLOCK reports */
	uint64_t len[IAV_NEW_PARTS_COUNT];
};

/* ioctl on the caller's /dev/iav (replaced in host tests): 0, or a negative errno. */
typedef int (*iav_new_parts_ioctl_fn)(void *ctx, unsigned long cmd, void *arg);

/* Allocate or reuse each partition. Returns how many are usable (allocated or reused); *st says why. */
int iav_new_parts_ensure(iav_new_parts_ioctl_fn fn, void *ctx, struct iav_new_parts_state *st);
/* Free each partition that reports the expected length (qualification build only). Returns how many were freed. */
int iav_new_parts_free(iav_new_parts_ioctl_fn fn, void *ctx, struct iav_new_parts_state *st);

/* The same on /dev/iav, logging one line per partition. */
int iav_new_parts_ensure_dev(void);
int iav_new_parts_free_dev(void);

const char *iav_new_part_state_name(int state);

#endif /* IAV_NEW_PARTS_H */
