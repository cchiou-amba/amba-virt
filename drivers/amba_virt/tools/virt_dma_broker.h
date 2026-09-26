/*
 * virt_dma_broker.h
 *
 * Virtual Peripheral DMA Broker for amba-virt-server.
 * Validates CID-to-Channel ACLs, ivshmem memory bounds,
 * and manages 500ms hardware watchdog timers per transfer.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_DMA_BROKER_H_
#define _VIRT_DMA_BROKER_H_

#include <stdint.h>
#include <stddef.h>
#include "amba_virt.h"

#define VIRT_DMA_MAX_CHANNELS       16
#define VIRT_DMA_WATCHDOG_MS        500
#define VIRT_DMA_MAX_TRANSFER_LEN   ((1u << 22) - 1)  /* 4 MiB - 1 */

/* Security Event IDs */
#define VIRT_DMA_EVT_BOUNDS_VIOLATION   91  /* EVT-091 */
#define VIRT_DMA_EVT_ACL_VIOLATION      92  /* EVT-092 */
#define VIRT_DMA_EVT_WATCHDOG_TIMEOUT   93  /* EVT-093 */

/*
 * Initialize the DMA broker subsystem.
 * Called once at server startup.
 */
int virt_dma_broker_init(void);

/*
 * Cleanup the DMA broker subsystem.
 */
void virt_dma_broker_cleanup(void);

/*
 * Handle a DMA slave configuration request.
 *
 * Validates:
 *   - CID has AMBA_VIRT_CAP_DMA_SLAVE capability
 *   - CID is authorized for the requested channel (EVT-092)
 *   - Slave parameters are within valid ranges
 *
 * Returns response payload size, or 0 on error.
 */
int virt_dma_handle_slave_cfg(uint32_t cid,
                              const struct amba_virt_dma_slave_cfg *req,
                              struct amba_virt_dma_slave_cfg *resp);

/*
 * Handle a DMA transfer submit request.
 *
 * Validates:
 *   - CID authorization for channel (EVT-092)
 *   - buf_offset + buf_len within ivshmem window (EVT-091)
 *   - Transfer length within hardware limits
 *
 * On success, arms 500ms watchdog and dispatches to hardware.
 * Returns response payload size, or 0 on error.
 */
int virt_dma_handle_submit(uint32_t cid,
                           int host_fd,
                           uint64_t tenant_phys_base,
                           size_t ivshmem_size,
                           const struct amba_virt_dma_submit *req,
                           struct amba_virt_dma_submit *resp);

/*
 * Handle a DMA terminate (abort) request.
 *
 * Validates CID authorization, then aborts any active transfer
 * on the specified channel and disarms the watchdog.
 */
int virt_dma_handle_terminate(uint32_t cid,
                              const struct amba_virt_dma_terminate *req,
                              struct amba_virt_dma_terminate *resp);

/*
 * Validate CID authorization for a specific DMA channel.
 *
 * Returns 0 if authorized, -EPERM if denied (EVT-092).
 */
int virt_dma_validate_channel_access(uint32_t cid, uint32_t channel);

/*
 * Unified split DMA request handler.
 * Validates token-bucket rate limits, forwards request to lease-scoped
 * kernel device node (/dev/amba_dma_lease<N>), and populates response.
 */
int virt_dma_handle_request(uint32_t cid,
                            const struct amba_virt_dma_request *req,
                            struct amba_virt_dma_response *resp);

/* Bind CID to paired lease device */
int virt_dma_bind_cid_lease(uint32_t cid, uint32_t lease_id);

#endif /* _VIRT_DMA_BROKER_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
