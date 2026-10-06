/*
 * drivers/amba_virt/tools/test/TestHostCavalryEveSync.cxx
 *
 * CppUTest test suite for Dynamic EVE Shared-Memory Slice Geometry and Nonce Binding.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "CppUTest/TestHarness.h"
#include "CppUTest/CommandLineTestRunner.h"

extern "C" {
#include "cavalry_proxy.h"
#include "virt_mem_pool.h"
#include "amba_virt.h"
#include "amba_virt_slice.h"
}

TEST_GROUP(CavalryEveSync)
{
    void setup() override
    {
        cavalry_proxy_init(-1, nullptr, 0x80000000ULL, 0x100000000ULL);
        virt_mem_pool_init(0x40000000U);
    }

    void teardown() override
    {
        cavalry_proxy_cleanup();
        virt_mem_pool_cleanup();
    }
};

/*
 * Test 1: Window length and usable size
 * Window length 2 * page_size and 2 indices yields 2 slices of page_size,
 * offsets 0 and page_size, and usable_size of 0. Must be rejected.
 * Window of 2 * (page_size + page_size) = 16384 yields 2 usable slices.
 */
TEST(CavalryEveSync, test_window_length_and_usable_size)
{
    uint32_t page_sz = 4096;
    uint32_t slice_cnt = 2;
    struct amba_virt_slice_list list;
    memset(&list, 0, sizeof(list));

    // Case A: 2 * page_size -> usable_size is 0 -> rejected
    uint64_t small_window = 2 * (uint64_t)page_sz;
    int ret = amba_virt_slice_compute_geometry(0x100000000ULL, small_window, page_sz, slice_cnt, &list);
    CHECK_TRUE(ret < 0 || list.count == 0);

    // Case B: 2 * (page_size + page_size) = 16384 -> 2 usable slices
    uint64_t valid_window = 2 * (uint64_t)(page_sz + page_sz);
    memset(&list, 0, sizeof(list));
    ret = amba_virt_slice_compute_geometry(0x100000000ULL, valid_window, page_sz, slice_cnt, &list);
    LONGS_EQUAL(0, ret);
    LONGS_EQUAL(2, list.count);

    // Slice 0
    LONGS_EQUAL(0, list.slices[0].index);
    LONGS_EQUAL(8192, list.slices[0].slice_size);
    LONGS_EQUAL(4096, list.slices[0].usable_size);
    LONGS_EQUAL(0, list.slices[0].offset);
    LONGS_EQUAL(0x100000000ULL, list.slices[0].phys);

    // Slice 1
    LONGS_EQUAL(1, list.slices[1].index);
    LONGS_EQUAL(8192, list.slices[1].slice_size);
    LONGS_EQUAL(4096, list.slices[1].usable_size);
    LONGS_EQUAL(8192, list.slices[1].offset);
    LONGS_EQUAL(0x100002000ULL, list.slices[1].phys);
}

/*
 * Test 2: Unaligned or uneven window produces an empty list
 */
TEST(CavalryEveSync, test_unaligned_or_uneven_window_rejected)
{
    uint32_t page_sz = 4096;
    uint32_t slice_cnt = 2;
    struct amba_virt_slice_list list;

    // Case A: Not page-aligned (e.g. 8193 bytes)
    memset(&list, 0, sizeof(list));
    int ret = amba_virt_slice_compute_geometry(0x100000000ULL, 8193, page_sz, slice_cnt, &list);
    CHECK_TRUE(ret < 0 || list.count == 0);

    // Case B: Does not divide evenly (e.g. 3 pages into 2 slices)
    memset(&list, 0, sizeof(list));
    ret = amba_virt_slice_compute_geometry(0x100000000ULL, 3 * page_sz, page_sz, slice_cnt, &list);
    CHECK_TRUE(ret < 0 || list.count == 0);

    // Case C: 64-bit overflow check (slice_size * slice_count overflow)
    memset(&list, 0, sizeof(list));
    ret = amba_virt_slice_compute_geometry(0x100000000ULL, 0xFFFFFFFFFFFFF000ULL, page_sz, slice_cnt, &list);
    CHECK_TRUE(ret < 0 || list.count == 0);
}

/*
 * Test 3: Nonce A binds CID 10 to slice 0, CID 11 with nonce A returns busy,
 * and Nonce B binds CID 11 to slice 1.
 */
TEST(CavalryEveSync, test_nonce_binding_and_busy_rejection)
{
    uint64_t nonce_a = 0xA1A2A3A4B1B2B3B4ULL;
    uint64_t nonce_b = 0xC1C2C3C4D1D2D3D4ULL;

    cavalry_proxy_set_slice_nonce(0, nonce_a);
    cavalry_proxy_set_slice_nonce(1, nonce_b);

    // Direct bind request on cavalry_proxy / amba-virt-server slice binding API
    struct amba_virt_slice_bind bind_req;
    memset(&bind_req, 0, sizeof(bind_req));
    bind_req.cid = 10;
    bind_req.nonce = nonce_a;

    int ret = cavalry_proxy_bind_slice(10, nonce_a, &bind_req.slice);
    LONGS_EQUAL(0, ret);
    LONGS_EQUAL(0, bind_req.slice.index);

    // CID 10 lookup returns valid descriptor
    struct cavalry_tenant_ctx *t10 = cavalry_proxy_get_tenant(10);
    CHECK_TRUE(t10 != nullptr);
    LONGS_EQUAL(10, t10->cid);
    LONGS_EQUAL(0, t10->tenant_idx);

    // CID 11 trying to claim slice 0 with Nonce A returns busy (-EBUSY)
    memset(&bind_req, 0, sizeof(bind_req));
    bind_req.cid = 11;
    bind_req.nonce = nonce_a;
    ret = cavalry_proxy_bind_slice(11, nonce_a, &bind_req.slice);
    LONGS_EQUAL(-EBUSY, ret);

    // Nonce B binds CID 11 to slice 1
    memset(&bind_req, 0, sizeof(bind_req));
    bind_req.cid = 11;
    bind_req.nonce = nonce_b;
    ret = cavalry_proxy_bind_slice(11, nonce_b, &bind_req.slice);
    LONGS_EQUAL(0, ret);
    LONGS_EQUAL(1, bind_req.slice.index);

    struct cavalry_tenant_ctx *t11 = cavalry_proxy_get_tenant(11);
    CHECK_TRUE(t11 != nullptr);
    LONGS_EQUAL(11, t11->cid);
    LONGS_EQUAL(1, t11->tenant_idx);
}

/*
 * Test 4: Cleared CID removes binding, nonce is invalidated, and new nonce binds new CID
 */
TEST(CavalryEveSync, test_cleared_cid_invalidates_nonce)
{
    uint64_t nonce_a = 0xA1A2A3A4B1B2B3B4ULL;
    struct amba_virt_slice_desc desc;

    cavalry_proxy_set_slice_nonce(0, nonce_a);

    int ret = cavalry_proxy_bind_slice(10, nonce_a, &desc);
    LONGS_EQUAL(0, ret);

    // Clear CID 10
    ret = cavalry_proxy_unregister_tenant(10);
    LONGS_EQUAL(0, ret);

    // Lookup of CID 10 is empty
    struct cavalry_tenant_ctx *t10 = cavalry_proxy_get_tenant(10);
    CHECK_TRUE(t10 == nullptr);

    // Nonce A is no longer valid (must return -ENOENT or -EBUSY, cannot re-bind)
    ret = cavalry_proxy_bind_slice(12, nonce_a, &desc);
    CHECK_TRUE(ret != 0);

    // A fresh nonce for slice 0 successfully binds new CID 12
    uint64_t nonce_fresh = 0xF1F2F3F4E1E2E3E4ULL;
    cavalry_proxy_set_slice_nonce(0, nonce_fresh);

    ret = cavalry_proxy_bind_slice(12, nonce_fresh, &desc);
    LONGS_EQUAL(0, ret);
    LONGS_EQUAL(0, desc.index);

    struct cavalry_tenant_ctx *t12 = cavalry_proxy_get_tenant(12);
    CHECK_TRUE(t12 != nullptr);
    LONGS_EQUAL(12, t12->cid);
    LONGS_EQUAL(0, t12->tenant_idx);
}

/*
 * Test 5: Unknown nonce and CID 2 do not create a tenant
 */
TEST(CavalryEveSync, test_unknown_nonce_and_cid_2_rejection)
{
    struct amba_virt_slice_desc desc;

    // Unknown random nonce
    uint64_t invalid_nonce = 0xDEADBEEF00000000ULL;
    int ret = cavalry_proxy_bind_slice(15, invalid_nonce, &desc);
    LONGS_EQUAL(-ENOENT, ret);
    CHECK_TRUE(cavalry_proxy_get_tenant(15) == nullptr);

    // CID 0, 1, 2 are rejected
    uint64_t nonce_a = 0xA1A2A3A4B1B2B3B4ULL;
    cavalry_proxy_set_slice_nonce(0, nonce_a);

    ret = cavalry_proxy_bind_slice(2, nonce_a, &desc);
    LONGS_EQUAL(-EINVAL, ret);
    CHECK_TRUE(cavalry_proxy_get_tenant(2) == nullptr);

    ret = cavalry_proxy_bind_slice(1, nonce_a, &desc);
    LONGS_EQUAL(-EINVAL, ret);

    ret = cavalry_proxy_bind_slice(0, nonce_a, &desc);
    LONGS_EQUAL(-EINVAL, ret);
}

/*
 * Test 6: virt_mem_pool_get_tenant() of an unbound CID returns NULL
 */
TEST(CavalryEveSync, test_virt_mem_pool_unbound_cid_returns_null)
{
    // Querying an unregistered CID must strictly return NULL without auto-allocating
    struct virt_tenant_pool *pool = virt_mem_pool_get_tenant(9999);
    CHECK_TRUE(pool == nullptr);
}

/*
 * Test 7: cavalry_proxy.c contains neither legacy UUID
 */
TEST(CavalryEveSync, test_cavalry_proxy_contains_neither_legacy_uuid)
{
    FILE *f = fopen("cavalry_proxy.c", "r");
    if (!f)
        f = fopen("../cavalry_proxy.c", "r");
    if (!f)
        f = fopen("drivers/amba_virt/tools/cavalry_proxy.c", "r");
    CHECK_TRUE(f != nullptr);

    char line[512];
    bool has_legacy_ubuntu = false;
    bool has_legacy_alpine = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "6b43e816-fbba-4abf-a2df-81964577931c"))
            has_legacy_ubuntu = true;
        if (strstr(line, "2721e105-6011-4b5e-81c6-3b18f4cec9e3"))
            has_legacy_alpine = true;
    }
    fclose(f);

    CHECK_FALSE_TEXT(has_legacy_ubuntu, "Legacy Ubuntu UUID 6b43e816 found in cavalry_proxy.c");
    CHECK_FALSE_TEXT(has_legacy_alpine, "Legacy Alpine UUID 2721e105 found in cavalry_proxy.c");
}

/*
 * Test 8: Slice-list compare function
 * An enumerated list matching the window is accepted; wrong offset or empty list is rejected.
 */
TEST(CavalryEveSync, test_slice_list_compare_function)
{
    struct amba_virt_slice_list expected;
    struct amba_virt_slice_list enumerated;
    memset(&expected, 0, sizeof(expected));
    memset(&enumerated, 0, sizeof(enumerated));

    expected.count = 2;
    expected.slices[0].index = 0;
    expected.slices[0].offset = 0;
    expected.slices[0].phys = 0x100000000ULL;
    expected.slices[0].usable_size = 0x3FFFF000U;
    expected.slices[0].slice_size = 0x40000000U;

    expected.slices[1].index = 1;
    expected.slices[1].offset = 0x40000000U;
    expected.slices[1].phys = 0x140000000ULL;
    expected.slices[1].usable_size = 0x3FFFF000U;
    expected.slices[1].slice_size = 0x40000000U;

    enumerated = expected;

    // Exact match must return 1 (accept)
    int match = amba_virt_slice_compare_list(&expected, &enumerated);
    LONGS_EQUAL(1, match);

    // Mismatched offset in slice 1 must return 0 (reject)
    enumerated.slices[1].offset = 0x40001000U;
    match = amba_virt_slice_compare_list(&expected, &enumerated);
    LONGS_EQUAL(0, match);

    // Empty list for nonzero window must return 0 (reject)
    memset(&enumerated, 0, sizeof(enumerated));
    match = amba_virt_slice_compare_list(&expected, &enumerated);
    LONGS_EQUAL(0, match);
}

/*
 * Register path clamps the Cavalry pool to the usable slice.
 * A usable size that does not extend past the pool base is not published.
 */
TEST(CavalryEveSync, test_register_tenant_clamps_pool)
{
    struct cavalry_tenant_ctx *tenant;
    uint32_t clamped;
    int ret;

    ret = cavalry_proxy_register_tenant(10, 0, -1, nullptr, CAVALRY_POOL_BASE, 0, 0);
    LONGS_EQUAL(-EINVAL, ret);
    CHECK_TRUE(cavalry_proxy_get_tenant(10) == nullptr);

    clamped = CAVALRY_POOL_BASE + 0x1000U;
    ret = cavalry_proxy_register_tenant(10, 0, -1, nullptr, clamped, 0x1000, 0);
    LONGS_EQUAL(0, ret);
    tenant = cavalry_proxy_get_tenant(10);
    CHECK_TRUE(tenant != nullptr);
    LONGS_EQUAL(0x1000U, tenant->cavalry_pool_size);
    LONGS_EQUAL(CAVALRY_POOL_BASE, tenant->cavalry_pool_base);

    cavalry_proxy_unregister_tenant(10);
    clamped = CAVALRY_POOL_BASE + CAVALRY_POOL_SIZE + 0x1000U;
    ret = cavalry_proxy_register_tenant(11, 1, -1, nullptr, clamped, 0x2000, 0);
    LONGS_EQUAL(0, ret);
    tenant = cavalry_proxy_get_tenant(11);
    CHECK_TRUE(tenant != nullptr);
    LONGS_EQUAL(CAVALRY_POOL_SIZE, tenant->cavalry_pool_size);
}

/*
 * Pool registration and lookup do not initialize the pool themselves.
 */
TEST(CavalryEveSync, test_pool_rejects_use_before_init)
{
    virt_mem_pool_cleanup();

    LONGS_EQUAL(-EINVAL, virt_mem_pool_register_tenant(10, 0, 4096, 4096));
    CHECK_TRUE(virt_mem_pool_get_tenant(10) == nullptr);

    virt_mem_pool_init(0x40000000U);
}

/*
 * A binding list entry keeps its CID when the server restores it.
 */
TEST(CavalryEveSync, test_restore_binding_keeps_cid)
{
    struct amba_virt_binding_list list;
    struct cavalry_tenant_ctx *tenant;
    int ret;

    memset(&list, 0, sizeof(list));
    list.count = 1;
    list.entries[0].cid = 21;
    list.entries[0].slice.index = 0;
    list.entries[0].slice.usable_size = CAVALRY_POOL_BASE + 0x2000U;
    list.entries[0].slice.offset = 0;
    list.entries[0].slice.phys = 0x100000000ULL;
    list.entries[0].slice.slice_size = list.entries[0].slice.usable_size + 4096U;

    ret = cavalry_proxy_restore_bindings(&list, nullptr, 0);
    LONGS_EQUAL(0, ret);

    tenant = cavalry_proxy_get_tenant(21);
    CHECK_TRUE(tenant != nullptr);
    LONGS_EQUAL(21, tenant->cid);
    LONGS_EQUAL(0, tenant->tenant_idx);
    LONGS_EQUAL(0x100000000ULL, tenant->phys_base);
    LONGS_EQUAL(0x2000U, tenant->cavalry_pool_size);
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
