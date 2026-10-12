/*
 * drivers/amba_virt/tools/test/TestHostIavLowSlice.cxx
 *
 * Slice geometry for the bases Envelope A recorded.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>

#include "CppUTest/TestHarness.h"

extern "C" {
#include "iav_low_slice.h"
}

static const uint64_t kDevkit = 0xb4000000ull;
static const uint64_t kPro = 0x94000000ull;

TEST_GROUP(IavLowSlice)
{
    struct iav_low_slice slices[IAV_LOW_SLICE_COUNT];

    void setup()
    {
        CHECK_EQUAL(0, iav_low_slice_init(slices, IAV_LOW_SLICE_COUNT,
                                          kDevkit, IAV_LOW_SLICE_COUNT * IAV_LOW_SLICE_BYTES));
    }
};

TEST(IavLowSlice, pro_and_devkit_bases_publish_eight_slices)
{
    struct iav_low_slice pro[IAV_LOW_SLICE_COUNT];

    CHECK_EQUAL(0, iav_low_slice_init(pro, IAV_LOW_SLICE_COUNT, kPro,
                                      IAV_LOW_SLICE_COUNT * IAV_LOW_SLICE_BYTES));
    UNSIGNED_LONGS_EQUAL(kDevkit + 7 * IAV_LOW_SLICE_BYTES, slices[7].phys);
    UNSIGNED_LONGS_EQUAL(kPro, pro[0].phys);
    CHECK_EQUAL(-EINVAL, iav_low_slice_init(slices, IAV_LOW_SLICE_COUNT,
                                            kDevkit + 1, IAV_LOW_SLICE_COUNT * IAV_LOW_SLICE_BYTES));
    CHECK_EQUAL(-EINVAL, iav_low_slice_init(slices, IAV_LOW_SLICE_COUNT, kDevkit, 4096));
}

TEST(IavLowSlice, claim_release_and_foreign_owner)
{
    uint64_t nonce = 0;

    CHECK_EQUAL(0, iav_low_slice_claim(&slices[3], 9501, &nonce));
    CHECK(nonce != 0);
    CHECK_EQUAL(-EBUSY, iav_low_slice_claim(&slices[3], 9502, &nonce));
    CHECK_EQUAL(-EPERM, iav_low_slice_release(&slices[3], 9502));
    CHECK_EQUAL(0, iav_low_slice_release(&slices[3], 9501));
    CHECK_EQUAL(-EINVAL, iav_low_slice_release(&slices[3], 9501));
}

TEST(IavLowSlice, length_overflow_and_stale_owner)
{
    uint64_t nonce = 0;

    CHECK_EQUAL(-EPERM, iav_low_slice_check(&slices[1], 1, 0, 4096));
    CHECK_EQUAL(0, iav_low_slice_claim(&slices[1], 1, &nonce));
    CHECK_EQUAL(-EINVAL, iav_low_slice_check(&slices[1], 1, 0, 0));
    CHECK_EQUAL(-EINVAL, iav_low_slice_check(&slices[1], 1, 1, 4096));
    CHECK_EQUAL(0, iav_low_slice_check(&slices[1], 1, 0, IAV_LOW_SLICE_USABLE));
    CHECK_EQUAL(-EPERM, iav_low_slice_check(&slices[1], 1, IAV_LOW_SLICE_USABLE, 4096));
    CHECK_EQUAL(-EPERM, iav_low_slice_check(&slices[1], 2, 0, 4096));
    CHECK_EQUAL(0, iav_low_slice_release(&slices[1], 1));
    CHECK_EQUAL(-EPERM, iav_low_slice_check(&slices[1], 1, 0, 4096));
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
