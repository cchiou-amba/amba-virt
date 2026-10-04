/*
 * drivers/amba_virt/tools/test/TestHostNv12Geometry.cxx
 *
 * CppUTest test suite for Dom0 NV12-to-RGB conversion, letterboxing,
 * aspect-ratio preservation, boundary clamping, and fault injection.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <vector>

#include "CppUTest/TestHarness.h"
#include "CppUTest/CommandLineTestRunner.h"
#include "iav_proxy.h"

TEST_GROUP(HostNv12Geometry)
{
    void setup() override
    {
    }

    void teardown() override
    {
    }

    // Helper to generate synthetic NV12 buffer
    void fill_nv12(std::vector<uint8_t> &buf, uint32_t width, uint32_t height, uint32_t pitch,
                   uint8_t default_y, uint8_t default_u, uint8_t default_v)
    {
        size_t total_size = (height + height / 2) * pitch;
        buf.assign(total_size, 0);

        uint8_t *y_plane = buf.data();
        uint8_t *uv_plane = buf.data() + (height * pitch);

        for (uint32_t y = 0; y < height; y++) {
            memset(y_plane + y * pitch, default_y, width);
        }

        for (uint32_t y = 0; y < height / 2; y++) {
            uint8_t *row = uv_plane + y * pitch;
            for (uint32_t x = 0; x < width; x += 2) {
                row[x] = default_u;
                row[x + 1] = default_v;
            }
        }
    }

    // Paint a solid color rectangle on NV12
    void paint_patch_nv12(std::vector<uint8_t> &buf, uint32_t width, uint32_t height, uint32_t pitch,
                          uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2,
                          uint8_t y_val, uint8_t u_val, uint8_t v_val)
    {
        uint8_t *y_plane = buf.data();
        uint8_t *uv_plane = buf.data() + (height * pitch);

        for (uint32_t y = y1; y < y2 && y < height; y++) {
            for (uint32_t x = x1; x < x2 && x < width; x++) {
                y_plane[y * pitch + x] = y_val;
            }
        }

        for (uint32_t y = y1 / 2; y < (y2 + 1) / 2 && y < height / 2; y++) {
            uint8_t *row = uv_plane + y * pitch;
            for (uint32_t x = (x1 / 2) * 2; x < x2 && x < width; x += 2) {
                row[x] = u_val;
                row[x + 1] = v_val;
            }
        }
    }
};

/*
 * Test 1: Nominal Forward Scale (16:9 1920x1080 -> 640x640 Planar RGB)
 * Verifies that a known color patch at (960, 540) scales by r = 1/3
 * to (320, 180) in planar RGB, and that rows 360..639 are filled with 114.
 */
TEST(HostNv12Geometry, NominalForwardScale)
{
    const uint32_t w = 1920;
    const uint32_t h = 1080;
    const uint32_t pitch = 1920;
    std::vector<uint8_t> nv12;
    // Default neutral gray (Y=114, U=128, V=128)
    fill_nv12(nv12, w, h, pitch, 114, 128, 128);

    // Paint bright Red patch around center (950..970, 530..550)
    // Red in BT.601: Y=82, U=90, V=240
    paint_patch_nv12(nv12, w, h, pitch, 950, 530, 970, 550, 82, 90, 240);

    std::vector<uint8_t> tensor(3 * 640 * 640, 0);
    int rc = proxy_convert_nv12_to_yolox_rgb_640x640(nv12.data(), w, h, pitch, tensor.data());
    LONGS_EQUAL(0, rc);

    const uint8_t *r_plane = tensor.data();
    const uint8_t *g_plane = tensor.data() + (640 * 640);
    const uint8_t *b_plane = tensor.data() + (2 * 640 * 640);

    // Center of scaled image is (320, 180)
    int center_idx = 180 * 640 + 320;
    CHECK(r_plane[center_idx] > 200);
    CHECK(g_plane[center_idx] < 60);
    CHECK(b_plane[center_idx] < 60);

    // Padded region: rows 360..639 must be exactly 114 across all planes
    for (int row = 360; row < 640; row++) {
        int idx = row * 640 + 320;
        LONGS_EQUAL(114, r_plane[idx]);
        LONGS_EQUAL(114, g_plane[idx]);
        LONGS_EQUAL(114, b_plane[idx]);
    }
}

/*
 * Test 2: Aspect Ratios (16:9, 4:3, 1:1, vertical 9:16)
 * Verifies correct letterbox padding and scaled dimensions.
 */
TEST(HostNv12Geometry, AspectRatios)
{
    std::vector<uint8_t> tensor(3 * 640 * 640, 0);

    // 16:9 (1280x720) -> scale r = 640/1280 = 0.5 -> new_w=640, new_h=360
    {
        std::vector<uint8_t> nv12;
        fill_nv12(nv12, 1280, 720, 1280, 200, 128, 128);
        int rc = proxy_convert_nv12_to_yolox_rgb_640x640(nv12.data(), 1280, 720, 1280, tensor.data());
        LONGS_EQUAL(0, rc);
        // Active image row 359 should be ~200
        CHECK(tensor[359 * 640 + 320] > 180);
        // Padded row 360 must be 114
        LONGS_EQUAL(114, tensor[360 * 640 + 320]);
    }

    // 4:3 (640x480) -> scale r = 1.0 -> new_w=640, new_h=480
    {
        std::vector<uint8_t> nv12;
        fill_nv12(nv12, 640, 480, 640, 200, 128, 128);
        int rc = proxy_convert_nv12_to_yolox_rgb_640x640(nv12.data(), 640, 480, 640, tensor.data());
        LONGS_EQUAL(0, rc);
        CHECK(tensor[479 * 640 + 320] > 180);
        LONGS_EQUAL(114, tensor[480 * 640 + 320]);
    }

    // 1:1 (640x640) -> scale r = 1.0 -> new_w=640, new_h=640
    {
        std::vector<uint8_t> nv12;
        fill_nv12(nv12, 640, 640, 640, 200, 128, 128);
        int rc = proxy_convert_nv12_to_yolox_rgb_640x640(nv12.data(), 640, 640, 640, tensor.data());
        LONGS_EQUAL(0, rc);
        CHECK(tensor[639 * 640 + 320] > 180);
    }

    // Vertical 9:16 (360x640) -> scale r = 640/640 = 1.0 -> new_w=360, new_h=640
    {
        std::vector<uint8_t> nv12;
        fill_nv12(nv12, 360, 640, 360, 200, 128, 128);
        int rc = proxy_convert_nv12_to_yolox_rgb_640x640(nv12.data(), 360, 640, 360, tensor.data());
        LONGS_EQUAL(0, rc);
        // Column 359 should be ~200
        CHECK(tensor[320 * 640 + 359] > 180);
        // Horizontal padding column 360..639 must be 114
        LONGS_EQUAL(114, tensor[320 * 640 + 360]);
        LONGS_EQUAL(114, tensor[320 * 640 + 639]);
    }
}

/*
 * Test 3: Boundary Clamping & Odd Sensor Resolutions
 * Verifies edge pixels (0, 0) and (W-1, H-1) do not overrun buffer boundaries.
 */
TEST(HostNv12Geometry, BoundaryClamping)
{
    const uint32_t w = 1919; // Odd width
    const uint32_t h = 1079; // Odd height
    const uint32_t pitch = 1920;
    std::vector<uint8_t> nv12;
    fill_nv12(nv12, w, h, pitch, 50, 128, 128);

    // Edge patches
    paint_patch_nv12(nv12, w, h, pitch, 0, 0, 4, 4, 255, 128, 128);
    paint_patch_nv12(nv12, w, h, pitch, w - 4, h - 4, w, h, 255, 128, 128);

    std::vector<uint8_t> tensor(3 * 640 * 640, 0);
    int rc = proxy_convert_nv12_to_yolox_rgb_640x640(nv12.data(), w, h, pitch, tensor.data());
    LONGS_EQUAL(0, rc);

    // Pixel (0, 0) should reflect the white patch
    CHECK(tensor[0] > 200);

    // Bottom-right edge should be valid without segfault
    float r = fminf(640.0f / (float)w, 640.0f / (float)h);
    int new_w = (int)roundf((float)w * r);
    int new_h = (int)roundf((float)h * r);
    int br_idx = (new_h - 1) * 640 + (new_w - 1);
    CHECK(tensor[br_idx] > 200);
}

/*
 * Test 4: Fault Injection - Zero Dimensions & Null Pointers
 */
TEST(HostNv12Geometry, FaultInjectionZeroDimensions)
{
    std::vector<uint8_t> buf(1920 * 1080);
    std::vector<uint8_t> tensor(3 * 640 * 640);

    LONGS_EQUAL(-EINVAL, proxy_convert_nv12_to_yolox_rgb_640x640(nullptr, 1920, 1080, 1920, tensor.data()));
    LONGS_EQUAL(-EINVAL, proxy_convert_nv12_to_yolox_rgb_640x640(buf.data(), 1920, 1080, 1920, nullptr));
    LONGS_EQUAL(-EINVAL, proxy_convert_nv12_to_yolox_rgb_640x640(buf.data(), 0, 1080, 1920, tensor.data()));
    LONGS_EQUAL(-EINVAL, proxy_convert_nv12_to_yolox_rgb_640x640(buf.data(), 1920, 0, 1920, tensor.data()));
    LONGS_EQUAL(-EINVAL, proxy_convert_nv12_to_yolox_rgb_640x640(buf.data(), 0, 0, 0, tensor.data()));
}

/*
 * Test 5: Fault Injection - Corrupt Stride (pitch < width)
 */
TEST(HostNv12Geometry, FaultInjectionCorruptStride)
{
    std::vector<uint8_t> buf(1920 * 1080);
    std::vector<uint8_t> tensor(3 * 640 * 640);

    // Pitch smaller than width is an illegal memory layout
    LONGS_EQUAL(-EINVAL, proxy_convert_nv12_to_yolox_rgb_640x640(buf.data(), 1920, 1080, 1000, tensor.data()));
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
