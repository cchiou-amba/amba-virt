/*
 * drivers/amba_virt/tools/iav_proxy.c
 *
 * IAV Frame Proxy & Cavalry Path B Bridge.
 * Binds live camera frames from iav_frame_tap into Cavalry Path B input handles
 * for hardware-accelerated deep neural network inference.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <math.h>

#include "cavalry_ioctl.h"

#include "amba_virt.h"
#include "iav_tap_abi.h"
#include "iav_proxy.h"
#include "cavalry_proxy.h"

#include <pb_decode.h>
#include <pb_encode.h>
#include "amba_virt.pb.h"

#include <uapi/specific/iav_ioctl.h>

static int g_fd_iav_bsb = -1;
static uint8_t *g_bsb_base = NULL;
static size_t g_bsb_len = 0;
static int g_mjpeg_stream_id = 1;

void iav_proxy_set_bsb(int fd_iav, void *bsb_base, size_t bsb_len, int mjpeg_stream_id)
{
    g_fd_iav_bsb = fd_iav;
    g_bsb_base = (uint8_t *)bsb_base;
    g_bsb_len = bsb_len;
    g_mjpeg_stream_id = mjpeg_stream_id;
}

#define MAX_IAV_TAP_CLIENTS 8

/* Standalone MD5 for Host Proxy Output Tensor Verification */
typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
} proxy_md5_ctx;

#define PROXY_F_MD5(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define PROXY_G_MD5(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define PROXY_H_MD5(x, y, z) ((x) ^ (y) ^ (z))
#define PROXY_I_MD5(x, y, z) ((y) ^ ((x) | (~z)))
#define PROXY_ROTL_MD5(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define PROXY_FF(a, b, c, d, x, s, ac) { \
    (a) += PROXY_F_MD5((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = PROXY_ROTL_MD5((a), (s)); \
    (a) += (b); \
}
#define PROXY_GG(a, b, c, d, x, s, ac) { \
    (a) += PROXY_G_MD5((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = PROXY_ROTL_MD5((a), (s)); \
    (a) += (b); \
}
#define PROXY_HH(a, b, c, d, x, s, ac) { \
    (a) += PROXY_H_MD5((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = PROXY_ROTL_MD5((a), (s)); \
    (a) += (b); \
}
#define PROXY_II(a, b, c, d, x, s, ac) { \
    (a) += PROXY_I_MD5((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = PROXY_ROTL_MD5((a), (s)); \
    (a) += (b); \
}

static void proxy_md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = ((uint32_t)block[i * 4]) |
               (((uint32_t)block[i * 4 + 1]) << 8) |
               (((uint32_t)block[i * 4 + 2]) << 16) |
               (((uint32_t)block[i * 4 + 3]) << 24);
    }
    PROXY_FF(a, b, c, d, x[ 0],  7, 0xd76aa478);
    PROXY_FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
    PROXY_FF(c, d, a, b, x[ 2], 17, 0x242070db);
    PROXY_FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
    PROXY_FF(a, b, c, d, x[ 4],  7, 0xf57c0faf);
    PROXY_FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
    PROXY_FF(c, d, a, b, x[ 6], 17, 0xa8304613);
    PROXY_FF(b, c, d, a, x[ 7], 22, 0xfd469501);
    PROXY_FF(a, b, c, d, x[ 8],  7, 0x698098d8);
    PROXY_FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
    PROXY_FF(c, d, a, b, x[10], 17, 0xffff5bb1);
    PROXY_FF(b, c, d, a, x[11], 22, 0x895cd7be);
    PROXY_FF(a, b, c, d, x[12],  7, 0x6b901122);
    PROXY_FF(d, a, b, c, x[13], 12, 0xfd987193);
    PROXY_FF(c, d, a, b, x[14], 17, 0xa679438e);
    PROXY_FF(b, c, d, a, x[15], 22, 0x49b40821);

    PROXY_GG(a, b, c, d, x[ 1],  5, 0xf61e2562);
    PROXY_GG(d, a, b, c, x[ 6],  9, 0xc040b340);
    PROXY_GG(c, d, a, b, x[11], 14, 0x265e5a51);
    PROXY_GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
    PROXY_GG(a, b, c, d, x[ 5],  5, 0xd62f105d);
    PROXY_GG(d, a, b, c, x[10],  9, 0x02441453);
    PROXY_GG(c, d, a, b, x[15], 14, 0xd8a1e681);
    PROXY_GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
    PROXY_GG(a, b, c, d, x[ 9],  5, 0x21e1cde6);
    PROXY_GG(d, a, b, c, x[14],  9, 0xc33707d6);
    PROXY_GG(c, d, a, b, x[ 3], 14, 0xf4d50d87);
    PROXY_GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
    PROXY_GG(a, b, c, d, x[13],  5, 0xa9e3e905);
    PROXY_GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);
    PROXY_GG(c, d, a, b, x[ 7], 14, 0x676f02d9);
    PROXY_GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

    PROXY_HH(a, b, c, d, x[ 5],  4, 0xfffa3942);
    PROXY_HH(d, a, b, c, x[ 8], 11, 0x8771f681);
    PROXY_HH(c, d, a, b, x[11], 16, 0x6d9d6122);
    PROXY_HH(b, c, d, a, x[14], 23, 0xfde5380c);
    PROXY_HH(a, b, c, d, x[ 1],  4, 0xa4beea44);
    PROXY_HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
    PROXY_HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60);
    PROXY_HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    PROXY_HH(a, b, c, d, x[13],  4, 0x289b7ec6);
    PROXY_HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
    PROXY_HH(c, d, a, b, x[ 3], 16, 0xd4ef3085);
    PROXY_HH(b, c, d, a, x[ 6], 23, 0x04881d05);
    PROXY_HH(a, b, c, d, x[ 9],  4, 0xd9d4d039);
    PROXY_HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    PROXY_HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
    PROXY_HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

    PROXY_II(a, b, c, d, x[ 0],  6, 0xf4292244);
    PROXY_II(d, a, b, c, x[ 7], 10, 0x432aff97);
    PROXY_II(c, d, a, b, x[14], 15, 0xab9423a7);
    PROXY_II(b, c, d, a, x[ 5], 21, 0xfc93a039);
    PROXY_II(a, b, c, d, x[12],  6, 0x655b59c3);
    PROXY_II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
    PROXY_II(c, d, a, b, x[10], 15, 0xffeff47d);
    PROXY_II(b, c, d, a, x[ 1], 21, 0x85845dd1);
    PROXY_II(a, b, c, d, x[ 8],  6, 0x6fa87e4f);
    PROXY_II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    PROXY_II(c, d, a, b, x[ 6], 15, 0xa3014314);
    PROXY_II(b, c, d, a, x[13], 21, 0x4e0811a1);
    PROXY_II(a, b, c, d, x[ 4],  6, 0xf7537e82);
    PROXY_II(d, a, b, c, x[11], 10, 0xbd3af235);
    PROXY_II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb);
    PROXY_II(b, c, d, a, x[ 9], 21, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void proxy_md5_init(proxy_md5_ctx *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void proxy_md5_update(proxy_md5_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t idx = (ctx->count[0] >> 3) & 0x3f;

    if ((ctx->count[0] += ((uint32_t)len << 3)) < ((uint32_t)len << 3))
        ctx->count[1]++;
    ctx->count[1] += ((uint32_t)len >> 29);

    uint32_t part_len = 64 - idx;
    uint32_t i = 0;

    if (len >= part_len) {
        memcpy(&ctx->buffer[idx], p, part_len);
        proxy_md5_transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64)
            proxy_md5_transform(ctx->state, &p[i]);
        idx = 0;
    }
    memcpy(&ctx->buffer[idx], &p[i], len - i);
}

static void proxy_md5_final(uint8_t digest[16], proxy_md5_ctx *ctx)
{
    static const uint8_t padding[64] = { 0x80 };
    uint8_t bits[8];

    for (int i = 0; i < 4; i++) {
        bits[i] = (uint8_t)((ctx->count[0] >> (i * 8)) & 0xff);
        bits[i + 4] = (uint8_t)((ctx->count[1] >> (i * 8)) & 0xff);
    }

    uint32_t idx = (ctx->count[0] >> 3) & 0x3f;
    uint32_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
    proxy_md5_update(ctx, padding, pad_len);
    proxy_md5_update(ctx, bits, 8);

    for (int i = 0; i < 4; i++) {
        digest[i]      = (uint8_t)((ctx->state[0] >> (i * 8)) & 0xff);
        digest[i + 4]  = (uint8_t)((ctx->state[1] >> (i * 8)) & 0xff);
        digest[i + 8]  = (uint8_t)((ctx->state[2] >> (i * 8)) & 0xff);
        digest[i + 12] = (uint8_t)((ctx->state[3] >> (i * 8)) & 0xff);
    }
}

static void proxy_calc_md5(const void *buf, size_t size, char *out_hex)
{
    proxy_md5_ctx ctx;
    uint8_t digest[16];

    proxy_md5_init(&ctx);
    proxy_md5_update(&ctx, buf, size);
    proxy_md5_final(digest, &ctx);

    for (int i = 0; i < 16; i++)
        sprintf(&out_hex[i * 2], "%02x", digest[i]);
    out_hex[32] = '\0';
}

/*
 * Letterbox & Color-Space Conversion:
 * Scales NV12 (Y + interleaved UV) camera frames into 1x3x640x640 Planar RGB.
 * Preserves 16:9 aspect ratio (640x360), pads remaining rows (360..639) with 114.
 */
int proxy_convert_nv12_to_yolox_rgb_640x640(const uint8_t *nv12_payload,
                                            uint32_t width, uint32_t height, uint32_t pitch,
                                            uint8_t *in_tensor)
{
    if (!nv12_payload || !in_tensor)
        return -EINVAL;
    if (width == 0 || height == 0 || pitch < width)
        return -EINVAL;

    memset(in_tensor, 114, 3 * 640 * 640);

    uint8_t *r_plane = in_tensor;
    uint8_t *g_plane = in_tensor + (640 * 640);
    uint8_t *b_plane = in_tensor + (2 * 640 * 640);

    const uint8_t *y_plane = nv12_payload;
    const uint8_t *uv_plane = nv12_payload + (height * pitch);

    float r = fminf(640.0f / (float)width, 640.0f / (float)height);
    int new_w = (int)roundf((float)width * r);
    int new_h = (int)roundf((float)height * r);
    if (new_w > 640) new_w = 640;
    if (new_h > 640) new_h = 640;

    for (int dy = 0; dy < new_h; dy++) {
        int sy = (int)((float)dy / r);
        if (sy >= (int)height) sy = height - 1;
        const uint8_t *y_row = y_plane + sy * pitch;
        const uint8_t *uv_row = uv_plane + (sy / 2) * pitch;
        int out_row_idx = dy * 640;

        for (int dx = 0; dx < new_w; dx++) {
            int sx = (int)((float)dx / r);
            if (sx >= (int)width) sx = width - 1;

            uint8_t y_val = y_row[sx];
            int uv_idx = (sx / 2) * 2;
            uint8_t u_val = uv_row[uv_idx];
            uint8_t v_val = uv_row[uv_idx + 1];

            /* Standard ITU-R BT.601 conversion */
            int c = (int)y_val - 16;
            if (c < 0) c = 0;
            int d = (int)u_val - 128;
            int e = (int)v_val - 128;
            int red   = (298 * c           + 409 * e + 128) >> 8;
            int green = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int blue  = (298 * c + 516 * d           + 128) >> 8;

            int out_idx = out_row_idx + dx;
            r_plane[out_idx] = (uint8_t)(red < 0 ? 0 : (red > 255 ? 255 : red));
            g_plane[out_idx] = (uint8_t)(green < 0 ? 0 : (green > 255 ? 255 : green));
            b_plane[out_idx] = (uint8_t)(blue < 0 ? 0 : (blue > 255 ? 255 : blue));
        }
    }

    return 0;
}

#define RESNET_INPUT_SIZE   224
#define RESNET_INPUT_PITCH  256    /* FP16 elements per row: 512-byte port pitch */
#define RESNET_INPUT_BYTES  (3 * RESNET_INPUT_SIZE * RESNET_INPUT_PITCH * 2)

/*
 * ResNet-50 (resnet50_base_cv72_cavalry.bin) input port: 1x3x224x224 FP16,
 * planar, 512-byte row pitch. The model carries no record of channel order,
 * scaling, or mean/std, so this feeds planar RGB with raw 0..255 values and
 * no normalization; outputs are structurally valid, not semantically
 * calibrated. Aspect-preserving letterbox to 224x126 at the top; padding
 * rows and pitch padding are 0.
 */
static void proxy_convert_nv12_to_resnet_fp16_224(const uint8_t *nv12_payload,
                                                  uint32_t width, uint32_t height, uint32_t pitch,
                                                  uint16_t *in_tensor)
{
    uint16_t fp16_lut[256];
    for (int v = 0; v < 256; v++) {
#if defined(__arm__) || defined(__aarch64__)
        __fp16 h = (__fp16)(float)v;
        memcpy(&fp16_lut[v], &h, sizeof(h));
#else
        if (v == 0) {
            fp16_lut[v] = 0;
        } else {
            uint32_t f_bits;
            float f_val = (float)v;
            memcpy(&f_bits, &f_val, sizeof(f_bits));
            uint32_t sign = (f_bits >> 16) & 0x8000;
            int32_t exp = ((f_bits >> 23) & 0xFF) - 127 + 15;
            uint32_t frac = (f_bits >> 13) & 0x03FF;
            fp16_lut[v] = (uint16_t)(sign | (exp << 10) | frac);
        }
#endif
    }

    memset(in_tensor, 0, RESNET_INPUT_BYTES);
    const size_t plane = (size_t)RESNET_INPUT_SIZE * RESNET_INPUT_PITCH;
    uint16_t *r_plane = in_tensor;
    uint16_t *g_plane = in_tensor + plane;
    uint16_t *b_plane = in_tensor + 2 * plane;

    const uint8_t *y_plane = nv12_payload;
    const uint8_t *uv_plane = nv12_payload + (height * pitch);

    float r = fminf((float)RESNET_INPUT_SIZE / (float)width,
                    (float)RESNET_INPUT_SIZE / (float)height);
    int new_w = (int)roundf((float)width * r);
    int new_h = (int)roundf((float)height * r);
    if (new_w > RESNET_INPUT_SIZE) new_w = RESNET_INPUT_SIZE;
    if (new_h > RESNET_INPUT_SIZE) new_h = RESNET_INPUT_SIZE;

    for (int dy = 0; dy < new_h; dy++) {
        int sy = (int)((float)dy / r);
        if (sy >= (int)height) sy = height - 1;
        const uint8_t *y_row = y_plane + sy * pitch;
        const uint8_t *uv_row = uv_plane + (sy / 2) * pitch;
        size_t out_row = (size_t)dy * RESNET_INPUT_PITCH;

        for (int dx = 0; dx < new_w; dx++) {
            int sx = (int)((float)dx / r);
            if (sx >= (int)width) sx = width - 1;

            int uv_idx = (sx / 2) * 2;
            int c = (int)y_row[sx] - 16;
            if (c < 0) c = 0;
            int d = (int)uv_row[uv_idx] - 128;
            int e = (int)uv_row[uv_idx + 1] - 128;
            int red   = (298 * c           + 409 * e + 128) >> 8;
            int green = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int blue  = (298 * c + 516 * d           + 128) >> 8;

            r_plane[out_row + dx] = fp16_lut[red < 0 ? 0 : (red > 255 ? 255 : red)];
            g_plane[out_row + dx] = fp16_lut[green < 0 ? 0 : (green > 255 ? 255 : green)];
            b_plane[out_row + dx] = fp16_lut[blue < 0 ? 0 : (blue > 255 ? 255 : blue)];
        }
    }
}

/*
 * A client that has not called into the tap for this long is evicted so a
 * guest that exited without DETACH cannot pin ring slots indefinitely.
 */
#define IAV_TAP_CLIENT_IDLE_US  (3ULL * 1000000ULL)

#define IAV_PROXY_HOST_LOG "/tmp/iav_proxy_host.log"

struct iav_tap_client {
    bool in_use;
    uint32_t cid;
    uint32_t session_id;
    uint64_t last_seq;
    uint64_t drop_count;
    uint64_t consumed;
    uint64_t last_active_us;
};

static struct iav_tap_ring *g_tap_ring = NULL;
static pthread_mutex_t g_tap_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tap_cond = PTHREAD_COND_INITIALIZER;
static struct iav_tap_client g_clients[MAX_IAV_TAP_CLIENTS];
static uint32_t g_tap_cohort = 1;
static uint64_t g_newest_seq = 0;

static uint64_t proxy_mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void proxy_host_log(const char *fmt, ...)
{
    va_list ap;
    FILE *fp = fopen(IAV_PROXY_HOST_LOG, "a");

    if (!fp)
        return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fclose(fp);
}

static int proxy_active_clients_locked(void)
{
    int cnt = 0;

    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (g_clients[i].in_use)
            cnt++;
    }
    return cnt;
}

/* Return every published slot no active client still needs. Caller holds g_tap_mutex. */
static void proxy_sweep_slots_locked(void)
{
    /* In 1-to-N decoupled broadcast, published slots remain published
     * until overwritten by the producer ring wrap-around. */
}

static void proxy_drop_client_locked(int i, const char *reason)
{
    proxy_host_log("[TAP_CLIENT] event=%s cid=%u session=%u consumed=%llu last_seq=%llu "
                   "published=%llu drops=%llu missed=%llu\n",
                   reason, g_clients[i].cid, g_clients[i].session_id,
                   (unsigned long long)g_clients[i].consumed,
                   (unsigned long long)g_clients[i].last_seq,
                   (unsigned long long)(g_tap_ring ? g_tap_ring->published_count : 0),
                   (unsigned long long)(g_tap_ring ? g_tap_ring->drop_count : 0),
                   (unsigned long long)(g_tap_ring ? g_tap_ring->missed_count : 0));
    printf("iav_proxy: client cid=%u session=%u %s (slot %d)\n",
           g_clients[i].cid, g_clients[i].session_id, reason, i);
    memset(&g_clients[i], 0, sizeof(g_clients[i]));
    if (g_tap_ring)
        proxy_sweep_slots_locked();
}

static void proxy_evict_idle_locked(uint64_t now_us, int self)
{
    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (i == self || !g_clients[i].in_use)
            continue;
        if (now_us - g_clients[i].last_active_us > IAV_TAP_CLIENT_IDLE_US)
            proxy_drop_client_locked(i, "evicted-idle");
    }
}

void iav_proxy_set_cohort(uint32_t cohort)
{
    pthread_mutex_lock(&g_tap_mutex);
    g_tap_cohort = cohort ? cohort : 1;
    pthread_mutex_unlock(&g_tap_mutex);
}

void iav_proxy_ring_lock(void)
{
    pthread_mutex_lock(&g_tap_mutex);
}

void iav_proxy_ring_unlock(void)
{
    pthread_mutex_unlock(&g_tap_mutex);
}

/* Caller holds the ring lock. */
bool iav_proxy_slot_writable_locked(const struct iav_tap_slot *slot)
{
    if (slot->state == IAV_TAP_SLOT_EMPTY)
        return true;
    if (slot->state == IAV_TAP_SLOT_PUBLISHED && slot->refcount == 0)
        return true;
    return false;
}

/* Caller holds the ring lock. */
void iav_proxy_note_published_locked(uint64_t seq)
{
    if (seq > g_newest_seq)
        g_newest_seq = seq;
    pthread_cond_broadcast(&g_tap_cond);
}

void iav_proxy_log_producer_loss(const char *reason, uint64_t first_seq, uint64_t last_seq)
{
    proxy_host_log("[TAP_LOSS] reason=%s first_seq=%llu last_seq=%llu\n",
                   reason, (unsigned long long)first_seq, (unsigned long long)last_seq);
}

int iav_proxy_init(struct iav_tap_ring *ring)
{
    pthread_mutex_lock(&g_tap_mutex);
    g_tap_ring = ring;
    g_newest_seq = 0;
    memset(g_clients, 0, sizeof(g_clients));
    pthread_mutex_unlock(&g_tap_mutex);

    FILE *fp = fopen(IAV_PROXY_HOST_LOG, "w");
    if (fp) {
        fprintf(fp, "# IAV Tap Proxy Live Inference Trace Log\n");
        fclose(fp);
    }
    return 0;
}

void iav_proxy_cleanup(void)
{
    pthread_mutex_lock(&g_tap_mutex);
    g_tap_ring = NULL;
    memset(g_clients, 0, sizeof(g_clients));
    pthread_mutex_unlock(&g_tap_mutex);
}

/*
 * A client starts strictly after the newest frame published at attach time.
 * Clients that have not consumed a frame yet are moved to the same start so a
 * cohort begins on one common sequence. Older published frames are discarded.
 */
int iav_proxy_attach(uint32_t client_cid, uint32_t session_id)
{
    int free_slot = -1;
    int slot = -1;
    uint64_t now_us = proxy_mono_us();

    pthread_mutex_lock(&g_tap_mutex);
    if (!g_tap_ring) {
        pthread_mutex_unlock(&g_tap_mutex);
        return -ENODEV;
    }

    proxy_evict_idle_locked(now_us, -1);

    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].cid == client_cid &&
            g_clients[i].session_id == session_id)
            proxy_drop_client_locked(i, "replaced");
        if (!g_clients[i].in_use && free_slot < 0)
            free_slot = i;
    }

    if (free_slot < 0) {
        pthread_mutex_unlock(&g_tap_mutex);
        return -EBUSY;
    }

    slot = free_slot;
    g_clients[slot].in_use = true;
    g_clients[slot].cid = client_cid;
    g_clients[slot].session_id = session_id;
    g_clients[slot].consumed = 0;
    g_clients[slot].last_active_us = now_us;

    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].consumed == 0)
            g_clients[i].last_seq = g_newest_seq;
    }
    proxy_sweep_slots_locked();

    proxy_host_log("[TAP_CLIENT] event=attach cid=%u session=%u start_after_seq=%llu "
                   "published=%llu drops=%llu missed=%llu\n",
                   client_cid, session_id, (unsigned long long)g_newest_seq,
                   (unsigned long long)g_tap_ring->published_count,
                   (unsigned long long)g_tap_ring->drop_count,
                   (unsigned long long)g_tap_ring->missed_count);
    pthread_mutex_unlock(&g_tap_mutex);

    printf("iav_proxy: client cid=%u session=%u attached to live camera tap (slot %d)\n",
           client_cid, session_id, slot);
    return 0;
}

int iav_proxy_detach(uint32_t client_cid, uint32_t session_id)
{
    pthread_mutex_lock(&g_tap_mutex);
    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].cid == client_cid &&
            g_clients[i].session_id == session_id) {
            proxy_drop_client_locked(i, "detach");
            pthread_mutex_unlock(&g_tap_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_tap_mutex);
    return -EINVAL;
}

void iav_proxy_client_disconnect(uint32_t client_cid)
{
    pthread_mutex_lock(&g_tap_mutex);
    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].cid == client_cid)
            proxy_drop_client_locked(i, "disconnect");
    }
    pthread_mutex_unlock(&g_tap_mutex);
}

int iav_proxy_active_client_count(void)
{
    int cnt;

    pthread_mutex_lock(&g_tap_mutex);
    cnt = proxy_active_clients_locked();
    pthread_mutex_unlock(&g_tap_mutex);
    return cnt;
}



static int iav_proxy_step_live_inference_internal(const ambarella_virt_v1_IavTapRunRequest *req,
                                                  uint32_t client_cid,
                                                  void *in_virt,
                                                  size_t in_size,
                                                  void *out_virt,
                                                  size_t out_size,
                                                  void *jpeg_virt,
                                                  size_t jpeg_handle_size,
                                                  ambarella_virt_v1_IavTapRunResponse *resp,
                                                  char *err_detail,
                                                  size_t err_detail_len)
{
    if (!g_tap_ring) return -ENODEV;

    if (access("/tmp/iav_tap_pause", F_OK) == 0) {
        resp->status = -EAGAIN;
        return -EAGAIN;
    }

    uint64_t now_us = proxy_mono_us();

    pthread_mutex_lock(&g_tap_mutex);

    struct iav_tap_client *client = NULL;
    int self = -1;
    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].cid == client_cid &&
            g_clients[i].session_id == req->session_id) {
            client = &g_clients[i];
            self = i;
            break;
        }
    }

    if (!client) {
        int free_slot = -1;
        for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
            if (!g_clients[i].in_use) {
                free_slot = i;
                break;
            }
        }
        if (free_slot < 0) {
            pthread_mutex_unlock(&g_tap_mutex);
            snprintf(err_detail, err_detail_len, "Max tap clients (%d) exceeded", MAX_IAV_TAP_CLIENTS);
            return -EBUSY;
        }
        client = &g_clients[free_slot];
        client->in_use = true;
        client->cid = client_cid;
        client->session_id = req->session_id;
        client->consumed = 0;
        client->drop_count = 0;
        client->last_seq = g_newest_seq > 0 ? (g_newest_seq - 1) : 0;
        self = free_slot;
    }

    client->last_active_us = now_us;
    proxy_evict_idle_locked(now_us, self);

    if (client->consumed == 0 && proxy_active_clients_locked() < (int)g_tap_cohort) {
        pthread_mutex_unlock(&g_tap_mutex);
        resp->status = -EAGAIN;
        return -EAGAIN;
    }

    struct iav_tap_slot *target_slot = NULL;
    int wait_attempts = 0;
    while (!target_slot && wait_attempts++ < 2) {
        if (req->take_latest) {
            for (int i = 0; i < IAV_TAP_RING_SLOTS; i++) {
                struct iav_tap_slot *slot = &g_tap_ring->slots[i];
                if (slot->state != IAV_TAP_SLOT_PUBLISHED)
                    continue;
                if (client->last_seq > 0 && slot->seq <= client->last_seq)
                    continue;
                if (!target_slot || slot->seq > target_slot->seq)
                    target_slot = slot;
            }
            if (target_slot) {
                if (client->last_seq > 0 && target_slot->seq > client->last_seq + 1) {
                    client->drop_count += (target_slot->seq - (client->last_seq + 1));
                }
            }
        } else {
            if (client->last_seq == 0) {
                /* Do not treat sequence 0 as a request for the oldest frame */
                client->last_seq = g_newest_seq > 0 ? (g_newest_seq - 1) : 0;
            }
            for (int i = 0; i < IAV_TAP_RING_SLOTS; i++) {
                struct iav_tap_slot *slot = &g_tap_ring->slots[i];
                if (slot->state != IAV_TAP_SLOT_PUBLISHED || slot->seq <= client->last_seq)
                    continue;
                if (!target_slot || slot->seq < target_slot->seq)
                    target_slot = slot;
            }
        }

        if (target_slot)
            break;

        /* Wait up to 40ms for the next 30 Hz sensor frame to arrive */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 40000000L; /* 40 ms */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&g_tap_cond, &g_tap_mutex, &ts);
        if (rc != 0) {
            break; /* Timed out */
        }
    }

    if (!target_slot) {
        resp->status = -EAGAIN;
        resp->active_seq = client->last_seq;
        resp->drop_count = client->drop_count + g_tap_ring->drop_count;
        pthread_mutex_unlock(&g_tap_mutex);
        return -EAGAIN;
    }

    target_slot->refcount++;
    uint64_t captured_seq = target_slot->seq;
    uint32_t slot_w = target_slot->width;
    uint32_t slot_h = target_slot->height;
    uint32_t slot_pitch = target_slot->pitch;
    uint32_t slot_fourcc = target_slot->fourcc;

    /* Fill tenant input handle with live camera frame */
    int transform_ok = slot_w > 0 && slot_h > 0;
    if (transform_ok && in_size == 3 * 640 * 640) {
        if (proxy_convert_nv12_to_yolox_rgb_640x640(target_slot->payload,
                                                    slot_w, slot_h, slot_pitch,
                                                    (uint8_t *)in_virt) != 0) {
            transform_ok = 0;
        }
    } else if (transform_ok && in_size == RESNET_INPUT_BYTES) {
        proxy_convert_nv12_to_resnet_fp16_224(target_slot->payload,
                                              slot_w, slot_h, slot_pitch,
                                              (uint16_t *)in_virt);
    } else {
        transform_ok = 0;
    }

    if (!transform_ok) {
        target_slot->refcount--;
        pthread_mutex_unlock(&g_tap_mutex);
        proxy_host_log("[TAP_RUN_ERROR] seq=%llu cid=%u reason=no_transform in_size=%zu "
                       "width=%u height=%u\n",
                       (unsigned long long)captured_seq, client_cid, in_size,
                       slot_w, slot_h);
        snprintf(err_detail, err_detail_len, "No transform for in_size %zu with %ux%u",
                 in_size, slot_w, slot_h);
        return -EINVAL;
    }

    /* Ingest DSP hardware-compressed JPEG into guest-owned handle if requested */
    if (req->want_jpeg) {
        if (target_slot->jpeg_size > 0) {
            uint32_t hw_jpeg_size = target_slot->jpeg_size;
            if (hw_jpeg_size > (uint32_t)req->jpeg_capacity || hw_jpeg_size > jpeg_handle_size) {
                target_slot->refcount--;
                pthread_mutex_unlock(&g_tap_mutex);
                snprintf(err_detail, err_detail_len,
                         "Hardware JPEG size %u exceeds capacity %u (handle size %zu)",
                         hw_jpeg_size, req->jpeg_capacity, jpeg_handle_size);
                return -ENOSPC;
            }
            memcpy(jpeg_virt, target_slot->jpeg_payload, hw_jpeg_size);
            resp->jpeg_handle_id = req->jpeg_handle_id;
            resp->jpeg_len = hw_jpeg_size;
        } else {
#ifdef BUILD_HOST_UNIT_TEST
            /* Mock JPEG payload ONLY for hermetic host unit testing without /dev/iav */
            static const uint8_t mock_jpeg[] = {
                0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F',
                0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
                0xFF, 0xDB, 0x00, 0x43, 0x00,
                0x10, 0x0B, 0x0C, 0x0E, 0x0C, 0x0A, 0x10, 0x0E,
                0x0D, 0x0E, 0x12, 0x11, 0x10, 0x13, 0x18, 0x28,
                0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x04, 0x38, 0x07, 0x80, 0x01, 0x01, 0x22, 0x00,
                0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00,
                0xAA, 0x55, 0xAA, 0x55,
                0xFF, 0xD9
            };
            uint32_t mock_len = (uint32_t)sizeof(mock_jpeg);
            if (mock_len > (uint32_t)req->jpeg_capacity || mock_len > jpeg_handle_size) {
                target_slot->refcount--;
                pthread_mutex_unlock(&g_tap_mutex);
                snprintf(err_detail, err_detail_len,
                         "Mock JPEG size %u exceeds capacity %u (handle size %zu)",
                         mock_len, req->jpeg_capacity, jpeg_handle_size);
                return -ENOSPC;
            }
            memcpy(jpeg_virt, mock_jpeg, mock_len);
            resp->jpeg_handle_id = req->jpeg_handle_id;
            resp->jpeg_len = mock_len;
#else
            /* Production live stream: NEVER emit fake mock JPEGs. If DSP hardware frame was not ready, return 0 */
            resp->jpeg_handle_id = 0;
            resp->jpeg_len = 0;
#endif
        }
    } else {
        resp->jpeg_handle_id = 0;
        resp->jpeg_len = 0;
    }

    /* Publish-then-copy contract: Drop refcount BEFORE DAG run */
    target_slot->refcount--;
    client->last_seq = captured_seq;
    client->consumed++;
    uint64_t client_drops = client->drop_count;
    uint64_t ring_drops = g_tap_ring->drop_count;
    uint64_t ring_published = g_tap_ring->published_count;
    uint64_t ring_missed = g_tap_ring->missed_count;
    proxy_sweep_slots_locked();
    pthread_mutex_unlock(&g_tap_mutex);

    /* 2. Execute registered DAG on hardware accelerator */
    uint32_t ticks = 0;
    uint32_t rval = 0;
    uint64_t sub_ns = 0, start_ns = 0, fin_ns = 0;
    int ret = cavalry_proxy_run_dag_with_handles(req->dag_id, req->in_handle_id, req->out_handle_id,
                                                 client_cid, req->session_id,
                                                 &ticks, &rval,
                                                 &sub_ns, &start_ns, &fin_ns);
    if (ret < 0) {
        fprintf(stderr, "iav_proxy: cavalry_proxy_run_dag_with_handles failed (ret=%d)\n", ret);
        proxy_host_log("[TAP_RUN_ERROR] seq=%llu cid=%u session=%u dag_id=%u ret=%d rval=0x%x\n",
                       (unsigned long long)captured_seq, client_cid, req->session_id,
                       req->dag_id, ret, rval);
        snprintf(err_detail, err_detail_len, "cavalry_proxy_run_dag_with_handles failed: %d", ret);
        return ret;
    }

    resp->status = 0;
    resp->active_seq = captured_seq;
    resp->drop_count = client_drops + ring_drops;
    resp->exec_ticks = ticks;
    resp->cavalry_rval = rval;
    resp->width = slot_w;
    resp->height = slot_h;
    resp->pitch = slot_pitch;
    resp->fourcc = slot_fourcc;

    /* 3. Output MD5 and logging (enabled only when /tmp/iav_proxy_md5 exists) */
    if (access("/tmp/iav_proxy_md5", F_OK) == 0) {
        char host_md5[33] = {0};
        if (out_virt && out_size > 0) {
            proxy_calc_md5(out_virt, out_size, host_md5);
        }
        char in_md5[33] = {0};
        if (in_virt && in_size > 0) {
            proxy_calc_md5(in_virt, in_size, in_md5);
        }

        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm;
        gmtime_r(&tv.tv_sec, &tm);
        char ts_buf[64];
        strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &tm);

        proxy_host_log("[TAP_HOST_RECORD] seq=%llu cid=%u session=%u input_md5=%s output_md5=%s "
                       "in_size=%zu out_size=%zu ticks=%u rval=0x%x dag_id=%u in_handle=%u out_handle=%u "
                       "published=%llu drops=%llu missed=%llu jpeg_len=%u "
                       "submit_ns=%llu start_ns=%llu finish_ns=%llu timestamp=%s\n",
                       (unsigned long long)captured_seq, client_cid, req->session_id, in_md5, host_md5,
                       in_size, out_size, ticks, rval, req->dag_id, req->in_handle_id, req->out_handle_id,
                       (unsigned long long)ring_published, (unsigned long long)resp->drop_count,
                       (unsigned long long)ring_missed, resp->jpeg_len,
                       (unsigned long long)sub_ns, (unsigned long long)start_ns,
                       (unsigned long long)fin_ns, ts_buf);
    }

    return 0;
}

int iav_proxy_handle_nanopb_rpc(const uint8_t *req_bytes,
                                size_t req_len,
                                uint8_t *resp_bytes,
                                size_t max_resp_len,
                                size_t *out_resp_len,
                                uint32_t client_cid)
{
    if (!req_bytes || !resp_bytes || !out_resp_len)
        return -EINVAL;

    pb_istream_t istream = pb_istream_from_buffer(req_bytes, req_len);
    ambarella_virt_v1_RpcEnvelope req_env = ambarella_virt_v1_RpcEnvelope_init_zero;
    ambarella_virt_v1_RpcEnvelope resp_env = ambarella_virt_v1_RpcEnvelope_init_zero;
    const ambarella_virt_v1_IavTapRunRequest *req = NULL;
    ambarella_virt_v1_IavTapRunResponse *resp = NULL;
    void *in_virt = NULL;
    size_t in_size = 0;
    void *out_virt = NULL;
    size_t out_size = 0;
    void *jpeg_virt = NULL;
    size_t jpeg_handle_size = 0;
    int ret = 0;

    if (!pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &req_env)) {
        resp_env.api_major = 1;
        resp_env.api_minor = 0;
        resp_env.request_id = 0;
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = -EBADMSG;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Protobuf decode failed: %s", PB_GET_ERROR(&istream));
        goto encode_out;
    }

    resp_env.api_major = 1;
    resp_env.api_minor = 0;
    resp_env.request_id = req_env.request_id;

    if (req_env.api_major != 1) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = -EPROTONOSUPPORT;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Unsupported api_major %u (expected 1)", req_env.api_major);
        goto encode_out;
    }

    if (req_env.which_body != ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = -EINVAL;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Expected iav_tap_run_request body");
        goto encode_out;
    }

    req = &req_env.body.iav_tap_run_request;

    if (req->session_id == 0) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = -EINVAL;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Invalid session_id 0");
        goto encode_out;
    }

    if (req->in_handle_id == 0 || req->out_handle_id == 0 ||
        req->in_handle_id == req->out_handle_id) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = -EINVAL;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Invalid in/out handle ids (in=%u out=%u)", req->in_handle_id, req->out_handle_id);
        goto encode_out;
    }

    /* Validate model input handle */
    ret = cavalry_proxy_get_handle_buffer(req->in_handle_id, client_cid, req->session_id,
                                          &in_virt, &in_size);
    if (ret < 0 || !in_virt) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = ret < 0 ? ret : -EINVAL;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Failed to get in_handle_id %u for cid %u", req->in_handle_id, client_cid);
        goto encode_out;
    }

    /* Requirement: Keep YOLOX input handle at 1,228,800 bytes */
    if (in_size != 3 * 640 * 640 && in_size != RESNET_INPUT_BYTES) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = -EINVAL;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Invalid in_handle size %zu (expected 1228800)", in_size);
        goto encode_out;
    }

    /* Validate model output handle */
    ret = cavalry_proxy_get_handle_buffer(req->out_handle_id, client_cid, req->session_id,
                                          &out_virt, &out_size);
    if (ret < 0 || !out_virt) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = ret < 0 ? ret : -EINVAL;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Failed to get out_handle_id %u for cid %u", req->out_handle_id, client_cid);
        goto encode_out;
    }

    /* Validate JPEG destination handle if want_jpeg is requested */
    if (req->want_jpeg) {
        if (req->jpeg_handle_id == 0 ||
            req->jpeg_handle_id == req->in_handle_id ||
            req->jpeg_handle_id == req->out_handle_id) {
            resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
            resp_env.body.error.status = -EINVAL;
            snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                     "jpeg_handle_id %u must be distinct from DAG in/out ports (%u, %u)",
                     req->jpeg_handle_id, req->in_handle_id, req->out_handle_id);
            goto encode_out;
        }

        ret = cavalry_proxy_get_handle_buffer(req->jpeg_handle_id, client_cid, req->session_id,
                                              &jpeg_virt, &jpeg_handle_size);
        if (ret < 0 || !jpeg_virt) {
            resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
            resp_env.body.error.status = ret < 0 ? ret : -EINVAL;
            snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                     "Failed to get jpeg_handle_id %u for cid %u (ret=%d)",
                     req->jpeg_handle_id, client_cid, ret);
            goto encode_out;
        }

        if (req->jpeg_capacity == 0 || req->jpeg_capacity > jpeg_handle_size) {
            resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
            resp_env.body.error.status = -EINVAL;
            snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                     "jpeg_capacity %u exceeds handle buffer size %zu",
                     req->jpeg_capacity, jpeg_handle_size);
            goto encode_out;
        }
    }

    if (!g_tap_ring) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = -ENODEV;
        snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                 "Live camera tap ring not initialized");
        goto encode_out;
    }

    /* Execute the live step */
    resp_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_response_tag;
    resp = &resp_env.body.iav_tap_run_response;
    *resp = (ambarella_virt_v1_IavTapRunResponse)ambarella_virt_v1_IavTapRunResponse_init_zero;

    ret = iav_proxy_step_live_inference_internal(req, client_cid, in_virt, in_size,
                                                 out_virt, out_size,
                                                 jpeg_virt, jpeg_handle_size,
                                                 resp, resp_env.body.error.detail,
                                                 sizeof(resp_env.body.error.detail));
    if (ret < 0 && ret != -EAGAIN) {
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
        resp_env.body.error.status = ret;
        if (resp_env.body.error.detail[0] == '\0') {
            snprintf(resp_env.body.error.detail, sizeof(resp_env.body.error.detail),
                     "Inference step failed with error %d", ret);
        }
        goto encode_out;
    }

encode_out: ;
    pb_ostream_t ostream = pb_ostream_from_buffer(resp_bytes, max_resp_len);
    if (!pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &resp_env)) {
        fprintf(stderr, "iav_proxy: failed to encode RpcEnvelope response: %s\n",
                PB_GET_ERROR(&ostream));
        return -EIO;
    }
    *out_resp_len = ostream.bytes_written;
    return 0;
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
