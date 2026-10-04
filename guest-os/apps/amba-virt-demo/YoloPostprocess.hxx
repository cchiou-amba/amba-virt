/*
 * YoloPostprocess.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_YOLO_POSTPROCESS_HXX
#define AMBA_VIRT_YOLO_POSTPROCESS_HXX

#include "YoloGeometry.hxx"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YOLO_INPUT_SIZE       640
#define YOLO_NUM_ANCHORS      8400
#define YOLO_NUM_CLASSES      80
#define YOLO_OUTPUT_PITCH     256
#define YOLO_MAX_CANDIDATES   2048

struct yolo_postprocess_config {
    float conf_thresh;       /* Confidence threshold (e.g. 0.30f) */
    float nms_thresh;        /* IoU threshold for NMS (e.g. 0.45f) */
    uint32_t max_dets;       /* Max output detections (e.g. 100) */
    uint32_t output_pitch;   /* Row pitch in bytes (default 256) */
    uint32_t num_classes;    /* Number of classes (default 80) */
};

/* Populate config with production defaults */
void yolo_postprocess_default_config(struct yolo_postprocess_config *cfg);

/* Compute Intersection over Union (IoU) of two normalized bounding boxes */
float yolo_compute_iou(const struct yolo_norm_box *a, const struct yolo_norm_box *b);

/*
 * Decode YOLOX output tensor (FP16 elements) across strides (8, 16, 32),
 * apply score filtering and greedy Non-Maximum Suppression (NMS),
 * and output normalized bounding boxes in [0.0, 1.0] coordinate space.
 *
 * Returns number of detections on success (>= 0), or negative errno on error (-EINVAL).
 */
int yolo_postprocess(const void *out_tensor,
                     const struct yolo_geometry_config *geo,
                     const struct yolo_postprocess_config *post,
                     struct yolo_norm_box *out_dets,
                     int max_dets);

/* Helper to convert float to FP16 (IEEE-754 binary16) */
uint16_t yolo_float_to_fp16(float f);

/* Helper to convert FP16 to float */
float yolo_fp16_to_float(uint16_t h);

#ifdef __cplusplus
}
#endif

#endif /* AMBA_VIRT_YOLO_POSTPROCESS_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
