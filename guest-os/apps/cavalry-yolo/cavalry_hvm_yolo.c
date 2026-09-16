/*
 * cavalry_hvm_yolo.c
 *
 * Ambarella Cavalry HVM YOLOX Detection & Dynamic Web Service
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/utsname.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <cavalry_ioctl.h>
#include "cavalry_ioctl_path_b.h"
#include <cavalry_mem.h>
#include <nnctrl.h>
#include "nnctrl_priv.h"

struct net_desc *get_net_desc(struct nnctrl_info *pctl, int net_id);

#define DEFAULT_MODEL_PATH "/opt/cavalry/n1-655_yolox_s_amba_optimized.bin"
#define DEFAULT_HTTP_BIND  "0.0.0.0:8080"
#define YOLO_INPUT_SIZE    640
#define YOLO_NUM_CLASSES   80
#define YOLO_NUM_ANCHORS   8400
#define YOLO_OUTPUT_PITCH  256
#define MAX_CANDIDATES     8400
#define MAX_DETECTIONS     256
#define MAX_PAYLOAD_SIZE   (25 * 1024 * 1024)

static const char *COCO_CLASSES[YOLO_NUM_CLASSES] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

/* 10 distinct vibrant colors for bounding boxes: RGB */
static const uint8_t CLASS_COLORS[10][3] = {
    {  59, 130, 246 }, /* Blue */
    {  16, 185, 129 }, /* Emerald */
    { 245, 158,  11 }, /* Amber */
    { 239,  68,  68 }, /* Red */
    { 168,  85, 247 }, /* Purple */
    { 236,  72, 153 }, /* Pink */
    {   6, 182, 212 }, /* Cyan */
    { 132, 204,  22 }, /* Lime */
    { 249, 115,  22 }, /* Orange */
    { 147,  51, 234 }  /* Violet */
};

/* 5x7 standard ASCII bitmap font (ASCII 32 to 126) */
static const uint8_t FONT5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x00, 0x00, 0x5f, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7f, 0x14, 0x7f, 0x14}, /* # */
    {0x24, 0x2a, 0x7f, 0x2a, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1c, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1c, 0x00}, /* ) */
    {0x14, 0x08, 0x3e, 0x08, 0x14}, /* * */
    {0x08, 0x08, 0x3e, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, /* 0 */
    {0x00, 0x42, 0x7f, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4b, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7f, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1e}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3e}, /* @ */
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, /* A */
    {0x7f, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3e, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, /* D */
    {0x7f, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7f, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, /* G */
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, /* H */
    {0x00, 0x41, 0x7f, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3f, 0x01}, /* J */
    {0x7f, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7f, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, /* M */
    {0x7f, 0x04, 0x08, 0x10, 0x7f}, /* N */
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, /* O */
    {0x7f, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, /* Q */
    {0x7f, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7f, 0x01, 0x01}, /* T */
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, /* U */
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, /* V */
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x7f, 0x41, 0x41, 0x00}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* \ */
    {0x00, 0x41, 0x41, 0x7f, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* ` */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* a */
    {0x7f, 0x48, 0x44, 0x44, 0x38}, /* b */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* c */
    {0x38, 0x44, 0x44, 0x48, 0x7f}, /* d */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* e */
    {0x08, 0x7e, 0x09, 0x01, 0x02}, /* f */
    {0x0c, 0x52, 0x52, 0x52, 0x3e}, /* g */
    {0x7f, 0x08, 0x04, 0x04, 0x78}, /* h */
    {0x00, 0x44, 0x7d, 0x40, 0x00}, /* i */
    {0x20, 0x40, 0x44, 0x3d, 0x00}, /* j */
    {0x7f, 0x10, 0x28, 0x44, 0x00}, /* k */
    {0x00, 0x41, 0x7f, 0x40, 0x00}, /* l */
    {0x7c, 0x04, 0x18, 0x04, 0x78}, /* m */
    {0x7c, 0x08, 0x04, 0x04, 0x78}, /* n */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* o */
    {0x7c, 0x14, 0x14, 0x14, 0x08}, /* p */
    {0x08, 0x14, 0x14, 0x18, 0x7c}, /* q */
    {0x7c, 0x08, 0x04, 0x04, 0x08}, /* r */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* s */
    {0x04, 0x3f, 0x44, 0x40, 0x20}, /* t */
    {0x3c, 0x40, 0x40, 0x20, 0x7c}, /* u */
    {0x1c, 0x20, 0x40, 0x20, 0x1c}, /* v */
    {0x3c, 0x40, 0x30, 0x40, 0x3c}, /* w */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* x */
    {0x0c, 0x50, 0x50, 0x50, 0x3c}, /* y */
    {0x44, 0x64, 0x54, 0x4c, 0x44}, /* z */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* { */
    {0x00, 0x00, 0x7f, 0x00, 0x00}, /* | */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* } */
    {0x08, 0x08, 0x2a, 0x1c, 0x08}  /* ~ */
};

struct bbox {
    float x1, y1, x2, y2;
    float score;
    int class_id;
};

struct yolo_runtime {
    int fd_cav;
    int net_id;
    struct net_cfg net_cf;
    struct net_mem net_m;
    struct net_input_cfg net_in;
    struct net_output_cfg net_out;
    struct cavalry_run_dags *run_dags;
    pthread_mutex_t lock;
    char os_pretty_name[256];
    char os_kernel[128];
    char os_machine[64];
    char os_sysname[64];
    uint32_t total_inferences;
};

static struct yolo_runtime g_yolo;

/* Helper: high-resolution timer */
static inline double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Parse /etc/os-release for PRETTY_NAME */
static void get_os_info(struct yolo_runtime *rt)
{
    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(rt->os_sysname, sizeof(rt->os_sysname), "%s", uts.sysname);
        snprintf(rt->os_kernel, sizeof(rt->os_kernel), "%s", uts.release);
        snprintf(rt->os_machine, sizeof(rt->os_machine), "%s", uts.machine);
    } else {
        snprintf(rt->os_sysname, sizeof(rt->os_sysname), "Linux");
        snprintf(rt->os_kernel, sizeof(rt->os_kernel), "unknown");
        snprintf(rt->os_machine, sizeof(rt->os_machine), "aarch64");
    }

    snprintf(rt->os_pretty_name, sizeof(rt->os_pretty_name), "%.60s %.60s (%.30s)",
             rt->os_sysname, rt->os_kernel, rt->os_machine);

    FILE *fp = fopen("/etc/os-release", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                char *val = line + 12;
                if (*val == '"') val++;
                char *end = val + strlen(val) - 1;
                while (end > val && (*end == '\n' || *end == '\r' || *end == '"')) {
                    *end = '\0';
                    end--;
                }
                snprintf(rt->os_pretty_name, sizeof(rt->os_pretty_name), "%s", val);
                break;
            }
        }
        fclose(fp);
    }
}

/* Base64 Encoding */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t input_length, size_t *output_length)
{
    size_t out_len = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(out_len + 1);
    if (!encoded_data) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = b64_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = (i > input_length + 1) ? '=' : b64_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = (i > input_length) ? '=' : b64_table[(triple >> 0 * 6) & 0x3F];
    }

    encoded_data[out_len] = '\0';
    if (output_length) *output_length = out_len;
    return encoded_data;
}

/* Stb image write memory buffer callback */
struct mem_write_ctx {
    uint8_t *data;
    size_t size;
    size_t cap;
};

static void stbi_write_mem_cb(void *context, void *data, int size)
{
    struct mem_write_ctx *ctx = (struct mem_write_ctx *)context;
    if (ctx->size + size > ctx->cap) {
        size_t new_cap = (ctx->cap == 0) ? 65536 : ctx->cap * 2;
        while (new_cap < ctx->size + size) new_cap *= 2;
        uint8_t *tmp = realloc(ctx->data, new_cap);
        if (!tmp) return;
        ctx->data = tmp;
        ctx->cap = new_cap;
    }
    memcpy(ctx->data + ctx->size, data, size);
    ctx->size += size;
}

/* Letterbox: bilinear downscale into top-left, pad right and bottom with 114 */
static float letterbox_preprocess(const uint8_t *orig_rgb, int orig_w, int orig_h,
                                  uint8_t *tensor_in)
{
    float r = fminf((float)YOLO_INPUT_SIZE / (float)orig_w,
                    (float)YOLO_INPUT_SIZE / (float)orig_h);
    int new_w = (int)roundf(orig_w * r);
    int new_h = (int)roundf(orig_h * r);
    if (new_w > YOLO_INPUT_SIZE) new_w = YOLO_INPUT_SIZE;
    if (new_h > YOLO_INPUT_SIZE) new_h = YOLO_INPUT_SIZE;

    /* Initialize target 3 planar buffers (NCHW) to pad value 114 */
    uint8_t *r_plane = tensor_in;
    uint8_t *g_plane = tensor_in + (YOLO_INPUT_SIZE * YOLO_INPUT_SIZE);
    uint8_t *b_plane = tensor_in + (2 * YOLO_INPUT_SIZE * YOLO_INPUT_SIZE);

    memset(tensor_in, 114, 3 * YOLO_INPUT_SIZE * YOLO_INPUT_SIZE);

    for (int dy = 0; dy < new_h; dy++) {
        float sy = (float)dy / r;
        int iy = (int)sy;
        if (iy >= orig_h - 1) iy = orig_h - 2;
        if (iy < 0) iy = 0;
        float fy = sy - iy;

        for (int dx = 0; dx < new_w; dx++) {
            float sx = (float)dx / r;
            int ix = (int)sx;
            if (ix >= orig_w - 1) ix = orig_w - 2;
            if (ix < 0) ix = 0;
            float fx = sx - ix;

            const uint8_t *p00 = &orig_rgb[(iy * orig_w + ix) * 3];
            const uint8_t *p01 = &orig_rgb[(iy * orig_w + (ix + 1)) * 3];
            const uint8_t *p10 = &orig_rgb[((iy + 1) * orig_w + ix) * 3];
            const uint8_t *p11 = &orig_rgb[((iy + 1) * orig_w + (ix + 1)) * 3];

            float w00 = (1.0f - fx) * (1.0f - fy);
            float w01 = fx * (1.0f - fy);
            float w10 = (1.0f - fx) * fy;
            float w11 = fx * fy;

            float r_val = w00 * p00[0] + w01 * p01[0] + w10 * p10[0] + w11 * p11[0];
            float g_val = w00 * p00[1] + w01 * p01[1] + w10 * p10[1] + w11 * p11[1];
            float b_val = w00 * p00[2] + w01 * p01[2] + w10 * p10[2] + w11 * p11[2];

            int out_idx = dy * YOLO_INPUT_SIZE + dx;
            r_plane[out_idx] = (uint8_t)fminf(fmaxf(r_val + 0.5f, 0.0f), 255.0f);
            g_plane[out_idx] = (uint8_t)fminf(fmaxf(g_val + 0.5f, 0.0f), 255.0f);
            b_plane[out_idx] = (uint8_t)fminf(fmaxf(b_val + 0.5f, 0.0f), 255.0f);
        }
    }

    return r;
}

/* Intersection over Union */
static inline float compute_iou(const struct bbox *a, const struct bbox *b)
{
    float x1 = fmaxf(a->x1, b->x1);
    float y1 = fmaxf(a->y1, b->y1);
    float x2 = fminf(a->x2, b->x2);
    float y2 = fminf(a->y2, b->y2);

    float inter_w = fmaxf(0.0f, x2 - x1);
    float inter_h = fmaxf(0.0f, y2 - y1);
    float inter_area = inter_w * inter_h;

    float area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    float area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
    float union_area = area_a + area_b - inter_area;

    if (union_area <= 0.0f) return 0.0f;
    return inter_area / union_area;
}

/* Bounding box comparator for qsort (score descending) */
static int compare_bboxes(const void *a, const void *b)
{
    const struct bbox *ba = (const struct bbox *)a;
    const struct bbox *bb = (const struct bbox *)b;
    if (ba->score < bb->score) return 1;
    if (ba->score > bb->score) return -1;
    return 0;
}

/* Sigmoid activation helper */
static inline float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

/* YOLOX Anchor Decode + NMS */
static int yolox_postprocess(const void *out_tensor, float scale_r,
                             int orig_w, int orig_h,
                             float conf_thresh, float nms_thresh,
                             struct bbox *out_dets, int max_dets)
{
    static struct bbox candidates[MAX_CANDIDATES];
    int cand_count = 0;

    const int strides[3] = { 8, 16, 32 };
    int anchor_idx = 0;
    float max_obj = 0.0f;
    int max_obj_anchor = -1;

    for (int s_idx = 0; s_idx < 3; s_idx++) {
        int stride = strides[s_idx];
        int grid_size = YOLO_INPUT_SIZE / stride;

        for (int gy = 0; gy < grid_size; gy++) {
            for (int gx = 0; gx < grid_size; gx++) {
                if (anchor_idx >= YOLO_NUM_ANCHORS) break;

                /* Each anchor row is stepped by YOLO_OUTPUT_PITCH bytes (256 bytes) */
                const __fp16 *row = (const __fp16 *)((const uint8_t *)out_tensor + anchor_idx * YOLO_OUTPUT_PITCH);
                int cur_anchor = anchor_idx++;

                float obj_score = (float)row[4];
                if (obj_score > max_obj) {
                    max_obj = obj_score;
                    max_obj_anchor = cur_anchor;
                }

                if (obj_score < conf_thresh * 0.5f)
                    continue;

                /* Find best class */
                int best_class = -1;
                float best_cls_score = 0.0f;

                for (int c = 0; c < YOLO_NUM_CLASSES; c++) {
                    float cls_val = (float)row[5 + c];
                    if (cls_val > best_cls_score) {
                        best_cls_score = cls_val;
                        best_class = c;
                    }
                }

                float final_score = obj_score * best_cls_score;
                if (final_score >= conf_thresh && cand_count < MAX_CANDIDATES) {
                    /* YOLOX box decoding */
                    float raw_cx = (float)row[0];
                    float raw_cy = (float)row[1];
                    float raw_w  = (float)row[2];
                    float raw_h  = (float)row[3];

                    float cx = (raw_cx + (float)gx) * (float)stride;
                    float cy = (raw_cy + (float)gy) * (float)stride;
                    float w = expf(raw_w) * (float)stride;
                    float h = expf(raw_h) * (float)stride;

                    float x1 = (cx - w * 0.5f) / scale_r;
                    float y1 = (cy - h * 0.5f) / scale_r;
                    float x2 = (cx + w * 0.5f) / scale_r;
                    float y2 = (cy + h * 0.5f) / scale_r;

                    /* Clamp to original image bounds */
                    candidates[cand_count].x1 = fmaxf(0.0f, fminf(x1, (float)orig_w));
                    candidates[cand_count].y1 = fmaxf(0.0f, fminf(y1, (float)orig_h));
                    candidates[cand_count].x2 = fmaxf(0.0f, fminf(x2, (float)orig_w));
                    candidates[cand_count].y2 = fmaxf(0.0f, fminf(y2, (float)orig_h));
                    candidates[cand_count].score = final_score;
                    candidates[cand_count].class_id = best_class;
                    cand_count++;
                }
            }
        }
    }

    printf("[*] Anchor search: scanned %d anchors, max obj_score=%.4f (anchor %d), candidates=%d\n",
           anchor_idx, max_obj, max_obj_anchor, cand_count);

    if (cand_count == 0) return 0;

    /* Sort candidates descending */
    qsort(candidates, cand_count, sizeof(struct bbox), compare_bboxes);

    /* Greedy NMS */
    int det_count = 0;
    bool *suppressed = calloc(cand_count, sizeof(bool));
    if (!suppressed) return 0;

    for (int i = 0; i < cand_count && det_count < max_dets; i++) {
        if (suppressed[i]) continue;

        out_dets[det_count++] = candidates[i];

        for (int j = i + 1; j < cand_count; j++) {
            if (suppressed[j]) continue;
            if (candidates[i].class_id == candidates[j].class_id) {
                if (compute_iou(&candidates[i], &candidates[j]) >= nms_thresh) {
                    suppressed[j] = true;
                }
            }
        }
    }

    free(suppressed);
    return det_count;
}

/* Draw a single character using the embedded 5x7 font */
static void draw_char(uint8_t *img, int w, int h, int x, int y, char c,
                      const uint8_t color[3], int scale)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *bits = FONT5x7[c - 32];

    for (int col = 0; col < 5; col++) {
        uint8_t col_bits = bits[col];
        for (int row = 0; row < 7; row++) {
            if ((col_bits >> row) & 1) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < w && py >= 0 && py < h) {
                            int idx = (py * w + px) * 3;
                            img[idx + 0] = color[0];
                            img[idx + 1] = color[1];
                            img[idx + 2] = color[2];
                        }
                    }
                }
            }
        }
    }
}

/* Draw a text string */
static void draw_text(uint8_t *img, int w, int h, int x, int y, const char *str,
                      const uint8_t text_color[3], const uint8_t bg_color[3], int scale)
{
    int len = strlen(str);
    int text_w = len * (5 * scale + scale);
    int text_h = 7 * scale + 2;

    /* Draw background box */
    for (int by = y - 1; by < y + text_h; by++) {
        for (int bx = x - 2; bx < x + text_w + 2; bx++) {
            if (bx >= 0 && bx < w && by >= 0 && by < h) {
                int idx = (by * w + bx) * 3;
                img[idx + 0] = bg_color[0];
                img[idx + 1] = bg_color[1];
                img[idx + 2] = bg_color[2];
            }
        }
    }

    /* Draw glyphs */
    int cur_x = x;
    for (int i = 0; i < len; i++) {
        draw_char(img, w, h, cur_x, y, str[i], text_color, scale);
        cur_x += (5 * scale + scale);
    }
}

/* Draw bounding boxes and class badges directly onto original RGB image */
static void draw_detections(uint8_t *img, int w, int h, const struct bbox *dets, int det_count)
{
    for (int i = 0; i < det_count; i++) {
        const struct bbox *d = &dets[i];
        int x1 = (int)roundf(d->x1);
        int y1 = (int)roundf(d->y1);
        int x2 = (int)roundf(d->x2);
        int y2 = (int)roundf(d->y2);

        const uint8_t *color = CLASS_COLORS[d->class_id % 10];
        const uint8_t white[3] = { 255, 255, 255 };

        /* Border thickness: 2 to 3 px depending on resolution */
        int thick = (w > 1200 || h > 1200) ? 3 : 2;

        /* Top & Bottom horizontal lines */
        for (int t = 0; t < thick; t++) {
            int py_top = y1 + t;
            int py_bot = y2 - t;
            for (int px = x1; px <= x2; px++) {
                if (px >= 0 && px < w) {
                    if (py_top >= 0 && py_top < h) {
                        int idx = (py_top * w + px) * 3;
                        img[idx + 0] = color[0];
                        img[idx + 1] = color[1];
                        img[idx + 2] = color[2];
                    }
                    if (py_bot >= 0 && py_bot < h) {
                        int idx = (py_bot * w + px) * 3;
                        img[idx + 0] = color[0];
                        img[idx + 1] = color[1];
                        img[idx + 2] = color[2];
                    }
                }
            }
        }

        /* Left & Right vertical lines */
        for (int t = 0; t < thick; t++) {
            int px_left = x1 + t;
            int px_right = x2 - t;
            for (int py = y1; py <= y2; py++) {
                if (py >= 0 && py < h) {
                    if (px_left >= 0 && px_left < w) {
                        int idx = (py * w + px_left) * 3;
                        img[idx + 0] = color[0];
                        img[idx + 1] = color[1];
                        img[idx + 2] = color[2];
                    }
                    if (px_right >= 0 && px_right < w) {
                        int idx = (py * w + px_right) * 3;
                        img[idx + 0] = color[0];
                        img[idx + 1] = color[1];
                        img[idx + 2] = color[2];
                    }
                }
            }
        }

        /* Format text badge */
        char label[64];
        const char *cls_name = (d->class_id >= 0 && d->class_id < YOLO_NUM_CLASSES) ?
            COCO_CLASSES[d->class_id] : "object";
        snprintf(label, sizeof(label), "%s %d%%", cls_name, (int)(d->score * 100.0f + 0.5f));

        int font_scale = (w > 1200 || h > 1200) ? 2 : 1;
        int text_y = (y1 > 18) ? (y1 - 10 * font_scale) : (y1 + 4);
        draw_text(img, w, h, x1 + 2, text_y, label, white, color, font_scale);
    }
}

/* Embedded HTML Web UI string */
static const char *HTML_TEMPLATE =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"  <meta charset=\"UTF-8\">\n"
"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"  <title>Ambarella Edge AI &mdash; Dual-HVM YOLOX Detection</title>\n"
"  <link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n"
"  <link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>\n"
"  <link href=\"https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;500;600&display=swap\" rel=\"stylesheet\">\n"
"  <style>\n"
"    :root {\n"
"      --bg-base: #0a0d14;\n"
"      --bg-surface: #111726;\n"
"      --bg-card: rgba(19, 27, 46, 0.7);\n"
"      --border: rgba(255, 255, 255, 0.08);\n"
"      --accent: #00e5ff;\n"
"      --accent-glow: rgba(0, 229, 255, 0.25);\n"
"      --emerald: #10b981;\n"
"      --amber: #f59e0b;\n"
"      --text-main: #f8fafc;\n"
"      --text-sub: #94a3b8;\n"
"    }\n"
"    * { box-sizing: border-box; margin: 0; padding: 0; }\n"
"    body {\n"
"      background: var(--bg-base);\n"
"      color: var(--text-main);\n"
"      font-family: 'Outfit', -apple-system, sans-serif;\n"
"      min-height: 100vh;\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      background-image: radial-gradient(circle at 50% 0%, rgba(0, 229, 255, 0.06), transparent 50%),\n"
"                        radial-gradient(circle at 100% 100%, rgba(16, 185, 129, 0.04), transparent 40%);\n"
"    }\n"
"    header {\n"
"      background: rgba(17, 23, 38, 0.85);\n"
"      backdrop-filter: blur(12px);\n"
"      border-bottom: 1px solid var(--border);\n"
"      padding: 1rem 2rem;\n"
"      display: flex;\n"
"      justify-content: space-between;\n"
"      align-items: center;\n"
"      position: sticky;\n"
"      top: 0;\n"
"      z-index: 100;\n"
"    }\n"
"    .logo-group { display: flex; align-items: center; gap: 0.8rem; }\n"
"    .brand { font-size: 1.25rem; font-weight: 700; letter-spacing: -0.02em; color: #fff; }\n"
"    .brand span { color: var(--accent); }\n"
"    .os-badge {\n"
"      display: inline-flex;\n"
"      align-items: center;\n"
"      gap: 0.5rem;\n"
"      padding: 0.35rem 0.85rem;\n"
"      border-radius: 9999px;\n"
"      font-size: 0.82rem;\n"
"      font-weight: 600;\n"
"      background: rgba(16, 185, 129, 0.12);\n"
"      border: 1px solid rgba(16, 185, 129, 0.35);\n"
"      color: #34d399;\n"
"    }\n"
"    .pulse-dot {\n"
"      width: 8px; height: 8px; border-radius: 50%;\n"
"      background: var(--emerald);\n"
"      box-shadow: 0 0 10px var(--emerald);\n"
"      animation: pulse 2s infinite;\n"
"    }\n"
"    @keyframes pulse { 0%, 100% { opacity: 1; transform: scale(1); } 50% { opacity: 0.4; transform: scale(0.85); } }\n"
"    main {\n"
"      flex: 1;\n"
"      max-width: 1400px;\n"
"      margin: 0 auto;\n"
"      width: 100%;\n"
"      padding: 2rem;\n"
"      display: grid;\n"
"      grid-template-columns: 340px 1fr;\n"
"      gap: 1.5rem;\n"
"    }\n"
"    .card {\n"
"      background: var(--bg-card);\n"
"      backdrop-filter: blur(10px);\n"
"      border: 1px solid var(--border);\n"
"      border-radius: 1rem;\n"
"      padding: 1.5rem;\n"
"      box-shadow: 0 10px 30px rgba(0,0,0,0.3);\n"
"    }\n"
"    h2 { font-size: 1.1rem; font-weight: 600; margin-bottom: 1rem; color: #fff; display: flex; align-items: center; gap: 0.5rem; }\n"
"    .drop-zone {\n"
"      border: 2px dashed rgba(255,255,255,0.15);\n"
"      border-radius: 0.75rem;\n"
"      padding: 2rem 1rem;\n"
"      text-align: center;\n"
"      cursor: pointer;\n"
"      transition: all 0.2s ease;\n"
"      background: rgba(255,255,255,0.02);\n"
"    }\n"
"    .drop-zone:hover, .drop-zone.dragover {\n"
"      border-color: var(--accent);\n"
"      background: rgba(0, 229, 255, 0.04);\n"
"    }\n"
"    .drop-zone svg { width: 36px; height: 36px; fill: var(--accent); margin-bottom: 0.5rem; }\n"
"    .btn {\n"
"      width: 100%;\n"
"      padding: 0.75rem;\n"
"      border-radius: 0.6rem;\n"
"      border: none;\n"
"      font-weight: 600;\n"
"      font-family: inherit;\n"
"      font-size: 0.95rem;\n"
"      cursor: pointer;\n"
"      transition: all 0.2s;\n"
"      display: flex;\n"
"      align-items: center;\n"
"      justify-content: center;\n"
"      gap: 0.5rem;\n"
"    }\n"
"    .btn-primary {\n"
"      background: linear-gradient(135deg, #00e5ff, #0099ff);\n"
"      color: #050b14;\n"
"      box-shadow: 0 4px 15px rgba(0, 229, 255, 0.35);\n"
"    }\n"
"    .btn-primary:hover {\n"
"      transform: translateY(-1px);\n"
"      box-shadow: 0 6px 20px rgba(0, 229, 255, 0.5);\n"
"    }\n"
"    .slider-group {\n"
"      margin-top: 1.2rem;\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      gap: 0.8rem;\n"
"    }\n"
"    .slider-label { display: flex; justify-content: space-between; font-size: 0.85rem; color: var(--text-sub); }\n"
"    input[type=\"range\"] {\n"
"      width: 100%;\n"
"      accent-color: var(--accent);\n"
"    }\n"
"    .telemetry-grid {\n"
"      display: grid;\n"
"      grid-template-columns: repeat(4, 1fr);\n"
"      gap: 1rem;\n"
"      margin-bottom: 1.5rem;\n"
"    }\n"
"    .stat-card {\n"
"      background: rgba(255, 255, 255, 0.03);\n"
"      border: 1px solid var(--border);\n"
"      border-radius: 0.75rem;\n"
"      padding: 1rem;\n"
"      text-align: center;\n"
"    }\n"
"    .stat-val {\n"
"      font-size: 1.6rem;\n"
"      font-weight: 700;\n"
"      color: #fff;\n"
"      font-family: 'JetBrains Mono', monospace;\n"
"      margin-top: 0.3rem;\n"
"    }\n"
"    .stat-lbl { font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.05em; color: var(--text-sub); }\n"
"    .viewport-box {\n"
"      position: relative;\n"
"      background: #000;\n"
"      border-radius: 0.75rem;\n"
"      overflow: hidden;\n"
"      min-height: 480px;\n"
"      display: flex;\n"
"      align-items: center;\n"
"      justify-content: center;\n"
"      border: 1px solid var(--border);\n"
"    }\n"
"    .viewport-box img { max-width: 100%; max-height: 600px; display: block; object-fit: contain; }\n"
"    .tag {\n"
"      display: inline-block;\n"
"      padding: 0.2rem 0.5rem;\n"
"      border-radius: 0.35rem;\n"
"      font-size: 0.75rem;\n"
"      font-weight: 600;\n"
"      font-family: 'JetBrains Mono', monospace;\n"
"    }\n"
"    .det-list {\n"
"      margin-top: 1rem;\n"
"      max-height: 200px;\n"
"      overflow-y: auto;\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      gap: 0.5rem;\n"
"    }\n"
"    .det-item {\n"
"      display: flex;\n"
"      justify-content: space-between;\n"
"      align-items: center;\n"
"      padding: 0.5rem 0.75rem;\n"
"      background: rgba(255,255,255,0.02);\n"
"      border-radius: 0.5rem;\n"
"      font-size: 0.85rem;\n"
"    }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <header>\n"
"    <div class=\"logo-group\">\n"
"      <div class=\"pulse-dot\"></div>\n"
"      <div class=\"brand\">AMBARELLA <span>EDGE AI</span></div>\n"
"    </div>\n"
"    <div class=\"os-badge\" id=\"osBadge\">\n"
"      <span id=\"osBadgeText\">Loading OS...</span>\n"
"    </div>\n"
"  </header>\n"
"\n"
"  <main>\n"
"    <aside>\n"
"      <div class=\"card\">\n"
"        <h2>Image Source</h2>\n"
"        <div class=\"drop-zone\" id=\"dropZone\">\n"
"          <svg viewBox=\"0 0 24 24\"><path d=\"M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM14 13v4h-4v-4H7l5-5 5 5h-3z\"/></svg>\n"
"          <p style=\"font-size: 0.85rem; color: var(--text-sub);\">Drag & drop image here or click to browse</p>\n"
"          <input type=\"file\" id=\"fileInput\" accept=\"image/*\" style=\"display:none;\">\n"
"        </div>\n"
"\n"
"        <div class=\"slider-group\">\n"
"          <div class=\"slider-label\">\n"
"            <span>Confidence Threshold</span>\n"
"            <span id=\"threshVal\" style=\"color: var(--accent); font-weight: 600;\">0.25</span>\n"
"          </div>\n"
"          <input type=\"range\" id=\"threshSlider\" min=\"0.05\" max=\"0.95\" step=\"0.05\" value=\"0.25\">\n"
"\n"
"          <div class=\"slider-label\">\n"
"            <span>NMS IoU Threshold</span>\n"
"            <span id=\"nmsVal\" style=\"color: var(--accent); font-weight: 600;\">0.45</span>\n"
"          </div>\n"
"          <input type=\"range\" id=\"nmsSlider\" min=\"0.1\" max=\"0.9\" step=\"0.05\" value=\"0.45\">\n"
"        </div>\n"
"\n"
"        <button class=\"btn btn-primary\" id=\"inferBtn\" style=\"margin-top: 1.5rem;\">\n"
"          Run VisORC Inference\n"
"        </button>\n"
"      </div>\n"
"\n"
"      <div class=\"card\" style=\"margin-top: 1rem;\">\n"
"        <h2>Detections</h2>\n"
"        <div class=\"det-list\" id=\"detList\">\n"
"          <div style=\"text-align: center; color: var(--text-sub); font-size: 0.85rem; padding: 1rem;\">\n"
"            No detections yet\n"
"          </div>\n"
"        </div>\n"
"      </div>\n"
"    </aside>\n"
"\n"
"    <section>\n"
"      <div class=\"telemetry-grid\">\n"
"        <div class=\"stat-card\">\n"
"          <div class=\"stat-lbl\">VisORC Hardware Time</div>\n"
"          <div class=\"stat-val\" id=\"statVisorcMs\" style=\"color: var(--accent);\">--</div>\n"
"        </div>\n"
"        <div class=\"stat-card\">\n"
"          <div class=\"stat-lbl\">In-Guest Total Latency</div>\n"
"          <div class=\"stat-val\" id=\"statTotalMs\" style=\"color: #34d399;\">--</div>\n"
"        </div>\n"
"        <div class=\"stat-card\">\n"
"          <div class=\"stat-lbl\">Throughput</div>\n"
"          <div class=\"stat-val\" id=\"statFps\">--</div>\n"
"        </div>\n"
"        <div class=\"stat-card\">\n"
"          <div class=\"stat-lbl\">Objects Detected</div>\n"
"          <div class=\"stat-val\" id=\"statCount\">0</div>\n"
"        </div>\n"
"      </div>\n"
"\n"
"      <div class=\"viewport-box\" id=\"viewport\">\n"
"        <img id=\"previewImg\" style=\"display: none;\" alt=\"Detection View\">\n"
"        <div id=\"placeholderText\" style=\"color: var(--text-sub); font-size: 0.95rem;\">\n"
"          Load or upload an image to begin live edge inference\n"
"        </div>\n"
"      </div>\n"
"    </section>\n"
"  </main>\n"
"\n"
"  <script>\n"
"    let activeFile = null;\n"
"    const dropZone = document.getElementById('dropZone');\n"
"    const fileInput = document.getElementById('fileInput');\n"
"    const previewImg = document.getElementById('previewImg');\n"
"    const placeholderText = document.getElementById('placeholderText');\n"
"    const inferBtn = document.getElementById('inferBtn');\n"
"    const threshSlider = document.getElementById('threshSlider');\n"
"    const threshVal = document.getElementById('threshVal');\n"
"    const nmsSlider = document.getElementById('nmsSlider');\n"
"    const nmsVal = document.getElementById('nmsVal');\n"
"    const osBadgeText = document.getElementById('osBadgeText');\n"
"    const detList = document.getElementById('detList');\n"
"\n"
"    threshSlider.oninput = () => { threshVal.innerText = threshSlider.value; };\n"
"    nmsSlider.oninput = () => { nmsVal.innerText = nmsSlider.value; };\n"
"\n"
"    dropZone.onclick = () => fileInput.click();\n"
"    dropZone.ondragover = (e) => { e.preventDefault(); dropZone.classList.add('dragover'); };\n"
"    dropZone.ondragleave = () => dropZone.classList.remove('dragover');\n"
"    dropZone.ondrop = (e) => {\n"
"      e.preventDefault();\n"
"      dropZone.classList.remove('dragover');\n"
"      if (e.dataTransfer.files.length) handleFile(e.dataTransfer.files[0]);\n"
"    };\n"
"    fileInput.onchange = () => {\n"
"      if (fileInput.files.length) handleFile(fileInput.files[0]);\n"
"    };\n"
"\n"
"    function handleFile(f) {\n"
"      activeFile = f;\n"
"      const r = new FileReader();\n"
"      r.onload = (e) => {\n"
"        previewImg.src = e.target.result;\n"
"        previewImg.style.display = 'block';\n"
"        placeholderText.style.display = 'none';\n"
"      };\n"
"      r.readAsDataURL(f);\n"
"    }\n"
"\n"
"    async function fetchInfo() {\n"
"      try {\n"
"        const res = await fetch('/api/info');\n"
"        const d = await res.json();\n"
"        osBadgeText.innerText = `${d.pretty_name} (${d.machine}, Linux ${d.kernel})`;\n"
"      } catch (e) { osBadgeText.innerText = 'Connected to Ambarella CVFlow'; }\n"
"    }\n"
"    fetchInfo();\n"
"\n"
"    inferBtn.onclick = async () => {\n"
"      if (!activeFile) {\n"
"        alert('Please choose or drop an image first!');\n"
"        return;\n"
"      }\n"
"      inferBtn.disabled = true;\n"
"      inferBtn.innerText = 'Running Inference...';\n"
"\n"
"      try {\n"
"        const thresh = threshSlider.value;\n"
"        const nms = nmsSlider.value;\n"
"        const res = await fetch(`/infer?thresh=${thresh}&nms=${nms}`, {\n"
"          method: 'POST',\n"
"          body: activeFile\n"
"        });\n"
"        const json = await res.json();\n"
"\n"
"        if (json.image_base64) {\n"
"          previewImg.src = 'data:image/jpeg;base64,' + json.image_base64;\n"
"          previewImg.style.display = 'block';\n"
"          placeholderText.style.display = 'none';\n"
"        }\n"
"\n"
"        document.getElementById('statVisorcMs').innerText = json.visorc_ms ? `${json.visorc_ms.toFixed(2)} ms` : '--';\n"
"        document.getElementById('statTotalMs').innerText = json.total_ms ? `${json.total_ms.toFixed(2)} ms` : '--';\n"
"        document.getElementById('statFps').innerText = json.total_ms ? `${(1000.0 / json.total_ms).toFixed(1)} FPS` : '--';\n"
"        document.getElementById('statCount').innerText = json.detections ? json.detections.length : 0;\n"
"\n"
"        if (json.detections && json.detections.length > 0) {\n"
"          detList.innerHTML = '';\n"
"          json.detections.forEach(d => {\n"
"            const item = document.createElement('div');\n"
"            item.className = 'det-item';\n"
"            item.innerHTML = `<span style=\"font-weight:600; color:var(--accent);\">${d.class}</span>` +\n"
"                             `<span class=\"tag\" style=\"background:rgba(0,229,255,0.1); color:#00e5ff;\">${(d.confidence*100).toFixed(1)}%</span>`;\n"
"            detList.appendChild(item);\n"
"          });\n"
"        } else {\n"
"          detList.innerHTML = '<div style=\"text-align: center; color: var(--text-sub); font-size: 0.85rem; padding: 1rem;\">No detections found</div>';\n"
"        }\n"
"      } catch (err) {\n"
"        alert('Inference error: ' + err.message);\n"
"      } finally {\n"
"        inferBtn.disabled = false;\n"
"        inferBtn.innerText = 'Run VisORC Inference';\n"
"      }\n"
"    };\n"
"  </script>\n"
"</body>\n"
"</html>\n";

/* Run a single complete inference pipeline on an image buffer in memory */
static int run_single_inference(struct yolo_runtime *rt,
                                const uint8_t *img_bytes, size_t img_len,
                                float conf_thresh, float nms_thresh,
                                struct bbox *out_dets, int *out_det_count,
                                uint8_t **out_annotated_jpeg, size_t *out_jpeg_len,
                                double *out_visorc_ms, double *out_total_ms)
{
    double t_start = get_time_sec();

    int orig_w = 0, orig_h = 0, orig_channels = 0;
    uint8_t *rgb = stbi_load_from_memory(img_bytes, img_len, &orig_w, &orig_h, &orig_channels, 3);
    if (!rgb || orig_w <= 0 || orig_h <= 0) {
        fprintf(stderr, "cavalry_hvm_yolo: image decoding failed\n");
        return -1;
    }

    pthread_mutex_lock(&rt->lock);

    /* 1. Letterbox preprocessing into input tensor */
    uint8_t *in_tensor = (uint8_t *)rt->net_in.in_desc[0].virt;
    float scale_r = letterbox_preprocess(rgb, orig_w, orig_h, in_tensor);

    /* 2. Dispatch VisORC execution */
    double t_v0 = get_time_sec();
    if (nnctrl_run_net(rt->net_id, NULL, NULL, NULL, NULL) < 0) {
        fprintf(stderr, "cavalry_hvm_yolo: nnctrl_run_net failed\n");
        pthread_mutex_unlock(&rt->lock);
        stbi_image_free(rgb);
        return -1;
    }
    double t_v1 = get_time_sec();
    double visorc_ms = (t_v1 - t_v0) * 1000.0;
    if (out_visorc_ms) *out_visorc_ms = visorc_ms;

    /* 3. Output postprocessing */
    const int16_t *out_tensor = (const int16_t *)rt->net_out.out_desc[0].virt;
    int det_cnt = yolox_postprocess(out_tensor, scale_r, orig_w, orig_h,
                                    conf_thresh, nms_thresh,
                                    out_dets, MAX_DETECTIONS);
    if (out_det_count) *out_det_count = det_cnt;
    rt->total_inferences++;

    pthread_mutex_unlock(&rt->lock);

    /* 4. Annotate image copy */
    if (out_annotated_jpeg && out_jpeg_len) {
        draw_detections(rgb, orig_w, orig_h, out_dets, det_cnt);

        struct mem_write_ctx w_ctx = { 0 };
        stbi_write_jpg_to_func(stbi_write_mem_cb, &w_ctx, orig_w, orig_h, 3, rgb, 88);
        *out_annotated_jpeg = w_ctx.data;
        *out_jpeg_len = w_ctx.size;
    }

    stbi_image_free(rgb);

    double t_end = get_time_sec();
    if (out_total_ms) *out_total_ms = (t_end - t_start) * 1000.0;
    return 0;
}

/* HTTP Request Handler */
static void handle_http_client(struct yolo_runtime *rt, int client_fd)
{
    char *buf = malloc(MAX_PAYLOAD_SIZE + 4096);
    if (!buf) {
        close(client_fd);
        return;
    }

    ssize_t n = recv(client_fd, buf, 4096, 0);
    if (n <= 0) {
        free(buf);
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    char method[16] = { 0 }, uri[256] = { 0 };
    sscanf(buf, "%15s %255s", method, uri);

    /* Parse query parameters ?thresh=...&nms=... */
    float conf_thresh = 0.25f;
    float nms_thresh = 0.45f;
    char *q = strchr(uri, '?');
    if (q) {
        *q = '\0';
        q++;
        char *p_thresh = strstr(q, "thresh=");
        if (p_thresh) conf_thresh = atof(p_thresh + 7);
        char *p_nms = strstr(q, "nms=");
        if (p_nms) nms_thresh = atof(p_nms + 4);
    }

    if (strcmp(method, "OPTIONS") == 0) {
        const char *resp = "HTTP/1.1 204 No Content\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: Content-Type\r\n"
                           "Connection: close\r\n\r\n";
        send(client_fd, resp, strlen(resp), 0);
    } else if (strcmp(method, "GET") == 0 && (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0)) {
        char header[256];
        size_t body_len = strlen(HTML_TEMPLATE);
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n", body_len);
        send(client_fd, header, strlen(header), 0);
        send(client_fd, HTML_TEMPLATE, body_len, 0);
    } else if (strcmp(method, "GET") == 0 && (strcmp(uri, "/api/info") == 0 || strcmp(uri, "/health") == 0)) {
        char json[1024];
        snprintf(json, sizeof(json),
                 "{\n"
                 "  \"status\": \"healthy\",\n"
                 "  \"pretty_name\": \"%s\",\n"
                 "  \"kernel\": \"%s\",\n"
                 "  \"machine\": \"%s\",\n"
                 "  \"sysname\": \"%s\",\n"
                 "  \"model\": \"YOLOX-S 640x640 (Ambarella Optimized)\",\n"
                 "  \"total_inferences\": %u\n"
                 "}\n",
                 rt->os_pretty_name, rt->os_kernel, rt->os_machine, rt->os_sysname, rt->total_inferences);

        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n", strlen(json));
        send(client_fd, header, strlen(header), 0);
        send(client_fd, json, strlen(json), 0);
    } else if (strcmp(method, "POST") == 0 && strstr(uri, "/infer")) {
        /* Find end of HTTP headers */
        char *body_start = strstr(buf, "\r\n\r\n");
        if (!body_start) {
            free(buf);
            close(client_fd);
            return;
        }
        body_start += 4;
        size_t header_len = body_start - buf;
        size_t body_recvd = n - header_len;

        /* Check Content-Length */
        size_t content_len = 0;
        char *cl_hdr = strcasestr(buf, "Content-Length:");
        if (cl_hdr) {
            content_len = strtoul(cl_hdr + 15, NULL, 10);
        }

        /* Read remaining payload if truncated */
        while (body_recvd < content_len && (header_len + body_recvd) < MAX_PAYLOAD_SIZE) {
            ssize_t r = recv(client_fd, buf + header_len + body_recvd,
                             content_len - body_recvd, 0);
            if (r <= 0) break;
            body_recvd += r;
        }

        /* Extract raw image bytes */
        const uint8_t *img_ptr = (const uint8_t *)body_start;
        size_t img_len = body_recvd;

        /* If multipart form data, scan for JPEG or PNG magic header within the body */
        for (size_t i = 0; i + 8 < body_recvd; i++) {
            if ((uint8_t)body_start[i] == 0xFF && (uint8_t)body_start[i+1] == 0xD8 &&
                (uint8_t)body_start[i+2] == 0xFF) {
                img_ptr = (const uint8_t *)body_start + i;
                img_len = body_recvd - i;
                break;
            }
            if ((uint8_t)body_start[i] == 0x89 && body_start[i+1] == 'P' &&
                body_start[i+2] == 'N' && body_start[i+3] == 'G') {
                img_ptr = (const uint8_t *)body_start + i;
                img_len = body_recvd - i;
                break;
            }
        }

        struct bbox dets[MAX_DETECTIONS];
        int det_cnt = 0;
        uint8_t *ann_jpg = NULL;
        size_t ann_len = 0;
        double visorc_ms = 0.0, total_ms = 0.0;

        int ret = run_single_inference(rt, img_ptr, img_len,
                                       conf_thresh, nms_thresh,
                                       dets, &det_cnt,
                                       &ann_jpg, &ann_len,
                                       &visorc_ms, &total_ms);
        if (ret < 0) {
            const char *err_resp = "HTTP/1.1 400 Bad Request\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Access-Control-Allow-Origin: *\r\n"
                                   "Connection: close\r\n\r\n"
                                   "{\"error\": \"Image decode or inference failed\"}\n";
            send(client_fd, err_resp, strlen(err_resp), 0);
        } else {
            char *b64_img = NULL;
            if (ann_jpg && ann_len > 0) {
                b64_img = base64_encode(ann_jpg, ann_len, NULL);
            }

            /* Construct JSON response */
            size_t json_cap = (b64_img ? strlen(b64_img) : 0) + 8192;
            char *resp_json = malloc(json_cap);
            if (resp_json) {
                int off = snprintf(resp_json, json_cap,
                                   "{\n"
                                   "  \"status\": \"success\",\n"
                                   "  \"pretty_name\": \"%s\",\n"
                                   "  \"kernel\": \"%s\",\n"
                                   "  \"machine\": \"%s\",\n"
                                   "  \"visorc_ms\": %.2f,\n"
                                   "  \"total_ms\": %.2f,\n"
                                   "  \"fps\": %.1f,\n"
                                   "  \"detections\": [\n",
                                   rt->os_pretty_name, rt->os_kernel, rt->os_machine,
                                   visorc_ms, total_ms, (total_ms > 0 ? 1000.0 / total_ms : 0.0));

                for (int d = 0; d < det_cnt; d++) {
                    const char *cname = (dets[d].class_id >= 0 && dets[d].class_id < YOLO_NUM_CLASSES) ?
                        COCO_CLASSES[dets[d].class_id] : "object";
                    off += snprintf(resp_json + off, json_cap - off,
                                    "    {\"class\": \"%s\", \"confidence\": %.3f, \"box\": [%.1f, %.1f, %.1f, %.1f]}%s\n",
                                    cname, dets[d].score,
                                    dets[d].x1, dets[d].y1, dets[d].x2, dets[d].y2,
                                    (d == det_cnt - 1) ? "" : ",");
                }

                off += snprintf(resp_json + off, json_cap - off,
                                "  ],\n"
                                "  \"image_base64\": \"%s\"\n"
                                "}\n",
                                b64_img ? b64_img : "");

                char header[256];
                snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Content-Length: %d\r\n"
                         "Connection: close\r\n\r\n", off);

                send(client_fd, header, strlen(header), 0);
                send(client_fd, resp_json, off, 0);
                free(resp_json);
            }

            if (b64_img) free(b64_img);
            if (ann_jpg) free(ann_jpg);
        }
    } else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\n"
                                "Content-Length: 0\r\n"
                                "Connection: close\r\n\r\n";
        send(client_fd, not_found, strlen(not_found), 0);
    }

    free(buf);
    close(client_fd);
}

/* HTTP Daemon Server Loop */
static int run_http_server(struct yolo_runtime *rt, const char *bind_addr)
{
    char ip_str[64] = "0.0.0.0";
    int port = 8080;

    const char *colon = strchr(bind_addr, ':');
    if (colon) {
        size_t iplen = colon - bind_addr;
        if (iplen < sizeof(ip_str)) {
            strncpy(ip_str, bind_addr, iplen);
            ip_str[iplen] = '\0';
        }
        port = atoi(colon + 1);
    } else {
        port = atoi(bind_addr);
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, ip_str, &sa.sin_addr);

    if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        close(sfd);
        return -1;
    }

    if (listen(sfd, 16) < 0) {
        perror("listen");
        close(sfd);
        return -1;
    }

    printf("\n===============================================================================\n");
    printf(" Ambarella Cavalry YOLOX Web Service Online\n");
    printf(" OS Target:     %s\n", rt->os_pretty_name);
    printf(" Kernel:        %s (%s)\n", rt->os_kernel, rt->os_machine);
    printf(" HTTP Listener: http://%s:%d/\n", ip_str, port);
    printf(" API Endpoint:  POST http://%s:%d/infer\n", ip_str, port);
    printf("===============================================================================\n\n");

    while (1) {
        struct sockaddr_in client_sa;
        socklen_t client_len = sizeof(client_sa);
        int cfd = accept(sfd, (struct sockaddr *)&client_sa, &client_len);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* Set client socket timeouts */
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        handle_http_client(rt, cfd);
    }

    close(sfd);
    return 0;
}

/* Offline CLI Mode */
static int run_cli_mode(struct yolo_runtime *rt, const char *image_path, const char *out_path,
                        float conf_thresh, float nms_thresh)
{
    FILE *fp = fopen(image_path, "rb");
    if (!fp) {
        perror("fopen input image");
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *buf = malloc(fsize);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, fsize, fp) != (size_t)fsize) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    struct bbox dets[MAX_DETECTIONS];
    int det_count = 0;
    uint8_t *ann_jpg = NULL;
    size_t ann_len = 0;
    double visorc_ms = 0.0, total_ms = 0.0;

    printf("[*] Processing image: %s (%ld bytes)...\n", image_path, fsize);
    if (run_single_inference(rt, buf, fsize, conf_thresh, nms_thresh,
                             dets, &det_count, &ann_jpg, &ann_len,
                             &visorc_ms, &total_ms) < 0) {
        fprintf(stderr, "Inference failed\n");
        free(buf);
        return -1;
    }
    free(buf);

    printf("\n===============================================================================\n");
    printf(" INFERENCE RESULTS: %d Object(s) Detected\n", det_count);
    printf(" OS Platform:       %s\n", rt->os_pretty_name);
    printf(" VisORC Time:       %.2f ms\n", visorc_ms);
    printf(" Total Pipeline:    %.2f ms (%.1f FPS)\n", total_ms, 1000.0 / total_ms);
    printf("===============================================================================\n");

    for (int i = 0; i < det_count; i++) {
        const char *cname = (dets[i].class_id >= 0 && dets[i].class_id < YOLO_NUM_CLASSES) ?
            COCO_CLASSES[dets[i].class_id] : "object";
        printf(" [%2d] %-16s | Confidence: %5.1f%% | Box: [%.0f, %.0f, %.0f, %.0f]\n",
               i + 1, cname, dets[i].score * 100.0f,
               dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
    }

    if (out_path && ann_jpg && ann_len > 0) {
        FILE *out_fp = fopen(out_path, "wb");
        if (out_fp) {
            fwrite(ann_jpg, 1, ann_len, out_fp);
            fclose(out_fp);
            printf("\n[PASS] Saved annotated detection overlay to: %s (%zu bytes)\n", out_path, ann_len);
        } else {
            perror("fopen out_path");
        }
    }

    if (ann_jpg) free(ann_jpg);
    return 0;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nOptions:\n");
    printf("  --model <path>      Path to compiled YOLOX .bin model (default: %s)\n", DEFAULT_MODEL_PATH);
    printf("  --bind <ip:port>    HTTP service bind address (default: %s)\n", DEFAULT_HTTP_BIND);
    printf("  --image <path>      Run offline inference on image file\n");
    printf("  --out <path>        Save annotated image with bounding boxes (CLI mode)\n");
    printf("  --thresh <float>    Detection confidence threshold (default: 0.25)\n");
    printf("  --nms <float>       NMS IoU threshold (default: 0.45)\n");
    printf("  --help              Display this help message\n");
}

int main(int argc, char **argv)
{
    const char *model_path = NULL;
    const char *bind_addr = NULL;
    const char *image_path = NULL;
    const char *out_path = NULL;
    float conf_thresh = 0.25f;
    float nms_thresh = 0.45f;

    static struct option long_opts[] = {
        { "model",   required_argument, 0, 'm' },
        { "bind",    required_argument, 0, 'b' },
        { "image",   required_argument, 0, 'i' },
        { "out",     required_argument, 0, 'o' },
        { "thresh",  required_argument, 0, 't' },
        { "nms",     required_argument, 0, 'n' },
        { "help",    no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "m:b:i:o:t:n:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'm': model_path = optarg; break;
        case 'b': bind_addr = optarg; break;
        case 'i': image_path = optarg; break;
        case 'o': out_path = optarg; break;
        case 't': conf_thresh = atof(optarg); break;
        case 'n': nms_thresh = atof(optarg); break;
        case 'h': print_usage(argv[0]); return 0;
        default: print_usage(argv[0]); return 1;
        }
    }

    /* Fallback to environment variables if not specified */
    if (!model_path) model_path = getenv("CAVALRY_HVM_YOLO_MODEL");
    if (!model_path) model_path = DEFAULT_MODEL_PATH;

    if (!bind_addr) bind_addr = getenv("CAVALRY_HVM_YOLO_BIND");
    if (!bind_addr) bind_addr = DEFAULT_HTTP_BIND;

    memset(&g_yolo, 0, sizeof(g_yolo));
    pthread_mutex_init(&g_yolo.lock, NULL);
    get_os_info(&g_yolo);

    printf("===============================================================================\n");
    printf(" Ambarella Cavalry Edge AI &mdash; Dual-HVM YOLOX Service\n");
    printf(" Target OS: %s\n", g_yolo.os_pretty_name);
    printf(" Model:     %s\n", model_path);
    printf("===============================================================================\n\n");

    /* 1. Open Cavalry device */
    g_yolo.fd_cav = open(CAVALRY_DEV_NODE, O_RDWR);
    if (g_yolo.fd_cav < 0) {
        perror("open /dev/cavalry");
        return 1;
    }

    /* 2. Initialize memory and nnctrl */
    if (cavalry_mem_init(g_yolo.fd_cav, 0) < 0 || nnctrl_init(g_yolo.fd_cav, 0) < 0) {
        fprintf(stderr, "Failed to initialize cavalry memory or nnctrl\n");
        close(g_yolo.fd_cav);
        return 1;
    }

    /* 3. Initialize network */
    g_yolo.net_cf.net_file = (char *)model_path;
    g_yolo.net_cf.no_chip_check = 1;
    g_yolo.net_id = nnctrl_init_net(&g_yolo.net_cf, NULL, NULL);
    if (g_yolo.net_id < 0) {
        fprintf(stderr, "nnctrl_init_net failed for model: %s\n", model_path);
        nnctrl_exit();
        cavalry_mem_exit();
        close(g_yolo.fd_cav);
        return 1;
    }

    /* 4. Allocate model memory and load net */
    if (cavalry_mem_alloc(&g_yolo.net_cf.net_mem_total, &g_yolo.net_m.phy_addr,
                          (void **)&g_yolo.net_m.virt_addr, 0) < 0) {
        fprintf(stderr, "cavalry_mem_alloc failed\n");
        nnctrl_exit_net(g_yolo.net_id);
        nnctrl_exit();
        cavalry_mem_exit();
        close(g_yolo.fd_cav);
        return 1;
    }
    g_yolo.net_m.mem_size = g_yolo.net_cf.net_mem_total;

    if (nnctrl_load_net(g_yolo.net_id, &g_yolo.net_m, NULL, NULL) < 0 ||
        nnctrl_get_net_io_cfg(g_yolo.net_id, &g_yolo.net_in, &g_yolo.net_out) < 0) {
        fprintf(stderr, "nnctrl_load_net or get_net_io_cfg failed\n");
        cavalry_mem_free(g_yolo.net_m.mem_size, g_yolo.net_m.phy_addr, g_yolo.net_m.virt_addr);
        nnctrl_exit_net(g_yolo.net_id);
        nnctrl_exit();
        cavalry_mem_exit();
        close(g_yolo.fd_cav);
        return 1;
    }

    struct nnctrl_info *pctl = get_nnctrl_global_context();
    struct net_desc *pnet = get_net_desc(pctl, g_yolo.net_id);
    if (!pnet || pnet->subgraph_exe_cnt == 0 || !pnet->execute_ctx[0].run_dags) {
        fprintf(stderr, "Failed to get execution context\n");
        return 1;
    }
    g_yolo.run_dags = pnet->execute_ctx[0].run_dags;

    printf("[PASS] YOLOX loaded and Path B registered successfully (subgraphs=%u, dags=%u)\n",
           pnet->subgraph_exe_cnt, g_yolo.run_dags->dag_cnt);

    /* Warmup coprocessor once */
    printf("[*] Warming up VisORC coprocessor...\n");
    if (nnctrl_run_net(g_yolo.net_id, NULL, NULL, NULL, NULL) < 0) {
        fprintf(stderr, "VisORC warmup failed\n");
    } else {
        printf("[PASS] VisORC warmup complete (ticks=%u)\n", g_yolo.run_dags->exec_total_ticks);
    }

    int ret = 0;
    if (image_path) {
        ret = run_cli_mode(&g_yolo, image_path, out_path, conf_thresh, nms_thresh);
    } else {
        ret = run_http_server(&g_yolo, bind_addr);
    }

    cavalry_mem_free(g_yolo.net_m.mem_size, g_yolo.net_m.phy_addr, g_yolo.net_m.virt_addr);
    nnctrl_exit_net(g_yolo.net_id);
    nnctrl_exit();
    cavalry_mem_exit();
    close(g_yolo.fd_cav);
    pthread_mutex_destroy(&g_yolo.lock);
    return ret;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
