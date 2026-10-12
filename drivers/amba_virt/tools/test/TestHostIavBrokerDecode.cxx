/*
 * TestHostIavBrokerDecode.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The decode calls the primary guest may make: every offset and length is checked against the decode window,
 * the consumer guests are refused, a pipeline without a decode partition refuses the calls that name offsets,
 * the timeout of a wait is bounded, and the display addresses the driver reports are cleared. The overlay calls
 * (the data and colour table of an area checked against the overlay window) are tested here as well.
 */

#include <stdint.h>
#include <string.h>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

namespace {

const uint64_t Len = MockIavDriver::DecLen;

struct iav_decoder_info decoder(uint64_t off, uint32_t size, uint8_t id = 0)
{
    struct iav_decoder_info d;

    memset(&d, 0, sizeof(d));
    d.decoder_id = id;
    d.decoder_type = IAV_DEC_TYPE_JPEG;
    d.video_width = 64;
    d.video_height = 48;
    d.bsb_start_offset = (unsigned long)off;
    d.bsb_size = size;
    return d;
}

struct iav_decode_jpeg jpeg(uint64_t joff, uint32_t jsize, uint64_t yoff, uint32_t ysize, uint8_t only = 1,
                            uint8_t id = 0)
{
    struct iav_decode_jpeg j;

    memset(&j, 0, sizeof(j));
    j.decoder_id = id;
    j.is_dec_only = only;
    j.jpeg_start_offset = (unsigned long)joff;
    j.jpeg_size = jsize;
    j.yuv_start_offset = (unsigned long)yoff;
    j.yuv_size = ysize;
    return j;
}

} // namespace

TEST_GROUP(IavBrokerDecode)
{
};

TEST(IavBrokerDecode, ThePrimaryGuestMayDecodeInsideTheWindow)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_decoder_info d = decoder(0, (uint32_t)Len);
    struct iav_decode_jpeg j = jpeg(0, 1000, Len / 2, (uint32_t)(Len / 2));
    struct iav_decode_video v;
    struct iav_decode_bsb b;
    struct iav_decode_status st;
    struct iav_decode_wait_eos w;

    h.addDecodeWindow();
    hd = h.mustOpen(7);
    memset(&v, 0, sizeof(v));
    v.start_ptr_offset = 0;
    v.end_ptr_offset = 4096;
    memset(&b, 0, sizeof(b));
    b.start_offset = 0;
    b.room = 4096;
    memset(&st, 0, sizeof(st));
    memset(&w, 0, sizeof(w));
    w.timeout_ticks = 10;

    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &d, sizeof(d)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &j, sizeof(j)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_DECODE_VIDEO, &v, sizeof(v)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_QUERY_DECODE_BSB, &b, sizeof(b)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_WAIT_DECODE_BSB, &b, sizeof(b)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_QUERY_DECODE_STATUS, &st, sizeof(st)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_WAIT_PLAYBACK_EOS, &w, sizeof(w)).r.status);
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerDecode, AConsumerGuestIsRefusedEveryDecodeCall)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_decoder_info d = decoder(0, 4096);
    struct iav_decode_jpeg j = jpeg(0, 100, 4096, 4096);
    struct iav_decode_status st;
    unsigned long calls;

    h.addDecodeWindow();
    hd = h.mustOpen(8);
    memset(&st, 0, sizeof(st));
    calls = h.drv.ioctlCalls();
    LONGS_EQUAL(-EPERM, h.ioctl(8, hd, IAV_IOC_CREATE_DECODER, &d, sizeof(d)).r.status);
    LONGS_EQUAL(-EPERM, h.ioctl(8, hd, IAV_IOC_DECODE_JPEG, &j, sizeof(j)).r.status);
    LONGS_EQUAL(-EPERM, h.ioctl(8, hd, IAV_IOC_QUERY_DECODE_STATUS, &st, sizeof(st)).r.status);
    LONGS_EQUAL(calls, h.drv.ioctlCalls());
}

TEST(IavBrokerDecode, WithoutADecodePartitionTheCallsThatNameOffsetsAreRefused)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd = h.mustOpen(7);
    struct iav_decoder_info d = decoder(0, 4096);
    struct iav_decode_jpeg j = jpeg(0, 100, 4096, 4096);
    struct iav_decode_bsb b;
    unsigned long calls = h.drv.ioctlCalls();

    memset(&b, 0, sizeof(b));
    LONGS_EQUAL(-EPERM, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &d, sizeof(d)).r.status);
    LONGS_EQUAL(-EPERM, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &j, sizeof(j)).r.status);
    LONGS_EQUAL(-EPERM, h.ioctl(7, hd, IAV_IOC_WAIT_DECODE_BSB, &b, sizeof(b)).r.status);
    LONGS_EQUAL(calls, h.drv.ioctlCalls());
}

TEST(IavBrokerDecode, OffsetsOutsideTheWindowAreRefusedAndNeverReachTheDriver)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    unsigned long calls;
    struct iav_decode_video v;
    struct iav_decode_bsb b;

    h.addDecodeWindow();
    hd = h.mustOpen(7);
    calls = h.drv.ioctlCalls();

    /* CREATE_DECODER: zero size, past the end by one, a start past the end, an overflowing start, a bad id. */
    {
        struct iav_decoder_info zero = decoder(0, 0), one = decoder(0, (uint32_t)Len + 1),
                                past = decoder(Len, 1), wrap = decoder(0xffffffffffffffffull, 16),
                                badId = decoder(0, 4096, IAV_MAX_DECODER_NUMBER);

        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &zero, sizeof(zero)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &one, sizeof(one)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &past, sizeof(past)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &wrap, sizeof(wrap)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &badId, sizeof(badId)).r.status);
    }
    /* DECODE_JPEG: the input and the output ranges are each checked; a decode that goes downstream has no output. */
    {
        struct iav_decode_jpeg inPast = jpeg(Len - 10, 11, 0, 100), outPast = jpeg(0, 100, Len - 99, 100),
                               zeroIn = jpeg(0, 0, 0, 100), zeroOut = jpeg(0, 100, 0, 0),
                               wrapIn = jpeg(0xfffffffffffffff0ull, 100, 0, 100),
                               badId = jpeg(0, 100, 4096, 4096, 1, 200);

        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &inPast, sizeof(inPast)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &outPast, sizeof(outPast)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &zeroIn, sizeof(zeroIn)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &zeroOut, sizeof(zeroOut)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &wrapIn, sizeof(wrapIn)).r.status);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &badId, sizeof(badId)).r.status);
    }
    /* DECODE_VIDEO: end before start, past the end. */
    memset(&v, 0, sizeof(v));
    v.start_ptr_offset = 100;
    v.end_ptr_offset = 50;
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_VIDEO, &v, sizeof(v)).r.status);
    v.start_ptr_offset = 0;
    v.end_ptr_offset = (unsigned long)Len + 1;
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_VIDEO, &v, sizeof(v)).r.status);
    /* WAIT_DECODE_BSB: the zone asked for runs past the end. */
    memset(&b, 0, sizeof(b));
    b.start_offset = (unsigned long)Len - 10;
    b.room = 11;
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_WAIT_DECODE_BSB, &b, sizeof(b)).r.status);

    LONGS_EQUAL(calls, h.drv.ioctlCalls());
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerDecode, TheLastByteOfTheWindowIsAllowed)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_decoder_info all = decoder(0, (uint32_t)Len), tail = decoder(Len - 1, 1);
    struct iav_decode_jpeg end = jpeg(Len - 100, 100, Len - 100, 100);

    h.addDecodeWindow();
    hd = h.mustOpen(7);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &all, sizeof(all)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &tail, sizeof(tail)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &end, sizeof(end)).r.status);
}

TEST(IavBrokerDecode, AWaitForPlaybackEndIsBounded)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_decode_wait_eos w;
    std::vector<uint8_t> seen;

    h.addDecodeWindow();
    hd = h.mustOpen(7);
    memset(&w, 0, sizeof(w));
    w.decoder_id = 2;
    /* Zero means ten seconds in the driver: the broker turns it into the longest explicit value. */
    h.drv.captureArgOf(IAV_IOC_WAIT_PLAYBACK_EOS, sizeof(w));
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_WAIT_PLAYBACK_EOS, &w, sizeof(w)).r.status);
    seen = h.drv.capturedArg();
    LONGS_EQUAL(sizeof(w), seen.size());
    LONGS_EQUAL(2, reinterpret_cast<struct iav_decode_wait_eos *>(&seen[0])->decoder_id);
    LONGS_EQUAL(255, reinterpret_cast<struct iav_decode_wait_eos *>(&seen[0])->timeout_ticks);
    /* An explicit value is left alone. */
    w.timeout_ticks = 7;
    h.drv.captureArgOf(IAV_IOC_WAIT_PLAYBACK_EOS, sizeof(w));
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_WAIT_PLAYBACK_EOS, &w, sizeof(w)).r.status);
    seen = h.drv.capturedArg();
    LONGS_EQUAL(7, reinterpret_cast<struct iav_decode_wait_eos *>(&seen[0])->timeout_ticks);
    /* A decoder id out of range is refused. */
    w.decoder_id = IAV_MAX_DECODER_NUMBER;
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_WAIT_PLAYBACK_EOS, &w, sizeof(w)).r.status);
}

TEST(IavBrokerDecode, TheDecodeStatusDoesNotCarryHostPhysicalAddresses)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_decode_status st;
    IavBrokerHarness::Result r;

    h.addDecodeWindow();
    hd = h.mustOpen(7);
    memset(&st, 0, sizeof(st));
    r = h.ioctl(7, hd, IAV_IOC_QUERY_DECODE_STATUS, &st, sizeof(st));
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(sizeof(st), r.out.size());
    {
        const struct iav_decode_status *o = reinterpret_cast<const struct iav_decode_status *>(&r.out[0]);

        LONGS_EQUAL(0, o->yuv422_y_addr);
        LONGS_EQUAL(0, o->yuv422_uv_addr);
        /* The rest of the answer is kept. */
        LONGS_EQUAL(64, o->decoded_still_pic_info.width);
        LONGS_EQUAL(48, o->decoded_still_pic_info.height);
    }
}

TEST(IavBrokerDecode, TheDecodeWindowCanBeMappedForReadingAndWriting)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    IavBrokerHarness::Result r;

    h.addDecodeWindow();
    hd = h.mustOpen(7);
    r = h.map(7, hd, MockIavDriver::DecBase, Len, PROT_READ | PROT_WRITE);
    LONGS_EQUAL(0, r.r.status);
    /* Not past the partition, not without PROT_READ. */
    LONGS_EQUAL(-EINVAL, h.map(7, hd, MockIavDriver::DecBase, Len + 4096, PROT_READ | PROT_WRITE).r.status);
    LONGS_EQUAL(-EPERM, h.map(7, hd, MockIavDriver::DecBase, Len, PROT_WRITE).r.status);
    LONGS_EQUAL(-EFAULT, h.map(7, hd, MockIavDriver::DecBase + 4096, 4096, PROT_READ).r.status);
}

TEST(IavBrokerDecode, OnlyThePrimaryGuestMayMapTheDecodeWindow)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t primary, other;

    h.addDecodeWindow();
    primary = h.mustOpen(7);
    other = h.mustOpen(8);
    LONGS_EQUAL(0, h.map(7, primary, MockIavDriver::DecBase, Len, PROT_READ | PROT_WRITE).r.status);
    LONGS_EQUAL(-EPERM, h.map(8, other, MockIavDriver::DecBase, Len, PROT_READ | PROT_WRITE).r.status);
    LONGS_EQUAL(-EPERM, h.map(8, other, MockIavDriver::DecBase, Len, PROT_READ).r.status);
    /* The other windows are unaffected: a consumer reads frames through them. */
    LONGS_EQUAL(0, h.map(8, other, MockIavDriver::DspBase, MockIavDriver::DspLen, PROT_READ).r.status);
}

namespace {

const uint64_t OLen = MockIavDriver::OvlLen;

/* One enabled area of 64 x 32 bytes with its colour table at clut and its data at data. */
struct iav_overlay_insert overlay(uint64_t clut, uint64_t data, uint32_t total = 64 * 32)
{
    struct iav_overlay_insert o;

    memset(&o, 0, sizeof(o));
    o.id = 1;
    o.enable = 1;
    o.osd_num_regions = 1;
    o.area[0].enable = 1;
    o.area[0].width = 64;
    o.area[0].pitch = 64;
    o.area[0].height = 32;
    o.area[0].total_size = total;
    o.area[0].clut_addr_offset = (unsigned long)clut;
    o.area[0].data_addr_offset = (unsigned long)data;
    return o;
}

} // namespace

TEST_GROUP(IavBrokerOverlay)
{
};

TEST(IavBrokerOverlay, ThePrimaryGuestMayInsertAnOverlayInsideTheWindow)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_overlay_insert o = overlay(0, 1024);

    h.addOverlayWindow();
    hd = h.mustOpen(7);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_GET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    /* The very end of the window is allowed. */
    o = overlay(OLen - 1024, OLen - 1024 - 2048);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerOverlay, AreasOutsideTheWindowAreRefusedAndNeverReachTheDriver)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    unsigned long calls;
    struct iav_overlay_insert o;

    h.addOverlayWindow();
    hd = h.mustOpen(7);
    calls = h.drv.ioctlCalls();

    o = overlay(0, OLen - 2047); /* one byte too far */
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    o = overlay(OLen - 1023, 2048); /* the colour table runs past the end */
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    o = overlay(0, ~0ull - 100); /* offset plus length wraps */
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    o = overlay(~0ull - 100, 2048);
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    /* A total_size smaller than pitch x height: the DSP reads pitch x height, so that is what is checked. */
    o = overlay(0, OLen - 1024, 16);
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    /* An area in a later slot is checked like the first. */
    o = overlay(0, 1024);
    o.area[15] = o.area[0];
    o.area[15].data_addr_offset = (unsigned long)OLen;
    LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    LONGS_EQUAL(calls, h.drv.ioctlCalls());
}

TEST(IavBrokerOverlay, CanvasOverlaysAndAConsumerAreRefused)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t primary, other;
    struct iav_overlay_insert o = overlay(0, 1024);
    unsigned long calls;

    h.addOverlayWindow();
    primary = h.mustOpen(7);
    other = h.mustOpen(8);
    calls = h.drv.ioctlCalls();
    o.id_for_canvas = 1;
    LONGS_EQUAL(-EPERM, h.ioctl(7, primary, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    o.id_for_canvas = 0;
    LONGS_EQUAL(-EPERM, h.ioctl(8, other, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    LONGS_EQUAL(-EPERM, h.ioctl(8, other, IAV_IOC_GET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    LONGS_EQUAL(calls, h.drv.ioctlCalls());
}

TEST(IavBrokerOverlay, WithoutAnOverlayWindowOnlyDisablingIsAllowed)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd = h.mustOpen(7);
    struct iav_overlay_insert o = overlay(0, 1024);

    LONGS_EQUAL(-EPERM, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
    o.enable = 0;
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_SET_OVERLAY_INSERT, &o, sizeof(o)).r.status);
}

TEST(IavBrokerOverlay, OnlyThePrimaryGuestMayMapTheOverlayWindow)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t primary, other;

    h.addOverlayWindow();
    primary = h.mustOpen(7);
    other = h.mustOpen(8);
    LONGS_EQUAL(0, h.map(7, primary, MockIavDriver::OvlBase, OLen, PROT_READ | PROT_WRITE).r.status);
    LONGS_EQUAL(-EINVAL, h.map(7, primary, MockIavDriver::OvlBase, OLen + 4096, PROT_READ | PROT_WRITE).r.status);
    LONGS_EQUAL(-EPERM, h.map(8, other, MockIavDriver::OvlBase, OLen, PROT_READ | PROT_WRITE).r.status);
    LONGS_EQUAL(-EPERM, h.map(8, other, MockIavDriver::OvlBase, OLen, PROT_READ).r.status);
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
