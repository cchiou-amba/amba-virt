/*
 * iav_slice_client.c
 *
 * Nanopb bytes for a private-IAV slice query and release. The encoded
 * message carries no physical address.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include "amba_virt.pb.h"

#include "iav_slice_client.h"
#include "iav_slice_rpc.h"

static int encode_env(const ambarella_virt_v1_RpcEnvelope *env,
		      uint8_t *buf, size_t cap, size_t *len)
{
	pb_ostream_t out = pb_ostream_from_buffer(buf, cap);

	if (!buf || !len)
		return -EINVAL;
	if (!pb_encode(&out, ambarella_virt_v1_RpcEnvelope_fields, env))
		return -EIO;
	*len = out.bytes_written;
	return 0;
}

int iav_slice_client_encode_query(uint8_t *buf, size_t cap, size_t *len)
{
	ambarella_virt_v1_RpcEnvelope env = ambarella_virt_v1_RpcEnvelope_init_zero;

	env.api_major = 1;
	env.request_id = 1;
	env.which_body = ambarella_virt_v1_RpcEnvelope_iav_slice_query_request_tag;
	env.body.iav_slice_query_request.window_kind = IAV_SLICE_WINDOW_KIND_PRIVATE;
	return encode_env(&env, buf, cap, len);
}

int iav_slice_client_encode_release(uint8_t *buf, size_t cap, size_t *len, uint32_t window_id)
{
	ambarella_virt_v1_RpcEnvelope env = ambarella_virt_v1_RpcEnvelope_init_zero;

	env.api_major = 1;
	env.request_id = 2;
	env.which_body = ambarella_virt_v1_RpcEnvelope_iav_slice_release_request_tag;
	env.body.iav_slice_release_request.window_id = window_id;
	return encode_env(&env, buf, cap, len);
}

int iav_slice_client_parse_query(const uint8_t *buf, size_t len, int32_t *status,
				 uint32_t *window_id, uint32_t *usable_length)
{
	ambarella_virt_v1_RpcEnvelope env = ambarella_virt_v1_RpcEnvelope_init_zero;
	pb_istream_t in;

	if (!buf || !status)
		return -EINVAL;
	in = pb_istream_from_buffer(buf, len);
	if (!pb_decode(&in, ambarella_virt_v1_RpcEnvelope_fields, &env))
		return -EPROTO;
	if (env.which_body == ambarella_virt_v1_RpcEnvelope_error_tag) {
		*status = env.body.error.status;
		return 0;
	}
	if (env.which_body != ambarella_virt_v1_RpcEnvelope_iav_slice_query_response_tag)
		return -EPROTO;
	*status = env.body.iav_slice_query_response.status;
	if (window_id)
		*window_id = env.body.iav_slice_query_response.window_id;
	if (usable_length)
		*usable_length = env.body.iav_slice_query_response.usable_length;
	return 0;
}

int iav_slice_client_parse_release(const uint8_t *buf, size_t len, int32_t *status,
				   uint32_t *lease_retained)
{
	ambarella_virt_v1_RpcEnvelope env = ambarella_virt_v1_RpcEnvelope_init_zero;
	pb_istream_t in;

	if (!buf || !status)
		return -EINVAL;
	in = pb_istream_from_buffer(buf, len);
	if (!pb_decode(&in, ambarella_virt_v1_RpcEnvelope_fields, &env))
		return -EPROTO;
	if (env.which_body != ambarella_virt_v1_RpcEnvelope_iav_slice_release_response_tag)
		return -EPROTO;
	*status = env.body.iav_slice_release_response.status;
	if (lease_retained)
		*lease_retained = env.body.iav_slice_release_response.lease_retained ? 1u : 0u;
	return 0;
}
