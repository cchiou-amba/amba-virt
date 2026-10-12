/*
 * drivers/amba_virt/tools/test/TestHostIavLowPool.cxx
 *
 * Restart reuses a matching 512 MiB partition and refuses a bad one.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdint.h>

#include "CppUTest/TestHarness.h"

extern "C" {
#include "iav_low_pool.h"
}

static struct iav_low_pool_part part(uint64_t addr, uint64_t length)
{
    struct iav_low_pool_part p = {};

    p.addr = addr;
    p.length = length;
    p.from_private = 1;
    p.acs = IAV_LOW_POOL_ACS_PRIVATE;
    p.present = length > 0;
    return p;
}

TEST_GROUP(IavLowPool)
{
};

TEST(IavLowPool, absent_partition_is_allocated_once)
{
    struct iav_low_pool_view view = {};

    LONGS_EQUAL(IAV_LOW_POOL_ALLOC, iav_low_pool_decide(&view, nullptr));
}

TEST(IavLowPool, matching_partition_is_reused)
{
    struct iav_low_pool_part self = part(0x90000000ull, IAV_LOW_POOL_BYTES);
    struct iav_low_pool_part other = part(0x40000000ull, 0x40000000ull);
    struct iav_low_pool_view view = {};
    uint64_t base = 0;

    view.self = self;
    view.others = &other;
    view.nother = 1;
    LONGS_EQUAL(IAV_LOW_POOL_REUSE, iav_low_pool_decide(&view, &base));
    UNSIGNED_LONGS_EQUAL(0x90000000ull, base);
}

TEST(IavLowPool, wrong_size_bank_or_accessibility_is_refused)
{
    struct iav_low_pool_part bad_size = part(0x90000000ull, 4096);
    struct iav_low_pool_part bad_align = part(0x90001000ull, IAV_LOW_POOL_BYTES);
    struct iav_low_pool_part shared = part(0x90000000ull, IAV_LOW_POOL_BYTES);
    struct iav_low_pool_part low = part(0x28000000ull, IAV_LOW_POOL_BYTES);
    struct iav_low_pool_view view = {};

    shared.from_private = 0;
    shared.acs = 0x7;
    view.self = bad_size;
    LONGS_EQUAL(IAV_LOW_POOL_REFUSE, iav_low_pool_decide(&view, nullptr));
    view.self = bad_align;
    LONGS_EQUAL(IAV_LOW_POOL_REFUSE, iav_low_pool_decide(&view, nullptr));
    view.self = shared;
    LONGS_EQUAL(IAV_LOW_POOL_REFUSE, iav_low_pool_decide(&view, nullptr));
    view.self = low;
    LONGS_EQUAL(IAV_LOW_POOL_REFUSE, iav_low_pool_decide(&view, nullptr));
}

TEST(IavLowPool, overlap_is_refused)
{
    struct iav_low_pool_part self = part(0x80000000ull, IAV_LOW_POOL_BYTES);
    struct iav_low_pool_part other = part(0x90000000ull, 0x1000ull);
    struct iav_low_pool_view view = {};

    view.self = self;
    view.others = &other;
    view.nother = 1;
    LONGS_EQUAL(IAV_LOW_POOL_REFUSE, iav_low_pool_decide(&view, nullptr));
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
