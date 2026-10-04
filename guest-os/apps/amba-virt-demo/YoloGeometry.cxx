/*
 * YoloGeometry.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "YoloGeometry.hxx"

#include <math.h>
#include <errno.h>
#include <string.h>

int yolo_geometry_init(struct yolo_geometry_config *cfg,
                       uint32_t input_w, uint32_t input_h,
                       uint32_t orig_w, uint32_t orig_h,
                       bool centered)
{
    if (!cfg)
        return -EINVAL;
    if (input_w == 0 || input_h == 0 || orig_w == 0 || orig_h == 0)
        return -EINVAL;

    cfg->input_w = input_w;
    cfg->input_h = input_h;
    cfg->orig_w = orig_w;
    cfg->orig_h = orig_h;
    cfg->is_centered = centered;

    float r_w = (float)input_w / (float)orig_w;
    float r_h = (float)input_h / (float)orig_h;
    cfg->scale_r = fminf(r_w, r_h);

    if (cfg->scale_r <= 0.0f || isnan(cfg->scale_r) || isinf(cfg->scale_r))
        return -EINVAL;

    float scaled_w = roundf((float)orig_w * cfg->scale_r);
    float scaled_h = roundf((float)orig_h * cfg->scale_r);

    if (centered) {
        cfg->pad_x = ((float)input_w - scaled_w) * 0.5f;
        cfg->pad_y = ((float)input_h - scaled_h) * 0.5f;
    } else {
        cfg->pad_x = 0.0f;
        cfg->pad_y = 0.0f;
    }

    return 0;
}

int yolo_geometry_forward(const struct yolo_geometry_config *cfg,
                          float orig_x, float orig_y,
                          float *tensor_x, float *tensor_y)
{
    if (!cfg || !tensor_x || !tensor_y)
        return -EINVAL;
    if (isnan(orig_x) || isnan(orig_y) || isinf(orig_x) || isinf(orig_y))
        return -EINVAL;
    if (cfg->orig_w == 0 || cfg->orig_h == 0 || cfg->scale_r <= 0.0f)
        return -EINVAL;

    /* Clamp coordinates to original image bounds */
    float cx = fmaxf(0.0f, fminf(orig_x, (float)cfg->orig_w));
    float cy = fmaxf(0.0f, fminf(orig_y, (float)cfg->orig_h));

    float tx = cx * cfg->scale_r + cfg->pad_x;
    float ty = cy * cfg->scale_r + cfg->pad_y;

    /* Clamp to tensor bounds */
    *tensor_x = fmaxf(0.0f, fminf(tx, (float)cfg->input_w));
    *tensor_y = fmaxf(0.0f, fminf(ty, (float)cfg->input_h));

    return 0;
}

int yolo_geometry_inverse(const struct yolo_geometry_config *cfg,
                          float tensor_x, float tensor_y,
                          float *norm_x, float *norm_y)
{
    if (!cfg || !norm_x || !norm_y)
        return -EINVAL;
    if (isnan(tensor_x) || isnan(tensor_y) || isinf(tensor_x) || isinf(tensor_y))
        return -EINVAL;
    if (cfg->orig_w == 0 || cfg->orig_h == 0 || cfg->scale_r <= 0.0f)
        return -EINVAL;

    /* Remove padding and scale */
    float ox = (tensor_x - cfg->pad_x) / cfg->scale_r;
    float oy = (tensor_y - cfg->pad_y) / cfg->scale_r;

    /* Clamp to original image bounds */
    ox = fmaxf(0.0f, fminf(ox, (float)cfg->orig_w));
    oy = fmaxf(0.0f, fminf(oy, (float)cfg->orig_h));

    /* Normalize to [0.0, 1.0] */
    *norm_x = ox / (float)cfg->orig_w;
    *norm_y = oy / (float)cfg->orig_h;

    /* Final clamp to strict [0.0, 1.0] */
    *norm_x = fmaxf(0.0f, fminf(*norm_x, 1.0f));
    *norm_y = fmaxf(0.0f, fminf(*norm_y, 1.0f));

    return 0;
}

int yolo_geometry_canvas_project(float norm_x, float norm_y,
                                 uint32_t canvas_w, uint32_t canvas_h,
                                 float *canvas_x, float *canvas_y)
{
    if (!canvas_x || !canvas_y)
        return -EINVAL;
    if (canvas_w == 0 || canvas_h == 0)
        return -EINVAL;
    if (isnan(norm_x) || isnan(norm_y) || isinf(norm_x) || isinf(norm_y))
        return -EINVAL;

    float nx = fmaxf(0.0f, fminf(norm_x, 1.0f));
    float ny = fmaxf(0.0f, fminf(norm_y, 1.0f));

    *canvas_x = nx * (float)canvas_w;
    *canvas_y = ny * (float)canvas_h;

    return 0;
}

int yolo_geometry_normalize_box(const struct yolo_geometry_config *cfg,
                                float tensor_x1, float tensor_y1,
                                float tensor_x2, float tensor_y2,
                                struct yolo_norm_box *out_box)
{
    if (!cfg || !out_box)
        return -EINVAL;
    if (isnan(tensor_x1) || isnan(tensor_y1) || isnan(tensor_x2) || isnan(tensor_y2) ||
        isinf(tensor_x1) || isinf(tensor_y1) || isinf(tensor_x2) || isinf(tensor_y2))
        return -EINVAL;

    /* Handle inverted boxes */
    float tx1 = tensor_x1;
    float tx2 = tensor_x2;
    float ty1 = tensor_y1;
    float ty2 = tensor_y2;

    if (tx1 > tx2) {
        float tmp = tx1;
        tx1 = tx2;
        tx2 = tmp;
    }
    if (ty1 > ty2) {
        float tmp = ty1;
        ty1 = ty2;
        ty2 = tmp;
    }

    int rc = yolo_geometry_inverse(cfg, tx1, ty1, &out_box->x1, &out_box->y1);
    if (rc < 0)
        return rc;

    rc = yolo_geometry_inverse(cfg, tx2, ty2, &out_box->x2, &out_box->y2);
    if (rc < 0)
        return rc;

    return 0;
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
