/*
 * TestUsbJpeg.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include "../UsbJpeg.hxx"
#include "../UsbCameraCapture.hxx"

#include <string.h>
#include <stdlib.h>
#include <vector>

namespace {

/* A JPEG header up to the first scan marker, with the parts a test varies. */
struct Header {
    uint8_t precision = 8;
    uint8_t components = 3;
    uint8_t hv0 = 0x22, hv1 = 0x11, hv2 = 0x11;
    uint8_t sofMarker = 0xC0;
    uint16_t width = 1280, height = 720;
    bool dqt = true, dht = false;   /* dht: one custom (empty) Huffman table; false: none, as UVC Motion-JPEG */
    bool jfif = false, avi1 = false, app1 = false, com = false;
    int restart = -1;       /* restart interval, -1 for none */
    int fill = 0;           /* 0xFF fill bytes before the SOF marker */
    int padApp = 0;         /* bytes of APP0 payload before the tables */
};

std::vector<uint8_t> build(const Header &h)
{
    std::vector<uint8_t> v = {0xFF, 0xD8};

    if (h.jfif)
        v.insert(v.end(), {0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    if (h.avi1) {
        v.insert(v.end(), {0xFF, 0xE0, 0x00, 0x21, 'A', 'V', 'I', '1'});
        v.insert(v.end(), 27, 0);
    }
    if (h.app1)
        v.insert(v.end(), {0xFF, 0xE1, 0x00, 0x04, 'x', 'y'});
    if (h.com)
        v.insert(v.end(), {0xFF, 0xFE, 0x00, 0x04, 'h', 'i'});
    if (h.padApp > 0) {
        v.push_back(0xFF); v.push_back(0xE0);
        v.push_back(static_cast<uint8_t>((h.padApp + 2) >> 8)); v.push_back(static_cast<uint8_t>(h.padApp + 2));
        v.insert(v.end(), static_cast<size_t>(h.padApp), 0);
    }
    if (h.dqt) {
        v.insert(v.end(), {0xFF, 0xDB, 0x00, 0x43, 0x00});
        v.insert(v.end(), 64, 8);
    }
    if (h.restart >= 0) {
        v.insert(v.end(), {0xFF, 0xDD, 0x00, 0x04, static_cast<uint8_t>(h.restart >> 8), static_cast<uint8_t>(h.restart)});
    }
    for (int i = 0; i < h.fill; i++)
        v.push_back(0xFF);
    v.push_back(0xFF); v.push_back(h.sofMarker);
    v.push_back(0x00); v.push_back(static_cast<uint8_t>(8 + 3 * h.components));
    v.push_back(h.precision);
    v.push_back(static_cast<uint8_t>(h.height >> 8)); v.push_back(static_cast<uint8_t>(h.height));
    v.push_back(static_cast<uint8_t>(h.width >> 8)); v.push_back(static_cast<uint8_t>(h.width));
    v.push_back(h.components);
    const uint8_t hv[3] = {h.hv0, h.hv1, h.hv2};
    for (int c = 0; c < h.components; c++) {
        v.push_back(static_cast<uint8_t>(c + 1)); v.push_back(c < 3 ? hv[c] : 0x11); v.push_back(0);
    }
    if (h.dht) {
        v.insert(v.end(), {0xFF, 0xC4, 0x00, 0x13, 0x00});
        v.insert(v.end(), 16, 0);
    }
    v.insert(v.end(), {0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00});
    v.insert(v.end(), 32, 0x55);
    return v;
}

/* The frame as the application hands it to the decoder: normalized, then judged. */
int kindOf(const std::vector<uint8_t> &v, bool *ok = nullptr, std::vector<uint8_t> *norm = nullptr)
{
    UsbJpeg::Info info{};
    std::vector<uint8_t> n;
    bool r = false;

    if (UsbJpeg::normalize(v.data(), v.size(), &n))
        r = UsbJpeg::inspect(n.data(), n.size(), &info);
    else
        info.kind = UsbJpeg::KindMalformed;
    if (ok)
        *ok = r;
    if (norm)
        *norm = n;
    return info.kind;
}

/* The kind of the frame as it is, without normalizing. */
int rawKindOf(const std::vector<uint8_t> &v, bool *ok = nullptr)
{
    UsbJpeg::Info info{};
    bool r = UsbJpeg::inspect(v.data(), v.size(), &info);

    if (ok)
        *ok = r;
    return info.kind;
}

bool hasMarker(const std::vector<uint8_t> &v, uint8_t marker)
{
    size_t i = 2;

    while (i + 4 <= v.size() && v[i] == 0xFF && v[i + 1] != 0xDA) {
        if (v[i + 1] == marker)
            return true;
        i += 2 + ((static_cast<size_t>(v[i + 2]) << 8) | v[i + 3]);
    }
    return false;
}

} // namespace

TEST_GROUP(UsbJpegAdmission)
{
};

TEST(UsbJpegAdmission, Baseline420IsTakenOnceNormalized)
{
    UsbJpeg::Info info{};
    std::vector<uint8_t> v = build(Header()), n;
    bool ok = true;

    CHECK_FALSE(UsbJpeg::inspect(v.data(), v.size(), &info));    /* as it is: no Huffman tables */
    LONGS_EQUAL(UsbJpeg::KindCustomHuffman, info.kind);
    LONGS_EQUAL(UsbJpeg::KindHw, kindOf(v, &ok, &n));
    CHECK_TRUE(ok);
    CHECK_TRUE(UsbJpeg::inspect(n.data(), n.size(), &info));
    LONGS_EQUAL(1280, info.width);
    LONGS_EQUAL(720, info.height);
}

TEST(UsbJpegAdmission, FourTwoTwoIsTaken)
{
    Header h;
    bool ok = false;

    h.hv0 = 0x21;
    LONGS_EQUAL(UsbJpeg::KindHw, kindOf(build(h), &ok));
    CHECK_TRUE(ok);
}

TEST(UsbJpegAdmission, OtherSamplingsAreRefused)
{
    Header h;
    bool ok = true;

    h.hv0 = 0x11;
    LONGS_EQUAL(UsbJpeg::KindSampling444, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
    h.hv0 = 0x12;
    LONGS_EQUAL(UsbJpeg::KindSamplingOther, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
    h.hv0 = 0x22; h.hv1 = 0x21;
    LONGS_EQUAL(UsbJpeg::KindSamplingOther, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
}

TEST(UsbJpegAdmission, NotBaselineEightBitThreeComponentRestart)
{
    Header h;
    bool ok = true;

    h.sofMarker = 0xC2;
    LONGS_EQUAL(UsbJpeg::KindProgressive, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
    h.sofMarker = 0xC1;
    LONGS_EQUAL(UsbJpeg::KindNotBaseline, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
    h = Header(); h.precision = 12;
    LONGS_EQUAL(UsbJpeg::KindNotEightBit, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
    h = Header(); h.components = 1;
    LONGS_EQUAL(UsbJpeg::KindNotThreeComponent, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
    h = Header(); h.restart = 20;
    LONGS_EQUAL(UsbJpeg::KindRestartInterval, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
    h = Header(); h.restart = 0;       /* any DRI is refused, as the server does */
    LONGS_EQUAL(UsbJpeg::KindRestartInterval, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
}

TEST(UsbJpegAdmission, AQuantisationTableIsRequired)
{
    Header h;
    bool ok = true;

    h.dqt = false;
    LONGS_EQUAL(UsbJpeg::KindMissingTables, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
}

TEST(UsbJpegAdmission, TheStandardHuffmanTablesAreInsertedWhenTheFrameHasNone)
{
    Header h;
    std::vector<uint8_t> n;
    bool ok = false;

    LONGS_EQUAL(UsbJpeg::KindHw, kindOf(build(h), &ok, &n));
    CHECK_TRUE(ok);
    CHECK_TRUE(hasMarker(n, 0xC4));
    /* the scan bytes after the header are unchanged */
    CHECK_TRUE(n.size() > 32);
    for (size_t i = n.size() - 32; i < n.size(); i++)
        BYTES_EQUAL(0x55, n[i]);
}

TEST(UsbJpegAdmission, ACustomHuffmanTableIsKeptAndRefused)
{
    Header h;
    std::vector<uint8_t> n;
    bool ok = true;

    h.dht = true;
    LONGS_EQUAL(UsbJpeg::KindCustomHuffman, kindOf(build(h), &ok, &n));
    CHECK_FALSE(ok);
    CHECK_TRUE(hasMarker(n, 0xC4));
}

TEST(UsbJpegAdmission, Avi1AndOtherSegmentsAreDroppedAndJfifIsKept)
{
    Header h;
    std::vector<uint8_t> n;
    bool ok = false;

    h.avi1 = true;
    LONGS_EQUAL(UsbJpeg::KindMarkerNotAllowed, rawKindOf(build(h)));
    LONGS_EQUAL(UsbJpeg::KindHw, kindOf(build(h), &ok, &n));
    CHECK_TRUE(ok);
    CHECK_FALSE(hasMarker(n, 0xE0));
    h = Header(); h.app1 = true; h.com = true; h.padApp = 20;
    LONGS_EQUAL(UsbJpeg::KindHw, kindOf(build(h), &ok, &n));
    CHECK_TRUE(ok);
    CHECK_FALSE(hasMarker(n, 0xE0));
    CHECK_FALSE(hasMarker(n, 0xE1));
    CHECK_FALSE(hasMarker(n, 0xFE));
    h = Header(); h.jfif = true;
    LONGS_EQUAL(UsbJpeg::KindHw, kindOf(build(h), &ok, &n));
    CHECK_TRUE(ok);
    CHECK_TRUE(hasMarker(n, 0xE0));
    LONGS_EQUAL(UsbJpeg::KindHw, rawKindOf(n, &ok));
    /* a second JFIF APP0 is dropped, the first kept */
    h.jfif = true;
    std::vector<uint8_t> two = build(h);
    two.insert(two.begin() + 2, two.begin() + 2, two.begin() + 20);
    LONGS_EQUAL(UsbJpeg::KindMarkerNotAllowed, rawKindOf(two));
    LONGS_EQUAL(UsbJpeg::KindHw, kindOf(two, &ok, &n));
    CHECK_TRUE(ok);
}

TEST(UsbJpegAdmission, FillBytesAreRefused)
{
    Header h;
    bool ok = true;

    h.fill = 3;
    kindOf(build(h), &ok);
    CHECK_FALSE(ok);
}

TEST(UsbJpegAdmission, OnlyTheFirst4096BytesAreRead)
{
    Header h;
    bool ok = true;

    h.padApp = 4200;                    /* the scan starts beyond the header window, as the server sees it */
    LONGS_EQUAL(UsbJpeg::KindMalformed, kindOf(build(h), &ok));
    CHECK_FALSE(ok);
}

TEST(UsbJpegAdmission, MalformedInputIsRefusedWithoutOverreading)
{
    std::vector<uint8_t> good, raw = build(Header());
    std::vector<uint8_t> n;
    bool ok = true;

    CHECK_FALSE(UsbJpeg::inspect(nullptr, 100, nullptr));
    CHECK_FALSE(UsbJpeg::normalize(nullptr, 100, &n));
    kindOf(raw, &ok, &good);
    CHECK_TRUE(ok);
    CHECK_FALSE(UsbJpeg::inspect(good.data(), 0, nullptr));
    CHECK_FALSE(UsbJpeg::inspect(good.data(), 3, nullptr));
    kindOf(std::vector<uint8_t>{0x00, 0x01, 0x02, 0x03, 0x04}, &ok);
    CHECK_FALSE(ok);
    /* every truncation of a good header is refused except the complete one, and never reads past the end */
    for (size_t k = 0; k + 1 < good.size() - 33; k++) {
        std::vector<uint8_t> t(good.begin(), good.begin() + static_cast<long>(k));
        CHECK_FALSE(UsbJpeg::inspect(t.empty() ? good.data() : t.data(), k, nullptr));
        std::vector<uint8_t> r(raw.begin(), raw.begin() + static_cast<long>(k < raw.size() ? k : raw.size()));
        if (k + 1 < raw.size() - 33)
            CHECK_FALSE(UsbJpeg::normalize(r.empty() ? raw.data() : r.data(), r.size(), &n));
    }
    /* a second SOI, an EOI before the scan, a segment longer than the data */
    std::vector<uint8_t> bad = good;
    bad[2] = 0xFF; bad[3] = 0xD8;
    CHECK_FALSE(UsbJpeg::inspect(bad.data(), bad.size(), nullptr));
    bad = good; bad[2] = 0xFF; bad[3] = 0xD9;
    CHECK_FALSE(UsbJpeg::inspect(bad.data(), bad.size(), nullptr));
    bad = good; bad[4] = 0xFF; bad[5] = 0xFF;
    CHECK_FALSE(UsbJpeg::inspect(bad.data(), bad.size(), nullptr));
}

TEST(UsbJpegAdmission, ByteFlipsNeverCrashAndNeverAdmitAnotherKind)
{
    std::vector<uint8_t> good;

    kindOf(build(Header()), nullptr, &good);
    srand(12345);
    for (int i = 0; i < 20000; i++) {
        std::vector<uint8_t> v = good;
        int flips = 1 + rand() % 3;

        for (int f = 0; f < flips; f++)
            v[static_cast<size_t>(rand()) % v.size()] = static_cast<uint8_t>(rand());
        UsbJpeg::Info info{};
        if (UsbJpeg::inspect(v.data(), v.size(), &info)) {
            /* an admitted header must have the properties the decoder needs, whatever was flipped */
            LONGS_EQUAL(UsbJpeg::KindHw, info.kind);
            CHECK_TRUE(info.width > 0 && info.height > 0 && info.width <= 16384 && info.height <= 16384);
        }
        /* the normalizer copes with the same input */
        std::vector<uint8_t> n;
        UsbJpeg::normalize(v.data(), v.size(), &n);
    }
}

TEST(UsbJpegAdmission, TheDecodedPictureNeedsTheBytesTheServerCounts)
{
    UsbJpeg::Info info{};
    std::vector<uint8_t> n;
    Header h;

    h.hv0 = 0x21;
    h.width = 1920; h.height = 1080;
    CHECK_TRUE(UsbJpeg::normalize(build(h).data(), build(h).size(), &n));
    CHECK_TRUE(UsbJpeg::inspect(n.data(), n.size(), &info));
    CHECK_TRUE(info.sub422);
    CHECK_TRUE(UsbJpeg::decodedBytes(info.width, info.height, info.sub422) == 4177920ull);
    CHECK_TRUE(UsbJpeg::decodedBytes(1920, 1080, false) == 3133440ull);
    CHECK_TRUE(UsbJpeg::decodedBytes(17, 17, false) == 32ull * 32 * 3 / 2);
}

TEST(UsbJpegAdmission, AspectRule)
{
    CHECK_TRUE(UsbJpeg::aspectIs16x9(1280, 720));
    CHECK_TRUE(UsbJpeg::aspectIs16x9(1920, 1080));
    CHECK_FALSE(UsbJpeg::aspectIs16x9(640, 480));
    CHECK_FALSE(UsbJpeg::aspectIs16x9(0, 0));
    CHECK_FALSE(UsbJpeg::aspectIs16x9(1280, 0));
}

TEST_GROUP(UsbNv12Convert)
{
};

TEST(UsbNv12Convert, GrayAndPrimaries)
{
    std::vector<uint8_t> nv12(640 * 640 * 3 / 2, 0), rgb(3 * 640 * 640, 114);
    const size_t plane = 640 * 640;

    /* rows 0..1 gray 128, rows 2..3 saturated blue (Y 29, U 255, V 107 is BT.601 blue) */
    for (size_t i = 0; i < 640 * 2; i++)
        nv12[i] = 128;
    for (size_t i = 0; i < 640; i += 2) {
        nv12[plane + i] = 128;
        nv12[plane + i + 1] = 128;
        nv12[plane + 640 + i] = 255;
        nv12[plane + 640 + i + 1] = 107;
    }
    for (size_t i = 640 * 2; i < 640 * 4; i++)
        nv12[i] = 29;
    UsbJpeg::nv12ToPlanar(nv12.data(), rgb.data(), 4);
    BYTES_EQUAL(128, rgb[0]);
    BYTES_EQUAL(128, rgb[plane]);
    BYTES_EQUAL(128, rgb[2 * plane]);
    CHECK_TRUE(rgb[2 * 640 * 2 * 0 + 2 * plane + 2 * 640] > 240);      /* blue, row 2 */
    CHECK_TRUE(rgb[2 * 640] < 20);                                     /* red, row 2 */
    /* rows beyond the converted ones are untouched (the caller's pad) */
    BYTES_EQUAL(114, rgb[4 * 640]);
    BYTES_EQUAL(114, rgb[2 * plane + 639 * 640]);
}

TEST(UsbNv12Convert, RowsAreClamped)
{
    std::vector<uint8_t> nv12(640 * 640 * 3 / 2, 128), rgb(3 * 640 * 640, 7);

    UsbJpeg::nv12ToPlanar(nv12.data(), rgb.data(), 100000);
    BYTES_EQUAL(128, rgb[640 * 640 - 1]);
    BYTES_EQUAL(128, rgb[3 * 640 * 640 - 1]);
}

TEST_GROUP(UsbExposureClamp)
{
};

TEST(UsbExposureClamp, ValueIsClampedToTheRange)
{
    LONGS_EQUAL(16, UsbCameraCapture::clampValue(0, 16, 255));
    LONGS_EQUAL(255, UsbCameraCapture::clampValue(9999, 16, 255));
    LONGS_EQUAL(128, UsbCameraCapture::clampValue(128, 16, 255));
    CHECK_TRUE(UsbCameraCapture::kFixedExposureMax <= 330);       /* the user bound: 33 ms in 100 us units */
    CHECK_TRUE(UsbCameraCapture::kFixedExposureDefault <= UsbCameraCapture::kFixedExposureMax);
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
