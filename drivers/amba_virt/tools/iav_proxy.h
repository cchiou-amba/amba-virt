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

/* IAV Tap Proxy RPC opcodes */
enum iav_tap_opcode {
    IAV_TAP_OP_ATTACH         = 1,
    IAV_TAP_OP_DETACH         = 2,
    IAV_TAP_OP_RUN_LIVE_DAG   = 3,
    IAV_TAP_OP_GET_STATUS     = 4,
};

struct iav_tap_rpc {
    uint32_t opcode;         /* enum iav_tap_opcode */
    int32_t  status;         /* 0 on success, -errno on error */
    uint32_t client_cid;     /* Caller CID (or 0 for local host) */
    uint32_t session_id;     /* Session token */
    uint32_t dag_id;         /* Registered Path B DAG ID */
    uint32_t in_handle_id;   /* Path B Input Handle ID */
    uint32_t out_handle_id;  /* Path B Output Handle ID */
    uint32_t active_seq;     /* Newest published sequence number */
    uint32_t drop_count;     /* Frame drop count */
    uint32_t exec_ticks;     /* VisORC hardware ticks */
    uint32_t rval;           /* VisORC execution return code */
    uint32_t width;          /* Frame width */
    uint32_t height;         /* Frame height */
    uint32_t pitch;          /* Frame pitch */
    uint32_t fourcc;         /* Frame format */
};

/* Lifecycle & Management */
int iav_proxy_init(struct iav_tap_ring *ring);
void iav_proxy_cleanup(void);

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

/* RPC Message Handling */
int iav_proxy_handle_rpc(const struct iav_tap_rpc *req,
                         struct iav_tap_rpc *resp,
                         uint32_t client_cid);

/* Frame Binding & VisORC Step Execution */
int iav_proxy_step_live_inference(uint32_t dag_id,
                                  uint32_t in_handle_id,
                                  uint32_t out_handle_id,
                                  uint32_t client_cid,
                                  uint32_t session_id,
                                  uint64_t *out_seq,
                                  uint32_t *out_ticks,
                                  uint32_t *out_rval,
                                  void *saved_input_copy,
                                  void *saved_output_copy);

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
