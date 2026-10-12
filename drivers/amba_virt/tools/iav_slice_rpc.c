/*
 * iav_slice_rpc.c
 *
 * Decode a private-IAV slice query or release. The release reply says the
 * lease is retained. This file never removes a binding.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include "amba_virt.pb.h"
#include "amba_virt.h"
#include "amba_virt_iav.h"

#include "iav_slice_rpc.h"

_Static_assert(AMBA_VIRT_MSG_IAV_SLICE_REQ != AMBA_VIRT_MSG_IAV_REQ,
	       "slice query must not share the IAV ioctl message number");
_Static_assert(AMBA_VIRT_MSG_IAV_SLICE_RESP != AMBA_VIRT_MSG_IAV_RESP,
	       "slice reply must not share the IAV ioctl message number");

#define SLICE_RPC_MAX 8
#define SLICE_USABLE_MAX ((64u << 20) - 4096u)

struct slice_binding {
	int used;
	uint32_t cid;
	uint32_t window_id;
	uint32_t usable_length;
};

static struct slice_binding g_bind[SLICE_RPC_MAX];

static struct slice_binding *find_cid(uint32_t cid)
{
	unsigned int i;

	if (cid == 0)
		return NULL;
	for (i = 0; i < SLICE_RPC_MAX; i++) {
		if (g_bind[i].used && g_bind[i].cid == cid)
			return &g_bind[i];
	}
	return NULL;
}

int iav_slice_rpc_bind(uint32_t cid, uint32_t window_id, uint32_t usable_length)
{
	struct slice_binding *slot;
	unsigned int i;

	if (cid == 0 || usable_length == 0 || usable_length > SLICE_USABLE_MAX)
		return -EINVAL;
	slot = find_cid(cid);
	if (!slot) {
		for (i = 0; i < SLICE_RPC_MAX; i++) {
			if (!g_bind[i].used) {
				slot = &g_bind[i];
				break;
			}
		}
	}
	if (!slot)
		return -ENOMEM;
	slot->used = 1;
	slot->cid = cid;
	slot->window_id = window_id;
	slot->usable_length = usable_length;
	return 0;
}

void iav_slice_rpc_unbind(uint32_t cid)
{
	struct slice_binding *slot = find_cid(cid);

	if (slot)
		memset(slot, 0, sizeof(*slot));
}

void iav_slice_rpc_reset(void)
{
	memset(g_bind, 0, sizeof(g_bind));
}

int iav_slice_rpc_held(uint32_t cid, uint32_t *window_id, uint32_t *usable_length)
{
	struct slice_binding *slot = find_cid(cid);

	if (!slot)
		return -ENOENT;
	if (window_id)
		*window_id = slot->window_id;
	if (usable_length)
		*usable_length = slot->usable_length;
	return 0;
}

static int encode_env(const ambarella_virt_v1_RpcEnvelope *env,
		      uint8_t *resp_bytes, size_t max_resp_len, size_t *out_resp_len)
{
	pb_ostream_t ostream = pb_ostream_from_buffer(resp_bytes, max_resp_len);

	if (!pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, env))
		return -EIO;
	*out_resp_len = ostream.bytes_written;
	return 0;
}

static void error_env(ambarella_virt_v1_RpcEnvelope *env, uint64_t request_id, int status)
{
	env->api_major = 1;
	env->api_minor = 0;
	env->request_id = request_id;
	env->which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
	env->body.error.status = status;
	snprintf(env->body.error.detail, sizeof(env->body.error.detail),
		 "iav slice rpc status %d", status);
}

int iav_slice_rpc_handle(const uint8_t *req_bytes, size_t req_len,
			 uint8_t *resp_bytes, size_t max_resp_len,
			 size_t *out_resp_len, uint32_t client_cid)
{
	pb_istream_t istream;
	ambarella_virt_v1_RpcEnvelope req_env = ambarella_virt_v1_RpcEnvelope_init_zero;
	ambarella_virt_v1_RpcEnvelope resp_env = ambarella_virt_v1_RpcEnvelope_init_zero;
	struct slice_binding *slot;

	if (!req_bytes || !resp_bytes || !out_resp_len)
		return -EINVAL;

	istream = pb_istream_from_buffer(req_bytes, req_len);
	if (!pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &req_env)) {
		error_env(&resp_env, 0, -EBADMSG);
		return encode_env(&resp_env, resp_bytes, max_resp_len, out_resp_len);
	}
	if (req_env.api_major != 1) {
		error_env(&resp_env, req_env.request_id, -EPROTONOSUPPORT);
		return encode_env(&resp_env, resp_bytes, max_resp_len, out_resp_len);
	}

	resp_env.api_major = 1;
	resp_env.api_minor = 0;
	resp_env.request_id = req_env.request_id;
	slot = find_cid(client_cid);

	if (req_env.which_body == ambarella_virt_v1_RpcEnvelope_iav_slice_query_request_tag) {
		const ambarella_virt_v1_IavSliceQueryRequest *q = &req_env.body.iav_slice_query_request;

		if (q->window_kind != IAV_SLICE_WINDOW_KIND_PRIVATE) {
			error_env(&resp_env, req_env.request_id, -EINVAL);
			return encode_env(&resp_env, resp_bytes, max_resp_len, out_resp_len);
		}
		if (!slot) {
			error_env(&resp_env, req_env.request_id, -ENOENT);
			return encode_env(&resp_env, resp_bytes, max_resp_len, out_resp_len);
		}
		resp_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_slice_query_response_tag;
		resp_env.body.iav_slice_query_response.status = 0;
		resp_env.body.iav_slice_query_response.window_id = slot->window_id;
		resp_env.body.iav_slice_query_response.usable_length = slot->usable_length;
		return encode_env(&resp_env, resp_bytes, max_resp_len, out_resp_len);
	}

	if (req_env.which_body == ambarella_virt_v1_RpcEnvelope_iav_slice_release_request_tag) {
		const ambarella_virt_v1_IavSliceReleaseRequest *rel = &req_env.body.iav_slice_release_request;
		int status = 0;

		if (!slot)
			status = -ENOENT;
		else if (rel->window_id != slot->window_id)
			status = -EINVAL;
		resp_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_slice_release_response_tag;
		resp_env.body.iav_slice_release_response.status = status;
		resp_env.body.iav_slice_release_response.lease_retained = true;
		return encode_env(&resp_env, resp_bytes, max_resp_len, out_resp_len);
	}

	error_env(&resp_env, req_env.request_id, -EINVAL);
	return encode_env(&resp_env, resp_bytes, max_resp_len, out_resp_len);
}
