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
#include <stdlib.h>
#include <string.h>

#include <vector>

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

namespace {

enum Edit { EditApp0Id, EditApp0Short, EditDropApp0, EditDoubleApp0, EditAddApp1, EditAddCom, EditTable, EditDropTable,
            EditDupTable, EditMergeTables, EditCutTable };

typedef std::vector<uint8_t> Bytes;

/* The offset of the nth segment with the marker (0 = SOI is skipped), or the size when there is none. */
size_t findSeg(const Bytes &b, unsigned int marker, unsigned int nth)
{
    size_t i = 2;

    while (i + 4 <= b.size() && b[i] == 0xFF && b[i + 1] != 0xDA) {
        if (b[i + 1] == marker && nth-- == 0) {
            return i;
        }
        i += 2 + (((size_t)b[i + 2] << 8) | b[i + 3]);
    }
    return b.size();
}

size_t segLen(const Bytes &b, size_t at)
{
    return 2 + (((size_t)b[at + 2] << 8) | b[at + 3]);
}

/* A header changed in one way. */
Bytes edit(Bytes b, Edit what, const char *arg)
{
    const size_t app0 = findSeg(b, 0xE0, 0);
    const unsigned int t = (unsigned int)atoi(arg);

    switch (what) {
    case EditApp0Id:
        memcpy(&b[app0 + 4], arg, 4);
        break;
    case EditApp0Short:
        b[app0 + 3] = 0x0E;
        b.erase(b.begin() + (long)app0 + 16, b.begin() + (long)app0 + 18);
        break;
    case EditDropApp0:
        b.erase(b.begin() + (long)app0, b.begin() + (long)(app0 + segLen(b, app0)));
        break;
    case EditDoubleApp0: {
        const Bytes seg(b.begin() + (long)app0, b.begin() + (long)(app0 + segLen(b, app0)));

        b.insert(b.begin() + (long)app0, seg.begin(), seg.end());
        break;
    }
    case EditAddApp1:
    case EditAddCom: {
        const uint8_t seg[] = { 0xFF, (uint8_t)(what == EditAddApp1 ? 0xE1 : 0xFE), 0x00, 0x06, 'a', 'b', 'c', 'd' };

        b.insert(b.begin() + (long)app0, seg, seg + sizeof(seg));
        break;
    }
    case EditTable: {
        const size_t at = findSeg(b, 0xC4, t);

        b[at + 4 + 17] ^= 0x01;     /* a code value of the table */
        break;
    }
    case EditDropTable: {
        const size_t at = findSeg(b, 0xC4, t);

        b.erase(b.begin() + (long)at, b.begin() + (long)(at + segLen(b, at)));
        break;
    }
    case EditDupTable: {
        const size_t at = findSeg(b, 0xC4, t);
        const Bytes seg(b.begin() + (long)at, b.begin() + (long)(at + segLen(b, at)));

        b.insert(b.begin() + (long)at, seg.begin(), seg.end());
        break;
    }
    case EditMergeTables: {
        const size_t first = findSeg(b, 0xC4, 0);
        Bytes body;

        for (unsigned int k = 0; k < 4; k++) {
            const size_t at = findSeg(b, 0xC4, 0);

            body.insert(body.end(), b.begin() + (long)at + 4, b.begin() + (long)(at + segLen(b, at)));
            b.erase(b.begin() + (long)at, b.begin() + (long)(at + segLen(b, at)));
        }
        const uint8_t hdr[] = { 0xFF, 0xC4, (uint8_t)((body.size() + 2) >> 8), (uint8_t)(body.size() + 2) };

        b.insert(b.begin() + (long)first, body.begin(), body.end());
        b.insert(b.begin() + (long)first, hdr, hdr + sizeof(hdr));
        break;
    }
    case EditCutTable: {
        const size_t at = findSeg(b, 0xC4, 3);
        const size_t len = segLen(b, at);

        b[at + 3] = (uint8_t)(len - 2 - 5);       /* the length stops five bytes short of the table's end */
        b.erase(b.begin() + (long)(at + len - 5), b.begin() + (long)(at + len));
        break;
    }
    }
    return b;
}

} // namespace

TEST(IavBrokerDecode, OnlyTheJpegKindTheDecoderTakesReachesTheDsp)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_decoder_info d = decoder(0, (uint32_t)Len);
    struct iav_decode_jpeg j = jpeg(0, 100000, 0x300000, 0x100000);
    unsigned long calls;
    struct Case { const char *what; Bytes bytes; int want; } cases[] = {
        { "4:2:0 baseline", MockIavDriver::jpegHeader(), 0 },
        { "4:2:2", MockIavDriver::jpegHeader(0x21), 0 },
        { "4:4:4", MockIavDriver::jpegHeader(0x11), -EINVAL },
        { "4:4:0", MockIavDriver::jpegHeader(0x12), -EINVAL },
        { "4:1:1", MockIavDriver::jpegHeader(0x41), -EINVAL },
        { "chroma sampled 2x2", MockIavDriver::jpegHeader(0x22, 0x22, 0x11), -EINVAL },
        { "progressive", MockIavDriver::jpegHeader(0x22, 0x11, 0x11, 0xC2), -EINVAL },
        { "extended sequential", MockIavDriver::jpegHeader(0x22, 0x11, 0x11, 0xC1), -EINVAL },
        { "lossless", MockIavDriver::jpegHeader(0x22, 0x11, 0x11, 0xC3), -EINVAL },
        { "arithmetic", MockIavDriver::jpegHeader(0x22, 0x11, 0x11, 0xC9), -EINVAL },
        { "restart interval", MockIavDriver::jpegHeader(0x22, 0x11, 0x11, 0xC0, 64), -EINVAL },
        { "no quantization table", MockIavDriver::jpegHeader(0x22, 0x11, 0x11, 0xC0, 0, false), -EINVAL },
        { "no Huffman table", MockIavDriver::jpegHeader(0x22, 0x11, 0x11, 0xC0, 0, true, false), -EINVAL },
        { "AVI1 APP0 (the camera's)", edit(MockIavDriver::jpegHeader(), EditApp0Id, "AVI1"), -EINVAL },
        { "AVI1 APP0 on 4:2:2", edit(MockIavDriver::jpegHeader(0x21), EditApp0Id, "AVI1"), -EINVAL },
        { "JFIF APP0 too short", edit(MockIavDriver::jpegHeader(), EditApp0Short, ""), -EINVAL },
        { "no APP0", edit(MockIavDriver::jpegHeader(), EditDropApp0, ""), 0 },
        { "two APP0", edit(MockIavDriver::jpegHeader(), EditDoubleApp0, ""), -EINVAL },
        { "APP1 (Exif)", edit(MockIavDriver::jpegHeader(), EditAddApp1, ""), -EINVAL },
        { "COM", edit(MockIavDriver::jpegHeader(), EditAddCom, ""), -EINVAL },
        { "a custom DC luma table", edit(MockIavDriver::jpegHeader(), EditTable, "0"), -EINVAL },
        { "a custom DC chroma table", edit(MockIavDriver::jpegHeader(), EditTable, "1"), -EINVAL },
        { "a custom AC luma table", edit(MockIavDriver::jpegHeader(), EditTable, "2"), -EINVAL },
        { "a custom AC chroma table", edit(MockIavDriver::jpegHeader(), EditTable, "3"), -EINVAL },
        { "a custom table on 4:2:2", edit(MockIavDriver::jpegHeader(0x21), EditTable, "2"), -EINVAL },
        { "the AC luma table missing", edit(MockIavDriver::jpegHeader(), EditDropTable, "2"), -EINVAL },
        { "a table given twice", edit(MockIavDriver::jpegHeader(), EditDupTable, "1"), -EINVAL },
        { "the four tables in one DHT segment", edit(MockIavDriver::jpegHeader(), EditMergeTables, ""), 0 },
        { "a table cut short", edit(MockIavDriver::jpegHeader(), EditCutTable, ""), -EINVAL },
        { "not a JPEG", Bytes(200, 0x55), -EINVAL },
        { "zeros", Bytes(200, 0x00), -EINVAL },
        { "SOI only", Bytes{ 0xFF, 0xD8 }, -EINVAL },
    };

    h.addDecodeWindow();
    hd = h.mustOpen(7);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &d, sizeof(d)).r.status);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        h.drv.setDecodeContent(cases[i].bytes);
        calls = h.drv.ioctlCalls();
        LONGS_EQUAL_TEXT(cases[i].want, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &j, sizeof(j)).r.status, cases[i].what);
        LONGS_EQUAL_TEXT(cases[i].want == 0 ? calls + 1 : calls, h.drv.ioctlCalls(), cases[i].what);
    }
    /* a header cut off inside a segment, a segment longer than the bytes read, a second SOF */
    {
        Bytes cut = MockIavDriver::jpegHeader();
        Bytes twice = MockIavDriver::jpegHeader();
        size_t at = 0;

        cut.resize(cut.size() - 20);
        h.drv.setDecodeContent(cut);
        calls = h.drv.ioctlCalls();
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &j, sizeof(j)).r.status);
        while (twice[at] != 0xFF || twice[at + 1] != 0xC0) {
            at++;
        }
        Bytes seg(twice.begin() + (long)at, twice.begin() + (long)at + 19);

        twice.insert(twice.begin() + (long)at, seg.begin(), seg.end());
        h.drv.setDecodeContent(twice);
        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &j, sizeof(j)).r.status);
        LONGS_EQUAL(calls, h.drv.ioctlCalls());
    }
    /* a header that cannot be read is refused, and a size under four bytes cannot be a JPEG */
    h.drv.setDecodeContent(MockIavDriver::jpegHeader());
    h.drv.failDecodeRead(true);
    LONGS_EQUAL(-EIO, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &j, sizeof(j)).r.status);
    h.drv.failDecodeRead(false);
    {
        struct iav_decode_jpeg tiny = jpeg(0, 3, 0x300000, 0x100000);

        LONGS_EQUAL(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &tiny, sizeof(tiny)).r.status);
    }
    CHECK(h.drv.violations().empty());
}

/* A header with the picture size changed. */
static Bytes withSize(Bytes b, unsigned int w, unsigned int h)
{
    const size_t at = findSeg(b, 0xC0, 0);

    b[at + 5] = (uint8_t)(h >> 8);
    b[at + 6] = (uint8_t)h;
    b[at + 7] = (uint8_t)(w >> 8);
    b[at + 8] = (uint8_t)w;
    return b;
}

TEST(IavBrokerDecode, TheRoomForThePictureMustHoldWhatTheHeaderDescribes)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_decoder_info d = decoder(0, (uint32_t)Len);
    unsigned long calls;
    struct Case { const char *what; Bytes bytes; uint64_t need; } cases[] = {
        { "1920x1080 4:2:0", withSize(MockIavDriver::jpegHeader(), 1920, 1080), 1920ull * 1088 * 3 / 2 },
        { "1920x1080 4:2:2", withSize(MockIavDriver::jpegHeader(0x21), 1920, 1080), 1920ull * 1088 * 2 },
        { "1280x720 4:2:2", withSize(MockIavDriver::jpegHeader(0x21), 1280, 720), 1280ull * 720 * 2 },
        { "odd size rounds up to 16", withSize(MockIavDriver::jpegHeader(), 17, 17), 32ull * 32 * 3 / 2 },
        { "the largest the header can say", withSize(MockIavDriver::jpegHeader(0x21), 16384, 16384),
          16384ull * 16384 * 2 },
    };

    h.addDecodeWindow();
    hd = h.mustOpen(7);
    LONGS_EQUAL(0, h.ioctl(7, hd, IAV_IOC_CREATE_DECODER, &d, sizeof(d)).r.status);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const uint64_t room = Len - 0x1000;
        struct iav_decode_jpeg ok = jpeg(0, 1000, 0x1000, (uint32_t)(cases[i].need < room ? cases[i].need : room));
        struct iav_decode_jpeg shortOne = ok;

        h.drv.setDecodeContent(cases[i].bytes);
        shortOne.yuv_size = (uint32_t)(cases[i].need - 1);
        calls = h.drv.ioctlCalls();
        LONGS_EQUAL_TEXT(-EINVAL, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &shortOne, sizeof(shortOne)).r.status, cases[i].what);
        LONGS_EQUAL_TEXT(calls, h.drv.ioctlCalls(), cases[i].what);
        if (cases[i].need <= room) {
            LONGS_EQUAL_TEXT(0, h.ioctl(7, hd, IAV_IOC_DECODE_JPEG, &ok, sizeof(ok)).r.status, cases[i].what);
            LONGS_EQUAL_TEXT(calls + 1, h.drv.ioctlCalls(), cases[i].what);
        }
    }
    /* decode-only is the case that names an output room; the other kind names none */
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerDecode, TheLastByteOfTheWindowIsAllowed)
{
    IavBrokerHarness h(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, 7);
    uint64_t hd;
    struct iav_decoder_info all = decoder(0, (uint32_t)Len), tail = decoder(Len - 1, 1);
    struct iav_decode_jpeg end = jpeg(Len - 1000, 1000, Len - 6000, 5000);

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
    /* Up to twice the partition (the driver's ring rule), not past it, not without PROT_READ. */
    LONGS_EQUAL(0, h.map(7, hd, MockIavDriver::DecBase, 2 * Len, PROT_READ | PROT_WRITE).r.status);
    LONGS_EQUAL(-EINVAL, h.map(7, hd, MockIavDriver::DecBase, 2 * Len + 4096, PROT_READ | PROT_WRITE).r.status);
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
    LONGS_EQUAL(0, h.map(7, primary, MockIavDriver::OvlBase, 2 * OLen, PROT_READ | PROT_WRITE).r.status);
    LONGS_EQUAL(-EINVAL, h.map(7, primary, MockIavDriver::OvlBase, 2 * OLen + 4096, PROT_READ | PROT_WRITE).r.status);
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
