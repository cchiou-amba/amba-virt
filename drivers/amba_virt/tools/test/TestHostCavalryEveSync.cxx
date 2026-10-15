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
#include "cavalry_ioctl.h"
int cavalry_proxy_get_last_mfd_dag(uint32_t dag_idx,
                                   struct cavalry_dag_desc_mfd *out_dag) __attribute__((weak));
}

#include <sys/mman.h>

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
 * Eight 1 GiB slices from an 8 GiB window keep slice 0 at the parent base
 * and slice 1 one gibibyte above it. Counts outside 1..8 are rejected.
 */
TEST(CavalryEveSync, test_eight_one_gib_slices)
{
    const uint32_t page_sz = 4096;
    const uint64_t gib = 1ULL << 30;
    struct amba_virt_slice_list list;
    int ret;

    memset(&list, 0, sizeof(list));
    ret = amba_virt_slice_compute_geometry(0x100000000ULL, 8 * gib, page_sz, 8, &list);
    LONGS_EQUAL(0, ret);
    LONGS_EQUAL(8, list.count);
    UNSIGNED_LONGS_EQUAL(0, list.slices[0].index);
    UNSIGNED_LONGS_EQUAL(gib, list.slices[0].slice_size);
    UNSIGNED_LONGS_EQUAL(gib - page_sz, list.slices[0].usable_size);
    UNSIGNED_LONGS_EQUAL(0x100000000ULL, list.slices[0].phys);
    UNSIGNED_LONGS_EQUAL(0x140000000ULL, list.slices[1].phys);
    UNSIGNED_LONGS_EQUAL(0x100000000ULL + 7 * gib, list.slices[7].phys);

    memset(&list, 0, sizeof(list));
    ret = amba_virt_slice_compute_geometry(0x100000000ULL, 8 * gib, page_sz, 0, &list);
    CHECK_TRUE(ret < 0 || list.count == 0);
    memset(&list, 0, sizeof(list));
    ret = amba_virt_slice_compute_geometry(0x100000000ULL, 9 * gib, page_sz, 9, &list);
    CHECK_TRUE(ret < 0 || list.count == 0);
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
 * Envelope 4: Claim and register for every slice index 0 to 7;
 * the slice view (shm_map) and 64-bit slice_offset exist and are exact for each.
 */
TEST(CavalryEveSync, test_claim_and_register_slices_0_to_7)
{
    const uint64_t gib = 1ULL << 30;
    const uint64_t win_bytes = 8 * gib;
    const uint64_t phys_base = 0x100000000ULL;

    unsigned char *win_map = static_cast<unsigned char *>(
        mmap(nullptr, win_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    CHECK_TRUE(win_map != MAP_FAILED);

    cavalry_proxy_cleanup();
    int ret = cavalry_proxy_init(-1, win_map, win_bytes, phys_base);
    LONGS_EQUAL(0, ret);

    for (uint32_t i = 0; i < 8; i++) {
        uint64_t nonce = 0x1111000000000000ULL + i + 1;
        uint32_t cid = 30 + i;
        struct amba_virt_slice_desc desc;
        memset(&desc, 0, sizeof(desc));

        cavalry_proxy_set_slice_nonce(i, nonce);
        ret = cavalry_proxy_bind_slice(cid, nonce, &desc);
        LONGS_EQUAL(0, ret);
        UNSIGNED_LONGS_EQUAL(i, desc.index);
        UNSIGNED_LONGS_EQUAL(i * gib, desc.offset);
        UNSIGNED_LONGS_EQUAL(phys_base + i * gib, desc.phys);
        UNSIGNED_LONGS_EQUAL(gib, desc.slice_size);
        UNSIGNED_LONGS_EQUAL(gib - 4096U, desc.usable_size);

        struct cavalry_tenant_ctx *tenant = cavalry_proxy_get_tenant(cid);
        CHECK_TRUE(tenant != nullptr);
        POINTERS_EQUAL(win_map + i * gib, tenant->shm_map);
        UNSIGNED_LONGS_EQUAL(i * gib, static_cast<uint64_t>(tenant->slice_offset));
        UNSIGNED_LONGS_EQUAL(phys_base + i * gib, tenant->phys_base);
    }

    munmap(win_map, win_bytes);
}

/*
 * Envelope 4: DAG register and run translate to the correct absolute 64-bit offset
 * for indexes 0, 3, 4 and 7, and two tenants never receive overlapping translated ranges.
 */
TEST(CavalryEveSync, test_dag_register_and_run_slices_0_3_4_7)
{
    const uint64_t gib = 1ULL << 30;
    const uint64_t win_bytes = 8 * gib;
    const uint64_t phys_base = 0x100000000ULL;
    const uint32_t indices[4] = { 0, 3, 4, 7 };

    CHECK_TRUE_TEXT(cavalry_proxy_get_last_mfd_dag != nullptr,
                    "cavalry_proxy_get_last_mfd_dag missing");

    unsigned char *win_map = static_cast<unsigned char *>(
        mmap(nullptr, win_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    CHECK_TRUE(win_map != MAP_FAILED);

    cavalry_proxy_cleanup();
    LONGS_EQUAL(0, cavalry_proxy_init(-1, win_map, win_bytes, phys_base));

    uint64_t translated_dvi_start[4] = { 0 };
    uint64_t translated_dvi_end[4] = { 0 };
    uint64_t translated_port_start[4] = { 0 };
    uint64_t translated_port_end[4] = { 0 };

    for (size_t k = 0; k < 4; k++) {
        uint32_t idx = indices[k];
        uint32_t cid = 40 + idx;
        uint64_t nonce = 0x2222000000000000ULL + idx + 1;
        struct amba_virt_slice_desc desc;
        memset(&desc, 0, sizeof(desc));

        cavalry_proxy_set_slice_nonce(idx, nonce);
        LONGS_EQUAL(0, cavalry_proxy_bind_slice(cid, nonce, &desc));

        struct cavalry_tenant_ctx *tenant = cavalry_proxy_get_tenant(cid);
        CHECK_TRUE(tenant != nullptr);
        UNSIGNED_LONGS_EQUAL(idx * gib, static_cast<uint64_t>(tenant->slice_offset));

        /* 1. Allocate a 64 KiB buffer inside the tenant's Cavalry pool */
        struct amba_virt_cavalry_rpc req, resp;
        memset(&req, 0, sizeof(req));
        req.opcode = VCAV_OP_ALLOC_MEM;
        req.session_id = 1;
        req.size = 65536;
        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        LONGS_EQUAL(0, resp.status);
        uint32_t buf_off = resp.bar_offset;
        CHECK_TRUE(buf_off >= CAVALRY_POOL_BASE);

        /* 2. Build a Path A RUN_DAGS request in the tenant's RPC arena */
        size_t run_bytes = sizeof(struct cavalry_run_dags) + sizeof(struct cavalry_dag_desc);
        struct cavalry_run_dags *run_req = reinterpret_cast<struct cavalry_run_dags *>(
            tenant->shm_map + tenant->rpc_arena_offset);
        memset(run_req, 0, run_bytes);
        run_req->dag_cnt = 1;
        run_req->session_id = 1;
        run_req->dag_desc[0].dvi_dram_addr = buf_off;
        run_req->dag_desc[0].dvi_img_size = 4096;
        run_req->dag_desc[0].port_cnt = 1;
        run_req->dag_desc[0].port_desc[0].port_dram_addr = buf_off + 4096;
        run_req->dag_desc[0].port_desc[0].port_dram_size = 4096;

        memset(&req, 0, sizeof(req));
        req.opcode = VCAV_OP_RUN_DAGS;
        req.session_id = 1;
        req.bar_offset = tenant->rpc_arena_offset;
        req.arena_len = static_cast<uint32_t>(run_bytes);
        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        LONGS_EQUAL(0, resp.status);

        struct cavalry_dag_desc_mfd mfd_dag;
        memset(&mfd_dag, 0, sizeof(mfd_dag));
        LONGS_EQUAL(0, cavalry_proxy_get_last_mfd_dag(0, &mfd_dag));
        UNSIGNED_LONGS_EQUAL(idx * gib + buf_off, mfd_dag.dvi_dram_addr_offset);
        UNSIGNED_LONGS_EQUAL(idx * gib + buf_off + 4096, mfd_dag.port_desc[0].port_dram_addr);

        translated_dvi_start[k] = mfd_dag.dvi_dram_addr_offset;
        translated_dvi_end[k] = mfd_dag.dvi_dram_addr_offset + 4096;

        /* 3. Register DAG (Path B) and run with handles */
        size_t reg_bytes = sizeof(struct amba_virt_cavalry_reg_dag_desc) + run_bytes;
        auto *reg_desc = reinterpret_cast<struct amba_virt_cavalry_reg_dag_desc *>(
            tenant->shm_map + tenant->rpc_arena_offset);
        memset(reg_desc, 0, reg_bytes);
        reg_desc->staging_bar_offset = buf_off;
        reg_desc->staging_size = 16384;
        reg_desc->run_dags_bytes = static_cast<uint32_t>(run_bytes);
        struct cavalry_run_dags *reg_run = static_cast<struct cavalry_run_dags *>(calloc(1, run_bytes));
        CHECK_TRUE(reg_run != nullptr);
        reg_run->dag_cnt = 1;
        reg_run->dag_desc[0].dvi_dram_addr = buf_off;
        reg_run->dag_desc[0].dvi_img_size = 4096;
        reg_run->dag_desc[0].port_cnt = 2;
        reg_run->dag_desc[0].port_desc[0].port_dram_addr = buf_off + 4096;
        reg_run->dag_desc[0].port_desc[0].port_dram_size = 4096;
        reg_run->dag_desc[0].port_desc[1].port_dram_addr = buf_off + 8192;
        reg_run->dag_desc[0].port_desc[1].port_dram_size = 4096;
        memcpy(tenant->shm_map + tenant->rpc_arena_offset + sizeof(*reg_desc), reg_run, run_bytes);
        free(reg_run);

        memset(&req, 0, sizeof(req));
        req.opcode = VCAV_OP_REGISTER_DAG;
        req.session_id = 1;
        req.bar_offset = tenant->rpc_arena_offset;
        req.arena_len = static_cast<uint32_t>(reg_bytes);
        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        LONGS_EQUAL(0, resp.status);
        uint32_t dag_id = resp.dag_id;
        CHECK_TRUE(dag_id > 0);

        memset(&req, 0, sizeof(req));
        req.opcode = VCAV_OP_ALLOC_HANDLE;
        req.session_id = 1;
        req.size = 4096;
        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        LONGS_EQUAL(0, resp.status);
        uint32_t in_hid = resp.dag_id;
        uint32_t in_off = resp.bar_offset;

        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        LONGS_EQUAL(0, resp.status);
        uint32_t out_hid = resp.dag_id;

        void *h_virt = nullptr;
        size_t h_size = 0;
        LONGS_EQUAL(0, cavalry_proxy_get_handle_buffer(in_hid, cid, 1, &h_virt, &h_size));
        POINTERS_EQUAL(win_map + idx * gib + in_off, h_virt);
        UNSIGNED_LONGS_EQUAL(4096, h_size);

        uint32_t ticks = 0, rval = 0;
        LONGS_EQUAL(0, cavalry_proxy_run_dag_with_handles(
            dag_id, in_hid, out_hid, cid, 1, &ticks, &rval, nullptr, nullptr, nullptr));
        memset(&mfd_dag, 0, sizeof(mfd_dag));
        LONGS_EQUAL(0, cavalry_proxy_get_last_mfd_dag(0, &mfd_dag));
        UNSIGNED_LONGS_EQUAL(phys_base + idx * gib + in_off, mfd_dag.port_desc[0].port_dram_addr);

        translated_port_start[k] = mfd_dag.port_desc[0].port_dram_addr;
        translated_port_end[k] = mfd_dag.port_desc[0].port_dram_addr + 4096;
    }

    /* Two tenants never receive overlapping translated ranges */
    for (size_t a = 0; a < 4; a++) {
        for (size_t b = a + 1; b < 4; b++) {
            CHECK_TRUE(translated_dvi_end[a] <= translated_dvi_start[b] ||
                       translated_dvi_end[b] <= translated_dvi_start[a]);
            CHECK_TRUE(translated_port_end[a] <= translated_port_start[b] ||
                       translated_port_end[b] <= translated_port_start[a]);
        }
    }

    munmap(win_map, win_bytes);
}

/*
 * Envelope 4: End of slice 7 is exactly the window end (accepted) and one byte more is refused;
 * guest address plus offset that overflows 64 bits is refused;
 * guest address outside its own slice is refused for every index 0..7.
 */
TEST(CavalryEveSync, test_slice_7_end_overflow_and_out_of_slice_rejected)
{
    const uint64_t gib = 1ULL << 30;
    const uint64_t win_bytes = 8 * gib;
    const uint64_t phys_base = 0x100000000ULL;

    unsigned char *win_map = static_cast<unsigned char *>(
        mmap(nullptr, win_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    CHECK_TRUE(win_map != MAP_FAILED);

    cavalry_proxy_cleanup();
    LONGS_EQUAL(0, cavalry_proxy_init(-1, win_map, win_bytes, phys_base));

    /* End of slice 7 == window end (8 GiB) is accepted */
    LONGS_EQUAL(0, cavalry_proxy_register_tenant(
        77, 7, -1, win_map + 7 * gib, gib, phys_base + 7 * gib, 7 * gib));
    cavalry_proxy_unregister_tenant(77);

    /* One byte beyond window end at slice 7 is refused */
    CHECK_TRUE(cavalry_proxy_register_tenant(
        77, 7, -1, win_map + 7 * gib, gib + 1, phys_base + 7 * gib, 7 * gib) < 0);
    CHECK_TRUE(cavalry_proxy_register_tenant(
        77, 7, -1, win_map + 7 * gib, gib, phys_base + 7 * gib, 7 * gib + 1) < 0);

    /* 64-bit addition overflow on slice_offset + shm_size is refused */
    CHECK_TRUE(cavalry_proxy_register_tenant(
        77, 7, -1, nullptr, CAVALRY_POOL_BASE + 4096U, phys_base, UINT64_MAX - 1024ULL) < 0);

    /* For every index 0..7: out-of-slice guest address and 64-bit overflow guest address refused */
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t cid = 60 + i;
        uint64_t nonce = 0x3333000000000000ULL + i + 1;
        struct amba_virt_slice_desc desc;
        memset(&desc, 0, sizeof(desc));

        cavalry_proxy_set_slice_nonce(i, nonce);
        LONGS_EQUAL(0, cavalry_proxy_bind_slice(cid, nonce, &desc));
        struct cavalry_tenant_ctx *tenant = cavalry_proxy_get_tenant(cid);
        CHECK_TRUE(tenant != nullptr);

        struct amba_virt_cavalry_rpc req, resp;
        memset(&req, 0, sizeof(req));
        req.opcode = VCAV_OP_ALLOC_MEM;
        req.session_id = 1;
        req.size = 65536;
        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        LONGS_EQUAL(0, resp.status);
        uint32_t valid_off = resp.bar_offset;

        size_t run_bytes = sizeof(struct cavalry_run_dags) + sizeof(struct cavalry_dag_desc);
        auto *run_req = reinterpret_cast<struct cavalry_run_dags *>(
            tenant->shm_map + tenant->rpc_arena_offset);

        /* Case A: guest address outside slice (e.g. >= 1 GiB or upper 32 bits set) */
        memset(run_req, 0, run_bytes);
        run_req->dag_cnt = 1;
        run_req->session_id = 1;
        run_req->dag_desc[0].dvi_dram_addr = (1ULL << 32) | valid_off;
        run_req->dag_desc[0].dvi_img_size = 4096;
        memset(&req, 0, sizeof(req));
        req.opcode = VCAV_OP_RUN_DAGS;
        req.session_id = 1;
        req.bar_offset = tenant->rpc_arena_offset;
        req.arena_len = static_cast<uint32_t>(run_bytes);
        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        CHECK_TRUE(resp.status < 0);

        /* Case B: guest address + slice_offset overflows 64 bits */
        memset(run_req, 0, run_bytes);
        run_req->dag_cnt = 1;
        run_req->session_id = 1;
        run_req->dag_desc[0].dvi_dram_addr = UINT64_MAX - (i * gib) + 4096ULL;
        run_req->dag_desc[0].dvi_img_size = 4096;
        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        CHECK_TRUE(resp.status < 0);

        /* Case C: guest port address outside slice */
        memset(run_req, 0, run_bytes);
        run_req->dag_cnt = 1;
        run_req->session_id = 1;
        run_req->dag_desc[0].dvi_dram_addr = valid_off;
        run_req->dag_desc[0].dvi_img_size = 4096;
        run_req->dag_desc[0].port_cnt = 1;
        run_req->dag_desc[0].port_desc[0].port_dram_addr = gib;
        run_req->dag_desc[0].port_desc[0].port_dram_size = 4096;
        LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, cid));
        CHECK_TRUE(resp.status < 0);
    }

    munmap(win_map, win_bytes);
}

/*
 * Envelope 4: Restored binding for slices 4 to 7 gets the true 64-bit offset (F10),
 * 32-bit saturated window_size fails for high slices, and a stale tenant after restart
 * does not keep another index's view.
 */
TEST(CavalryEveSync, test_restore_bindings_slices_4_to_7_and_stale_tenant)
{
    const uint64_t gib = 1ULL << 30;
    const uint64_t win_bytes = 8 * gib;
    const uint64_t phys_base = 0x100000000ULL;

    unsigned char *win_map = static_cast<unsigned char *>(
        mmap(nullptr, win_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    CHECK_TRUE(win_map != MAP_FAILED);

    cavalry_proxy_cleanup();
    LONGS_EQUAL(0, cavalry_proxy_init(-1, win_map, win_bytes, phys_base));

    /* Register stale tenants on slice 0 (CID 80) and slice 1 (CID 81) */
    LONGS_EQUAL(0, cavalry_proxy_register_tenant(
        80, 0, -1, win_map, gib - 4096U, phys_base, 0));
    LONGS_EQUAL(0, cavalry_proxy_register_tenant(
        81, 1, -1, win_map + gib, gib - 4096U, phys_base + gib, gib));

    /* Build binding list for slices 4..7 (CID 80 moved from slice 0 to slice 4; CID 81 gone) */
    struct amba_virt_binding_list list;
    memset(&list, 0, sizeof(list));
    list.count = 4;
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t idx = 4 + k;
        list.entries[k].cid = (k == 0) ? 80 : (80 + idx);
        list.entries[k].slice.index = idx;
        list.entries[k].slice.usable_size = gib - 4096U;
        list.entries[k].slice.offset = idx * gib;
        list.entries[k].slice.phys = phys_base + idx * gib;
        list.entries[k].slice.slice_size = gib;
    }

    /* 32-bit saturated window_size (0xffffffffU) must fail for slices 4..7 */
    CHECK_TRUE(cavalry_proxy_restore_bindings(&list, win_map, 0xffffffffU) < 0);

    /* True 64-bit window_size succeeds and restores exact 64-bit offsets and views */
    LONGS_EQUAL(0, cavalry_proxy_restore_bindings(&list, win_map, win_bytes));

    /* CID 81 is no longer present */
    CHECK_TRUE(cavalry_proxy_get_tenant(81) == nullptr);

    /* CID 80 now has slice 4's view and offset, NOT slice 0's */
    struct cavalry_tenant_ctx *t80 = cavalry_proxy_get_tenant(80);
    CHECK_TRUE(t80 != nullptr);
    UNSIGNED_LONGS_EQUAL(4, t80->tenant_idx);
    UNSIGNED_LONGS_EQUAL(4 * gib, static_cast<uint64_t>(t80->slice_offset));
    POINTERS_EQUAL(win_map + 4 * gib, t80->shm_map);

    for (uint32_t idx = 5; idx <= 7; idx++) {
        struct cavalry_tenant_ctx *t = cavalry_proxy_get_tenant(80 + idx);
        CHECK_TRUE(t != nullptr);
        UNSIGNED_LONGS_EQUAL(idx, t->tenant_idx);
        UNSIGNED_LONGS_EQUAL(idx * gib, static_cast<uint64_t>(t->slice_offset));
        POINTERS_EQUAL(win_map + idx * gib, t->shm_map);
    }

    munmap(win_map, win_bytes);
}

/*
 * Envelope 4: Embedded change qualification contracts — zero length, maximum length,
 * stale handle, foreign handle, duplicate release, and teardown during use.
 */
TEST(CavalryEveSync, test_qualification_contracts_handles_and_bounds)
{
    const uint64_t gib = 1ULL << 30;
    const uint64_t win_bytes = 8 * gib;
    const uint64_t phys_base = 0x100000000ULL;

    unsigned char *win_map = static_cast<unsigned char *>(
        mmap(nullptr, win_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    CHECK_TRUE(win_map != MAP_FAILED);

    cavalry_proxy_cleanup();
    LONGS_EQUAL(0, cavalry_proxy_init(-1, win_map, win_bytes, phys_base));

    struct amba_virt_slice_desc desc0, desc4;
    cavalry_proxy_set_slice_nonce(0, 0xAAAA0001ULL);
    cavalry_proxy_set_slice_nonce(4, 0xAAAA0005ULL);
    LONGS_EQUAL(0, cavalry_proxy_bind_slice(90, 0xAAAA0001ULL, &desc0));
    LONGS_EQUAL(0, cavalry_proxy_bind_slice(94, 0xAAAA0005ULL, &desc4));

    struct amba_virt_cavalry_rpc req, resp;

    /* 1. Zero length allocation refused */
    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_ALLOC_MEM;
    req.session_id = 1;
    req.size = 0;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(-EINVAL, resp.status);

    req.opcode = VCAV_OP_ALLOC_HANDLE;
    req.size = 0;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(-EINVAL, resp.status);

    /* 2. Maximum length: tenant->cavalry_pool_size accepted, + 4096 refused */
    const struct cavalry_tenant_ctx *tenant90 = cavalry_proxy_get_tenant(90);
    CHECK_TRUE(tenant90 != nullptr);
    req.opcode = VCAV_OP_ALLOC_MEM;
    req.size = tenant90->cavalry_pool_size + 4096U;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(-ENOMEM, resp.status);

    req.size = tenant90->cavalry_pool_size;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(0, resp.status);
    uint32_t max_off = resp.bar_offset;

    req.opcode = VCAV_OP_FREE_MEM;
    req.bar_offset = max_off;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(0, resp.status);

    /* Duplicate release of memory refused */
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(-EINVAL, resp.status);

    /* 3. Handle lifecycle: foreign handle (-EACCES), stale handle, duplicate release */
    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_ALLOC_HANDLE;
    req.session_id = 1;
    req.size = 4096;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(0, resp.status);
    uint32_t hid90 = resp.dag_id;

    /* Foreign tenant (CID 94) denied access to CID 90's handle */
    void *virt = nullptr;
    size_t sz = 0;
    LONGS_EQUAL(-EACCES, cavalry_proxy_get_handle_buffer(hid90, 94, 1, &virt, &sz));

    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_FREE_HANDLE;
    req.session_id = 1;
    req.dag_id = hid90;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 94));
    LONGS_EQUAL(-EACCES, resp.status);

    /* Owner frees handle */
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(0, resp.status);

    /* Duplicate free of handle refused */
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    LONGS_EQUAL(-EINVAL, resp.status);

    /* Stale handle lookup refused */
    LONGS_EQUAL(-ENOENT, cavalry_proxy_get_handle_buffer(hid90, 90, 1, &virt, &sz));

    /* 4. Teardown during use reclaims resources without affecting other tenant */
    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_ALLOC_HANDLE;
    req.session_id = 2;
    req.size = 4096;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 90));
    uint32_t active_hid90 = resp.dag_id;
    LONGS_EQUAL(0, cavalry_proxy_handle_rpc(&req, &resp, 94));
    uint32_t active_hid94 = resp.dag_id;

    LONGS_EQUAL(0, cavalry_proxy_unregister_tenant(90));
    LONGS_EQUAL(-ENODEV, cavalry_proxy_get_handle_buffer(active_hid90, 90, 2, &virt, &sz));
    LONGS_EQUAL(0, cavalry_proxy_get_handle_buffer(active_hid94, 94, 2, &virt, &sz));

    munmap(win_map, win_bytes);
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

