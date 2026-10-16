/*
 * virt_iav_aperture.h
 *
 * The per-guest aux aperture of the virtual /dev/iav (plan_iav_usb_yolo Envelope F, memory_adapter_proposal_v2 3c
 * and 3e): one node per Cavalry lease index, /dev/amba_virt_iav_aux<n>, composed by the server from the partitions the
 * guest's role may map, attached by kvm.go when the grant carries aux=<size>, and described to the guest by the IAV
 * broker's OP_INFO. This module composes the aperture, configures and clears the nodes, keeps the table the broker asks,
 * and adopts what a node already holds when the server restarts while guests run. See f_design.md in the plan
 * artifacts for the byte map.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef VIRT_IAV_APERTURE_H
#define VIRT_IAV_APERTURE_H

#include <stddef.h>
#include <stdint.h>

#include "amba_virt_iav_win.h"

#define VIRT_IAV_APERTURE_NODES		AMBA_VIRT_IAV_AUX_NODES
#define VIRT_IAV_APERTURE_MAX_PARTS	AMBA_VIRT_IAV_AUX_MAX_RANGES
#define VIRT_IAV_APERTURE_MAX_AVOID	4u

/* A partition an aperture may carry, as QUERY_MEMBLOCK reports it. */
struct virt_iav_aperture_part {
	uint32_t pid;
	uint64_t base;
	uint64_t len;
};

/* An attached aperture as the broker sees it. */
struct virt_iav_aperture_view {
	uint64_t size;
	uint64_t epoch;
	uint32_t count;
	struct {
		uint32_t pid;
		uint64_t base;
		uint64_t len;
		uint64_t offset;
	} e[VIRT_IAV_APERTURE_MAX_PARTS];
};

/* The node operations (replaced in host tests). Each returns 0 or a negative errno. */
struct virt_iav_aperture_ops {
	void *ctx;
	int (*node_ioctl)(void *ctx, uint32_t node, unsigned long cmd, void *arg);
};

/* The partitions of the primary role, in the order they are placed in the aperture (f_design.md 1.2). */
extern const uint32_t virt_iav_aperture_primary_pids[];
extern const unsigned int virt_iav_aperture_primary_count;

/*
 * Compose an aperture for the primary role from the partitions given (any order; those not of the primary set, or not
 * page aligned, or empty, are left out): each at the next 64 KiB boundary in the fixed order, the size the smallest
 * power of two that holds them and the header page, doubled while it equals one of avoid[] (the sizes of legacy
 * windows that have no padding). Returns the number of ranges placed (0: nothing to attach) with *out filled, or a
 * negative errno.
 */
int virt_iav_aperture_compose(const struct virt_iav_aperture_part *parts, unsigned int n, const uint64_t *avoid,
			      unsigned int navoid, uint64_t epoch, struct amba_virt_iav_aux_cfg *out);

/* Forget everything (host tests). The real node operations are the default. */
void virt_iav_aperture_reset(void);
void virt_iav_aperture_set_ops(const struct virt_iav_aperture_ops *ops);

/*
 * Register the partitions the apertures are built from (the node module's sources) and the sizes to avoid. Returns 0,
 * or a negative errno when the nodes cannot take them (an older window module has no aperture nodes: -ENOENT or
 * -ENOTTY), in which case no aperture is ever attached by this server.
 */
int virt_iav_aperture_init(const struct virt_iav_aperture_part *parts, unsigned int n, const uint64_t *avoid,
			   unsigned int navoid);

/*
 * A reserve of lease index `index`, epoch `epoch`: for the primary role compose and configure the node (or keep it, if it
 * already holds exactly that composition, as for a repeated reserve of a running guest) and return its size in *size; for
 * a consumer, clear the node if nothing maps it and return 0. A node that is mapped with another composition is left
 * alone and 0 returned (the guest runs without an aperture). Returns 0 or a negative errno; *size is 0 unless attached.
 */
int virt_iav_aperture_attach(uint32_t index, uint64_t epoch, int primary, uint64_t *size);

/* The lease of `index` was released: forget its aperture and clear the node if nothing maps it (else at the next reserve). */
void virt_iav_aperture_detach(uint32_t index);

/*
 * At a server start, for a lease that already holds `index`: adopt what the node holds when it is exactly the
 * composition for this lease (the guest's QEMU maps it), else clear the node if nothing maps it. Returns 1 adopted, 0 not.
 */
int virt_iav_aperture_adopt(uint32_t index, uint64_t epoch, int primary);

/* What the guest on `index` has attached: 0 with *out (count 0: none). */
int virt_iav_aperture_get(uint32_t index, struct virt_iav_aperture_view *out);

/* One status line, "APERTURE state=<..> sources=<n> nodes=<i>:<size>/<count>,...\n". Returns bytes written or -1. */
int virt_iav_aperture_format(char *buf, size_t len);

#endif /* VIRT_IAV_APERTURE_H */
