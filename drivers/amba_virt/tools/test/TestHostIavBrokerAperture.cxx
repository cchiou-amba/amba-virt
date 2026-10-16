/*
 * TestHostIavBrokerAperture.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The per-guest aux aperture and the calls it unlocks (plan_iav_usb_yolo, Envelope F): OP_HELLO grants the aperture
 * only to a primary guest that has one attached and takes the extended OP_INFO; OP_INFO describes the caller's own
 * aperture; OP_MAP of an entry answers its id and offset in the aperture BAR; every call that names a table in MASK,
 * WARP, QPMATRIX, CE_GO_MAP, CANVAS_OVERLAY, ARB_BLUR or YUV_REMAP is confined to the caller's entry, and the ids the
 * handlers shift by or index with are bounded. IMG_GET_MISC_INFO carries its user pointers inline. The mock driver
 * records a violation for anything that reaches it outside those rules, and the tests count the driver calls.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "IavBrokerHarness.hxx"

#include <ambas_imgproc_ioctl_arch.h>

#include "CppUTest/TestHarness.h"

namespace {

const uint32_t Primary = 7;
const uint32_t Consumer = 9;
const uint64_t Offered = AMBA_VIRT_IAV_F_SIZE_OVERRIDE | AMBA_VIRT_IAV_F_EXT_INFO | AMBA_VIRT_IAV_F_APERTURE |
                         AMBA_VIRT_IAV_F_INLINE;
const uint64_t MaskLen = 0x2000000ull, WarpLen = 0x360000ull, CovlLen = 0x200000ull, ArbLen = 0x100000ull,
               RemapLen = 0x10000ull, CeLen = 0x1000ull;

IavBrokerHarness::Result hello(IavBrokerHarness &h, uint32_t cid, uint64_t features)
{
    struct amba_virt_iav_hello hl;

    memset(&hl, 0, sizeof(hl));
    hl.revision = AMBA_VIRT_IAV_REVISION;
    hl.page_size = 4096;
    hl.features = features;
    hl.max_window_id = 3;
    std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, &hl, sizeof(hl));
    return h.send(cid, &v[0], v.size());
}

uint64_t granted(const IavBrokerHarness::Result &r)
{
    return r.r.status == 0 ? r.r.value : 0;
}

std::vector<struct amba_virt_iav_entry> entries(const IavBrokerHarness::Result &r)
{
    std::vector<struct amba_virt_iav_entry> v;

    if (r.r.status != 0 || r.out.size() % sizeof(struct amba_virt_iav_entry)) {
        return v;
    }
    v.resize(r.out.size() / sizeof(struct amba_virt_iav_entry));
    if (!v.empty()) {
        memcpy(&v[0], &r.out[0], r.out.size());
    }
    return v;
}

const struct amba_virt_iav_entry *findId(const std::vector<struct amba_virt_iav_entry> &v, uint32_t id)
{
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].id == id) {
            return &v[i];
        }
    }
    return NULL;
}

/* A primary guest (7) with the standard aperture, negotiated; a consumer (9) negotiated without one. */
struct Fixture
{
    IavBrokerHarness h;
    uint64_t hp, hc;

    Fixture() : h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary)
    {
        h.addOverlayWindow();
        h.drv.setStandardAperture(Primary);
        h.drv.setBlurPartitions(0x100000, 0);
        iav_broker_set_aux_sources(&h.b, MockIavDriver::StandardAux, MockIavDriver::AuxCount);
        hello(h, Primary, Offered);
        hello(h, Consumer, Offered);
        hp = h.mustOpen(Primary);
        hc = h.mustOpen(Consumer);
    }

    int32_t status(uint32_t cid, unsigned long cmd, const void *arg, size_t len)
    {
        return h.ioctl(cid, cid == Primary ? hp : hc, cmd, arg, (uint32_t)len).r.status;
    }
};

struct iav_video_proc vproc(int cid)
{
    struct iav_video_proc v;

    memset(&v, 0, sizeof(v));
    memcpy(&v.cid, &cid, sizeof(cid));
    return v;
}

struct iav_stream_cfg streamCfg(uint32_t id, int cid)
{
    struct iav_stream_cfg c;

    memset(&c, 0, sizeof(c));
    c.id = id;
    memcpy(&c.cid, &cid, sizeof(cid));
    return c;
}

void overlayArea(struct iav_overlay_area &a, uint64_t data, uint64_t clut, unsigned int pitch, unsigned int h)
{
    memset(&a, 0, sizeof(a));
    a.enable = 1;
    a.width = (u16)pitch;
    a.pitch = (u16)pitch;
    a.height = (u16)h;
    a.total_size = pitch * h;
    a.data_addr_offset = data;
    a.clut_addr_offset = clut;
}

} // namespace

TEST_GROUP(IavBrokerAperture)
{
};

/* ---- negotiation, OP_INFO and OP_MAP ---- */

TEST(IavBrokerAperture, TheApertureIsGrantedOnlyToAPrimaryWithOneAttachedThatTakesTheExtendedInfo)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);

    /* nothing attached yet */
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    h.drv.setStandardAperture(Primary);
    CHECK(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    /* not without the extended OP_INFO that describes it, not unless offered */
    CHECK_FALSE(granted(hello(h, Primary, Offered & ~AMBA_VIRT_IAV_F_EXT_INFO)) & AMBA_VIRT_IAV_F_APERTURE);
    CHECK_FALSE(granted(hello(h, Primary, Offered & ~AMBA_VIRT_IAV_F_APERTURE)) & AMBA_VIRT_IAV_F_APERTURE);
    /* a consumer, even one the server (wrongly) reports an aperture for */
    h.drv.setStandardAperture(Consumer);
    CHECK_FALSE(granted(hello(h, Consumer, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    LONGS_EQUAL(0, h.drv.ioctlCalls());
}

TEST(IavBrokerAperture, OpInfoDescribesTheCallersOwnApertureOnly)
{
    Fixture f;
    std::vector<struct amba_virt_iav_entry> v = entries(f.h.info(Primary));
    const struct amba_virt_iav_entry *d = findId(v, AMBA_VIRT_IAV_ENT_ID_APERTURE);

    CHECK(d != NULL);
    UNSIGNED_LONGS_EQUAL(MockIavDriver::StandardAuxSize, d->size);
    UNSIGNED_LONGS_EQUAL(MockIavDriver::StandardAuxSize - 4096, d->offset);
    UNSIGNED_LONGS_EQUAL(MockIavDriver::StandardAuxEpoch, d->reserved);
    CHECK(d->flags & AMBA_VIRT_IAV_ENT_APERTURE);
    for (unsigned int i = 0; i < MockIavDriver::AuxCount; i++) {
        const struct iav_broker_aux_entry &a = MockIavDriver::StandardAux[i];
        const struct amba_virt_iav_entry *e = findId(v, AMBA_VIRT_IAV_ENT_ID_AUX + a.pid);

        CHECK(e != NULL);
        UNSIGNED_LONGS_EQUAL(a.base, e->base);
        UNSIGNED_LONGS_EQUAL(a.len, e->part_len);
        UNSIGNED_LONGS_EQUAL(a.offset, e->offset);
        UNSIGNED_LONGS_EQUAL(a.pid, e->pid);
        CHECK(e->size >= e->part_len && e->offset + e->size <= MockIavDriver::StandardAuxSize - 4096);
        CHECK(e->flags & AMBA_VIRT_IAV_ENT_PRIMARY_ONLY);
    }
    /* the consumer sees no aperture, nor does a primary that did not negotiate it */
    CHECK(findId(entries(f.h.info(Consumer)), AMBA_VIRT_IAV_ENT_ID_APERTURE) == NULL);
    hello(f.h, Primary, Offered & ~AMBA_VIRT_IAV_F_APERTURE);
    CHECK(findId(entries(f.h.info(Primary)), AMBA_VIRT_IAV_ENT_ID_APERTURE) == NULL);
    CHECK(findId(entries(f.h.info(Primary)), AMBA_VIRT_IAV_ENT_ID_AUX + IAV_PART_MASK) == NULL);
}

TEST(IavBrokerAperture, AGuestDemotedAfterItNegotiatedSeesAndMapsNoMore)
{
    Fixture f;
    const struct iav_broker_aux_entry &m = MockIavDriver::StandardAux[0];

    CHECK(findId(entries(f.h.info(Primary)), AMBA_VIRT_IAV_ENT_ID_APERTURE) != NULL);
    /* the role table now says consumer for it (set-role): the next call sees that, whatever it negotiated */
    f.h.drv.setPrimaryCid(0);
    CHECK(findId(entries(f.h.info(Primary)), AMBA_VIRT_IAV_ENT_ID_APERTURE) == NULL);
    CHECK(findId(entries(f.h.info(Primary)), AMBA_VIRT_IAV_ENT_ID_AUX + IAV_PART_MASK) == NULL);
    LONGS_EQUAL(-EFAULT, f.h.map(Primary, f.hp, m.base, m.len, PROT_READ).r.status);
    LONGS_EQUAL(0, f.h.drv.ioctlCalls());
}

TEST(IavBrokerAperture, AMalformedApertureFromTheServerIsNoAperture)
{
    struct iav_broker_aux_entry bad[2];
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);

    memcpy(bad, MockIavDriver::StandardAux, sizeof(bad));
    /* two entries in one place of the BAR */
    bad[1].offset = bad[0].offset;
    h.drv.setAperture(Primary, MockIavDriver::StandardAuxSize, 5, bad, 2);
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    /* an entry over the header page */
    memcpy(bad, MockIavDriver::StandardAux, sizeof(bad));
    bad[0].offset = MockIavDriver::StandardAuxSize - 0x10000;
    bad[0].len = 0x10000;
    h.drv.setAperture(Primary, MockIavDriver::StandardAuxSize, 5, bad, 1);
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    /* not page aligned, offset not 64 KiB aligned, epoch 0 or too large, size not a power of two */
    memcpy(bad, MockIavDriver::StandardAux, sizeof(bad));
    bad[0].len = 0x1800;
    h.drv.setAperture(Primary, MockIavDriver::StandardAuxSize, 5, bad, 1);
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    memcpy(bad, MockIavDriver::StandardAux, sizeof(bad));
    bad[0].offset = 0x1000;
    h.drv.setAperture(Primary, MockIavDriver::StandardAuxSize, 5, bad, 1);
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    memcpy(bad, MockIavDriver::StandardAux, sizeof(bad));
    h.drv.setAperture(Primary, MockIavDriver::StandardAuxSize, 0, bad, 1);
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    h.drv.setAperture(Primary, MockIavDriver::StandardAuxSize, 0x100000000ull, bad, 1);
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    h.drv.setAperture(Primary, MockIavDriver::StandardAuxSize - 4096, 5, bad, 1);
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
    /* a pid that is not an IAV partition id */
    bad[0].pid = 0x1ff;
    h.drv.setAperture(Primary, MockIavDriver::StandardAuxSize, 5, bad, 1);
    CHECK_FALSE(granted(hello(h, Primary, Offered)) & AMBA_VIRT_IAV_F_APERTURE);
}

TEST(IavBrokerAperture, AnEntryIsMappedAtItsAddressFromTheApertureBar)
{
    Fixture f;
    const struct iav_broker_aux_entry &w = MockIavDriver::StandardAux[1];
    IavBrokerHarness::Result r = f.h.map(Primary, f.hp, w.base, w.len, PROT_READ | PROT_WRITE);
    struct amba_virt_iav_map m;

    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(sizeof(m), r.out.size());
    memcpy(&m, &r.out[0], sizeof(m));
    UNSIGNED_LONGS_EQUAL(AMBA_VIRT_IAV_ENT_ID_AUX + IAV_PART_WARP, m.window);
    UNSIGNED_LONGS_EQUAL(w.offset, m.window_offset);
    UNSIGNED_LONGS_EQUAL(w.len, m.length);
    /* a ring map of twice the entry; more is refused */
    LONGS_EQUAL(0, f.h.map(Primary, f.hp, w.base, 2 * w.len, PROT_READ).r.status);
    LONGS_EQUAL(-EINVAL, f.h.map(Primary, f.hp, w.base, 2 * w.len + 4096, PROT_READ).r.status);
    /* an address inside the entry is not an entry; neither is the end */
    LONGS_EQUAL(-EFAULT, f.h.map(Primary, f.hp, w.base + 4096, 4096, PROT_READ).r.status);
    LONGS_EQUAL(-EFAULT, f.h.map(Primary, f.hp, w.base + w.len, 4096, PROT_READ).r.status);
    /* the consumer, and a primary that did not negotiate the aperture, map nothing of it */
    LONGS_EQUAL(-EFAULT, f.h.map(Consumer, f.hc, w.base, w.len, PROT_READ).r.status);
    hello(f.h, Primary, Offered & ~AMBA_VIRT_IAV_F_APERTURE);
    LONGS_EQUAL(-EFAULT, f.h.map(Primary, f.hp, w.base, w.len, PROT_READ).r.status);
    LONGS_EQUAL(0, f.h.drv.ioctlCalls());
}

TEST(IavBrokerAperture, AMovedAperturePartitionStopsTheApertureLikeAMovedWindow)
{
    Fixture f;
    struct iav_yuv_remap r;

    LONGS_EQUAL(0, iav_broker_recheck_windows(&f.h.b));
    f.h.drv.setAuxPartition(IAV_PART_ARB_BLUR, 0xb5000000ull, ArbLen);
    LONGS_EQUAL(1, iav_broker_recheck_windows(&f.h.b));
    LONGS_EQUAL(-EIO, f.h.map(Primary, f.hp, MockIavDriver::StandardAux[0].base, MaskLen, PROT_READ).r.status);
    memset(&r, 0, sizeof(r));
    LONGS_EQUAL(-EIO, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    /* back where it was: served again */
    f.h.drv.setAuxPartition(IAV_PART_ARB_BLUR, MockIavDriver::StandardAux[5].base, ArbLen);
    LONGS_EQUAL(0, iav_broker_recheck_windows(&f.h.b));
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    CHECK(f.h.drv.violations().empty());
}

/* ---- CFG_VIDEO_PROC ---- */

TEST(IavBrokerAperture, PrivacyMaskNeedsTheMaskEntryAndABoundedChannel)
{
    Fixture f;
    struct iav_video_proc v = vproc(IAV_VIDEO_PROC_MASK);

    v.arg.mask.channel_id = 1;
    v.arg.mask.enable = 1;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.mask.channel_id = 4;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.mask.channel_id = 32;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    /* without the entry the guest cannot have written the mask */
    f.h.drv.setAperture(0, 0, 0, NULL, 0);
    v.arg.mask.channel_id = 0;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    CHECK(f.h.drv.violations().empty());
}

TEST(IavBrokerAperture, VideoProcessingCidsAreBoundedAndThePartitionlessOnesRefused)
{
    Fixture f;
    const int refused[] = { IAV_VIDEO_PROC_MULTI_BLEND, IAV_VIDEO_PROC_BLEND_WARP, IAV_VIDEO_PROC_MATCH_TABLE,
                            IAV_VIDEO_PROC_ARB_BLEND, IAV_VIDEO_PROC_FUSION, IAV_VIDEO_PROC_OF_NN,
                            IAV_VIDEO_PROC_TML_FUSION, IAV_VIDEO_PROC_WARP, IAV_VIDEO_PROC_VSRC_REGION,
                            IAV_VIDEO_PROC_BLEND_DPTZ, IAV_VIDEO_PROC_NUM, -1, 0x7fffffff };
    struct iav_video_proc v;
    unsigned long before = f.h.drv.ioctlCalls();

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        v = vproc(refused[i]);
        LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    }
    UNSIGNED_LONGS_EQUAL(before, f.h.drv.ioctlCalls());

    v = vproc(IAV_VIDEO_PROC_DPTZ);
    v.arg.dptz.channel_id = 1;
    v.arg.dptz.pass_id = 1;
    v.arg.dptz.buf_id = 7;
    v.arg.dptz.buf_cfg.canvas_id = IAV_INVALID_CANVAS_ID;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.dptz.buf_id = 8;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.dptz.buf_id = 0;
    v.arg.dptz.pass_id = 2;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.dptz.pass_id = 0;
    v.arg.dptz.buf_cfg.canvas_id = 20;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));

    v = vproc(IAV_VIDEO_PROC_CANVAS_FPS);
    v.arg.canvas_fps.id = 19;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.canvas_fps.id = 20;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v = vproc(IAV_VIDEO_PROC_CANVAS_VV_SYNC_CFG);
    v.arg.canvas_vv_sync.id = 31;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v = vproc(IAV_VIDEO_PROC_CANVAS_DUMMY_LATENCY);
    v.arg.canvas_dummy_latency.id = 25;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));

    v = vproc(IAV_VIDEO_PROC_CANVAS_ORDER);
    v.arg.canvas_order.id = 0;
    v.arg.canvas_order.chan_num = 2;
    v.arg.canvas_order.chan_order[0] = 1;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    /* the driver shifts by every entry up to the count it stored, which may be larger than this one */
    v.arg.canvas_order.chan_order[15] = 32;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.canvas_order.chan_order[15] = 0;
    v.arg.canvas_order.chan_num = 17;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));

    v = vproc(IAV_VIDEO_PROC_POSTP_SCALE);
    v.arg.postp_scale.canvas_id = 1;
    v.arg.postp_scale.area[6].enable = 1;
    v.arg.postp_scale.area[6].src_canvas_id = 20;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.postp_scale.area[6].src_canvas_id = 2;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));

    const int chanCids[] = { IAV_VIDEO_PROC_PYRAMID_ROI, IAV_VIDEO_PROC_PYRAMID_FPS, IAV_VIDEO_PROC_VCAP_FLUSH,
                             IAV_VIDEO_PROC_CAWARP };
    for (size_t i = 0; i < sizeof(chanCids) / sizeof(chanCids[0]); i++) {
        v = vproc(chanCids[i]);
        LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
        if (chanCids[i] == IAV_VIDEO_PROC_PYRAMID_ROI) {
            v.arg.pyramid_roi.chan_id = 4;
        } else if (chanCids[i] == IAV_VIDEO_PROC_PYRAMID_FPS) {
            v.arg.pyramid_fps.chan_id = 4;
        } else if (chanCids[i] == IAV_VIDEO_PROC_VCAP_FLUSH) {
            v.arg.vcap_flush.channel_id = 4;
        } else {
            v.arg.cawarp.chan_id = 255;
        }
        LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    }
    CHECK(f.h.drv.violations().empty());
}

TEST(IavBrokerAperture, AVsrcUpdateOfADecoderChannelIsPinned)
{
    Fixture f;
    struct iav_video_proc v = vproc(IAV_VIDEO_PROC_VSRC_UPDATE);

    f.h.drv.setChannel(1, true, 1, false);
    v.arg.vsrc_update.chan_id = 0;
    v.arg.vsrc_update.vsrc_id = 2;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    v.arg.vsrc_update.chan_id = 1;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    CHECK(f.h.b.n_pinned >= 1);
    v.arg.vsrc_update.chan_id = 0;
    v.arg.vsrc_update.vsrc_id = 32;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    /* a failed query refuses */
    f.h.drv.failIoctl(IAV_IOC_GET_CHAN_CONFIG, EIO);
    v.arg.vsrc_update.vsrc_id = 0;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
    f.h.drv.failIoctl(IAV_IOC_GET_CHAN_CONFIG, 0);
    CHECK(f.h.drv.violations().empty());
}

/* ---- canvas overlays (decision D4) and stream overlays ---- */

TEST(IavBrokerAperture, ACanvasOverlayLiesInTheCanvasOverlayEntry)
{
    Fixture f;
    struct iav_overlay_insert o;

    memset(&o, 0, sizeof(o));
    o.id = 0;
    o.id_for_canvas = 1;
    o.enable = 1;
    overlayArea(o.area[0], 0x1000, 0, 128, 64);
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    /* the last bytes of the entry, then one byte past */
    overlayArea(o.area[3], CovlLen - 128 * 64, CovlLen - 1024, 128, 64);
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    overlayArea(o.area[3], CovlLen - 128 * 64 + 1, 0, 128, 64);
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    overlayArea(o.area[3], 0, CovlLen - 1023, 128, 64);
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    /* a short total_size does not make the pitch-by-height read shorter; a wrapping offset is caught */
    overlayArea(o.area[3], CovlLen - 1024, 0, 128, 64);
    o.area[3].total_size = 64;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    overlayArea(o.area[3], ~0ul - 100, 0, 128, 64);
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    memset(&o.area[3], 0, sizeof(o.area[3]));
    /* canvas ids past the table; the same check through the video-processing and canvas-sync calls */
    o.id = 20;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    o.id = 1;
    {
        struct iav_video_proc v = vproc(IAV_VIDEO_PROC_CANVAS_OVERLAY);
        struct iav_canvas_sync_cfg c;
        int cid = IAV_CANVAS_SYNC_CFG_OVERLAY;

        v.arg.overlay = o;
        v.arg.overlay.id_for_canvas = 0;	/* the driver forces it to 1 */
        LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
        v.arg.overlay.area[0].clut_addr_offset = CovlLen;
        LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_VIDEO_PROC, &v, sizeof(v)));
        memset(&c, 0, sizeof(c));
        c.id = 2;
        memcpy(&c.cid, &cid, sizeof(cid));
        c.arg.overlay = o;
        LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)));
        c.id = 20;
        LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)));
    }
    /* reading it back needs the entry too */
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_GET_OVERLAY_INSERT, &o, sizeof(o)));
    f.h.drv.setAperture(0, 0, 0, NULL, 0);
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_GET_OVERLAY_INSERT, &o, sizeof(o)));
    CHECK(f.h.drv.violations().empty());
}

TEST(IavBrokerAperture, AStreamOverlayIdTheDriverShiftsByIsBounded)
{
    Fixture f;
    struct iav_overlay_insert o;

    memset(&o, 0, sizeof(o));
    o.id = 31;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    o.id = 32;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    o.id = 0xffffffffu;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)));
    CHECK(f.h.drv.violations().empty());
}

/* ---- warp ---- */

TEST(IavBrokerAperture, WarpTablesLieInTheWarpEntry)
{
    Fixture f;
    struct iav_warp_ctrl w;
    int cid;

    memset(&w, 0, sizeof(w));
    w.channel_id = 0;
    w.arg.main.area[0].enable = 1;
    w.arg.main.area[0].h_map.data_addr_offset = 0;
    w.arg.main.area[0].v_map.data_addr_offset = WarpLen - 1;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    w.arg.main.area[11].enable = 1;
    w.arg.main.area[11].me1_v_map.data_addr_offset = WarpLen;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    w.arg.main.area[11].me1_v_map.data_addr_offset = ~0ul;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    w.arg.main.area[11].enable = 0;	/* a disabled area's offsets are not read */
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    w.channel_id = 4;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    w.channel_id = 0;
    cid = IAV_WARP_CTRL_DPTZ;
    memcpy(&w.cid, &cid, sizeof(cid));
    w.arg.dptz.buf_id = 8;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    w.arg.dptz.buf_id = 1;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    cid = IAV_WARP_CTRL_MAX_CFG;
    memcpy(&w.cid, &cid, sizeof(cid));
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    cid = IAV_WARP_CTRL_ENCOFS;
    memcpy(&w.cid, &cid, sizeof(cid));
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    /* MAIN without the entry */
    cid = IAV_WARP_CTRL_MAIN;
    memcpy(&w.cid, &cid, sizeof(cid));
    memset(&w.arg, 0, sizeof(w.arg));
    f.h.drv.setAperture(0, 0, 0, NULL, 0);
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_CFG_WARP_CTRL, &w, sizeof(w)));
    CHECK(f.h.drv.violations().empty());
}

/* ---- stream configuration, frame sync, QP ROI ---- */

TEST(IavBrokerAperture, StreamConfigurationBoundsTheStreamAndConfinesQpRoi)
{
    Fixture f;
    const unsigned long cmds[] = { IAV_IOC_SET_STREAM_CONFIG, IAV_IOC_CFG_FRAME_SYNC_PROC };

    for (size_t k = 0; k < 2; k++) {
        struct iav_stream_cfg c = streamCfg(1, IAV_H264_CFG_BITRATE);

        LONGS_EQUAL(0, f.status(Primary, cmds[k], &c, sizeof(c)));
        c.id = 32;
        LONGS_EQUAL(-EINVAL, f.status(Primary, cmds[k], &c, sizeof(c)));
        c = streamCfg(0, IAV_STMCFG_CUSTOM_SEI);
        LONGS_EQUAL(-EPERM, f.status(Primary, cmds[k], &c, sizeof(c)));
        c = streamCfg(0, IAV_H264_CFG_QP_ROI);
        c.arg.h264_roi.enable = 1;
        LONGS_EQUAL(0, f.status(Primary, cmds[k], &c, sizeof(c)));
        c.arg.h264_roi.qpm_raw = 1;
        c.arg.h264_roi.data_offset = 0x40000000ul;
        LONGS_EQUAL(-EPERM, f.status(Primary, cmds[k], &c, sizeof(c)));
        c = streamCfg(0, IAV_H265_CFG_QP_ROI);
        c.arg.h265_roi.qpm_raw = 1;
        LONGS_EQUAL(-EPERM, f.status(Primary, cmds[k], &c, sizeof(c)));
    }
    /* QP ROI without the QPMATRIX entry */
    f.h.drv.setAperture(0, 0, 0, NULL, 0);
    struct iav_stream_cfg c = streamCfg(0, IAV_H264_CFG_QP_ROI);
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_SET_STREAM_CONFIG, &c, sizeof(c)));
    CHECK(f.h.drv.violations().empty());
}

TEST(IavBrokerAperture, FrameSyncOverlayBlurAndRemapAreCheckedAsTheirOwnCalls)
{
    Fixture f;
    struct iav_stream_cfg c = streamCfg(1, IAV_STMCFG_OVERLAY);

    c.arg.overlay.enable = 1;
    overlayArea(c.arg.overlay.area[0], MockIavDriver::OvlLen - 128 * 4, 0, 128, 4);
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_FRAME_SYNC_PROC, &c, sizeof(c)));
    /* checked as a stream overlay (the driver's path), whatever id_for_canvas says */
    c.arg.overlay.id_for_canvas = 1;
    overlayArea(c.arg.overlay.area[0], MockIavDriver::OvlLen - 128 * 4 + 64, 0, 128, 4);
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_FRAME_SYNC_PROC, &c, sizeof(c)));

    c = streamCfg(1, IAV_STMCFG_YUV_REMAP);
    c.arg.yuv_remap.enable = 1;
    c.arg.yuv_remap.area[0].enable = 1;
    c.arg.yuv_remap.area[0].clut_addr_offset = RemapLen - 1024;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_FRAME_SYNC_PROC, &c, sizeof(c)));
    c.arg.yuv_remap.area[0].clut_addr_offset = RemapLen - 1023;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_FRAME_SYNC_PROC, &c, sizeof(c)));

    c = streamCfg(1, IAV_STMCFG_BLUR);
    c.arg.blur.stream_id = 1;
    c.arg.blur.enable = 1;
    c.arg.blur.area[0].enable = 1;
    c.arg.blur.area[0].use_arb_blur = 1;
    c.arg.blur.area[0].width = 64;
    c.arg.blur.area[0].height = 16;
    c.arg.blur.area[0].arb_blur_pitch = 64;
    /* 15 rows of pitch, then at most 64 / 8 + 1 = 9 bytes of the last row: the end of the entry, then one past */
    c.arg.blur.area[0].arb_blur_offset = (u32)(ArbLen - 64 * 15 - 9);
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CFG_FRAME_SYNC_PROC, &c, sizeof(c)));
    c.arg.blur.area[0].arb_blur_offset = (u32)(ArbLen - 64 * 15 - 8);
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_FRAME_SYNC_PROC, &c, sizeof(c)));
    c.arg.blur.area[0].arb_blur_offset = (u32)(ArbLen - 64 * 15 - 9);
    /* the blur's own stream id is bounded too (the driver keeps it for non-frame sync types) */
    c.arg.blur.stream_id = 32;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CFG_FRAME_SYNC_PROC, &c, sizeof(c)));
    CHECK(f.h.drv.violations().empty());
}

/* ---- blur ---- */

TEST(IavBrokerAperture, AnArbitraryBlurBitmapLiesInTheArbBlurEntry)
{
    Fixture f;
    struct iav_blur_insert b;

    memset(&b, 0, sizeof(b));
    b.stream_id = 1;
    b.enable = 1;
    b.area[5].enable = 1;
    b.area[5].use_arb_blur = 1;
    b.area[5].width = 1920;
    b.area[5].height = 1080;
    b.area[5].arb_blur_pitch = 256;
    b.area[5].arb_blur_offset = 0;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    /* the driver's own check uses a 16-bit size from the width (256 x 1080 wraps there); here 1079 rows of pitch and
     * 1920 / 8 + 1 bytes of the last: the last byte of the entry, then one past */
    b.area[5].arb_blur_offset = (u32)(ArbLen - 256 * 1079 - 241);
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    b.area[5].arb_blur_offset = (u32)(ArbLen - 256 * 1079 - 240);
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    /* a pitch the driver does not compare with the area: every row the DSP reads counts */
    b.area[5].arb_blur_offset = 0;
    b.area[5].arb_blur_pitch = 64 * 1023;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    b.area[5].arb_blur_pitch = 256;
    b.area[5].arb_blur_offset = 0xffffffffu;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    b.area[5].arb_blur_offset = 0;
    b.area[5].height = 0;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    b.area[5].height = 1080;
    /* smooth arbitrary blur names ARB_SMOOTH_BLUR, which has no entry */
    b.type = IAV_BLUR_TYPE_SMOOTH;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    b.type = IAV_BLUR_TYPE_PM;
    /* ids */
    b.stream_id = 32;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    b.id_for_canvas = 1;
    b.canvas_id = 20;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    b.canvas_id = 0;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    /* without the entry */
    f.h.drv.setAperture(0, 0, 0, NULL, 0);
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    /* a normal blur needs no entry, only the blur partition */
    b.area[5].use_arb_blur = 0;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    CHECK(f.h.drv.violations().empty());
}

/* ---- YUV remap ---- */

TEST(IavBrokerAperture, YuvRemapTablesLieInTheYuvRemapEntry)
{
    Fixture f;
    struct iav_yuv_remap r;

    memset(&r, 0, sizeof(r));
    r.id = 1;
    r.enable = 1;
    for (unsigned int i = 0; i < MAX_NUM_YUV_REMAP_AREA; i++) {
        r.area[i].enable = 1;
        r.area[i].clut_addr_offset = i * 1024;
    }
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    r.area[15].clut_addr_offset = RemapLen - 1023;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    r.area[15].clut_addr_offset = ~0ul - 512;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    r.area[15].enable = 0;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    r.id = 32;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    r.id = 1;
    f.h.drv.setAperture(0, 0, 0, NULL, 0);
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    /* disabling needs no table */
    r.enable = 0;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SET_YUV_REMAP_INSERT, &r, sizeof(r)));
    CHECK(f.h.drv.violations().empty());
}

/* ---- CE GO map ---- */

TEST(IavBrokerAperture, CeGoMapBuffersAreBoundedAndNeedTheEntry)
{
    Fixture f;
    struct iav_ce_go_map_request_buffer q;
    struct iav_ce_go_map_handshake_buffer h;

    memset(&q, 0, sizeof(q));
    memset(&h, 0, sizeof(h));
    q.chan_id = 3;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CE_GO_MAP_REQUEST_BUFFER, &q, sizeof(q)));
    q.chan_id = 4;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CE_GO_MAP_REQUEST_BUFFER, &q, sizeof(q)));
    h.chan_id = 1;
    h.buffer_idx = 7;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, &h, sizeof(h)));
    h.buffer_idx = 8;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, &h, sizeof(h)));
    h.buffer_idx = 255;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, &h, sizeof(h)));
    h.buffer_idx = 0;
    h.chan_id = 4;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, &h, sizeof(h)));
    f.h.drv.setAperture(0, 0, 0, NULL, 0);
    q.chan_id = 0;
    h.chan_id = 0;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_CE_GO_MAP_REQUEST_BUFFER, &q, sizeof(q)));
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, &h, sizeof(h)));
    CHECK(f.h.drv.violations().empty());
}

/* ---- GDMA and cache maintenance on the guest's own entries ---- */

TEST(IavBrokerAperture, GdmaAndCacheCallsReachTheEntriesAsTheGuestsOwnContent)
{
    Fixture f;
    struct iav_gdma_copy g;
    struct iav_sync_cache_mem_part c;
    int opt = IAV_CACHE_INVALIDATE;

    memset(&g, 0, sizeof(g));
    g.src_mmap_type = IAV_PART_DEC_BSB;
    g.dst_mmap_type = IAV_PART_ARB_BLUR;
    g.src_pitch = g.dst_pitch = 256;
    g.width = 240;
    g.height = 16;
    g.dst_offset = ArbLen - 256 * 16;
    f.h.addOverlayWindow();
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_GDMA_COPY, &g, sizeof(g)));
    g.dst_offset += 1;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_GDMA_COPY, &g, sizeof(g)));
    memset(&c, 0, sizeof(c));
    c.pid = IAV_PART_MASK;
    c.offset = MaskLen - 4096;
    c.length = 4096;
    memcpy(&c.opt, &opt, sizeof(opt));
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    c.length = 4097;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    /* a partition of another guest's aperture, or none: not this guest's */
    f.h.drv.setStandardAperture(Consumer);
    c.length = 4096;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    CHECK(f.h.drv.violations().empty());
}

/* ---- the consumer ---- */

TEST(IavBrokerAperture, AConsumerIsRefusedEveryCallTheApertureUnlocks)
{
    Fixture f;
    uint8_t arg[AMBA_VIRT_MAX_MSG];
    const unsigned long cmds[] = { IAV_IOC_CFG_VIDEO_PROC, IAV_IOC_CFG_WARP_CTRL, IAV_IOC_SET_STREAM_CONFIG,
                                   IAV_IOC_CFG_FRAME_SYNC_PROC, IAV_IOC_SET_YUV_REMAP_INSERT,
                                   IAV_IOC_CE_GO_MAP_REQUEST_BUFFER, IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER,
                                   IAV_IOC_IMG_GET_MISC_INFO, IAV_IOC_SET_OVERLAY_INSERT, IAV_IOC_SET_BLUR_INSERT };
    unsigned long before = f.h.drv.ioctlCalls();

    /* even with an aperture the server (wrongly) reports for it */
    f.h.drv.setStandardAperture(Consumer);
    memset(arg, 0, sizeof(arg));
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        const struct iav_policy_entry *e = iav_policy_lookup((unsigned int)cmds[i]);

        LONGS_EQUAL_TEXT(-EPERM, f.status(Consumer, cmds[i], arg, e->size), e->name);
    }
    UNSIGNED_LONGS_EQUAL(before, f.h.drv.ioctlCalls());
}

/* ---- the applies of the configurations served here (their full struct, audited) ---- */

TEST(IavBrokerAperture, TheAppliesCarryTheirWholeStructToANegotiatedGuestAndOnlyTheirAuditedFields)
{
    Fixture f;
    struct iav_apply_flag v[IAV_VIDEO_PROC_NUM];
    struct iav_apply_flag w;
    struct iav_apply_canvas_frame_sync c;
    std::vector<uint8_t> got;

    memset(v, 0, sizeof(v));
    v[IAV_VIDEO_PROC_MASK].apply = 1;
    v[IAV_VIDEO_PROC_MASK].param = 0x1;
    v[IAV_VIDEO_PROC_CANVAS_FPS].apply = 1;
    v[IAV_VIDEO_PROC_CANVAS_FPS].param = 0x3;
    /* the negotiated guest sends the wire size; the driver gets every byte of it */
    f.h.drv.captureArgOf(IAV_IOC_APPLY_VIDEO_PROC, sizeof(v));
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_APPLY_VIDEO_PROC, v, sizeof(v)));
    got = f.h.drv.capturedArg();
    LONGS_EQUAL(sizeof(v), got.size());
    CHECK(memcmp(&got[0], v, sizeof(v)) == 0);
    /* the applies of configurations a guest cannot set, and maps past the driver's ids */
    v[IAV_VIDEO_PROC_ARB_BLEND].apply = 1;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_APPLY_VIDEO_PROC, v, sizeof(v)));
    v[IAV_VIDEO_PROC_ARB_BLEND].apply = 0;
    v[IAV_VIDEO_PROC_TML_FUSION].apply = 1;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_APPLY_VIDEO_PROC, v, sizeof(v)));
    v[IAV_VIDEO_PROC_TML_FUSION].apply = 0;
    v[IAV_VIDEO_PROC_MASK].param = 0x10;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_APPLY_VIDEO_PROC, v, sizeof(v)));
    v[IAV_VIDEO_PROC_MASK].param = 0x1;
    v[IAV_VIDEO_PROC_CANVAS_FPS].param = 1u << 20;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_APPLY_VIDEO_PROC, v, sizeof(v)));

    w.apply = IAV_WARP_CTRL_MAIN;
    w.param = 0x1;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_APPLY_WARP_CTRL, &w, sizeof(w)));
    w.apply = IAV_WARP_CTRL_MAX_CFG;
    LONGS_EQUAL(-EPERM, f.status(Primary, IAV_IOC_APPLY_WARP_CTRL, &w, sizeof(w)));
    w.apply = IAV_WARP_CTRL_DPTZ;
    w.param = 0x30;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_APPLY_WARP_CTRL, &w, sizeof(w)));

    memset(&c, 0, sizeof(c));
    c.canvas_updated_map = 0x1f;
    LONGS_EQUAL(0, f.status(Primary, IAV_IOC_APPLY_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)));
    c.canvas_updated_map = 1u << 20;
    LONGS_EQUAL(-EINVAL, f.status(Primary, IAV_IOC_APPLY_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)));
    CHECK(f.h.drv.violations().empty());
}

TEST(IavBrokerAperture, ALegacyGuestStillSendsOnlyTheHeaderSizeOfAnApply)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    uint64_t hd = h.mustOpen(Primary);
    uint8_t one = 1;
    std::vector<uint8_t> got;

    h.drv.captureArgOf(IAV_IOC_APPLY_VIDEO_PROC, sizeof(struct iav_apply_flag) * IAV_VIDEO_PROC_NUM);
    LONGS_EQUAL(0, h.ioctl(Primary, hd, IAV_IOC_APPLY_VIDEO_PROC, &one, 1).r.status);
    got = h.drv.capturedArg();
    LONGS_EQUAL(1, got[0]);
    for (size_t i = 1; i < got.size(); i++) {
        LONGS_EQUAL_TEXT(0, got[i], "a byte past the header reached the driver from a legacy guest");
    }
    /* the struct size from a guest that did not negotiate it: the wrong length */
    {
        struct iav_apply_flag v[IAV_VIDEO_PROC_NUM];

        memset(v, 0, sizeof(v));
        LONGS_EQUAL(-EMSGSIZE, h.ioctl(Primary, hd, IAV_IOC_APPLY_VIDEO_PROC, v, sizeof(v)).r.status);
    }
    CHECK(h.drv.violations().empty());
}

/* ---- IMG_GET_MISC_INFO, inline ---- */

namespace {

std::vector<uint8_t> miscArg(uintptr_t q, uintptr_t v, uintptr_t e, uint32_t count, const uint8_t *query)
{
    struct img_misc_info m;
    struct amba_virt_iav_inline_misc t;
    std::vector<uint8_t> a(sizeof(m) + sizeof(t));

    memset(&m, 0, sizeof(m));
    memset(&t, 0, sizeof(t));
    memcpy(&m.query_extra_raw, &q, sizeof(q));
    memcpy(&m.extra_raw_valid, &v, sizeof(v));
    memcpy(&m.extra_raw, &e, sizeof(e));
    t.count = count;
    if (query) {
        memcpy(t.query, query, AMBA_VIRT_IAV_INLINE_MISC_CHANS);
    }
    memcpy(&a[0], &m, sizeof(m));
    memcpy(&a[sizeof(m)], &t, sizeof(t));
    return a;
}

} // namespace

TEST(IavBrokerAperture, MiscInfoPointersTravelInlineAndNeverAsAddresses)
{
    Fixture f;
    uint8_t query[AMBA_VIRT_IAV_INLINE_MISC_CHANS] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    std::vector<uint8_t> a = miscArg(1, 1, 1, 2, query);
    IavBrokerHarness::Result r = f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size());
    struct img_misc_info m;
    struct amba_virt_iav_inline_misc t;
    uintptr_t q;

    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(0, r.r.rc);
    LONGS_EQUAL(sizeof(m) + sizeof(t), r.out.size());
    memcpy(&m, &r.out[0], sizeof(m));
    memcpy(&t, &r.out[sizeof(m)], sizeof(t));
    /* the flags come back, not the broker's addresses */
    memcpy(&q, &m.query_extra_raw, sizeof(q));
    UNSIGNED_LONGS_EQUAL(1, q);
    LONGS_EQUAL(11, m.ik_id_in_use[0]);
    /* the driver saw count queries (the rest zeroed) and wrote two entries */
    LONGS_EQUAL(1, t.valid[0]);
    LONGS_EQUAL(1, t.valid[1]);
    LONGS_EQUAL(0xa1, t.extra[1][31]);
    LONGS_EQUAL(0, t.valid[2]);
    CHECK(f.h.drv.violations().empty());

    /* a query with no buffers: the driver's own EINVAL, no pointer of the broker's leaks */
    a = miscArg(1, 0, 0, 2, query);
    r = f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size());
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(-1, r.r.rc);
    LONGS_EQUAL(EINVAL, r.r.err);
    /* no query: nothing read or written */
    a = miscArg(0, 0, 0, 0, NULL);
    LONGS_EQUAL(0, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size()).r.rc);
    CHECK(f.h.drv.violations().empty());
}

TEST(IavBrokerAperture, MiscInfoRefusesAddressesBadCountsAndTheLegacyFraming)
{
    Fixture f;
    std::vector<uint8_t> a;
    unsigned long before = f.h.drv.ioctlCalls();

    /* a guest address in a pointer field */
    a = miscArg(0x7fff0000u, 1, 1, 2, NULL);
    LONGS_EQUAL(-EINVAL, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size()).r.status);
    a = miscArg(1, 2, 1, 2, NULL);
    LONGS_EQUAL(-EINVAL, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size()).r.status);
    /* more entries than the trailer has */
    a = miscArg(1, 1, 1, AMBA_VIRT_IAV_INLINE_MISC_CHANS + 1, NULL);
    LONGS_EQUAL(-EINVAL, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size()).r.status);
    /* a reserved field set */
    a = miscArg(1, 1, 1, 2, NULL);
    a[a.size() - 1] = 1;
    LONGS_EQUAL(-EINVAL, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size()).r.status);
    /* the struct alone (the legacy framing), or a short or long trailer */
    a = miscArg(0, 0, 0, 0, NULL);
    LONGS_EQUAL(-EMSGSIZE, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0],
                                     (uint32_t)sizeof(struct img_misc_info)).r.status);
    LONGS_EQUAL(-EMSGSIZE, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size() - 1).r.status);
    a.push_back(0);
    LONGS_EQUAL(-EMSGSIZE, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size()).r.status);
    /* a guest that did not negotiate the inline marshal */
    hello(f.h, Primary, Offered & ~AMBA_VIRT_IAV_F_INLINE);
    a = miscArg(0, 0, 0, 0, NULL);
    LONGS_EQUAL(-EPERM, f.h.ioctl(Primary, f.hp, IAV_IOC_IMG_GET_MISC_INFO, &a[0], (uint32_t)a.size()).r.status);
    UNSIGNED_LONGS_EQUAL(before, f.h.drv.ioctlCalls());
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
