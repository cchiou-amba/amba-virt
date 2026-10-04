/*
 * TestYoloGeometry.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include "../YoloGeometry.hxx"

#include <math.h>
#include <errno.h>

TEST_GROUP(YoloGeometry)
{
    void setup() override {}
    void teardown() override {}
};

TEST(YoloGeometry, ForwardInverseRoundTrip)
{
    struct yolo_geometry_config cfg;
    int rc = yolo_geometry_init(&cfg, 640, 640, 1920, 1080, false);
    LONGS_EQUAL(0, rc);

    const float test_points[][2] = {
        { 0.0f, 0.0f },
        { 960.0f, 540.0f },
        { 1920.0f, 1080.0f },
        { 240.5f, 135.25f },
        { 1600.0f, 900.0f }
    };

    for (size_t i = 0; i < sizeof(test_points) / sizeof(test_points[0]); i++) {
        float ox = test_points[i][0];
        float oy = test_points[i][1];
        float tx = 0.0f, ty = 0.0f;
        float nx = 0.0f, ny = 0.0f;
        float cx = 0.0f, cy = 0.0f;

        rc = yolo_geometry_forward(&cfg, ox, oy, &tx, &ty);
        LONGS_EQUAL(0, rc);

        rc = yolo_geometry_inverse(&cfg, tx, ty, &nx, &ny);
        LONGS_EQUAL(0, rc);

        rc = yolo_geometry_canvas_project(nx, ny, 1920, 1080, &cx, &cy);
        LONGS_EQUAL(0, rc);

        DOUBLES_EQUAL((double)ox, (double)cx, 1e-3);
        DOUBLES_EQUAL((double)oy, (double)cy, 1e-3);
    }

    /* Verify centered letterboxing mode roundtrip */
    rc = yolo_geometry_init(&cfg, 640, 640, 1920, 1080, true);
    LONGS_EQUAL(0, rc);

    for (size_t i = 0; i < sizeof(test_points) / sizeof(test_points[0]); i++) {
        float ox = test_points[i][0];
        float oy = test_points[i][1];
        float tx = 0.0f, ty = 0.0f;
        float nx = 0.0f, ny = 0.0f;
        float cx = 0.0f, cy = 0.0f;

        rc = yolo_geometry_forward(&cfg, ox, oy, &tx, &ty);
        LONGS_EQUAL(0, rc);

        rc = yolo_geometry_inverse(&cfg, tx, ty, &nx, &ny);
        LONGS_EQUAL(0, rc);

        rc = yolo_geometry_canvas_project(nx, ny, 1920, 1080, &cx, &cy);
        LONGS_EQUAL(0, rc);

        DOUBLES_EQUAL((double)ox, (double)cx, 1e-3);
        DOUBLES_EQUAL((double)oy, (double)cy, 1e-3);
    }
}

TEST(YoloGeometry, AspectRatioLetterbox)
{
    struct yolo_geometry_config cfg;

    /* 16:9 input (1920x1080) -> 640x640: scaled_w=640, scaled_h=360 */
    LONGS_EQUAL(0, yolo_geometry_init(&cfg, 640, 640, 1920, 1080, true));
    DOUBLES_EQUAL(640.0 / 1920.0, (double)cfg.scale_r, 1e-5);
    DOUBLES_EQUAL(0.0, (double)cfg.pad_x, 1e-5);
    DOUBLES_EQUAL(140.0, (double)cfg.pad_y, 1e-5);

    /* 4:3 input (640x480) -> 640x640: scaled_w=640, scaled_h=480 */
    LONGS_EQUAL(0, yolo_geometry_init(&cfg, 640, 640, 640, 480, true));
    DOUBLES_EQUAL(1.0, (double)cfg.scale_r, 1e-5);
    DOUBLES_EQUAL(0.0, (double)cfg.pad_x, 1e-5);
    DOUBLES_EQUAL(80.0, (double)cfg.pad_y, 1e-5);

    /* 1:1 input (500x500) -> 640x640: scaled_w=640, scaled_h=640 */
    LONGS_EQUAL(0, yolo_geometry_init(&cfg, 640, 640, 500, 500, true));
    DOUBLES_EQUAL(640.0 / 500.0, (double)cfg.scale_r, 1e-5);
    DOUBLES_EQUAL(0.0, (double)cfg.pad_x, 1e-5);
    DOUBLES_EQUAL(0.0, (double)cfg.pad_y, 1e-5);

    /* Vertical 9:16 input (1080x1920) -> 640x640: scaled_w=360, scaled_h=640 */
    LONGS_EQUAL(0, yolo_geometry_init(&cfg, 640, 640, 1080, 1920, true));
    DOUBLES_EQUAL(640.0 / 1920.0, (double)cfg.scale_r, 1e-5);
    DOUBLES_EQUAL(140.0, (double)cfg.pad_x, 1e-5);
    DOUBLES_EQUAL(0.0, (double)cfg.pad_y, 1e-5);
}

TEST(YoloGeometry, EdgeClampingBoundary)
{
    struct yolo_geometry_config cfg;
    LONGS_EQUAL(0, yolo_geometry_init(&cfg, 640, 640, 1920, 1080, false));

    float nx = -1.0f, ny = -1.0f;
    LONGS_EQUAL(0, yolo_geometry_inverse(&cfg, 0.0f, 0.0f, &nx, &ny));
    DOUBLES_EQUAL(0.0, (double)nx, 1e-5);
    DOUBLES_EQUAL(0.0, (double)ny, 1e-5);

    /* 1920 scaled down by 1/3 is 640, 1080 is 360 */
    LONGS_EQUAL(0, yolo_geometry_inverse(&cfg, 640.0f, 360.0f, &nx, &ny));
    DOUBLES_EQUAL(1.0, (double)nx, 1e-5);
    DOUBLES_EQUAL(1.0, (double)ny, 1e-5);

    /* Coordinates in the padded bottom area (e.g. y = 500) must clamp to 1.0 */
    LONGS_EQUAL(0, yolo_geometry_inverse(&cfg, 640.0f, 500.0f, &nx, &ny));
    DOUBLES_EQUAL(1.0, (double)nx, 1e-5);
    DOUBLES_EQUAL(1.0, (double)ny, 1e-5);
}

TEST(YoloGeometry, FaultInjectionOutOfRange)
{
    struct yolo_geometry_config cfg;
    LONGS_EQUAL(0, yolo_geometry_init(&cfg, 640, 640, 1920, 1080, false));

    float tx = 0.0f, ty = 0.0f;
    /* Extreme negative coordinates */
    LONGS_EQUAL(0, yolo_geometry_forward(&cfg, -1000.0f, -500.0f, &tx, &ty));
    DOUBLES_EQUAL(0.0, (double)tx, 1e-5);
    DOUBLES_EQUAL(0.0, (double)ty, 1e-5);

    /* Extreme positive coordinates */
    LONGS_EQUAL(0, yolo_geometry_forward(&cfg, 10000.0f, 5000.0f, &tx, &ty));
    DOUBLES_EQUAL(640.0, (double)tx, 1e-5);
    DOUBLES_EQUAL(360.0, (double)ty, 1e-5);
}

TEST(YoloGeometry, FaultInjectionInvertedBoxes)
{
    struct yolo_geometry_config cfg;
    LONGS_EQUAL(0, yolo_geometry_init(&cfg, 640, 640, 1920, 1080, false));

    struct yolo_norm_box box;
    memset(&box, 0, sizeof(box));

    /* Inverted box: x1 > x2 and y1 > y2 */
    int rc = yolo_geometry_normalize_box(&cfg, 320.0f, 180.0f, 100.0f, 50.0f, &box);
    LONGS_EQUAL(0, rc);

    CHECK(box.x1 < box.x2);
    CHECK(box.y1 < box.y2);
    CHECK(box.x1 >= 0.0f && box.x2 <= 1.0f);
    CHECK(box.y1 >= 0.0f && box.y2 <= 1.0f);
}

TEST(YoloGeometry, FaultInjectionZeroDimensions)
{
    struct yolo_geometry_config cfg;
    LONGS_EQUAL(-EINVAL, yolo_geometry_init(&cfg, 0, 640, 1920, 1080, false));
    LONGS_EQUAL(-EINVAL, yolo_geometry_init(&cfg, 640, 0, 1920, 1080, false));
    LONGS_EQUAL(-EINVAL, yolo_geometry_init(&cfg, 640, 640, 0, 1080, false));
    LONGS_EQUAL(-EINVAL, yolo_geometry_init(&cfg, 640, 640, 1920, 0, false));
    LONGS_EQUAL(-EINVAL, yolo_geometry_init(nullptr, 640, 640, 1920, 1080, false));

    /* Canvas projection zero dimensions */
    float cx = 0.0f, cy = 0.0f;
    LONGS_EQUAL(-EINVAL, yolo_geometry_canvas_project(0.5f, 0.5f, 0, 1080, &cx, &cy));
    LONGS_EQUAL(-EINVAL, yolo_geometry_canvas_project(0.5f, 0.5f, 1920, 0, &cx, &cy));
}

TEST(YoloGeometry, FaultInjectionNaNInf)
{
    struct yolo_geometry_config cfg;
    LONGS_EQUAL(0, yolo_geometry_init(&cfg, 640, 640, 1920, 1080, false));

    float tx = 0.0f, ty = 0.0f;
    LONGS_EQUAL(-EINVAL, yolo_geometry_forward(&cfg, NAN, 100.0f, &tx, &ty));
    LONGS_EQUAL(-EINVAL, yolo_geometry_forward(&cfg, 100.0f, INFINITY, &tx, &ty));

    float nx = 0.0f, ny = 0.0f;
    LONGS_EQUAL(-EINVAL, yolo_geometry_inverse(&cfg, NAN, 50.0f, &nx, &ny));
    LONGS_EQUAL(-EINVAL, yolo_geometry_inverse(&cfg, 50.0f, -INFINITY, &nx, &ny));

    float cx = 0.0f, cy = 0.0f;
    LONGS_EQUAL(-EINVAL, yolo_geometry_canvas_project(NAN, 0.5f, 1920, 1080, &cx, &cy));
    LONGS_EQUAL(-EINVAL, yolo_geometry_canvas_project(0.5f, INFINITY, 1920, 1080, &cx, &cy));

    struct yolo_norm_box box;
    LONGS_EQUAL(-EINVAL, yolo_geometry_normalize_box(&cfg, NAN, 10.0f, 20.0f, 30.0f, &box));
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
