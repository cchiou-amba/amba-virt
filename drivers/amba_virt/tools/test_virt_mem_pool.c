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

int main(void)
{
	printf("=== Starting virt_mem_pool unit test suite ===\n");
	test_auto_offset();
	test_collision_and_suggested_offset();
	test_quota_exceeded_exact_fails();
	test_quota_exceeded_best_effort_clamps();
	test_duplicate_registration();
	test_bar_overflow();
	test_invalid_alignment_and_unknown_dev();
	test_dynamic_extents();
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
