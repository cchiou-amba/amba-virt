/*
 * amba_virt_iav_slice.c
 *
 * Ask the host which private IAV slice this guest already holds.
 * The Nanopb bytes are built by iav_slice_client. This file only sends them.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "amba_virt.h"
#include "amba_virt_kernel.h"
#include "iav_slice_client.h"

static int slice_rpc(const uint8_t *payload, size_t payload_len,
		     uint8_t *answer, size_t answer_cap, size_t *answer_len)
{
	uint8_t *buf;
	struct amba_virt_msg *msg;
	struct amba_virt_msg *rmsg;
	/* the capacity of the answer buffer: amba_virt_rpc_seq refuses a zero capacity with -EMSGSIZE before sending */
	u32 rlen = AMBA_VIRT_MAX_MSG;
	int ret;

	if (payload_len > AMBA_VIRT_MAX_MSG - sizeof(*msg))
		return -EMSGSIZE;
	buf = kzalloc(AMBA_VIRT_MAX_MSG * 2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	msg = (struct amba_virt_msg *)buf;
	msg->type = AMBA_VIRT_MSG_IAV_SLICE_REQ;
	msg->seq = 1;
	memcpy(buf + sizeof(*msg), payload, payload_len);
	ret = amba_virt_rpc_seq(buf, sizeof(*msg) + payload_len,
				buf + AMBA_VIRT_MAX_MSG, &rlen, 1000);
	if (ret) {
		kfree(buf);
		return ret;
	}
	rmsg = (struct amba_virt_msg *)(buf + AMBA_VIRT_MAX_MSG);
	if (rlen < sizeof(*rmsg) || rmsg->type != AMBA_VIRT_MSG_IAV_SLICE_RESP ||
	    rmsg->seq != msg->seq || rlen - sizeof(*rmsg) > answer_cap) {
		kfree(buf);
		return -EPROTO;
	}
	memcpy(answer, buf + AMBA_VIRT_MAX_MSG + sizeof(*rmsg), rlen - sizeof(*rmsg));
	*answer_len = rlen - sizeof(*rmsg);
	kfree(buf);
	return 0;
}

int amba_virt_iav_slice_query(u32 *window_id, u32 *usable_length)
{
	uint8_t req[128];
	uint8_t resp[128];
	size_t req_len = 0;
	size_t resp_len = 0;
	int32_t status = -EIO;
	int ret;

	if (!window_id || !usable_length)
		return -EINVAL;
	ret = iav_slice_client_encode_query(req, sizeof(req), &req_len);
	if (ret)
		return ret;
	ret = slice_rpc(req, req_len, resp, sizeof(resp), &resp_len);
	if (ret)
		return ret;
	ret = iav_slice_client_parse_query(resp, resp_len, &status, window_id, usable_length);
	if (ret)
		return ret;
	return status;
}
EXPORT_SYMBOL_GPL(amba_virt_iav_slice_query);

int amba_virt_iav_slice_release(u32 window_id, u32 *lease_retained)
{
	uint8_t req[128];
	uint8_t resp[128];
	size_t req_len = 0;
	size_t resp_len = 0;
	int32_t status = -EIO;
	int ret;

	ret = iav_slice_client_encode_release(req, sizeof(req), &req_len, window_id);
	if (ret)
		return ret;
	ret = slice_rpc(req, req_len, resp, sizeof(resp), &resp_len);
	if (ret)
		return ret;
	ret = iav_slice_client_parse_release(resp, resp_len, &status, lease_retained);
	if (ret)
		return ret;
	return status;
}
EXPORT_SYMBOL_GPL(amba_virt_iav_slice_release);
