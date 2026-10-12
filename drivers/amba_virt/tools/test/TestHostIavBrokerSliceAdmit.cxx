/*
 * drivers/amba_virt/tools/test/TestHostIavBrokerSliceAdmit.cxx
 *
 * A private IAV submission is admitted before the driver is called.
 * A refusal makes zero driver ioctls.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

extern "C" {
#include "virt_iav_guest_slice.h"
}

static const uint64_t kSlice = 64ULL << 20;
static const uint32_t kPart = 193;
static const uint64_t kBase = 0x94000000ULL;
static uint32_t g_window = 0;
static int g_have_window = 1;

static int slice_of(void *ctx, uint32_t cid, uint32_t *window)
{
    (void)ctx;
    if (!g_have_window || cid != 7 || window == NULL)
        return -EPERM;
    *window = g_window;
    return 0;
}

static struct amba_virt_iav_slice_req slice_arg(uint32_t claimed, uint32_t part,
                                                uint64_t offset, uint64_t length,
                                                uint64_t phys)
{
    struct amba_virt_iav_slice_req in;

    memset(&in, 0, sizeof(in));
    in.claimed_slice = claimed;
    in.partition_id = part;
    in.offset = offset;
    in.length = length;
    in.phys = phys;
    return in;
}

TEST_GROUP(IavBrokerSliceAdmit)
{
    IavBrokerHarness *h;
    uint64_t hd;
    unsigned long ioctls;

    void setup() override
    {
        struct virt_iav_guest_pool pool;

        g_window = 0;
        g_have_window = 1;
        h = new IavBrokerHarness();
        memset(&pool, 0, sizeof(pool));
        pool.partition_id = kPart;
        pool.parent_base = kBase;
        pool.parent_bytes = 8 * kSlice;
        pool.slice_count = 8;
        pool.slice_bytes = kSlice;
        iav_broker_set_guest_pool(&h->b, &pool);
        h->b.ops.slice_of = slice_of;
        hd = h->mustOpen(7);
        CHECK(hd != 0);
        ioctls = h->drv.ioctlCalls();
    }

    void teardown() override
    {
        CHECK(h->drv.violations().empty());
        delete h;
    }

    IavBrokerHarness::Result submit(const struct amba_virt_iav_slice_req &in)
    {
        std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_SLICE, hd,
                                                         IAV_IOC_GET_CHIP_ID, 0, 0,
                                                         &in, sizeof(in));
        return h->send(7, &v[0], v.size());
    }
};

TEST(IavBrokerSliceAdmit, admitted_buffer_reaches_the_driver_once)
{
    struct amba_virt_iav_slice_req in = slice_arg(0, kPart, 4096, 4096, 0);
    IavBrokerHarness::Result r = submit(in);

    LONGS_EQUAL(0, r.r.status);
    UNSIGNED_LONGS_EQUAL(kBase + 4096, r.r.value);
    UNSIGNED_LONGS_EQUAL(ioctls + 1, h->drv.ioctlCalls());
}

TEST(IavBrokerSliceAdmit, foreign_slice_makes_zero_driver_calls)
{
    struct amba_virt_iav_slice_req in = slice_arg(1, kPart, 4096, 4096, 0);
    IavBrokerHarness::Result r = submit(in);

    LONGS_EQUAL(-EPERM, r.r.status);
    UNSIGNED_LONGS_EQUAL(ioctls, h->drv.ioctlCalls());
}

TEST(IavBrokerSliceAdmit, guest_physical_address_makes_zero_driver_calls)
{
    struct amba_virt_iav_slice_req in = slice_arg(0, kPart, 4096, 4096, kBase);
    IavBrokerHarness::Result r = submit(in);

    LONGS_EQUAL(-EPERM, r.r.status);
    UNSIGNED_LONGS_EQUAL(ioctls, h->drv.ioctlCalls());
}

TEST(IavBrokerSliceAdmit, shared_camera_partition_makes_zero_driver_calls)
{
    struct amba_virt_iav_slice_req in = slice_arg(0, 0, 4096, 4096, 0);
    IavBrokerHarness::Result r = submit(in);

    LONGS_EQUAL(-EPERM, r.r.status);
    UNSIGNED_LONGS_EQUAL(ioctls, h->drv.ioctlCalls());
}

TEST(IavBrokerSliceAdmit, overflow_makes_zero_driver_calls)
{
    struct amba_virt_iav_slice_req in = slice_arg(0, kPart, 4096, UINT64_MAX, 0);
    IavBrokerHarness::Result r = submit(in);

    LONGS_EQUAL(-EPERM, r.r.status);
    UNSIGNED_LONGS_EQUAL(ioctls, h->drv.ioctlCalls());
}

TEST(IavBrokerSliceAdmit, cross_slice_makes_zero_driver_calls)
{
    struct amba_virt_iav_slice_req in = slice_arg(0, kPart, 0, kSlice + 1, 0);
    IavBrokerHarness::Result r = submit(in);

    LONGS_EQUAL(-EPERM, r.r.status);
    UNSIGNED_LONGS_EQUAL(ioctls, h->drv.ioctlCalls());
}
