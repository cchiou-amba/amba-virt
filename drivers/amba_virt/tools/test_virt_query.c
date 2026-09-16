/*
 * test_virt_query.c
 *
 * Unit test for virt_query introspection API.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virt_acl.h"
#include "virt_mem_pool.h"
#include "virt_query.h"

int main(void)
{
	struct amba_virt_query_req req;
	struct amba_virt_query_resp resp;
	int ret;

	printf("=== Starting virt_query unit test suite ===\n");

	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_acl_init();

	/* Setup CID 15 with STANDARD role + PEERS & TOPO, CID 20 with UNTRUSTED role */
	virt_acl_set_rule(15, "standard-vm",
			  AMBA_VIRT_ROLE_STANDARD | AMBA_VIRT_CAP_QUERY_PEERS | AMBA_VIRT_CAP_QUERY_TOPO,
			  1024, 1);
	virt_acl_set_rule(20, "untrusted-vm", AMBA_VIRT_ROLE_UNTRUSTED, 512, 0);

	/* Setup device boundaries for CID 15 */
	struct amba_virt_dev_bounds_req breq;
	struct amba_virt_dev_bounds_resp bresp;
	memset(&breq, 0, sizeof(breq));
	breq.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	breq.requested_size = 512 * 1024 * 1024;
	breq.preferred_offset = 0x04000000;
	breq.rpc_arena_size = 1 * 1024 * 1024;
	breq.flags = AMBA_VIRT_DEV_F_EXACT;
	ret = virt_mem_pool_set_device_bounds(15, &breq, &bresp, NULL);
	assert(ret == 0 && bresp.status == 0);

	/* 1. Test QUERY_SELF for CID 15 */
	memset(&req, 0, sizeof(req));
	req.query_op = AMBA_VIRT_QUERY_SELF;
	ret = virt_query_handle_req(15, &req, &resp);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.count == 1);
	struct amba_virt_peer_desc *pdesc = (struct amba_virt_peer_desc *)resp.payload;
	assert(pdesc->cid == 15);
	assert(pdesc->status == 1);
	assert((pdesc->caps & AMBA_VIRT_ROLE_STANDARD) == AMBA_VIRT_ROLE_STANDARD);
	printf("QUERY_SELF: PASS\n");

	/* 2. Test QUERY_PEERS for CID 15 (Allowed) */
	memset(&req, 0, sizeof(req));
	req.query_op = AMBA_VIRT_QUERY_PEERS;
	ret = virt_query_handle_req(15, &req, &resp);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.count >= 2);
	printf("QUERY_PEERS (allowed): PASS\n");

	/* 3. Test QUERY_PEERS for CID 20 (Untrusted -> Must return -EPERM) */
	memset(&req, 0, sizeof(req));
	req.query_op = AMBA_VIRT_QUERY_PEERS;
	ret = virt_query_handle_req(20, &req, &resp);
	assert(ret == 0);
	assert(resp.status == -EPERM);
	printf("QUERY_PEERS (untrusted denied -EPERM): PASS\n");

	/* 4. Test QUERY_DEV_TOPOLOGY for CID 15 */
	memset(&req, 0, sizeof(req));
	req.query_op = AMBA_VIRT_QUERY_DEV_TOPOLOGY;
	ret = virt_query_handle_req(15, &req, &resp);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.count == 1);
	struct amba_virt_topo_desc *topo = (struct amba_virt_topo_desc *)resp.payload;
	assert(topo->chip_id == 0x655);
	assert(topo->host_phys_addr == 0); /* Verify HPA masked */
	printf("QUERY_DEV_TOPOLOGY (HPA masked): PASS\n");

	/* 5. Test QUERY_DEV_MEM for CID 15 */
	memset(&req, 0, sizeof(req));
	req.query_op = AMBA_VIRT_QUERY_DEV_MEM;
	req.dev_id = AMBA_VIRT_DEV_TYPE_CAVALRY;
	ret = virt_query_handle_req(15, &req, &resp);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.count == 1);
	struct amba_virt_dev_mem_desc *ddesc = (struct amba_virt_dev_mem_desc *)resp.payload;
	assert(ddesc->dev_type == AMBA_VIRT_DEV_TYPE_CAVALRY);
	assert(ddesc->base_offset == 0x04000000);
	assert(ddesc->size == 512 * 1024 * 1024);
	printf("QUERY_DEV_MEM: PASS\n");

	virt_acl_cleanup();
	virt_mem_pool_cleanup();

	printf("=== All virt_query unit tests passed successfully! ===\n");
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
