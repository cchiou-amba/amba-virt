/*
 * TestHostIavBrokerArena.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The per-guest arena (plan_iav_usb_yolo, Envelope E): OP_HELLO grants the arena only to a primary guest that holds a
 * slice, OP_INFO describes the caller's own slice, and OP_ARENA translates a virtual anonymous partition id and a
 * block of the caller's slice into the pinned pool partition and an offset inside that guest's slice, for
 * SET_IMG_SCALE, GDMA_COPY and SYNC_CACHE_MEM_PART only. Every forged, malformed, foreign, stale or out-of-slice
 * request is refused before the driver: the mock driver records a violation for any pool buffer outside the ranges
 * the test says the guest owns, and the tests count the driver calls.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <thread>
#include <vector>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

namespace {

const uint32_t Primary = 7;
const uint32_t Second = 8;
const uint32_t Consumer = 9;
const uint32_t NoSlice = 10;
const uint64_t Slice = MockIavDriver::SliceBytes;
const uint64_t Usable = Slice - 4096;
const uint32_t PoolPid = MockIavDriver::PoolPid;
const uint32_t Vid1 = (1u << 8) | 0xffu;
const uint32_t Vid2 = (2u << 8) | 0xffu;
const uint64_t AllFeatures = AMBA_VIRT_IAV_F_SIZE_OVERRIDE | AMBA_VIRT_IAV_F_EXT_INFO | AMBA_VIRT_IAV_F_ARENA;

/* The slices the server leased: the primary holds slice 4, the second primary-capable guest slice 2. */
uint32_t g_sliceOf[16];
std::atomic<int> g_quarantined[8];
std::atomic<int> g_begins;
std::atomic<int> g_ends;
std::atomic<int> g_inflight;
std::atomic<int> g_maxInflight;
std::atomic<int> g_dropAfterBegin;

int sliceOf(void *ctx, uint32_t cid, uint32_t *window)
{
    (void)ctx;
    if (cid >= 16 || g_sliceOf[cid] == 0xffffffffu || window == NULL) {
        return -ENOENT;
    }
    *window = g_sliceOf[cid];
    return 0;
}

int arenaBegin(void *ctx, uint32_t index)
{
    (void)ctx;
    if (index >= 8 || g_quarantined[index]) {
        return -EBUSY;
    }
    g_begins++;
    int now = ++g_inflight;
    int seen = g_maxInflight;
    while (now > seen && !g_maxInflight.compare_exchange_weak(seen, now)) {
    }
    if (g_dropAfterBegin) {
        /* the lease is released between the begin and the job: the binding is gone */
        g_sliceOf[Primary] = 0xffffffffu;
    }
    return 0;
}

void arenaEnd(void *ctx, uint32_t index)
{
    (void)ctx;
    (void)index;
    g_ends++;
    g_inflight--;
}

struct amba_virt_iav_hello helloArg(uint64_t features)
{
    struct amba_virt_iav_hello hl;

    memset(&hl, 0, sizeof(hl));
    hl.revision = AMBA_VIRT_IAV_REVISION;
    hl.page_size = 4096;
    hl.features = features;
    hl.max_window_id = 3;
    return hl;
}

struct amba_virt_iav_arena_req header(uint32_t vid, uint64_t off, uint64_t len)
{
    struct amba_virt_iav_arena_req a;

    memset(&a, 0, sizeof(a));
    a.count = 1;
    a.blk[0].vid = vid;
    a.blk[0].offset = off;
    a.blk[0].length = len;
    return a;
}

struct amba_virt_iav_arena_req header2(uint32_t v1, uint64_t o1, uint64_t l1, uint32_t v2, uint64_t o2, uint64_t l2)
{
    struct amba_virt_iav_arena_req a = header(v1, o1, l1);

    a.count = 2;
    a.blk[1].vid = v2;
    a.blk[1].offset = o2;
    a.blk[1].length = l2;
    return a;
}

/* A 640x640 NV12 scale from the decode window into a block, as the Cooper allocation pattern does. */
struct iav_img_scale scaleIntoBlock(uint32_t vid)
{
    struct iav_img_scale s;

    memset(&s, 0, sizeof(s));
    s.format = IAV_YUV_FORMAT_YUV420;
    s.input.width = 1280;
    s.input.height = 720;
    s.output.width = 640;
    s.output.height = 360;
    s.input_buf.pid = IAV_PART_DEC_BSB;
    s.input_buf.y_offset = 0x300000;
    s.input_buf.uv_offset = 0x3e1000;
    s.input_buf.pitch = 1280;
    s.input_buf.height = 720;
    s.output_buf.pid = vid;
    s.output_buf.y_offset = 0;
    s.output_buf.uv_offset = 640 * 640;
    s.output_buf.pitch = 640;
    s.output_buf.height = 640;
    return s;
}

struct iav_gdma_copy gdma(uint32_t srcPid, unsigned long srcOff, uint32_t dstPid, unsigned long dstOff)
{
    struct iav_gdma_copy g;

    memset(&g, 0, sizeof(g));
    g.src_mmap_type = srcPid;
    g.src_offset = srcOff;
    g.dst_mmap_type = dstPid;
    g.dst_offset = dstOff;
    g.src_pitch = 256;
    g.dst_pitch = 256;
    g.width = 200;
    g.height = 16;
    return g;
}

struct iav_sync_cache_mem_part sync(uint32_t pid, unsigned long off, unsigned long len, int opt)
{
    struct iav_sync_cache_mem_part c;

    memset(&c, 0, sizeof(c));
    c.pid = pid;
    c.offset = off;
    c.length = len;
    memcpy(&c.opt, &opt, sizeof(opt));
    return c;
}

} // namespace

TEST_GROUP(IavBrokerArena)
{
    IavBrokerHarness *h;
    uint64_t hp, h2, hc;

    void setup() override
    {
        struct virt_iav_guest_pool pool;

        for (unsigned int i = 0; i < 16; i++) {
            g_sliceOf[i] = 0xffffffffu;
        }
        g_sliceOf[Primary] = 4;
        g_sliceOf[Second] = 2;
        g_sliceOf[Consumer] = 1;
        for (unsigned int i = 0; i < 8; i++) {
            g_quarantined[i] = 0;
        }
        g_begins = g_ends = g_inflight = g_maxInflight = g_dropAfterBegin = 0;
        /* the server-wide profile is consumer; Primary and Second act as the primary (as tickets would decide) */
        h = new IavBrokerHarness(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
        h->addDecodeWindow();
        memset(&pool, 0, sizeof(pool));
        pool.partition_id = PoolPid;
        pool.parent_base = MockIavDriver::PoolBase;
        pool.parent_bytes = MockIavDriver::PoolLen;
        pool.slice_count = 8;
        pool.slice_bytes = Slice;
        iav_broker_set_guest_pool(&h->b, &pool);
        h->b.ops.slice_of = sliceOf;
        h->b.ops.arena_begin = arenaBegin;
        h->b.ops.arena_end = arenaEnd;
        /* the primary owns its slice's arena: slice 4 less its claim page */
        h->drv.allowArena(4 * Slice, Usable);
        hp = h->mustOpen(Primary);
        h2 = h->mustOpen(Second);
        hc = h->mustOpen(Consumer);
        CHECK(hp != 0 && h2 != 0 && hc != 0);
    }

    void teardown() override
    {
        std::vector<std::string> v = h->drv.violations();

        for (size_t i = 0; i < v.size(); i++) {
            fprintf(stderr, "violation: %s\n", v[i].c_str());
        }
        CHECK(v.empty());
        LONGS_EQUAL(g_begins.load(), g_ends.load());
        delete h;
    }

    IavBrokerHarness::Result hello(uint32_t cid, uint64_t features)
    {
        struct amba_virt_iav_hello hl = helloArg(features);
        std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, &hl, sizeof(hl));

        return h->send(cid, &v[0], v.size());
    }

    uint64_t granted(uint32_t cid, uint64_t features)
    {
        IavBrokerHarness::Result r = hello(cid, features);

        return r.r.status == 0 ? r.r.value : ~0ull;
    }

    IavBrokerHarness::Result arena(uint32_t cid, uint64_t handle, unsigned long cmd,
                                   const struct amba_virt_iav_arena_req &a, const void *arg, size_t len)
    {
        std::vector<uint8_t> body(sizeof(a) + len);

        memcpy(&body[0], &a, sizeof(a));
        memcpy(&body[sizeof(a)], arg, len);
        std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_ARENA, handle, (uint32_t)cmd, 0, 0, &body[0],
                                                         (uint32_t)body.size());
        return h->send(cid, &v[0], v.size());
    }

    /* A refusal with err that made no driver call. */
    void refused(const IavBrokerHarness::Result &r, int err, unsigned long before)
    {
        LONGS_EQUAL(0, r.ret);
        LONGS_EQUAL(-err, r.r.status);
        UNSIGNED_LONGS_EQUAL(before, h->drv.ioctlCalls());
    }
};

/* ---- negotiation and OP_INFO ---- */

TEST(IavBrokerArena, TheArenaIsGrantedOnlyToAPrimaryGuestWithASliceAndTheExtendedInfo)
{
    UNSIGNED_LONGS_EQUAL(AllFeatures, granted(Primary, AllFeatures));
    /* a consumer, a guest without a slice, a guest that does not take the extended OP_INFO: no arena */
    UNSIGNED_LONGS_EQUAL(AllFeatures & ~AMBA_VIRT_IAV_F_ARENA, granted(Consumer, AllFeatures));
    g_sliceOf[Primary] = 0xffffffffu;
    UNSIGNED_LONGS_EQUAL(AllFeatures & ~AMBA_VIRT_IAV_F_ARENA, granted(Primary, AllFeatures));
    g_sliceOf[Primary] = 4;
    UNSIGNED_LONGS_EQUAL(AMBA_VIRT_IAV_F_ARENA & 0, granted(Primary, AMBA_VIRT_IAV_F_ARENA) & AMBA_VIRT_IAV_F_ARENA);
    /* a slice index the pool does not have is no slice */
    g_sliceOf[Primary] = 8;
    UNSIGNED_LONGS_EQUAL(0, granted(Primary, AllFeatures) & AMBA_VIRT_IAV_F_ARENA);
}

TEST(IavBrokerArena, WithoutThePoolThereIsNoArena)
{
    IavBrokerHarness bare(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    struct amba_virt_iav_hello hl = helloArg(AllFeatures);
    std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, &hl, sizeof(hl));

    bare.b.ops.slice_of = sliceOf;
    UNSIGNED_LONGS_EQUAL(0, bare.send(Primary, &v[0], v.size()).r.value & AMBA_VIRT_IAV_F_ARENA);
}

TEST(IavBrokerArena, OpInfoDescribesTheCallersOwnSliceOnly)
{
    struct amba_virt_iav_entry e;
    IavBrokerHarness::Result r;
    bool found = false;

    granted(Primary, AllFeatures);
    r = h->info(Primary);
    LONGS_EQUAL(0, r.r.status);
    for (size_t off = 0; off + sizeof(e) <= r.out.size(); off += sizeof(e)) {
        memcpy(&e, &r.out[off], sizeof(e));
        if (e.id == AMBA_VIRT_IAV_ENT_ID_ARENA) {
            found = true;
            UNSIGNED_LONGS_EQUAL(MockIavDriver::PoolBase + 4 * Slice, e.base);
            UNSIGNED_LONGS_EQUAL(Slice, e.size);
            UNSIGNED_LONGS_EQUAL(Usable, e.part_len);
            UNSIGNED_LONGS_EQUAL(PoolPid, e.pid);
            UNSIGNED_LONGS_EQUAL(4, e.reserved);
            CHECK((e.flags & AMBA_VIRT_IAV_ENT_ARENA) != 0);
        }
    }
    CHECK(found);
    /* a consumer and a legacy guest get no arena entry */
    granted(Consumer, AllFeatures);
    r = h->info(Consumer);
    for (size_t off = 0; off + sizeof(e) <= r.out.size(); off += sizeof(e)) {
        memcpy(&e, &r.out[off], sizeof(e));
        CHECK(e.id != AMBA_VIRT_IAV_ENT_ID_ARENA);
    }
    r = h->info(NoSlice);
    UNSIGNED_LONGS_EQUAL(0, r.out.size() % sizeof(struct amba_virt_iav_window));
    LONGS_EQUAL(2, r.r.value);
}

/* ---- calls that reach the driver ---- */

TEST(IavBrokerArena, AScaleIntoABlockReachesTheDriverInsideTheCallersSlice)
{
    struct iav_img_scale s = scaleIntoBlock(Vid1);
    struct amba_virt_iav_arena_req a = header(Vid1, 0x20000, 0x100000);
    IavBrokerHarness::Result r;
    struct iav_img_scale seen, back;
    std::vector<uint8_t> cap;

    granted(Primary, AllFeatures);
    h->drv.captureArgOf(IAV_IOC_SET_IMG_SCALE, sizeof(s));
    r = arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, a, &s, sizeof(s));
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(0, r.r.rc);
    cap = h->drv.capturedArg();
    LONGS_EQUAL(sizeof(seen), cap.size());
    memcpy(&seen, &cap[0], sizeof(seen));
    /* the driver sees the pool partition and offsets inside slice 4, the input untouched */
    UNSIGNED_LONGS_EQUAL(PoolPid, seen.output_buf.pid);
    UNSIGNED_LONGS_EQUAL(4 * Slice + 0x20000, seen.output_buf.y_offset);
    UNSIGNED_LONGS_EQUAL(4 * Slice + 0x20000 + 640 * 640, seen.output_buf.uv_offset);
    UNSIGNED_LONGS_EQUAL(IAV_PART_DEC_BSB, seen.input_buf.pid);
    UNSIGNED_LONGS_EQUAL(0x300000, seen.input_buf.y_offset);
    /* the guest gets its own id and offsets back, and the task id the driver gave */
    LONGS_EQUAL(sizeof(back), r.out.size());
    memcpy(&back, &r.out[0], sizeof(back));
    UNSIGNED_LONGS_EQUAL(Vid1, back.output_buf.pid);
    UNSIGNED_LONGS_EQUAL(0, back.output_buf.y_offset);
    UNSIGNED_LONGS_EQUAL(640 * 640, back.output_buf.uv_offset);
    UNSIGNED_LONGS_EQUAL(77, back.task_id);
    LONGS_EQUAL(1, g_begins.load());
}

TEST(IavBrokerArena, AScaleFromOneBlockToAnotherAndInPlaceIsServed)
{
    struct iav_img_scale s = scaleIntoBlock(Vid2);
    struct amba_virt_iav_arena_req a = header2(Vid1, 0, 0x200000, Vid2, 0x200000, 0x100000);

    granted(Primary, AllFeatures);
    s.input_buf.pid = Vid1;
    s.input_buf.y_offset = 0;
    s.input_buf.uv_offset = 1280 * 720;
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, a, &s, sizeof(s)).r.status);
    /* one block for both: the call declares it once */
    a = header(Vid1, 0, 0x300000);
    s.output_buf.pid = Vid1;
    s.output_buf.y_offset = 0x200000;
    s.output_buf.uv_offset = 0x200000 + 640 * 640;
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, a, &s, sizeof(s)).r.status);
}

TEST(IavBrokerArena, GdmaIntoAndOutOfBlocksAndCacheCallsOnABlockAreServed)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0x1000);
    struct iav_sync_cache_mem_part c = sync(Vid1, 0, 0x10000, IAV_CACHE_INVALIDATE);
    struct amba_virt_iav_arena_req a = header(Vid1, 0x30000, 0x10000);
    std::vector<uint8_t> cap;
    struct iav_gdma_copy seen;

    granted(Primary, AllFeatures);
    h->drv.captureArgOf(IAV_IOC_GDMA_COPY, sizeof(g));
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_GDMA_COPY, a, &g, sizeof(g)).r.status);
    cap = h->drv.capturedArg();
    memcpy(&seen, &cap[0], sizeof(seen));
    UNSIGNED_LONGS_EQUAL(PoolPid, seen.dst_mmap_type);
    UNSIGNED_LONGS_EQUAL(4 * Slice + 0x30000 + 0x1000, seen.dst_offset);
    UNSIGNED_LONGS_EQUAL(IAV_PART_DEC_BSB, seen.src_mmap_type);
    /* block to block, and block back into the decode window */
    g = gdma(Vid1, 0, Vid2, 0);
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_GDMA_COPY, header2(Vid1, 0, 0x10000, Vid2, 0x10000, 0x10000), &g,
                         sizeof(g)).r.status);
    g = gdma(Vid1, 0, IAV_PART_DEC_BSB, 0x200000);
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_GDMA_COPY, a, &g, sizeof(g)).r.status);
    /* clean and invalidate on the guest's own block */
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)).r.status);
    c = sync(Vid1, 0x8000, 0x8000, IAV_CACHE_CLEAN);
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)).r.status);
    LONGS_EQUAL(5, g_begins.load());
}

TEST(IavBrokerArena, TheLargestBlockAndTheLastByteOfTheArenaAreAllowed)
{
    struct iav_sync_cache_mem_part c = sync(Vid1, 0, Usable, IAV_CACHE_CLEAN);

    granted(Primary, AllFeatures);
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0, Usable), &c, sizeof(c)).r.status);
    /* the last 64 KiB-aligned block runs to the claim page: its last byte is the arena's last byte */
    c = sync(Vid1, 0xefff, 1, IAV_CACHE_CLEAN);
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0x3ff0000, Usable - 0x3ff0000), &c,
                         sizeof(c)).r.status);
    c = sync(Vid1, 0xefff, 2, IAV_CACHE_CLEAN);
    CHECK(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0x3ff0000, Usable - 0x3ff0000), &c,
                sizeof(c)).r.status != 0);
}

TEST(IavBrokerArena, TwoGuestsNamingTheSameBlockLandInTheirOwnSlices)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0);
    std::vector<uint8_t> cap;
    struct iav_gdma_copy seen;

    granted(Primary, AllFeatures);
    h->drv.setPrimaryCid(0);
    h->drv.allowPrimarySet(true);
    /* the second guest is primary-capable too (as under a qualification server) */
    IavBrokerHarness q(IAV_PROFILE_QUALIFICATION);
    struct virt_iav_guest_pool pool;

    q.addDecodeWindow();
    memset(&pool, 0, sizeof(pool));
    pool.partition_id = PoolPid;
    pool.parent_base = MockIavDriver::PoolBase;
    pool.parent_bytes = MockIavDriver::PoolLen;
    pool.slice_count = 8;
    pool.slice_bytes = Slice;
    iav_broker_set_guest_pool(&q.b, &pool);
    q.b.ops.slice_of = sliceOf;
    q.drv.allowArena(4 * Slice, Usable);
    q.drv.allowArena(2 * Slice, Usable);
    uint64_t q7 = q.mustOpen(Primary), q8 = q.mustOpen(Second);
    struct amba_virt_iav_hello hl = helloArg(AllFeatures);
    std::vector<uint8_t> hv = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, &hl, sizeof(hl));

    q.send(Primary, &hv[0], hv.size());
    q.send(Second, &hv[0], hv.size());
    for (uint32_t cid : {Primary, Second}) {
        struct amba_virt_iav_arena_req a = header(Vid1, 0x10000, 0x10000);
        std::vector<uint8_t> body(sizeof(a) + sizeof(g));

        memcpy(&body[0], &a, sizeof(a));
        memcpy(&body[sizeof(a)], &g, sizeof(g));
        std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_ARENA, cid == Primary ? q7 : q8,
                                                         IAV_IOC_GDMA_COPY, 0, 0, &body[0], (uint32_t)body.size());
        q.drv.captureArgOf(IAV_IOC_GDMA_COPY, sizeof(g));
        LONGS_EQUAL(0, q.send(cid, &v[0], v.size()).r.status);
        cap = q.drv.capturedArg();
        memcpy(&seen, &cap[0], sizeof(seen));
        UNSIGNED_LONGS_EQUAL((cid == Primary ? 4 : 2) * Slice + 0x10000, seen.dst_offset);
    }
    CHECK(q.drv.violations().empty());
}

/* ---- refusals before the driver ---- */

TEST(IavBrokerArena, AGuestThatDidNotNegotiateTheArenaIsRefused)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0);
    unsigned long before = h->drv.ioctlCalls();

    /* no HELLO at all, then a HELLO without the arena bit */
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EPERM, before);
    granted(Primary, AMBA_VIRT_IAV_F_EXT_INFO);
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EPERM, before);
}

TEST(IavBrokerArena, AConsumerIsRefusedEvenWithAForgedNegotiation)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0);
    unsigned long before;

    granted(Consumer, AllFeatures);
    before = h->drv.ioctlCalls();
    refused(arena(Consumer, hc, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EPERM, before);
    /* the primary demoted after it negotiated: refused at the next call */
    granted(Primary, AllFeatures);
    h->drv.setPrimaryCid(0);
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EPERM, before);
}

TEST(IavBrokerArena, OnlyTheThreeBufferCallsMayNameTheArena)
{
    struct iav_decode_jpeg dj;
    u32 id = 0;
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    memset(&dj, 0, sizeof(dj));
    dj.jpeg_size = 4096;
    refused(arena(Primary, hp, IAV_IOC_DECODE_JPEG, header(Vid1, 0, 0x10000), &dj, sizeof(dj)), EPERM, before);
    refused(arena(Primary, hp, IAV_IOC_GET_CHIP_ID, header(Vid1, 0, 0x10000), &id, 0), EPERM, before);
    refused(arena(Primary, hp, IAV_IOC_WAIT_IMG_SCALE, header(Vid1, 0, 0x10000), &id, sizeof(id)), EPERM, before);
}

TEST(IavBrokerArena, ABlockOutsideTheCallersArenaIsRefused)
{
    struct iav_sync_cache_mem_part c = sync(Vid1, 0, 0x10000, IAV_CACHE_CLEAN);
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    /* the next slice, the claim page, past the arena, a wrapping length, an offset near the top of 64 bits */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, Slice, 0x10000), &c, sizeof(c)), EPERM,
            before);
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, Slice - 0x10000, 0x10000), &c, sizeof(c)),
            EPERM, before);
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0, Usable + 4096), &c, sizeof(c)), EPERM,
            before);
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0x10000, 0xffffffffffff0000ull), &c,
                  sizeof(c)), EPERM, before);
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0xffffffffffff0000ull, 0x10000), &c,
                  sizeof(c)), EPERM, before);
    /* 4 * Slice: the primary's slice as an absolute pool offset is not an offset in its slice */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 4 * Slice, 0x10000), &c, sizeof(c)), EPERM,
            before);
}

TEST(IavBrokerArena, AMalformedHeaderIsRefused)
{
    struct iav_sync_cache_mem_part c = sync(Vid1, 0, 0x1000, IAV_CACHE_CLEAN);
    struct amba_virt_iav_arena_req a;
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    a = header(Vid1, 0x1000, 0x10000);		/* misaligned block */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header(Vid1, 0, 0);			/* zero length */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header(Vid1, 0, 0x10001);		/* not a page multiple */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header(0x1ff00, 0, 0x10000);		/* not an anonymous id */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header(0xff, 0, 0x10000);		/* the anonymous tag with index 0 */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header(Vid1, 0, 0x10000);
    a.count = 0;
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a.count = 3;
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header(Vid1, 0, 0x10000);
    a.reserved = 1;
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header(Vid1, 0, 0x10000);
    a.blk[0].reserved = 1;
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header(Vid1, 0, 0x10000);
    a.blk[1].offset = 0x10000;			/* an unused slot that is not zero */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    a = header2(Vid1, 0, 0x10000, Vid1, 0x10000, 0x10000);	/* one id twice */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    {
        /* overlapping blocks, both named by the call */
        struct iav_gdma_copy g = gdma(Vid1, 0, Vid2, 0);

        a = header2(Vid1, 0, 0x20000, Vid2, 0x10000, 0x10000);
        refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, a, &g, sizeof(g)), EINVAL, before);
    }
    a = header2(Vid1, 0, 0x10000, Vid2, 0x10000, 0x10000);	/* a block the call does not name */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, a, &c, sizeof(c)), EINVAL, before);
    /* a header shorter than its structure */
    {
        std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_ARENA, hp, IAV_IOC_SYNC_CACHE_MEM_PART, 0, 0,
                                                         &a, sizeof(a) - 1);

        refused(h->send(Primary, &v[0], v.size()), EMSGSIZE, before);
    }
    /* the ioctl argument must still have its exact size */
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0, 0x10000), &c, sizeof(c) - 1), EMSGSIZE,
            before);
}

TEST(IavBrokerArena, AFieldMustNameADeclaredBlockAndStayInsideIt)
{
    struct iav_img_scale s = scaleIntoBlock(Vid2);
    struct iav_gdma_copy g;
    struct iav_sync_cache_mem_part c;
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    /* an id the header does not declare (a freed id, another guest's id): unknown */
    refused(arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, header(Vid1, 0, 0x100000), &s, sizeof(s)), EINVAL, before);
    /* the output picture does not fit its block: the planes run past it */
    s = scaleIntoBlock(Vid1);
    refused(arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, header(Vid1, 0, 0x80000), &s, sizeof(s)), EINVAL, before);
    /* an offset past the block */
    s.output_buf.y_offset = 0x100000 + 1;
    refused(arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, header(Vid1, 0, 0x100000), &s, sizeof(s)), EINVAL, before);
    s = scaleIntoBlock(Vid1);
    s.output_buf.uv_offset = ~0ul;
    refused(arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, header(Vid1, 0, 0x100000), &s, sizeof(s)), EINVAL, before);
    /* GDMA rows past the block, a width larger than the pitch, zero sizes */
    g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0x10000 - 256 * 15);
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EINVAL, before);
    g = gdma(Vid1, 0, IAV_PART_DEC_BSB, 0);
    g.width = 300;
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EINVAL, before);
    g = gdma(Vid1, 0, IAV_PART_DEC_BSB, 0);
    g.height = 0;
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EINVAL, before);
    /* a block copied into a partition the guest does not own */
    g = gdma(Vid1, 0, IAV_PART_DSP, 0);
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EPERM, before);
    /* a cache range past the block, a zero length */
    c = sync(Vid1, 0x8000, 0x9000, IAV_CACHE_CLEAN);
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0, 0x10000), &c, sizeof(c)), EINVAL, before);
    c = sync(Vid1, 0, 0, IAV_CACHE_CLEAN);
    refused(arena(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, header(Vid1, 0, 0x10000), &c, sizeof(c)), EINVAL, before);
}

TEST(IavBrokerArena, APhysicalAddressOrADmaBufFdIsNeverTranslated)
{
    struct iav_img_scale s = scaleIntoBlock(Vid1);
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0);
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    s.output_buf.use_phys = 1;
    s.output_buf.y_addr = MockIavDriver::PoolBase + 4 * Slice;
    CHECK(arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, header(Vid1, 0, 0x100000), &s, sizeof(s)).r.status < 0);
    s = scaleIntoBlock(Vid1);
    s.output_buf.use_dma_buf_fd = 1;
    CHECK(arena(Primary, hp, IAV_IOC_SET_IMG_SCALE, header(Vid1, 0, 0x100000), &s, sizeof(s)).r.status < 0);
    g.dst_use_phys = 1;
    CHECK(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)).r.status < 0);
    UNSIGNED_LONGS_EQUAL(before, h->drv.ioctlCalls());
}

TEST(IavBrokerArena, ThePoolPartitionCannotBeNamedDirectly)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, PoolPid, 4 * Slice);
    struct iav_sync_cache_mem_part c = sync(PoolPid, 4 * Slice, 0x1000, IAV_CACHE_CLEAN);
    struct iav_img_scale s = scaleIntoBlock(PoolPid);
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    /* through OP_IOCTL: no arena slot, so the pool is no buffer of the guest's */
    refused(h->ioctl(Primary, hp, IAV_IOC_GDMA_COPY, &g, sizeof(g)), EPERM, before);
    refused(h->ioctl(Primary, hp, IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)), EPERM, before);
    s.output_buf.y_offset = 4 * Slice;
    s.output_buf.uv_offset = 4 * Slice + 640 * 640;
    refused(h->ioctl(Primary, hp, IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)), EPERM, before);
    /* through OP_ARENA next to a declared block: the pool id is not the block */
    g = gdma(Vid1, 0, PoolPid, 4 * Slice);
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EPERM, before);
}

TEST(IavBrokerArena, AQuarantinedSliceRefusesNewJobs)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0);
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    g_quarantined[4] = 1;
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EPERM, before);
    LONGS_EQUAL(0, g_begins.load());
    g_quarantined[4] = 0;
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)).r.status);
    LONGS_EQUAL(1, g_ends.load());
}

TEST(IavBrokerArena, ALeaseReleasedDuringTheCallIsRefusedAndAccountedFor)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0);
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    g_dropAfterBegin = 1;
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EPERM, before);
    LONGS_EQUAL(1, g_begins.load());
    LONGS_EQUAL(1, g_ends.load());
    /* the slice is gone: no arena entry and no arena at the next negotiation */
    UNSIGNED_LONGS_EQUAL(0, granted(Primary, AllFeatures) & AMBA_VIRT_IAV_F_ARENA);
}

TEST(IavBrokerArena, StaleWindowsAndAMovedPoolStopArenaCalls)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0);
    unsigned long before;

    granted(Primary, AllFeatures);
    LONGS_EQUAL(0, iav_broker_recheck_windows(&h->b));
    h->drv.setPool(MockIavDriver::PoolBase + Slice, MockIavDriver::PoolLen);
    LONGS_EQUAL(1, iav_broker_recheck_windows(&h->b));
    before = h->drv.ioctlCalls();
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EIO, before);
    h->drv.setPool(MockIavDriver::PoolBase, MockIavDriver::PoolLen);
    LONGS_EQUAL(0, iav_broker_recheck_windows(&h->b));
    LONGS_EQUAL(0, arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)).r.status);
}

TEST(IavBrokerArena, AForeignOrStaleHandleIsRefused)
{
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, 0);
    unsigned long before;

    granted(Primary, AllFeatures);
    before = h->drv.ioctlCalls();
    refused(arena(Primary, h2, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), EACCES, before);
    h->close(Primary, hp);
    refused(arena(Primary, hp, IAV_IOC_GDMA_COPY, header(Vid1, 0, 0x10000), &g, sizeof(g)), ESTALE, before);
    LONGS_EQUAL(g_begins.load(), g_ends.load());
}

TEST(IavBrokerArena, ConcurrentArenaCallsAndQuarantineStayBalanced)
{
    std::atomic<bool> stop(false);
    std::atomic<int> served(0), refusedCount(0);
    std::vector<std::thread> t;

    granted(Primary, AllFeatures);
    for (int i = 0; i < 4; i++) {
        t.push_back(std::thread([&, i]() {
            uint64_t hd = h->mustOpen(Primary);
            struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, Vid1, (unsigned long)i * 0x1000);
            struct amba_virt_iav_arena_req a = header(Vid1, 0x10000 * (uint64_t)i, 0x10000);
            uint8_t body[sizeof(a) + sizeof(g)];
            struct amba_virt_iav_req q;
            uint8_t req[sizeof(q) + sizeof(body)];

            memcpy(body, &a, sizeof(a));
            memcpy(body + sizeof(a), &g, sizeof(g));
            memset(&q, 0, sizeof(q));
            q.proto = AMBA_VIRT_IAV_PROTO;
            q.op = AMBA_VIRT_IAV_OP_ARENA;
            q.handle = hd;
            q.cmd = (uint32_t)IAV_IOC_GDMA_COPY;
            q.arg_len = sizeof(body);
            memcpy(req, &q, sizeof(q));
            memcpy(req + sizeof(q), body, sizeof(body));
            for (int k = 0; k < 400; k++) {
                uint8_t resp[AMBA_VIRT_MAX_MSG];
                struct amba_virt_iav_resp r;
                size_t n = 0;

                if (iav_broker_handle(&h->b, Primary, req, sizeof(req), resp, sizeof(resp), &n) == 0 &&
                    n >= sizeof(r)) {
                    memcpy(&r, resp, sizeof(r));
                    if (r.status == 0) {
                        served++;
                    } else {
                        refusedCount++;
                    }
                }
            }
        }));
    }
    t.push_back(std::thread([&]() {
        for (int k = 0; k < 200 && !stop; k++) {
            g_quarantined[4] = k & 1;
            std::this_thread::yield();
        }
        g_quarantined[4] = 0;
    }));
    for (auto &x : t) {
        x.join();
    }
    LONGS_EQUAL(1600, served.load() + refusedCount.load());
    CHECK(served.load() > 0);
    LONGS_EQUAL(0, g_inflight.load());
    LONGS_EQUAL(g_begins.load(), g_ends.load());
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
