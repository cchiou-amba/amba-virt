/*
 * TestYoloPostprocess.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include "../YoloGeometry.hxx"
#include "../YoloPostprocess.hxx"

#include <math.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

TEST_GROUP(YoloPostprocess)
{
    uint8_t *tensor_buf;
    struct yolo_geometry_config geo;
    struct yolo_postprocess_config post;

    void setup() override
    {
        tensor_buf = (uint8_t *)calloc(YOLO_NUM_ANCHORS, YOLO_OUTPUT_PITCH);
        CHECK(tensor_buf != nullptr);
        LONGS_EQUAL(0, yolo_geometry_init(&geo, 640, 640, 1920, 1080, false));
        yolo_postprocess_default_config(&post);
    }

    void teardown() override
    {
        free(tensor_buf);
        tensor_buf = nullptr;
    }

    void inject_target(int s_idx, int gx, int gy,
                       float offset_x, float offset_y,
                       float target_w, float target_h,
                       float obj_score, int class_id, float class_score)
    {
        const int strides[3] = { 8, 16, 32 };
        const int base_offsets[3] = { 0, 6400, 8000 };
        const int grid_sizes[3] = { 80, 40, 20 };

        int stride = strides[s_idx];
        int base = base_offsets[s_idx];
        int grid = grid_sizes[s_idx];
        int anchor_idx = base + (gy * grid) + gx;

        CHECK(anchor_idx < YOLO_NUM_ANCHORS);
        uint16_t *row = (uint16_t *)(tensor_buf + (anchor_idx * YOLO_OUTPUT_PITCH));
        row[0] = yolo_float_to_fp16(offset_x);
        row[1] = yolo_float_to_fp16(offset_y);
        row[2] = yolo_float_to_fp16(logf(target_w / (float)stride));
        row[3] = yolo_float_to_fp16(logf(target_h / (float)stride));
        row[4] = yolo_float_to_fp16(obj_score);
        row[5 + class_id] = yolo_float_to_fp16(class_score);
    }
};

TEST(YoloPostprocess, GoldenSyntheticVectorsIoU)
{
    /*
     * Inject 5 synthetic targets across image:
     * 1. Center target (stride 16, grid 20, 11)
     * 2. Top-left target (stride 8, grid 5, 5)
     * 3. Top-right target (stride 8, grid 70, 5)
     * 4. Large target (stride 32, grid 10, 5)
     * 5. Small target (stride 8, grid 40, 20)
     */
    struct GroundTruth {
        int s_idx;
        int gx, gy;
        float off_x, off_y;
        float w, h;
        float obj;
        int cls;
        float cls_score;
    } targets[5] = {
        { 1, 20, 11, 0.5f, 0.25f, 80.0f, 120.0f, 0.95f, 0, 0.98f },   /* Center */
        { 0, 5, 5, 0.1f, 0.2f, 32.0f, 48.0f, 0.90f, 1, 0.92f },       /* Top-Left */
        { 0, 70, 5, 0.3f, 0.4f, 40.0f, 40.0f, 0.88f, 2, 0.95f },      /* Top-Right */
        { 2, 10, 5, 0.5f, 0.5f, 200.0f, 160.0f, 0.92f, 0, 0.90f },    /* Large */
        { 0, 40, 20, 0.0f, 0.0f, 16.0f, 24.0f, 0.85f, 5, 0.99f }      /* Small */
    };

    const int strides[3] = { 8, 16, 32 };
    struct yolo_norm_box expected_boxes[5];

    for (int i = 0; i < 5; i++) {
        inject_target(targets[i].s_idx, targets[i].gx, targets[i].gy,
                      targets[i].off_x, targets[i].off_y,
                      targets[i].w, targets[i].h,
                      targets[i].obj, targets[i].cls, targets[i].cls_score);

        int stride = strides[targets[i].s_idx];
        float cx = (targets[i].off_x + (float)targets[i].gx) * (float)stride;
        float cy = (targets[i].off_y + (float)targets[i].gy) * (float)stride;
        float tx1 = cx - targets[i].w * 0.5f;
        float ty1 = cy - targets[i].h * 0.5f;
        float tx2 = cx + targets[i].w * 0.5f;
        float ty2 = cy + targets[i].h * 0.5f;

        expected_boxes[i].class_id = targets[i].cls;
        expected_boxes[i].score = targets[i].obj * targets[i].cls_score;
        LONGS_EQUAL(0, yolo_geometry_normalize_box(&geo, tx1, ty1, tx2, ty2, &expected_boxes[i]));
    }

    struct yolo_norm_box detected[10];
    int count = yolo_postprocess(tensor_buf, &geo, &post, detected, 10);
    LONGS_EQUAL(5, count);

    /* Verify each detected box matches an expected ground-truth with IoU >= 0.99 */
    for (int i = 0; i < 5; i++) {
        bool matched = false;
        for (int d = 0; d < count; d++) {
            if (detected[d].class_id == expected_boxes[i].class_id) {
                float iou = yolo_compute_iou(&detected[d], &expected_boxes[i]);
                if (iou >= 0.99f) {
                    matched = true;
                    DOUBLES_EQUAL((double)expected_boxes[i].score, (double)detected[d].score, 0.05);
                    break;
                }
            }
        }
        CHECK_TEXT(matched, "Expected synthetic target failed IoU >= 0.99 match");
    }
}

TEST(YoloPostprocess, MultiScaleAnchorStrides)
{
    /* Test anchor decoding at boundary anchors of each stride */
    /* Stride 8: first (0, 0) and last (79, 79) */
    inject_target(0, 0, 0, 0.5f, 0.5f, 16.0f, 16.0f, 0.9f, 0, 0.9f);
    inject_target(0, 79, 79, 0.5f, 0.5f, 16.0f, 16.0f, 0.9f, 0, 0.9f);

    /* Stride 16: first (0, 0) and last (39, 39) */
    inject_target(1, 0, 0, 0.5f, 0.5f, 32.0f, 32.0f, 0.9f, 1, 0.9f);
    inject_target(1, 39, 39, 0.5f, 0.5f, 32.0f, 32.0f, 0.9f, 1, 0.9f);

    /* Stride 32: first (0, 0) and last (19, 19) */
    inject_target(2, 0, 0, 0.5f, 0.5f, 64.0f, 64.0f, 0.9f, 2, 0.9f);
    inject_target(2, 19, 19, 0.5f, 0.5f, 64.0f, 64.0f, 0.9f, 2, 0.9f);

    struct yolo_norm_box detected[10];
    int count = yolo_postprocess(tensor_buf, &geo, &post, detected, 10);
    LONGS_EQUAL(6, count);
}

TEST(YoloPostprocess, GreedyNmsSuppression)
{
    /* Inject two heavily overlapping candidates of the same class (person, cls=0) */
    /* Higher score box (0.95 * 0.95 = 0.9025) */
    inject_target(1, 20, 10, 0.5f, 0.5f, 64.0f, 96.0f, 0.95f, 0, 0.95f);
    /* Lower score duplicate box (0.80 * 0.80 = 0.64) at adjacent grid with IoU > 0.95 */
    inject_target(1, 20, 11, 0.5f, -0.45f, 64.0f, 96.0f, 0.80f, 0, 0.80f);

    struct yolo_norm_box detected[10];
    int count = yolo_postprocess(tensor_buf, &geo, &post, detected, 10);

    /* Only the higher score box should survive NMS */
    LONGS_EQUAL(1, count);
    LONGS_EQUAL(0, detected[0].class_id);
    DOUBLES_EQUAL(0.9025, (double)detected[0].score, 0.01);
}

TEST(YoloPostprocess, BoundaryConfidenceThresholds)
{
    inject_target(1, 10, 10, 0.5f, 0.5f, 50.0f, 50.0f, 0.5f, 0, 0.5f); // score = 0.25

    struct yolo_norm_box detected[10];

    /* When conf_thresh = 0.0f, score 0.25 passes */
    post.conf_thresh = 0.0f;
    int count = yolo_postprocess(tensor_buf, &geo, &post, detected, 10);
    LONGS_EQUAL(1, count);

    /* When conf_thresh = 1.0f, score 0.25 is rejected */
    post.conf_thresh = 1.0f;
    count = yolo_postprocess(tensor_buf, &geo, &post, detected, 10);
    LONGS_EQUAL(0, count);
}

TEST(YoloPostprocess, MaxDetectionsCapping)
{
    /* Inject 20 distinct non-overlapping targets */
    for (int i = 0; i < 20; i++) {
        inject_target(0, i * 4, 10, 0.5f, 0.5f, 20.0f, 20.0f, 0.9f, 0, 0.9f);
    }

    struct yolo_norm_box detected[25];

    /* Cap at max_dets = 5 */
    post.max_dets = 5;
    int count = yolo_postprocess(tensor_buf, &geo, &post, detected, 25);
    LONGS_EQUAL(5, count);

    /* Cap at max_dets = 12 */
    post.max_dets = 12;
    count = yolo_postprocess(tensor_buf, &geo, &post, detected, 25);
    LONGS_EQUAL(12, count);
}

TEST(YoloPostprocess, FaultInjectionZeroScores)
{
    /* Entire tensor is zero (calloc'd) */
    struct yolo_norm_box detected[10];
    int count = yolo_postprocess(tensor_buf, &geo, &post, detected, 10);
    LONGS_EQUAL(0, count);
}

TEST(YoloPostprocess, FaultInjectionDegenerateDimensions)
{
    /* Inject raw_w and raw_h with extreme values (-100, +100) */
    uint16_t *row = (uint16_t *)(tensor_buf);
    row[0] = yolo_float_to_fp16(0.0f);
    row[1] = yolo_float_to_fp16(0.0f);
    row[2] = yolo_float_to_fp16(-100.0f);
    row[3] = yolo_float_to_fp16(100.0f);
    row[4] = yolo_float_to_fp16(0.95f);
    row[5] = yolo_float_to_fp16(0.95f);

    struct yolo_norm_box detected[10];
    int count = yolo_postprocess(tensor_buf, &geo, &post, detected, 10);
    CHECK(count >= 0);
}

TEST(YoloPostprocess, FaultInjectionNullPointers)
{
    struct yolo_norm_box detected[10];
    LONGS_EQUAL(-EINVAL, yolo_postprocess(nullptr, &geo, &post, detected, 10));
    LONGS_EQUAL(-EINVAL, yolo_postprocess(tensor_buf, nullptr, &post, detected, 10));
    LONGS_EQUAL(-EINVAL, yolo_postprocess(tensor_buf, &geo, nullptr, detected, 10));
    LONGS_EQUAL(-EINVAL, yolo_postprocess(tensor_buf, &geo, &post, nullptr, 10));
    LONGS_EQUAL(0, yolo_postprocess(tensor_buf, &geo, &post, detected, 0));
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
