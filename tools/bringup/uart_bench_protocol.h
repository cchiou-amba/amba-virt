/*
 * tools/bringup/uart_bench_protocol.h
 *
 * Wire protocol definition, CRC-32, SHA-256, and PRBS generator
 * for Ambarella hardware serial benchmark tooling.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef TOOLS_BRINGUP_UART_BENCH_PROTOCOL_H
#define TOOLS_BRINGUP_UART_BENCH_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define BENCH_MAGIC 0x55415254U /* 'UART' in ASCII */

#pragma pack(push, 1)
struct bench_frame_hdr {
    uint32_t magic;
    uint32_t trial_id;
    uint32_t seq;
    uint32_t payload_len;
};
#pragma pack(pop)

/* ========================================================================== */
/* Portable CRC-32 (IEEE 802.3 Polynomial 0xEDB88320)                        */
/* ========================================================================== */

static inline uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(crc & 1)));
        }
    }
    return ~crc;
}

/* ========================================================================== */
/* Self-Contained SHA-256 Implementation (FIPS PUB 180-4)                     */
/* ========================================================================== */

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx_t;

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_EP1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static inline void sha256_transform(uint32_t state[8], const uint8_t data[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    uint32_t w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SHA256_SIG1(w[i - 2]) + w[i - 7] + SHA256_SIG0(w[i - 15]) + w[i - 16];
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static inline void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t buffer_idx = (size_t)(ctx->count & 63);
    ctx->count += len;

    size_t i = 0;
    if (buffer_idx > 0 && buffer_idx + len >= 64) {
        size_t fill = 64 - buffer_idx;
        memcpy(ctx->buffer + buffer_idx, data, fill);
        sha256_transform(ctx->state, ctx->buffer);
        i += fill;
        buffer_idx = 0;
    }
    while (i + 64 <= len) {
        sha256_transform(ctx->state, data + i);
        i += 64;
    }
    if (i < len) {
        memcpy(ctx->buffer + buffer_idx, data + i, len - i);
    }
}

static inline void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32])
{
    uint64_t total_bits = ctx->count * 8;
    size_t buffer_idx = (size_t)(ctx->count & 63);
    ctx->buffer[buffer_idx++] = 0x80;

    if (buffer_idx > 56) {
        memset(ctx->buffer + buffer_idx, 0, 64 - buffer_idx);
        sha256_transform(ctx->state, ctx->buffer);
        buffer_idx = 0;
    }
    memset(ctx->buffer + buffer_idx, 0, 56 - buffer_idx);
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(total_bits >> ((7 - i) * 8));
    }
    sha256_transform(ctx->state, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static inline void sha256_hex(const uint8_t hash[32], char hex_out[65])
{
    for (int i = 0; i < 32; i++) {
        sprintf(hex_out + (i * 2), "%02x", hash[i]);
    }
    hex_out[64] = '\0';
}

/* ========================================================================== */
/* PRBS Pattern Generator (Galois LFSR PRBS-9: x^9 + x^5 + 1)                 */
/* ========================================================================== */

static inline void generate_prbs_payload(uint32_t seed, uint8_t *buf, size_t len)
{
    uint32_t lfsr = (seed == 0) ? 0x1FEU : (seed & 0x1FFU);
    if (lfsr == 0) lfsr = 0x1FEU;

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            uint32_t bit = ((lfsr >> 8) ^ (lfsr >> 4)) & 1U;
            lfsr = ((lfsr << 1) | bit) & 0x1FFU;
            byte = (byte << 1) | bit;
        }
        buf[i] = byte;
    }
}

/* ========================================================================== */
/* Frame Validation & Verification Helpers                                    */
/* ========================================================================== */

static inline int bench_frame_validate_hdr(const struct bench_frame_hdr *hdr, size_t max_payload)
{
    if (!hdr) return -1;
    if (hdr->magic != BENCH_MAGIC) return -2;
    if (hdr->payload_len > max_payload) return -3;
    return 0;
}

static inline int bench_frame_verify_crc(const uint8_t *payload, size_t len, uint32_t expected_crc)
{
    if (!payload && len > 0) return -1;
    uint32_t actual = crc32_ieee(payload, len);
    return (actual == expected_crc) ? 0 : 1;
}

#endif /* TOOLS_BRINGUP_UART_BENCH_PROTOCOL_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
