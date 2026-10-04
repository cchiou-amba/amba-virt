/*
 * drivers/amba_virt/tools/iav_proxy.h
 *
 * IAV Frame Proxy & Cavalry Path B Bridge.
 * Binds live camera frames from iav_frame_tap into Cavalry Path B input handles
 * for hardware-accelerated deep neural network inference.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef _IAV_PROXY_H_
#define _IAV_PROXY_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "iav_tap_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle & Management */
int iav_proxy_init(struct iav_tap_ring *ring);
void iav_proxy_cleanup(void);
void iav_proxy_set_bsb(int fd_iav, void *bsb_base, size_t bsb_len, int mjpeg_stream_id);

int iav_proxy_attach(uint32_t client_cid, uint32_t session_id);
int iav_proxy_detach(uint32_t client_cid, uint32_t session_id);
void iav_proxy_client_disconnect(uint32_t client_cid);
int iav_proxy_active_client_count(void);

/* Number of attached clients required before any client consumes a frame. */
void iav_proxy_set_cohort(uint32_t cohort);

/* Producer-side ring access; the _locked calls require iav_proxy_ring_lock(). */
void iav_proxy_ring_lock(void);
void iav_proxy_ring_unlock(void);
bool iav_proxy_slot_writable_locked(const struct iav_tap_slot *slot);
void iav_proxy_note_published_locked(uint64_t seq);
void iav_proxy_log_producer_loss(const char *reason, uint64_t first_seq, uint64_t last_seq);

/* RPC Message Handling via Nanopb RpcEnvelope */
int iav_proxy_handle_nanopb_rpc(const uint8_t *req_bytes,
                                size_t req_len,
                                uint8_t *resp_bytes,
                                size_t max_resp_len,
                                size_t *out_resp_len,
                                uint32_t client_cid);

/* Color conversion & Geometry helpers (for unit testing and pipeline processing) */
int proxy_convert_nv12_to_yolox_rgb_640x640(const uint8_t *nv12_payload,
                                            uint32_t width, uint32_t height, uint32_t pitch,
                                            uint8_t *in_tensor);

#ifdef __cplusplus
}
#endif

#endif /* _IAV_PROXY_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
