/*
 * iav_slice_rpc.h
 *
 * Nanopb query and release for the private IAV slice already leased to a
 * guest CID. Release is acknowledged and does not drop the lease.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef IAV_SLICE_RPC_H
#define IAV_SLICE_RPC_H

#include <stddef.h>
#include <stdint.h>

#define IAV_SLICE_WINDOW_KIND_PRIVATE 1u

int iav_slice_rpc_bind(uint32_t cid, uint32_t window_id, uint32_t usable_length);
void iav_slice_rpc_unbind(uint32_t cid);
void iav_slice_rpc_reset(void);
int iav_slice_rpc_held(uint32_t cid, uint32_t *window_id, uint32_t *usable_length);

int iav_slice_rpc_handle(const uint8_t *req_bytes, size_t req_len,
			 uint8_t *resp_bytes, size_t max_resp_len,
			 size_t *out_resp_len, uint32_t client_cid);

#endif
