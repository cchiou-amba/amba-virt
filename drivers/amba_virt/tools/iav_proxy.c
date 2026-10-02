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
static void proxy_convert_nv12_to_yolox_rgb_640x640(const uint8_t *nv12_payload,
                                                    uint32_t width, uint32_t height, uint32_t pitch,
                                                    uint8_t *in_tensor)
{
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
        __fp16 h = (__fp16)(float)v;
        memcpy(&fp16_lut[v], &h, sizeof(h));
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
    uint64_t consumed;
    uint64_t last_active_us;
};

static struct iav_tap_ring *g_tap_ring = NULL;
static pthread_mutex_t g_tap_mutex = PTHREAD_MUTEX_INITIALIZER;
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
    uint64_t floor_seq = UINT64_MAX;
    bool any = false;

    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (g_clients[i].in_use) {
            any = true;
            if (g_clients[i].last_seq < floor_seq)
                floor_seq = g_clients[i].last_seq;
        }
    }

    for (int i = 0; i < IAV_TAP_RING_SLOTS; i++) {
        struct iav_tap_slot *slot = &g_tap_ring->slots[i];

        if (slot->state != IAV_TAP_SLOT_PUBLISHED || slot->refcount != 0)
            continue;
        if (!any || slot->seq <= floor_seq)
            slot->state = IAV_TAP_SLOT_EMPTY;
    }
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
    return slot->state == IAV_TAP_SLOT_PUBLISHED && slot->refcount == 0 &&
        proxy_active_clients_locked() == 0;
}

/* Caller holds the ring lock. */
void iav_proxy_note_published_locked(uint64_t seq)
{
    if (seq > g_newest_seq)
        g_newest_seq = seq;
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

int iav_proxy_handle_rpc(const struct iav_tap_rpc *req,
                         struct iav_tap_rpc *resp,
                         uint32_t client_cid)
{
    if (!req || !resp) return -EINVAL;

    memset(resp, 0, sizeof(*resp));
    resp->opcode = req->opcode;
    resp->client_cid = client_cid;
    resp->session_id = req->session_id;

    switch (req->opcode) {
    case IAV_TAP_OP_ATTACH:
        resp->status = iav_proxy_attach(client_cid, req->session_id);
        break;

    case IAV_TAP_OP_DETACH:
        resp->status = iav_proxy_detach(client_cid, req->session_id);
        break;

    case IAV_TAP_OP_GET_STATUS:
        pthread_mutex_lock(&g_tap_mutex);
        if (g_tap_ring) {
            resp->status = 0;
            resp->width = g_tap_ring->active_width;
            resp->height = g_tap_ring->active_height;
            resp->pitch = g_tap_ring->active_pitch;
            resp->fourcc = g_tap_ring->active_fourcc;
            resp->drop_count = (uint32_t)g_tap_ring->drop_count;
            resp->active_seq = (uint32_t)g_tap_ring->published_count;
        } else {
            resp->status = -ENODEV;
        }
        pthread_mutex_unlock(&g_tap_mutex);
        break;

    case IAV_TAP_OP_RUN_LIVE_DAG: {
        uint64_t seq = 0;
        uint32_t ticks = 0;
        uint32_t rval = 0;
        resp->status = iav_proxy_step_live_inference(req->dag_id,
                                                    req->in_handle_id,
                                                    req->out_handle_id,
                                                    client_cid,
                                                    req->session_id,
                                                    &seq,
                                                    &ticks,
                                                    &rval,
                                                    NULL,
                                                    NULL);
        resp->active_seq = (uint32_t)seq;
        resp->exec_ticks = ticks;
        resp->rval = rval;
        break;
    }

    default:
        resp->status = -ENOSYS;
        break;
    }

    return 0;
}

/*
 * Step Live Inference:
 * 1. Select the oldest PUBLISHED frame newer than this client's last frame.
 *    Never hand back a frame the client already consumed.
 * 2. Hold the slot while transforming it into the Path B input handle.
 * 3. Release the slot before VisORC runs; it returns to EMPTY once every
 *    attached client has consumed it.
 * 4. Dispatch DAG execution to real /dev/cavalry under visorc_hw_mutex.
 */
int iav_proxy_step_live_inference(uint32_t dag_id,
                                  uint32_t in_handle_id,
                                  uint32_t out_handle_id,
                                  uint32_t client_cid,
                                  uint32_t session_id,
                                  uint64_t *out_seq,
                                  uint32_t *out_ticks,
                                  uint32_t *out_rval,
                                  void *saved_input_copy,
                                  void *saved_output_copy)
{
    if (!g_tap_ring) return -ENODEV;

    /* 1. Retrieve handle input buffer pointer */
    void *in_virt = NULL;
    size_t in_size = 0;
    int ret = cavalry_proxy_get_handle_buffer(in_handle_id, client_cid, session_id,
                                             &in_virt, &in_size);
    if (ret < 0) {
        fprintf(stderr, "iav_proxy: failed to get input handle %u for cid %u (ret=%d)\n",
                in_handle_id, client_cid, ret);
        return ret;
    }

    if (access("/tmp/iav_tap_pause", F_OK) == 0) {
        return -EAGAIN;
    }

    uint64_t now_us = proxy_mono_us();

    pthread_mutex_lock(&g_tap_mutex);

    struct iav_tap_client *client = NULL;
    int self = -1;
    for (int i = 0; i < MAX_IAV_TAP_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].cid == client_cid &&
            g_clients[i].session_id == session_id) {
            client = &g_clients[i];
            self = i;
            break;
        }
    }

    if (!client) {
        pthread_mutex_unlock(&g_tap_mutex);
        return -ENOTCONN;
    }
    client->last_active_us = now_us;
    proxy_evict_idle_locked(now_us, self);

    if (client->consumed == 0 && proxy_active_clients_locked() < (int)g_tap_cohort) {
        pthread_mutex_unlock(&g_tap_mutex);
        return -EAGAIN;
    }

    struct iav_tap_slot *target_slot = NULL;
    for (int i = 0; i < IAV_TAP_RING_SLOTS; i++) {
        struct iav_tap_slot *slot = &g_tap_ring->slots[i];

        if (slot->state != IAV_TAP_SLOT_PUBLISHED || slot->seq <= client->last_seq)
            continue;
        if (!target_slot || slot->seq < target_slot->seq)
            target_slot = slot;
    }

    if (!target_slot) {
        pthread_mutex_unlock(&g_tap_mutex);
        return -EAGAIN;
    }

    target_slot->state = IAV_TAP_SLOT_HELD;
    target_slot->refcount++;
    uint64_t captured_seq = target_slot->seq;
    uint32_t nbytes = target_slot->nbytes;

    /* Fill tenant input handle with live camera frame */
    int transform_ok = target_slot->width > 0 && target_slot->height > 0;
    if (transform_ok && in_size == 3 * 640 * 640) {
        proxy_convert_nv12_to_yolox_rgb_640x640(target_slot->payload,
                                                target_slot->width,
                                                target_slot->height,
                                                target_slot->pitch,
                                                (uint8_t *)in_virt);
    } else if (transform_ok && in_size == RESNET_INPUT_BYTES) {
        proxy_convert_nv12_to_resnet_fp16_224(target_slot->payload,
                                              target_slot->width,
                                              target_slot->height,
                                              target_slot->pitch,
                                              (uint16_t *)in_virt);
    } else {
        transform_ok = 0;
    }
    if (!transform_ok) {
        uint32_t slot_w = target_slot->width, slot_h = target_slot->height;
        target_slot->refcount--;
        target_slot->state = IAV_TAP_SLOT_PUBLISHED;
        pthread_mutex_unlock(&g_tap_mutex);
        proxy_host_log("[TAP_RUN_ERROR] seq=%llu cid=%u reason=no_transform in_size=%zu "
                       "width=%u height=%u nbytes=%u\n",
                       (unsigned long long)captured_seq, client_cid, in_size,
                       slot_w, slot_h, nbytes);
        return -EINVAL;
    }
    if (saved_input_copy)
        memcpy(saved_input_copy, in_virt, in_size);

    /* Publish-then-copy contract: Drop refcount BEFORE DAG run */
    target_slot->refcount--;
    target_slot->state = IAV_TAP_SLOT_PUBLISHED;
    client->last_seq = captured_seq;
    client->consumed++;
    uint64_t ring_published = g_tap_ring->published_count;
    uint64_t ring_drops = g_tap_ring->drop_count;
    uint64_t ring_missed = g_tap_ring->missed_count;
    proxy_sweep_slots_locked();
    pthread_mutex_unlock(&g_tap_mutex);

    if (out_seq) *out_seq = captured_seq;

    /* 2. Execute registered DAG on hardware accelerator */
    uint32_t ticks = 0;
    uint32_t rval = 0;
    uint64_t sub_ns = 0, start_ns = 0, fin_ns = 0;
    ret = cavalry_proxy_run_dag_with_handles(dag_id, in_handle_id, out_handle_id,
                                             client_cid, session_id,
                                             &ticks, &rval,
                                             &sub_ns, &start_ns, &fin_ns);
    if (ret < 0) {
        fprintf(stderr, "iav_proxy: cavalry_proxy_run_dag_with_handles failed (ret=%d)\n", ret);
        proxy_host_log("[TAP_RUN_ERROR] seq=%llu cid=%u session=%u dag_id=%u ret=%d rval=0x%x\n",
                       (unsigned long long)captured_seq, client_cid, session_id, dag_id, ret, rval);
        return ret;
    }

    if (out_ticks) *out_ticks = ticks;
    if (out_rval) *out_rval = rval;

    /* 3. Retrieve output handle buffer and compute output MD5 */
    void *out_virt = NULL;
    size_t out_size = 0;
    char host_md5[33] = {0};
    if (out_handle_id) {
        if (cavalry_proxy_get_handle_buffer(out_handle_id, client_cid, session_id,
                                            &out_virt, &out_size) == 0 && out_virt && out_size > 0) {
            proxy_calc_md5(out_virt, out_size, host_md5);
            if (saved_output_copy) {
                memcpy(saved_output_copy, out_virt, out_size);
            }
        }
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

    /* 4. Log host tap execution paired to sequence */
    proxy_host_log("[TAP_HOST_RECORD] seq=%llu cid=%u session=%u input_md5=%s output_md5=%s "
                   "in_size=%zu out_size=%zu ticks=%u rval=0x%x dag_id=%u in_handle=%u out_handle=%u "
                   "published=%llu drops=%llu missed=%llu "
                   "submit_ts=%llu start_ts=%llu finish_ts=%llu timestamp=%s\n",
                   (unsigned long long)captured_seq, client_cid, session_id, in_md5, host_md5,
                   in_size, out_size, ticks, rval, dag_id, in_handle_id, out_handle_id,
                   (unsigned long long)ring_published, (unsigned long long)ring_drops,
                   (unsigned long long)ring_missed,
                   (unsigned long long)sub_ns, (unsigned long long)start_ns,
                   (unsigned long long)fin_ns, ts_buf);

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
