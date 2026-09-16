/*
 * virt_query.c
 *
 * Guest Introspection & Topology Query API Subsystem for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cavalry_proxy.h"
#include "virt_acl.h"
#include "virt_mem_pool.h"
#include "virt_query.h"

int virt_query_handle_req(uint32_t caller_cid,
			  const struct amba_virt_query_req *req,
			  struct amba_virt_query_resp *resp)
{
	if (!req || !resp)
		return -EINVAL;

	memset(resp, 0, sizeof(*resp));
	resp->query_op = req->query_op;

	switch (req->query_op) {
	case AMBA_VIRT_QUERY_SELF: {
		if (!virt_acl_has_cap(caller_cid, AMBA_VIRT_CAP_QUERY_SELF)) {
			resp->status = -EPERM;
			return 0;
		}

		struct amba_virt_peer_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.cid = caller_cid;

		struct virt_tenant_pool *pool = virt_mem_pool_get_tenant(caller_cid);
		if (pool) {
			desc.tenant_idx = pool->tenant_idx;
			desc.mem_allocated_mb = pool->allocated_bytes / (1024 * 1024);
		}
		desc.status = 1; /* ONLINE */
		desc.caps = virt_acl_get_caps(caller_cid);

		memcpy(resp->payload, &desc, sizeof(desc));
		resp->count = 1;
		resp->status = 0;
		break;
	}

	case AMBA_VIRT_QUERY_PEERS: {
		if (!virt_acl_has_cap(caller_cid, AMBA_VIRT_CAP_QUERY_PEERS)) {
			resp->status = -EPERM;
			return 0;
		}

		struct virt_acl_entry entries[VIRT_ACL_MAX_RULES];
		int total = virt_acl_get_all_entries(entries, VIRT_ACL_MAX_RULES);
		int out_count = 0;

		for (int i = 0; i < total; i++) {
			if (req->target_cid != 0 && entries[i].cid != req->target_cid)
				continue;

			if ((out_count + 1) * sizeof(struct amba_virt_peer_desc) > sizeof(resp->payload))
				break;

			struct amba_virt_peer_desc desc;
			memset(&desc, 0, sizeof(desc));
			desc.cid = entries[i].cid;
			desc.tenant_idx = i;
			desc.status = 1; /* ONLINE */

			uint32_t q_max = 0, q_alloc = 0;
			virt_mem_pool_get_quota(entries[i].cid, &q_max, &q_alloc);
			desc.mem_allocated_mb = q_alloc / (1024 * 1024);
			desc.caps = entries[i].caps;

			memcpy(resp->payload + out_count * sizeof(desc), &desc, sizeof(desc));
			out_count++;
		}

		resp->count = out_count;
		resp->status = 0;
		break;
	}

	case AMBA_VIRT_QUERY_DEV_TOPOLOGY: {
		if (!virt_acl_has_cap(caller_cid, AMBA_VIRT_CAP_QUERY_TOPO)) {
			resp->status = -EPERM;
			return 0;
		}

		struct amba_virt_topo_desc topo;
		memset(&topo, 0, sizeof(topo));
		topo.chip_id = 0x655; /* N1-655 */
		topo.npu_core_cnt = 4;
		topo.npu_freq_mhz = 1000;
		topo.gdma_channels = 2;
		topo.total_cvmem_mb = 1024;
		topo.host_phys_addr = 0; /* HPA masked for security */

		memcpy(resp->payload, &topo, sizeof(topo));
		resp->count = 1;
		resp->status = 0;
		break;
	}

	case AMBA_VIRT_QUERY_DEV_MEM: {
		if (!virt_acl_has_cap(caller_cid, AMBA_VIRT_CAP_QUERY_SELF)) {
			resp->status = -EPERM;
			return 0;
		}

		struct virt_tenant_pool *pool = virt_mem_pool_get_tenant(caller_cid);
		int out_count = 0;

		if (pool) {
			for (int i = 0; i < VIRT_MEM_MAX_DEVICES; i++) {
				if (!pool->devices[i].in_use)
					continue;
				if (req->dev_id != 0 && pool->devices[i].dev_type != req->dev_id)
					continue;

				if ((out_count + 1) * sizeof(struct amba_virt_dev_mem_desc) > sizeof(resp->payload))
					break;

				struct amba_virt_dev_mem_desc ddesc;
				memset(&ddesc, 0, sizeof(ddesc));
				ddesc.dev_type = pool->devices[i].dev_type;
				ddesc.base_offset = pool->devices[i].base_offset;
				ddesc.size = pool->devices[i].size;
				ddesc.rpc_arena_offset = pool->devices[i].rpc_arena_offset;
				ddesc.rpc_arena_size = pool->devices[i].rpc_arena_size;
				ddesc.flags = pool->devices[i].flags;

				memcpy(resp->payload + out_count * sizeof(ddesc), &ddesc, sizeof(ddesc));
				out_count++;
			}
		}

		resp->count = out_count;
		resp->status = 0;
		break;
	}

	default:
		resp->status = -EOPNOTSUPP;
		break;
	}

	return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
