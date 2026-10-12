/*
 * drivers/amba_virt/tools/test/TestHostIavBrokerGuestSlice.cxx
 *
 * A guest private-IAV buffer is admitted only inside the slice the server
 * leased. These checks refuse the inputs that would let one guest name
 * another guest's bytes, or name Dom0 memory.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "CppUTest/TestHarness.h"

extern "C" {
#include "virt_iav_guest_slice.h"
}

static const uint64_t kSlice = 64ULL << 20;
static const uint64_t kParent = 512ULL << 20;
static const uint64_t kBase = 0x90000000ULL;
static const uint32_t kPart = 192;

static struct virt_iav_guest_pool pool(void)
{
    struct virt_iav_guest_pool p;

    memset(&p, 0, sizeof(p));
    p.partition_id = kPart;
    p.parent_base = kBase;
    p.parent_bytes = kParent;
    p.slice_count = 8;
    p.slice_bytes = kSlice;
    return p;
}

static struct virt_iav_guest_req req_for(uint32_t slice, uint64_t offset, uint64_t length)
{
    struct virt_iav_guest_req r;

    memset(&r, 0, sizeof(r));
    r.caller_slice = slice;
    r.claimed_slice = slice;
    r.partition_id = kPart;
    r.offset = offset;
    r.length = length;
    r.phys = 0;
    return r;
}

TEST_GROUP(IavBrokerGuestSlice)
{
};

TEST(IavBrokerGuestSlice, admits_the_callers_slice_and_no_other_address)
{
    struct virt_iav_guest_pool p = pool();
    struct virt_iav_guest_req r = req_for(3, 3 * kSlice + 4096, 8192);
    struct virt_iav_guest_dma dma;
    uint64_t phys;

    memset(&dma, 0, sizeof(dma));
    LONGS_EQUAL(0, virt_iav_guest_slice_admit(&p, &r, &dma));
    phys = kBase + r.offset;
    UNSIGNED_LONGS_EQUAL(kPart, dma.partition_id);
    UNSIGNED_LONGS_EQUAL(phys, dma.phys);
    UNSIGNED_LONGS_EQUAL(r.length, dma.length);
    CHECK_TRUE(dma.phys >= kBase + 3 * kSlice);
    CHECK_TRUE(dma.phys + dma.length <= kBase + 4 * kSlice);
    CHECK_TRUE(dma.phys >= kBase);
    CHECK_TRUE(dma.phys + dma.length <= kBase + kParent);
}

TEST(IavBrokerGuestSlice, last_byte_of_the_last_slice_is_inside)
{
    struct virt_iav_guest_pool p = pool();
    struct virt_iav_guest_req r = req_for(7, kParent - 1, 1);
    struct virt_iav_guest_dma dma;

    LONGS_EQUAL(0, virt_iav_guest_slice_admit(&p, &r, &dma));
    UNSIGNED_LONGS_EQUAL(kBase + kParent - 1, dma.phys);
}

TEST(IavBrokerGuestSlice, refuses_a_guest_physical_address)
{
    struct virt_iav_guest_pool p = pool();
    struct virt_iav_guest_req r = req_for(0, 0, 4096);
    struct virt_iav_guest_dma dma;

    r.phys = kBase;
    memset(&dma, 0xa5, sizeof(dma));
    LONGS_EQUAL(-EPERM, virt_iav_guest_slice_admit(&p, &r, &dma));
    BYTES_EQUAL(0xa5, ((unsigned char *)&dma)[0]);
}

TEST(IavBrokerGuestSlice, refuses_a_foreign_slice_claim)
{
    struct virt_iav_guest_pool p = pool();
    struct virt_iav_guest_req r = req_for(1, kSlice, 4096);
    struct virt_iav_guest_dma dma;

    r.claimed_slice = 0;
    LONGS_EQUAL(-EPERM, virt_iav_guest_slice_admit(&p, &r, &dma));
}

TEST(IavBrokerGuestSlice, refuses_another_guests_bytes_and_a_span_across_the_boundary)
{
    struct virt_iav_guest_pool p = pool();
    struct virt_iav_guest_dma dma;
    struct virt_iav_guest_req earlier = req_for(2, kSlice, 4096);
    struct virt_iav_guest_req next = req_for(2, 3 * kSlice, 4096);
    struct virt_iav_guest_req span = req_for(2, 3 * kSlice - 4096, 8192);

    LONGS_EQUAL(-EPERM, virt_iav_guest_slice_admit(&p, &earlier, &dma));
    LONGS_EQUAL(-EPERM, virt_iav_guest_slice_admit(&p, &next, &dma));
    LONGS_EQUAL(-EPERM, virt_iav_guest_slice_admit(&p, &span, &dma));
}

TEST(IavBrokerGuestSlice, refuses_the_shared_camera_partitions)
{
    struct virt_iav_guest_pool p = pool();
    struct virt_iav_guest_req r = req_for(0, 0, 4096);
    struct virt_iav_guest_dma dma;
    uint32_t foreign_parts[] = { 0, 1, 4, 39, 191, 255 };
    unsigned int i;

    for (i = 0; i < sizeof(foreign_parts) / sizeof(foreign_parts[0]); i++) {
        r.partition_id = foreign_parts[i];
        LONGS_EQUAL(-EPERM, virt_iav_guest_slice_admit(&p, &r, &dma));
    }
}

TEST(IavBrokerGuestSlice, refuses_overflow_zero_and_a_range_past_the_parent)
{
    struct virt_iav_guest_pool p = pool();
    struct virt_iav_guest_dma dma;
    struct virt_iav_guest_req zero = req_for(0, 0, 0);
    struct virt_iav_guest_req wrap = req_for(0, 0, UINT64_MAX);
    struct virt_iav_guest_req past = req_for(7, kParent - 4096, 8192);
    struct virt_iav_guest_req huge = req_for(0, kParent, 1);

    LONGS_EQUAL(-EINVAL, virt_iav_guest_slice_admit(&p, &zero, &dma));
    LONGS_EQUAL(-EINVAL, virt_iav_guest_slice_admit(&p, &wrap, &dma));
    LONGS_EQUAL(-EINVAL, virt_iav_guest_slice_admit(&p, &past, &dma));
    LONGS_EQUAL(-EINVAL, virt_iav_guest_slice_admit(&p, &huge, &dma));
    LONGS_EQUAL(-EINVAL, virt_iav_guest_slice_admit(nullptr, &zero, &dma));
}

TEST(IavBrokerGuestSlice, two_guests_receive_disjoint_ranges)
{
    struct virt_iav_guest_pool p = pool();
    struct virt_iav_guest_req a = req_for(0, 0, kSlice);
    struct virt_iav_guest_req b = req_for(1, kSlice, kSlice);
    struct virt_iav_guest_dma da, db;

    LONGS_EQUAL(0, virt_iav_guest_slice_admit(&p, &a, &da));
    LONGS_EQUAL(0, virt_iav_guest_slice_admit(&p, &b, &db));
    CHECK_TRUE(da.phys + da.length <= db.phys);
    CHECK_TRUE(da.phys + da.length <= kBase + kSlice);
    CHECK_TRUE(db.phys >= kBase + kSlice);
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
