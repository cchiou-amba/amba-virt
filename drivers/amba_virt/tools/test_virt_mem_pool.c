/*
 * test_virt_mem_pool.c
 *
 * Unit tests for virt_mem_pool dynamic extent manager and conflict resolution.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virt_mem_pool.h"

#define TEST_CID 15

static void test_auto_offset(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* 1. Allocate GDMA 64 MiB AUTO */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
	req.requested_size = 64 * 1024 * 1024;
	req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.err_code == AMBA_VIRT_ERR_NONE);
	assert(resp.granted_size == 64 * 1024 * 1024);
	assert(resp.granted_offset == 0);

	/* 2. Allocate Cavalry 512 MiB AUTO with 1 MiB arena */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 512 * 1024 * 1024;
	req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
	req.rpc_arena_size = 1 * 1024 * 1024;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.err_code == AMBA_VIRT_ERR_NONE);
	assert(resp.granted_size == 512 * 1024 * 1024);
	/* Should be placed after GDMA (64 MiB) */
	assert(resp.granted_offset >= 64 * 1024 * 1024);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_collision_and_suggested_offset(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* 1. Register GDMA at [0x0, 0x04000000) (64 MiB) */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
	req.requested_size = 64 * 1024 * 1024;
	req.preferred_offset = 0x00000000;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0 && resp.status == 0);
	assert(resp.granted_offset == 0);

	/* 2. Request Cavalry explicitly at 0x02000000 (32 MiB) -> Collides with GDMA */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 256 * 1024 * 1024;
	req.preferred_offset = 0x02000000;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -EEXIST);
	assert(resp.err_code == AMBA_VIRT_ERR_OFFSET_COLLISION);
	assert(resp.colliding_dev == AMBA_VIRT_DEV_TYPE_GDMA);
	assert(resp.suggested_offset == 0x04000000);

	/* 3. Re-request at suggested_offset (0x04000000) -> Must SUCCEED */
	req.preferred_offset = resp.suggested_offset;
	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.granted_offset == 0x04000000);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_quota_exceeded_exact_fails(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* Set tenant quota to 512 MiB */
	ret = virt_mem_pool_set_quota(TEST_CID, 512);
	assert(ret == 0);

	/* Request 768 MiB with F_EXACT (under 1 GiB BAR, but over 512 MiB quota) */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 768 * 1024 * 1024;
	req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -ENOMEM);
	assert(resp.err_code == AMBA_VIRT_ERR_QUOTA_EXCEEDED);
	assert(resp.max_avail_size == 512 * 1024 * 1024);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_quota_exceeded_best_effort_clamps(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* Set tenant quota to 512 MiB */
	ret = virt_mem_pool_set_quota(TEST_CID, 512);
	assert(ret == 0);

	/* Request 768 MiB for DEV_TYPE_SCRATCH with F_BEST_EFFORT */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
	req.requested_size = 768 * 1024 * 1024;
	req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
	req.flags = AMBA_VIRT_DEV_F_BEST_EFFORT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.err_code == AMBA_VIRT_ERR_NONE);
	assert(resp.granted_size == 512 * 1024 * 1024);

	/* Cavalry with F_BEST_EFFORT must NOT clamp; exact-or-fail rule */
	ret = virt_mem_pool_release_device_bounds(TEST_CID, AMBA_VIRT_DEV_TYPE_SCRATCH);
	assert(ret == 0);

	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 768 * 1024 * 1024;
	req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
	req.flags = AMBA_VIRT_DEV_F_BEST_EFFORT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -ENOMEM);
	assert(resp.err_code == AMBA_VIRT_ERR_QUOTA_EXCEEDED);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_duplicate_registration(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* Register Cavalry */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 256 * 1024 * 1024;
	req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0 && resp.status == 0);

	/* Re-register without F_REPLACE -> Must fail with -EBUSY */
	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -EBUSY);
	assert(resp.err_code == AMBA_VIRT_ERR_DEV_ALREADY_REG);

	/* Re-register with F_REPLACE -> Must SUCCEED */
	req.flags |= AMBA_VIRT_DEV_F_REPLACE;
	req.requested_size = 512 * 1024 * 1024;
	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.granted_size == 512 * 1024 * 1024);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_bar_overflow(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024); /* 1 GiB BAR */
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* Request 1536 MiB -> Exceeds BAR, must return -ERANGE / BAR_OVERFLOW */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 1536U * 1024U * 1024U;
	req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -ERANGE);
	assert(resp.err_code == AMBA_VIRT_ERR_BAR_OVERFLOW);

	/* Request with offset + size > 1 GiB */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
	req.requested_size = 256 * 1024 * 1024;
	req.preferred_offset = 800 * 1024 * 1024;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -ERANGE);
	assert(resp.err_code == AMBA_VIRT_ERR_BAR_OVERFLOW);

	/* Set quota > BAR -> must fail with -ERANGE */
	ret = virt_mem_pool_set_quota(TEST_CID, 1536);
	assert(ret == -ERANGE);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_invalid_alignment_and_unknown_dev(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* Unknown device type */
	memset(&req, 0, sizeof(req));
	req.dev_type = 99;
	req.requested_size = 64 * 1024 * 1024;
	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -EINVAL);
	assert(resp.err_code == AMBA_VIRT_ERR_UNKNOWN_DEVICE);

	/* Unaligned offset for Cavalry (requires 2 MiB align) */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 64 * 1024 * 1024;
	req.preferred_offset = 0x00100000; /* 1 MiB (not 2 MiB aligned) */
	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -EINVAL);
	assert(resp.err_code == AMBA_VIRT_ERR_INVALID_ALIGN);
	assert(resp.suggested_offset == 0x00200000);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_dynamic_extents(void)
{
	struct amba_virt_mem_req mreq;
	struct amba_virt_mem_resp mresp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* Alloc 16 MiB extent */
	memset(&mreq, 0, sizeof(mreq));
	mreq.op = AMBA_VIRT_MEM_OP_ALLOC;
	mreq.size = 16 * 1024 * 1024;
	ret = virt_mem_pool_alloc_extent(TEST_CID, &mreq, &mresp, NULL, 0x100000000ULL);
	assert(ret == 0);
	assert(mresp.status == 0);
	assert(mresp.allocated_size == 16 * 1024 * 1024);
	assert(mresp.bar_offset == 0);
	assert(mresp.phys_addr == 0x100000000ULL);

	/* Validate range */
	assert(virt_mem_pool_validate_range(TEST_CID, 0, 16 * 1024 * 1024) == 0);
	assert(virt_mem_pool_validate_range(TEST_CID, 0, 17 * 1024 * 1024) == -EFAULT);

	/* Free extent */
	ret = virt_mem_pool_free_extent(TEST_CID, mresp.bar_offset, &mresp);
	assert(ret == 0);
	assert(mresp.status == 0);
	assert(virt_mem_pool_validate_range(TEST_CID, 0, 16 * 1024 * 1024) == -EFAULT);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

/*
 * A restarted guest keeps its slice index and gets a new cid. The previous cid's pool entry must not
 * survive it: otherwise every guest restart leaks one of the eight entries and the ninth claim fails.
 */
static void test_restart_on_same_slice_reclaims_previous_tenant(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	uint32_t cid;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);

	for (cid = 100; cid < 100 + 4 * VIRT_MEM_MAX_TENANTS; cid++) {
		ret = virt_mem_pool_register_tenant(cid, 2, 1024 * 1024 * 1024, 1024 * 1024 * 1024);
		assert(ret == 0);
		assert(virt_mem_pool_get_tenant(cid) != NULL);
		if (cid > 100)
			assert(virt_mem_pool_get_tenant(cid - 1) == NULL);
	}

	/* The previous occupant's allocations do not carry over to the new one */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
	req.requested_size = 64 * 1024 * 1024;
	req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
	req.flags = AMBA_VIRT_DEV_F_EXACT;
	ret = virt_mem_pool_set_device_bounds(cid - 1, &req, &resp, NULL);
	assert(ret == 0 && resp.status == 0);
	ret = virt_mem_pool_register_tenant(cid, 2, 1024 * 1024 * 1024, 1024 * 1024 * 1024);
	assert(ret == 0);
	assert(virt_mem_pool_get_tenant(cid)->allocated_bytes == 0);
	ret = virt_mem_pool_set_device_bounds(cid, &req, &resp, NULL);
	assert(ret == 0 && resp.status == 0 && resp.granted_offset == 0);
	assert(virt_mem_pool_get_tenant(cid - 1) == NULL);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

/* One entry per slice index: the pool still holds every slice at once and refuses a ninth tenant */
static void test_one_entry_per_slice_index(void)
{
	uint32_t idx;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);

	for (idx = 0; idx < VIRT_MEM_MAX_TENANTS; idx++) {
		ret = virt_mem_pool_register_tenant(200 + idx, idx, 1024 * 1024 * 1024, 1024 * 1024 * 1024);
		assert(ret == 0);
	}
	for (idx = 0; idx < VIRT_MEM_MAX_TENANTS; idx++)
		assert(virt_mem_pool_get_tenant(200 + idx) != NULL);

	/* A new cid on a slice index that is not held by anyone has no entry to take: the pool is full */
	ret = virt_mem_pool_register_tenant(300, VIRT_MEM_MAX_TENANTS, 1024 * 1024 * 1024, 1024 * 1024 * 1024);
	assert(ret == -ENOSPC);
	for (idx = 0; idx < VIRT_MEM_MAX_TENANTS; idx++)
		assert(virt_mem_pool_get_tenant(200 + idx) != NULL);

	/* A restart on any held index still works while the pool is full, and touches only that entry */
	ret = virt_mem_pool_register_tenant(301, 3, 1024 * 1024 * 1024, 1024 * 1024 * 1024);
	assert(ret == 0);
	assert(virt_mem_pool_get_tenant(203) == NULL);
	assert(virt_mem_pool_get_tenant(301) != NULL);
	for (idx = 0; idx < VIRT_MEM_MAX_TENANTS; idx++)
		if (idx != 3)
			assert(virt_mem_pool_get_tenant(200 + idx) != NULL);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

/* The same cid registering again, also on another index, keeps one entry and takes over that index */
static void test_same_cid_reregistration_and_index_move(void)
{
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);

	ret = virt_mem_pool_register_tenant(400, 1, 1024 * 1024 * 1024, 1024 * 1024 * 1024);
	assert(ret == 0);
	ret = virt_mem_pool_register_tenant(401, 2, 1024 * 1024 * 1024, 1024 * 1024 * 1024);
	assert(ret == 0);
	ret = virt_mem_pool_register_tenant(400, 1, 0, 0);
	assert(ret == 0);
	assert(virt_mem_pool_get_tenant(400) != NULL && virt_mem_pool_get_tenant(401) != NULL);

	/* 400 moves to index 2: the entry that held index 2 is reclaimed, nothing is duplicated */
	ret = virt_mem_pool_register_tenant(400, 2, 0, 0);
	assert(ret == 0);
	assert(virt_mem_pool_get_tenant(400) != NULL);
	assert(virt_mem_pool_get_tenant(401) == NULL);
	assert(virt_mem_pool_get_tenant(400)->tenant_idx == 2);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

/* Registration before init and a cid that never registered are refused, not crashed on */
static void test_unregistered_and_uninitialised(void)
{
	printf("Running %s...", __func__);
	virt_mem_pool_cleanup();
	assert(virt_mem_pool_register_tenant(500, 0, 1, 1) == -EINVAL);
	virt_mem_pool_init(1024 * 1024 * 1024);
	assert(virt_mem_pool_get_tenant(500) == NULL);
	assert(virt_mem_pool_unregister_tenant(500) == -ENOENT);
	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_collision_retry_with_arena(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* 1. Register GDMA at [0x0, 0x04000000) (64 MiB) */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
	req.requested_size = 64 * 1024 * 1024;
	req.preferred_offset = 0x00000000;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.granted_offset == 0);

	/* 2. Request Cavalry at 0x02000000 (64 MiB, 1 MiB arena) -> Collides with GDMA */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 64 * 1024 * 1024;
	req.preferred_offset = 0x02000000;
	req.rpc_arena_size = 1 * 1024 * 1024;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -EEXIST);
	assert(resp.err_code == AMBA_VIRT_ERR_OFFSET_COLLISION);
	assert(resp.colliding_dev == AMBA_VIRT_DEV_TYPE_GDMA);
	assert(resp.suggested_offset == 0x04000000);

	/* 3. Retry at suggested_offset (0x04000000) -> Must SUCCEED with arena at 0x08000000 */
	req.preferred_offset = resp.suggested_offset;
	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == 0);
	assert(resp.granted_offset == 0x04000000);
	assert(resp.granted_size == 64 * 1024 * 1024);
	assert(resp.rpc_arena_offset == 0x08000000);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

static void test_explicit_arena_at_bar_end(void)
{
	struct amba_virt_dev_bounds_req req;
	struct amba_virt_dev_bounds_resp resp;
	int ret;

	printf("Running %s...", __func__);
	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_mem_pool_register_tenant(TEST_CID, 0, 1024 * 1024 * 1024, 1024 * 1024 * 1024);

	/* 1. Request Cavalry at 0x3FE00000 (2 MiB, 1 MiB arena) -> Arena crosses 1 GiB BAR end */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
	req.requested_size = 2 * 1024 * 1024;
	req.rpc_arena_size = 1 * 1024 * 1024;
	req.preferred_offset = 0x3FE00000;
	req.align = 0;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == -ERANGE);
	assert(resp.err_code == AMBA_VIRT_ERR_BAR_OVERFLOW);

	/* 2. Register GDMA at offset 0 for 4 KiB to prove the rejected call committed nothing */
	memset(&req, 0, sizeof(req));
	req.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
	req.requested_size = 4096;
	req.preferred_offset = 0;
	req.flags = AMBA_VIRT_DEV_F_EXACT;

	ret = virt_mem_pool_set_device_bounds(TEST_CID, &req, &resp, NULL);
	assert(ret == 0);
	assert(resp.status == 0);

	virt_mem_pool_cleanup();
	printf(" PASS\n");
}

int main(void)
{
	printf("=== Starting virt_mem_pool unit test suite ===\n");
	test_auto_offset();
	test_collision_and_suggested_offset();
	test_collision_retry_with_arena();
	test_explicit_arena_at_bar_end();
	test_quota_exceeded_exact_fails();
	test_quota_exceeded_best_effort_clamps();
	test_duplicate_registration();
	test_bar_overflow();
	test_invalid_alignment_and_unknown_dev();
	test_dynamic_extents();
	test_restart_on_same_slice_reclaims_previous_tenant();
	test_one_entry_per_slice_index();
	test_same_cid_reregistration_and_index_move();
	test_unregistered_and_uninitialised();
	printf("=== All virt_mem_pool unit tests passed successfully! ===\n");
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
