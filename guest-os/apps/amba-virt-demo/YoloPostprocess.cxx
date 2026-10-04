/*
 * YoloPostprocess.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "YoloPostprocess.hxx"

#include <math.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

void yolo_postprocess_default_config(struct yolo_postprocess_config *cfg)
{
    if (!cfg) return;
    cfg->conf_thresh = 0.30f;
    cfg->nms_thresh = 0.45f;
    cfg->max_dets = 100;
    cfg->output_pitch = YOLO_OUTPUT_PITCH;
    cfg->num_classes = YOLO_NUM_CLASSES;
}

uint16_t yolo_float_to_fp16(float f)
{
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = ((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = (x >> 13) & 0x3ff;

    if (exp <= 0) {
        return (uint16_t)sign;
    }
    if (exp >= 31) {
        return (uint16_t)(sign | 0x7c00);
    }
    return (uint16_t)(sign | (exp << 10) | mant);
}

float yolo_fp16_to_float(uint16_t h)
{
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;

    if (exp == 0) {
        if (mant == 0) return sign ? -0.0f : 0.0f;
        while ((mant & 0x400) == 0) {
            mant <<= 1;
            exp--;
        }
        exp++;
        mant &= 0x3ff;
    } else if (exp == 31) {
        return (mant == 0) ? (sign ? -INFINITY : INFINITY) : NAN;
    }

    uint32_t f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    float res;
    memcpy(&res, &f, sizeof(res));
    return res;
}

float yolo_compute_iou(const struct yolo_norm_box *a, const struct yolo_norm_box *b)
{
    if (!a || !b) return 0.0f;

    float inter_x1 = fmaxf(a->x1, b->x1);
    float inter_y1 = fmaxf(a->y1, b->y1);
    float inter_x2 = fminf(a->x2, b->x2);
    float inter_y2 = fminf(a->y2, b->y2);

    float inter_w = fmaxf(0.0f, inter_x2 - inter_x1);
    float inter_h = fmaxf(0.0f, inter_y2 - inter_y1);
    float inter_area = inter_w * inter_h;

    float a_area = fmaxf(0.0f, a->x2 - a->x1) * fmaxf(0.0f, a->y2 - a->y1);
    float b_area = fmaxf(0.0f, b->x2 - b->x1) * fmaxf(0.0f, b->y2 - b->y1);
    float union_area = a_area + b_area - inter_area;

    if (union_area <= 0.0f)
        return 0.0f;

    return inter_area / union_area;
}

static int compare_norm_bboxes(const void *a, const void *b)
{
    const struct yolo_norm_box *box_a = (const struct yolo_norm_box *)a;
    const struct yolo_norm_box *box_b = (const struct yolo_norm_box *)b;
    if (box_b->score > box_a->score) return 1;
    if (box_b->score < box_a->score) return -1;
    return 0;
}

int yolo_postprocess(const void *out_tensor,
                     const struct yolo_geometry_config *geo,
                     const struct yolo_postprocess_config *post,
                     struct yolo_norm_box *out_dets,
                     int max_dets)
{
    if (!out_tensor || !geo || !post)
        return -EINVAL;
    if (max_dets > 0 && !out_dets)
        return -EINVAL;
    if (max_dets <= 0)
        return 0;

    uint32_t num_classes = post->num_classes > 0 ? post->num_classes : YOLO_NUM_CLASSES;
    uint32_t pitch = post->output_pitch > 0 ? post->output_pitch : YOLO_OUTPUT_PITCH;
    int max_to_return = (int)post->max_dets < max_dets ? (int)post->max_dets : max_dets;
    if (max_to_return <= 0)
        return 0;

    /* Stack-allocated candidate and suppression buffers: zero heap allocation */
    struct yolo_norm_box candidates[512];
    bool suppressed[512];
    const int max_cands = 512;
    int cand_count = 0;

    const int strides[3] = { 8, 16, 32 };
    int anchor_idx = 0;

    /* Pre-compute FP16 confidence threshold for single-cycle integer comparison */
    uint16_t conf_thresh_fp16 = post->conf_thresh > 0.0f ? yolo_float_to_fp16(post->conf_thresh) : 0;
    bool has_conf_filter = (post->conf_thresh > 0.0f);

    for (int s_idx = 0; s_idx < 3; s_idx++) {
        int stride = strides[s_idx];
        int grid_size = YOLO_INPUT_SIZE / stride;

        for (int gy = 0; gy < grid_size; gy++) {
            for (int gx = 0; gx < grid_size; gx++) {
                if (anchor_idx >= YOLO_NUM_ANCHORS)
                    break;

                const uint16_t *row = (const uint16_t *)(
                    (const uint8_t *)out_tensor + (anchor_idx * pitch));
                anchor_idx++;

                /*
                 * Early FP16 integer rejection: for positive IEEE-754 floats in [0, 1],
                 * bit representations are monotonic. If raw objectness is negative
                 * or below threshold, final_score = obj_score * cls_score <= obj_score
                 * can NEVER reach conf_thresh. This bypasses ~98% of transcendental ops.
                 */
                uint16_t raw_obj = row[4];
                if (has_conf_filter) {
                    if ((raw_obj & 0x8000) || raw_obj < conf_thresh_fp16)
                        continue;
                }

                float obj_score = yolo_fp16_to_float(raw_obj);
                if (isnan(obj_score) || isinf(obj_score) || obj_score < post->conf_thresh)
                    continue;

                int best_class = -1;
                float best_cls_score = 0.0f;

                for (uint32_t c = 0; c < num_classes; c++) {
                    uint16_t raw_cls = row[5 + c];
                    /* Discard negative or zero class scores */
                    if (raw_cls & 0x8000)
                        continue;

                    float cls_val = yolo_fp16_to_float(raw_cls);
                    if (isnan(cls_val) || isinf(cls_val))
                        continue;
                    if (cls_val > best_cls_score) {
                        best_cls_score = cls_val;
                        best_class = (int)c;
                    }
                }

                if (best_class < 0)
                    continue;

                float final_score = obj_score * best_cls_score;
                if (final_score >= post->conf_thresh && cand_count < max_cands) {
                    float raw_cx = yolo_fp16_to_float(row[0]);
                    float raw_cy = yolo_fp16_to_float(row[1]);
                    float raw_w  = yolo_fp16_to_float(row[2]);
                    float raw_h  = yolo_fp16_to_float(row[3]);

                    if (isnan(raw_cx) || isnan(raw_cy) || isnan(raw_w) || isnan(raw_h) ||
                        isinf(raw_cx) || isinf(raw_cy) || isinf(raw_w) || isinf(raw_h))
                        continue;

                    /* Clamp exponential arguments to prevent float overflow */
                    raw_w = fmaxf(-20.0f, fminf(raw_w, 20.0f));
                    raw_h = fmaxf(-20.0f, fminf(raw_h, 20.0f));

                    float cx = (raw_cx + (float)gx) * (float)stride;
                    float cy = (raw_cy + (float)gy) * (float)stride;
                    float w = expf(raw_w) * (float)stride;
                    float h = expf(raw_h) * (float)stride;

                    float tensor_x1 = cx - w * 0.5f;
                    float tensor_y1 = cy - h * 0.5f;
                    float tensor_x2 = cx + w * 0.5f;
                    float tensor_y2 = cy + h * 0.5f;

                    struct yolo_norm_box cand;
                    cand.score = final_score;
                    cand.class_id = best_class;

                    if (yolo_geometry_normalize_box(geo, tensor_x1, tensor_y1,
                                                    tensor_x2, tensor_y2, &cand) == 0) {
                        candidates[cand_count++] = cand;
                    }
                }
            }
        }
    }

    if (cand_count == 0)
        return 0;

    /* Sort candidates descending by confidence score */
    qsort(candidates, cand_count, sizeof(struct yolo_norm_box), compare_norm_bboxes);

    /* Zero suppression bitmask on stack */
    memset(suppressed, 0, cand_count * sizeof(bool));

    int det_count = 0;
    for (int i = 0; i < cand_count && det_count < max_to_return; i++) {
        if (suppressed[i])
            continue;

        out_dets[det_count++] = candidates[i];

        for (int j = i + 1; j < cand_count; j++) {
            if (suppressed[j])
                continue;

            if (candidates[i].class_id == candidates[j].class_id) {
                float iou = yolo_compute_iou(&candidates[i], &candidates[j]);
                if (iou >= post->nms_thresh) {
                    suppressed[j] = true;
                }
            }
        }
    }

    return det_count;
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
