/*
 * drivers/amba_virt/tools/test/TestHostIavBrokerWire.cxx
 *
 * The negotiated wire (OP_HELLO, per-guest OP_INFO, the wire size of the ioctl table), window pinning, and
 * OP_SLICE through the allow-list: the good paths and the hostile ones. A refusal makes no driver call unless
 * the test says so.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

extern "C" {
#include "virt_iav_guest_slice.h"
}

namespace {

const uint32_t Primary = 7;
const uint32_t Consumer = 9;

struct amba_virt_iav_hello helloArg(uint64_t features, uint32_t maxId = 3, uint32_t page = 4096)
{
    struct amba_virt_iav_hello hl;

    memset(&hl, 0, sizeof(hl));
    hl.revision = AMBA_VIRT_IAV_REVISION;
    hl.page_size = page;
    hl.features = features;
    hl.max_window_id = maxId;
    hl.table_rows = 1;
    hl.table_hash = 0x1234;
    return hl;
}

IavBrokerHarness::Result hello(IavBrokerHarness &h, uint32_t cid, const struct amba_virt_iav_hello &hl,
                               uint64_t handle = 0)
{
    std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, handle, 0, 0, 0, &hl, sizeof(hl));

    return h.send(cid, &v[0], v.size());
}

struct amba_virt_iav_hello_resp helloOut(const IavBrokerHarness::Result &r)
{
    struct amba_virt_iav_hello_resp out;

    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(sizeof(out), r.out.size());
    memcpy(&out, &r.out[0], sizeof(out));
    return out;
}

const uint64_t AllFeatures = AMBA_VIRT_IAV_F_NC_WINDOWS | AMBA_VIRT_IAV_F_SIZE_OVERRIDE | AMBA_VIRT_IAV_F_EXT_INFO;

} // namespace

/* ---- OP_HELLO ---- */

TEST_GROUP(IavBrokerHello)
{
};

TEST(IavBrokerHello, GrantsWhatTheBrokerAndTheHostProvideAndIgnoresUnknownBits)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    struct amba_virt_iav_hello_resp out;
    IavBrokerHarness::Result r;

    /* No Normal-NC host: the bit is not granted even when offered (no slice: no arena; no aperture attached). */
    r = hello(h, Primary, helloArg(~0ull));
    out = helloOut(r);
    UNSIGNED_LONGS_EQUAL(AMBA_VIRT_IAV_F_SIZE_OVERRIDE | AMBA_VIRT_IAV_F_EXT_INFO | AMBA_VIRT_IAV_F_INLINE, out.features);
    UNSIGNED_LONGS_EQUAL(out.features, r.r.value);
    LONGS_EQUAL(AMBA_VIRT_IAV_REVISION, out.revision);
    LONGS_EQUAL(AMBA_VIRT_IAV_ROLE_PRIMARY, out.role);
    LONGS_EQUAL(3, out.max_window_id);
    UNSIGNED_LONGS_EQUAL(h.b.table_hash, out.table_hash);
    LONGS_EQUAL(h.b.table_rows, out.table_rows);
    CHECK(out.table_rows == iav_policy_count());

    iav_broker_set_host_features(&h.b, AMBA_VIRT_IAV_F_NC_WINDOWS | (1ull << 40));
    out = helloOut(hello(h, Primary, helloArg(AllFeatures | (1ull << 40))));
    UNSIGNED_LONGS_EQUAL(AllFeatures, out.features);
    /* The aperture and the arena need what the guest has (an attached aperture, a slice); nothing not offered. */
    out = helloOut(hello(h, Primary, helloArg(AMBA_VIRT_IAV_F_APERTURE | AMBA_VIRT_IAV_F_ARENA)));
    UNSIGNED_LONGS_EQUAL(0, out.features);
    out = helloOut(hello(h, Consumer, helloArg(AMBA_VIRT_IAV_F_NC_WINDOWS)));
    UNSIGNED_LONGS_EQUAL(AMBA_VIRT_IAV_F_NC_WINDOWS, out.features);
    LONGS_EQUAL(AMBA_VIRT_IAV_ROLE_CONSUMER, out.role);
    LONGS_EQUAL(0, h.drv.ioctlCalls());
}

TEST(IavBrokerHello, TheQualificationServerReportsItsRole)
{
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION);

    LONGS_EQUAL(AMBA_VIRT_IAV_ROLE_QUALIFY, helloOut(hello(h, 3, helloArg(0))).role);
}

TEST(IavBrokerHello, TheWindowLimitOnlyLowers)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);

    LONGS_EQUAL(3, helloOut(hello(h, Primary, helloArg(0, 0xffffffffu))).max_window_id);
    LONGS_EQUAL(1, helloOut(hello(h, Primary, helloArg(0, 1))).max_window_id);
    LONGS_EQUAL(0, helloOut(hello(h, Primary, helloArg(0, 0))).max_window_id);
}

TEST(IavBrokerHello, MalformedNegotiationsAreRefusedAndChangeNothing)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    struct amba_virt_iav_hello hl = helloArg(AllFeatures);
    struct iav_broker_hello st;
    uint8_t big[sizeof(hl) + 8];
    uint32_t pages[] = {0, 1, 2048, 4095, 4097, 12288, 131072, 0x80000000u, 0xffffffffu};

    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        hl = helloArg(AllFeatures, 3, pages[i]);
        LONGS_EQUAL(-EINVAL, hello(h, Primary, hl).r.status);
    }
    hl = helloArg(AllFeatures);
    hl.revision = 0;
    LONGS_EQUAL(-EINVAL, hello(h, Primary, hl).r.status);
    hl = helloArg(AllFeatures);
    /* A handle where none belongs, a short and a long argument. */
    LONGS_EQUAL(-EMSGSIZE, hello(h, Primary, hl, 0x1234).r.status);
    {
        std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, &hl, sizeof(hl) - 1);

        LONGS_EQUAL(-EMSGSIZE, h.send(Primary, &v[0], v.size()).r.status);
        memset(big, 0, sizeof(big));
        memcpy(big, &hl, sizeof(hl));
        v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, big, sizeof(big));
        LONGS_EQUAL(-EMSGSIZE, h.send(Primary, &v[0], v.size()).r.status);
        v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, NULL, 0);
        LONGS_EQUAL(-EMSGSIZE, h.send(Primary, &v[0], v.size()).r.status);
    }
    iav_broker_cid_hello(&h.b, Primary, &st);
    LONGS_EQUAL(0, st.done);
    LONGS_EQUAL(0, h.drv.ioctlCalls());
}

TEST(IavBrokerHello, NegotiationIsPerGuestAndGoesWithTheGuest)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    struct iav_broker_hello st;

    helloOut(hello(h, Primary, helloArg(AllFeatures)));
    iav_broker_cid_hello(&h.b, Primary, &st);
    LONGS_EQUAL(1, st.done);
    iav_broker_cid_hello(&h.b, Consumer, &st);
    LONGS_EQUAL(0, st.done);
    iav_broker_client_gone(&h.b, Primary);
    iav_broker_cid_hello(&h.b, Primary, &st);
    LONGS_EQUAL(0, st.done);
}

/* ---- per-guest OP_INFO ---- */

TEST_GROUP(IavBrokerInfo)
{
    std::vector<struct amba_virt_iav_window> legacy(IavBrokerHarness &h, uint32_t cid)
    {
        IavBrokerHarness::Result r = h.info(cid);
        std::vector<struct amba_virt_iav_window> w;

        LONGS_EQUAL(0, r.r.status);
        LONGS_EQUAL(0, r.out.size() % sizeof(struct amba_virt_iav_window));
        w.resize(r.out.size() / sizeof(struct amba_virt_iav_window));
        if (!w.empty()) {
            memcpy(&w[0], &r.out[0], r.out.size());
        }
        UNSIGNED_LONGS_EQUAL(w.size(), r.r.value);
        return w;
    }

    std::vector<struct amba_virt_iav_entry> ext(IavBrokerHarness &h, uint32_t cid)
    {
        IavBrokerHarness::Result r = h.info(cid);
        std::vector<struct amba_virt_iav_entry> e;

        LONGS_EQUAL(0, r.r.status);
        LONGS_EQUAL(0, r.out.size() % sizeof(struct amba_virt_iav_entry));
        e.resize(r.out.size() / sizeof(struct amba_virt_iav_entry));
        if (!e.empty()) {
            memcpy(&e[0], &r.out[0], r.out.size());
        }
        return e;
    }
};

TEST(IavBrokerInfo, AGuestThatSentNoHelloSeesOnlyTheLegacyWindowsOfItsRole)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    std::vector<struct amba_virt_iav_window> w;

    h.addOverlayWindow();
    w = legacy(h, Primary);
    LONGS_EQUAL(4, w.size());
    for (size_t i = 0; i < w.size(); i++) {
        CHECK(w[i].id < AMBA_VIRT_IAV_MAX_WINDOWS);
        LONGS_EQUAL(0, w[i].flags);
    }
    /* A consumer has no call that uses the decode or overlay window: it is not told about them. */
    w = legacy(h, Consumer);
    LONGS_EQUAL(2, w.size());
    LONGS_EQUAL(AMBA_VIRT_IAV_WIN_DSP, w[0].id);
    LONGS_EQUAL(AMBA_VIRT_IAV_WIN_BSB, w[1].id);
}

TEST(IavBrokerInfo, ANegotiatedGuestGetsExtendedEntriesUpToItsLimit)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    std::vector<struct amba_virt_iav_entry> e;

    h.addOverlayWindow();
    iav_broker_set_host_features(&h.b, AMBA_VIRT_IAV_F_NC_WINDOWS);
    helloOut(hello(h, Primary, helloArg(AllFeatures)));
    e = ext(h, Primary);
    LONGS_EQUAL(4, e.size());
    for (size_t i = 0; i < e.size(); i++) {
        CHECK(e[i].flags & AMBA_VIRT_IAV_ENT_NORMAL_NC);
        CHECK(e[i].flags & AMBA_VIRT_IAV_ENT_RING);
        LONGS_EQUAL(0, e[i].offset);
        LONGS_EQUAL(0, e[i].reserved);
        if (e[i].id == AMBA_VIRT_IAV_WIN_DEC || e[i].id == AMBA_VIRT_IAV_WIN_OVL) {
            CHECK(e[i].flags & AMBA_VIRT_IAV_ENT_PRIMARY_ONLY);
        } else {
            CHECK_FALSE(e[i].flags & AMBA_VIRT_IAV_ENT_PRIMARY_ONLY);
        }
    }
    /* A module that holds two windows is told about two. */
    helloOut(hello(h, Primary, helloArg(AllFeatures, 1)));
    e = ext(h, Primary);
    LONGS_EQUAL(2, e.size());
    /* Without NC granted the flag is not set; without EXT_INFO the legacy entry comes back. */
    helloOut(hello(h, Consumer, helloArg(AMBA_VIRT_IAV_F_EXT_INFO)));
    e = ext(h, Consumer);
    LONGS_EQUAL(2, e.size());
    CHECK_FALSE(e[0].flags & AMBA_VIRT_IAV_ENT_NORMAL_NC);
    helloOut(hello(h, Consumer, helloArg(AMBA_VIRT_IAV_F_SIZE_OVERRIDE)));
    LONGS_EQUAL(2, legacy(h, Consumer).size());
}

/* ---- the wire size ---- */

TEST_GROUP(IavBrokerWireSize)
{
};

TEST(IavBrokerWireSize, TheTableNamesTheRowsWhoseHandlerCopiesAnotherType)
{
    unsigned int n = 0;

    for (size_t i = 0; i < iav_policy_count(); i++) {
        const struct iav_policy_entry *e = iav_policy_at(i);

        if (e->wire) {
            n++;
            CHECK_TEXT(e->kind == IAV_ARG_PTR, e->name);
            CHECK_TEXT(e->wire != e->size, e->name);
            CHECK_TEXT(e->wire <= AMBA_VIRT_IAV_MAX_ARG, e->name);
        }
    }
    LONGS_EQUAL(22, n);
    LONGS_EQUAL(sizeof(struct iav_alloc_mem_part), iav_policy_lookup(IAV_IOC_ALLOC_MEM_PART)->wire);
    LONGS_EQUAL(sizeof(u32), iav_policy_lookup(IAV_IOC_WAIT_RAW_ENCODE)->wire);
    LONGS_EQUAL(sizeof(struct iav_apply_flag) * IAV_VIDEO_PROC_NUM, iav_policy_lookup(IAV_IOC_APPLY_VIDEO_PROC)->wire);
    LONGS_EQUAL(sizeof(struct iav_preview_params), iav_policy_lookup(IAV_IOC_ENABLE_PREVIEW)->wire);
}

TEST(IavBrokerWireSize, ALegacyGuestCarriesTheHeaderSizeAndANegotiatedOneTheWireSize)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    uint64_t hd = h.mustOpen(Primary);
    uint8_t arg[sizeof(struct iav_preview_params)];
    std::vector<uint8_t> seen;

    memset(arg, 0xff, sizeof(arg));
    /* Legacy: one byte, as the header says; the full struct is a framing error. */
    LONGS_EQUAL(0, h.ioctl(Primary, hd, IAV_IOC_ENABLE_PREVIEW, arg, 1).r.status);
    LONGS_EQUAL(-EMSGSIZE, h.ioctl(Primary, hd, IAV_IOC_ENABLE_PREVIEW, arg, sizeof(arg)).r.status);

    helloOut(hello(h, Primary, helloArg(AMBA_VIRT_IAV_F_SIZE_OVERRIDE)));
    LONGS_EQUAL(-EMSGSIZE, h.ioctl(Primary, hd, IAV_IOC_ENABLE_PREVIEW, arg, 1).r.status);
    h.drv.captureArgOf(IAV_IOC_ENABLE_PREVIEW, sizeof(arg));
    LONGS_EQUAL(0, h.ioctl(Primary, hd, IAV_IOC_ENABLE_PREVIEW, arg, sizeof(arg)).r.status);
    /* Nobody audited the bytes past the header: the driver sees the header's byte and zeros, as before. */
    seen = h.drv.capturedArg();
    LONGS_EQUAL(sizeof(arg), seen.size());
    LONGS_EQUAL(0xff, seen[0]);
    for (size_t i = 1; i < seen.size(); i++) {
        LONGS_EQUAL_TEXT(0, seen[i], "unaudited wire byte reached the driver");
    }
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerWireSize, AnOutputOfTheWireSizeComesBackOnlyToANegotiatedGuest)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    uint64_t hd = h.mustOpen(Primary);
    uint8_t arg[sizeof(struct iav_apply_frame_sync_flag)];
    IavBrokerHarness::Result r;

    memset(arg, 0, sizeof(arg));
    r = h.ioctl(Primary, hd, IAV_IOC_APPLY_IDSP_SYNC_PROC, arg, 1);
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(1, r.out.size());
    helloOut(hello(h, Primary, helloArg(AMBA_VIRT_IAV_F_SIZE_OVERRIDE)));
    r = h.ioctl(Primary, hd, IAV_IOC_APPLY_IDSP_SYNC_PROC, arg, sizeof(arg));
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(sizeof(arg), r.out.size());
}

TEST(IavBrokerWireSize, TheRefusedOverrideRowsStayRefusedAtEitherSize)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    uint64_t hd = h.mustOpen(Primary);
    unsigned long refused[] = {IAV_IOC_ALLOC_MEM_PART, IAV_IOC_ALLOC_ANON_MEM_PART, IAV_IOC_ALLOC_ANON_MEM_PART_FD,
                               IAV_IOC_REGISTER_ANON_MEM_PART, IAV_IOC_UNREGISTER_ANON_MEM_PART,
                               IAV_IOC_EFM_SET_EXT_POOL, IAV_IOC_WAIT_RAW_ENCODE, IAV_IOC_FLUSH_FRAMEDESC};
    uint8_t arg[AMBA_VIRT_IAV_MAX_ARG];

    memset(arg, 0, sizeof(arg));
    helloOut(hello(h, Primary, helloArg(AllFeatures)));
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        const struct iav_policy_entry *e = iav_policy_lookup((unsigned int)refused[i]);

        CHECK(e != NULL);
        LONGS_EQUAL_TEXT(-EPERM, h.ioctl(Primary, hd, e->cmd, arg, (uint32_t)e->size).r.status, e->name);
        LONGS_EQUAL_TEXT(-EPERM, h.ioctl(Primary, hd, e->cmd, arg, (uint32_t)e->wire).r.status, e->name);
    }
    LONGS_EQUAL(0, h.drv.ioctlCalls());
}

/* ---- window pinning ---- */

TEST_GROUP(IavBrokerPinning)
{
    IavBrokerHarness *h;
    uint64_t hd;

    void setup() override
    {
        h = new IavBrokerHarness(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
        h->addOverlayWindow();
        /* the windows the server sets carry their partition ids */
        h->b.windows[0].pid = IAV_PART_DSP;
        h->b.windows[1].pid = IAV_PART_BSB;
        h->b.windows[2].pid = IAV_PART_DEC_BSB;
        h->b.windows[3].pid = IAV_PART_OVERLAY;
        /* channel 0 is the sensor, channel 1 the decoder fed from VIN source 1 */
        h->drv.setChannel(0, false, 0, false);
        h->drv.setChannel(1, true, 1, false);
        h->drv.setChanMap(0x3);
        hd = h->mustOpen(Primary);
    }

    void teardown() override
    {
        CHECK(h->drv.violations().empty());
        delete h;
    }

    IavBrokerHarness::Result chan(unsigned int id, bool preDec, unsigned int vsrc, bool swCe, uint32_t cid = Primary)
    {
        struct iav_chan_cfg c;

        memset(&c, 0, sizeof(c));
        c.chan_id = id;
        c.pre_dec_enable = preDec ? 1 : 0;
        c.vsrc_id = vsrc;
        c.sw_ce_enable = swCe ? 1 : 0;
        return h->ioctl(cid, hd, IAV_IOC_SET_CHAN_CONFIG, &c, sizeof(c));
    }

    IavBrokerHarness::Result resource(uint32_t chanMap)
    {
        struct iav_system_resource r;

        memset(&r, 0, sizeof(r));
        r.encode_mode = 0;
        r.chan_map = chanMap;
        return h->ioctl(Primary, hd, IAV_IOC_SET_SYSTEM_RESOURCE, &r, sizeof(r));
    }
};

TEST(IavBrokerPinning, ChannelChangesThatMoveAPartitionAreRefused)
{
    unsigned long before = h->drv.ioctlCalls();

    LONGS_EQUAL(0, chan(0, false, 0, false).r.status);           /* unchanged: reaches the driver */
    LONGS_EQUAL(0, chan(0, false, 2, false).r.status);           /* a sensor channel may change its source */
    LONGS_EQUAL(-EPERM, chan(0, true, 0, false).r.status);       /* a second decoder */
    LONGS_EQUAL(-EPERM, chan(1, false, 1, false).r.status);      /* the decoder goes */
    LONGS_EQUAL(-EPERM, chan(1, true, 2, false).r.status);       /* the decoder moves to another source */
    LONGS_EQUAL(-EPERM, chan(0, false, 0, true).r.status);       /* software CE resizes CE_GO_MAP */
    LONGS_EQUAL(4, h->b.n_pinned);
    /* two SETs reached the driver, and one GET per check */
    UNSIGNED_LONGS_EQUAL(before + 2 + 6, h->drv.ioctlCalls());
}

TEST(IavBrokerPinning, AChannelOfUnknownStateIsRefused)
{
    h->drv.failIoctl(IAV_IOC_GET_CHAN_CONFIG, EIO);
    LONGS_EQUAL(-EPERM, chan(0, false, 0, false).r.status);
    h->drv.failIoctl(IAV_IOC_GET_CHAN_CONFIG, 0);
    LONGS_EQUAL(-EPERM, chan(200, false, 0, false).r.status);    /* the driver does not know the channel */
}

TEST(IavBrokerPinning, AResourceChangeOfTheChannelMapIsRefused)
{
    LONGS_EQUAL(0, resource(0x3).r.status);
    LONGS_EQUAL(-EPERM, resource(0x7).r.status);
    LONGS_EQUAL(-EPERM, resource(0x1).r.status);
    LONGS_EQUAL(-EPERM, resource(0).r.status);
    LONGS_EQUAL(3, h->b.n_pinned);
    h->drv.failIoctl(IAV_IOC_GET_SYSTEM_RESOURCE, EIO);
    LONGS_EQUAL(-EPERM, resource(0x3).r.status);
}

TEST(IavBrokerPinning, AConsumerNeverReachesTheChecks)
{
    uint64_t other = h->mustOpen(Consumer);
    struct iav_chan_cfg c;
    unsigned long before = h->drv.ioctlCalls();

    memset(&c, 0, sizeof(c));
    LONGS_EQUAL(-EPERM, h->ioctl(Consumer, other, IAV_IOC_SET_CHAN_CONFIG, &c, sizeof(c)).r.status);
    LONGS_EQUAL(before, h->drv.ioctlCalls());
}

TEST(IavBrokerPinning, APartitionThatMovesAtPreviewStopsEveryWindowBackedCall)
{
    uint8_t one = 0;
    struct iav_decode_jpeg j;
    IavBrokerHarness::Result r;
    unsigned long before;
    char status[1024];

    LONGS_EQUAL(0, h->map(Primary, hd, MockIavDriver::DecBase, 4096, PROT_READ).r.status);
    h->drv.moveDecodeOnPreview(MockIavDriver::DecBase + 0x1000000, MockIavDriver::DecLen);
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_ENABLE_PREVIEW, &one, 1).r.status);
    LONGS_EQUAL(1, iav_broker_windows_stale(&h->b));
    CHECK(iav_broker_format_status(&h->b, status, sizeof(status)) > 0);
    CHECK(strstr(status, "windows=stale") != NULL);

    before = h->drv.ioctlCalls();
    LONGS_EQUAL(-EIO, h->map(Primary, hd, MockIavDriver::DecBase, 4096, PROT_READ).r.status);
    LONGS_EQUAL(-EIO, h->map(Primary, hd, MockIavDriver::DspBase, 4096, PROT_READ).r.status);
    memset(&j, 0, sizeof(j));
    j.jpeg_size = 16;
    LONGS_EQUAL(-EIO, h->ioctl(Primary, hd, IAV_IOC_DECODE_JPEG, &j, sizeof(j)).r.status);
    r = h->canvas(Primary, hd, false);
    LONGS_EQUAL(-EIO, r.r.status);
    LONGS_EQUAL(before, h->drv.ioctlCalls());
    /* Calls that name no window still work. */
    r = h->ioctl(Primary, hd, IAV_IOC_GET_IAV_STATE, NULL, 0);
    LONGS_EQUAL(0, r.r.status);
    CHECK(h->b.n_stale_refused >= 4);

    /* Back where the window points: the windows match again. */
    h->drv.setDecodePartition(MockIavDriver::DecBase, MockIavDriver::DecLen);
    LONGS_EQUAL(0, iav_broker_recheck_windows(&h->b));
    LONGS_EQUAL(0, iav_broker_windows_stale(&h->b));
    LONGS_EQUAL(0, h->map(Primary, hd, MockIavDriver::DecBase, 4096, PROT_READ).r.status);
}

TEST(IavBrokerPinning, AShorterOrMissingPartitionIsStaleToo)
{
    h->drv.setDecodePartition(MockIavDriver::DecBase, MockIavDriver::DecLen - 4096);
    LONGS_EQUAL(1, iav_broker_recheck_windows(&h->b));
    h->drv.setDecodePartition(0, 0);
    LONGS_EQUAL(1, iav_broker_recheck_windows(&h->b));
    h->drv.setDecodePartition(MockIavDriver::DecBase, MockIavDriver::DecLen);
    LONGS_EQUAL(0, iav_broker_recheck_windows(&h->b));
}

TEST(IavBrokerPinning, ARecheckThatCannotOpenTheDriverChangesNothing)
{
    h->drv.setDecodePartition(MockIavDriver::DecBase + 0x1000000, MockIavDriver::DecLen);
    LONGS_EQUAL(1, iav_broker_recheck_windows(&h->b));
    h->drv.setDecodePartition(MockIavDriver::DecBase, MockIavDriver::DecLen);
    h->drv.failOpensWith(EMFILE);
    LONGS_EQUAL(-EMFILE, iav_broker_recheck_windows(&h->b));
    LONGS_EQUAL(1, iav_broker_windows_stale(&h->b));
    h->drv.failOpensWith(0);
    LONGS_EQUAL(0, iav_broker_recheck_windows(&h->b));
}

/* ---- OP_SLICE through the allow-list (D5) ---- */

static int sliceOf7(void *ctx, uint32_t cid, uint32_t *window)
{
    (void)ctx;
    if (window == NULL || (cid != Primary && cid != Consumer)) {
        return -EPERM;
    }
    *window = cid == Primary ? 0 : 1;
    return 0;
}

TEST_GROUP(IavBrokerSliceOp)
{
    IavBrokerHarness *h;
    uint64_t hp, hc;

    void setup() override
    {
        struct virt_iav_guest_pool pool;

        h = new IavBrokerHarness(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
        memset(&pool, 0, sizeof(pool));
        pool.partition_id = 193;
        pool.parent_base = 0x94000000ull;
        pool.parent_bytes = 8ull << 26;
        pool.slice_count = 8;
        pool.slice_bytes = 1ull << 26;
        iav_broker_set_guest_pool(&h->b, &pool);
        h->b.ops.slice_of = sliceOf7;
        hp = h->mustOpen(Primary);
        hc = h->mustOpen(Consumer);
    }

    void teardown() override
    {
        CHECK(h->drv.violations().empty());
        delete h;
    }

    IavBrokerHarness::Result slice(uint32_t cid, uint64_t handle, unsigned long cmd, uint32_t claimed,
                                   const void *ioArg, uint32_t ioLen, uint64_t value = 0)
    {
        struct amba_virt_iav_slice_req sr;
        std::vector<uint8_t> a(sizeof(sr) + ioLen);
        std::vector<uint8_t> v;

        memset(&sr, 0, sizeof(sr));
        sr.claimed_slice = claimed;
        sr.partition_id = 193;
        sr.offset = (uint64_t)claimed * (1ull << 26) + 4096;	/* offsets are into the whole pool */
        sr.length = 4096;
        memcpy(&a[0], &sr, sizeof(sr));
        if (ioLen) {
            memcpy(&a[sizeof(sr)], ioArg, ioLen);
        }
        v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_SLICE, handle, (uint32_t)cmd, value, 0, &a[0], (uint32_t)a.size());
        return h->send(cid, &v[0], v.size());
    }
};

TEST(IavBrokerSliceOp, AnIoctlTheAllowListRefusesIsRefusedHereToo)
{
    unsigned long before = h->drv.ioctlCalls();

    /* the D5 hole: any number with a four-byte scratch; now the allow-list decides */
    LONGS_EQUAL(-EPERM, slice(Consumer, hc, IAV_IOC_ABORT_ENCODE, 1, NULL, 0, 1).r.status);
    LONGS_EQUAL(-EPERM, slice(Consumer, hc, IAV_IOC_START_ENCODE, 1, NULL, 0, 1).r.status);
    LONGS_EQUAL(-EPERM, slice(Primary, hp, IAV_IOC_RESET, 0, NULL, 0).r.status);
    LONGS_EQUAL(-ENOTTY, slice(Primary, hp, 0xdeadbeefu, 0, NULL, 0).r.status);
    LONGS_EQUAL(before, h->drv.ioctlCalls());
}

TEST(IavBrokerSliceOp, TheIoctlArgumentFollowsTheAdmissionAndIsFramedAsAnIoctl)
{
    u32 id = 0;
    IavBrokerHarness::Result r;
    uint8_t one = 0;
    unsigned long before = h->drv.ioctlCalls();

    r = slice(Consumer, hc, IAV_IOC_GET_CHIP_ID, 1, NULL, 0);
    LONGS_EQUAL(0, r.r.status);
    UNSIGNED_LONGS_EQUAL(0, r.r.value);
    LONGS_EQUAL(sizeof(id), r.out.size());
    /* a read-only argument carries nothing in; a value call carries no bytes */
    LONGS_EQUAL(-EMSGSIZE, slice(Consumer, hc, IAV_IOC_GET_CHIP_ID, 1, &id, sizeof(id)).r.status);
    LONGS_EQUAL(-EMSGSIZE, slice(Primary, hp, IAV_IOC_ENABLE_PREVIEW, 0, &one, 0).r.status);
    LONGS_EQUAL(0, slice(Primary, hp, IAV_IOC_ENABLE_PREVIEW, 0, &one, 1).r.status);
    /*
     * two ioctls, and the re-query after preview: one QUERY_MEMBLOCK for the DSP window and one for the pinned pool
     * behind the arenas
     */
    UNSIGNED_LONGS_EQUAL(before + 4, h->drv.ioctlCalls());
}

TEST(IavBrokerSliceOp, AdmissionComesFirstAndNoAddressGoesBack)
{
    unsigned long before = h->drv.ioctlCalls();
    std::vector<uint8_t> v;
    uint8_t shortArg[8] = {0};

    /* another guest's slice, then a request shorter than the admission header */
    LONGS_EQUAL(-EPERM, slice(Consumer, hc, IAV_IOC_GET_CHIP_ID, 0, NULL, 0).r.status);
    v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_SLICE, hc, IAV_IOC_GET_CHIP_ID, 0, 0, shortArg, sizeof(shortArg));
    LONGS_EQUAL(-EMSGSIZE, h->send(Consumer, &v[0], v.size()).r.status);
    /* a stale handle after admission */
    LONGS_EQUAL(-ESTALE, slice(Consumer, hc ^ (1ull << 16), IAV_IOC_GET_CHIP_ID, 1, NULL, 0).r.status);
    /* a foreign handle */
    LONGS_EQUAL(-EACCES, slice(Consumer, hp, IAV_IOC_GET_CHIP_ID, 1, NULL, 0).r.status);
    LONGS_EQUAL(before, h->drv.ioctlCalls());
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
