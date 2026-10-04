/*
 * YoloGeometry.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_YOLO_GEOMETRY_HXX
#define AMBA_VIRT_YOLO_GEOMETRY_HXX

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yolo_geometry_config {
    uint32_t input_w;   /* Target tensor width (e.g. 640) */
    uint32_t input_h;   /* Target tensor height (e.g. 640) */
    uint32_t orig_w;    /* Source image width (e.g. 1920) */
    uint32_t orig_h;    /* Source image height (e.g. 1080) */
    float scale_r;      /* Scaling factor: min(input_w/orig_w, input_h/orig_h) */
    float pad_x;        /* Horizontal padding offset in tensor */
    float pad_y;        /* Vertical padding offset in tensor */
    bool is_centered;   /* Whether letterboxing is centered vs top/left aligned */
};

struct yolo_norm_box {
    float x1;           /* Normalized coordinate [0.0, 1.0] */
    float y1;           /* Normalized coordinate [0.0, 1.0] */
    float x2;           /* Normalized coordinate [0.0, 1.0] */
    float y2;           /* Normalized coordinate [0.0, 1.0] */
    float score;
    int32_t class_id;
};

/*
 * Initialize geometry config.
 * When centered is false: top/left aligned letterboxing (matches iav_proxy).
 * When centered is true: centered letterboxing.
 * Returns 0 on success, -EINVAL on zero or invalid dimensions.
 */
int yolo_geometry_init(struct yolo_geometry_config *cfg,
                       uint32_t input_w, uint32_t input_h,
                       uint32_t orig_w, uint32_t orig_h,
                       bool centered);

/*
 * Forward projection: (orig_x, orig_y) -> (tensor_x, tensor_y).
 * Clamps coordinates to source image bounds before scaling.
 * Returns 0 on success, -EINVAL on null ptrs, NaN/Inf, or uninitialized config.
 */
int yolo_geometry_forward(const struct yolo_geometry_config *cfg,
                          float orig_x, float orig_y,
                          float *tensor_x, float *tensor_y);

/*
 * Inverse projection: (tensor_x, tensor_y) -> (norm_x, norm_y) in [0.0, 1.0].
 * Reverses padding and scaling, clamps to [0, orig] then divides by orig dimensions.
 * Returns 0 on success, -EINVAL on null ptrs, NaN/Inf, or uninitialized config.
 */
int yolo_geometry_inverse(const struct yolo_geometry_config *cfg,
                          float tensor_x, float tensor_y,
                          float *norm_x, float *norm_y);

/*
 * Canvas projection: (norm_x, norm_y) -> (canvas_x, canvas_y).
 * Scales normalized [0.0, 1.0] coordinate to arbitrary canvas dimensions.
 * Returns 0 on success, -EINVAL on null ptrs, zero dimensions, or NaN/Inf.
 */
int yolo_geometry_canvas_project(float norm_x, float norm_y,
                                 uint32_t canvas_w, uint32_t canvas_h,
                                 float *canvas_x, float *canvas_y);

/*
 * Normalizes tensor bounding box into [0.0, 1.0] space.
 * Detects inverted coordinates (x1 > x2 or y1 > y2) and swaps them.
 * Clamps result strictly to [0.0, 1.0].
 * Returns 0 on success, -EINVAL on null ptrs, NaN/Inf, or invalid config.
 */
int yolo_geometry_normalize_box(const struct yolo_geometry_config *cfg,
                                float tensor_x1, float tensor_y1,
                                float tensor_x2, float tensor_y2,
                                struct yolo_norm_box *out_box);

#ifdef __cplusplus
}
#endif

#endif /* AMBA_VIRT_YOLO_GEOMETRY_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
