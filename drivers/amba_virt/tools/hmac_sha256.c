/*
 * hmac_sha256.c
 *
 * Portable SHA-256, HMAC-SHA-256, and HKDF-SHA-256 implementation.
 * Conforms to FIPS 180-4, RFC 2104, and RFC 5869.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <string.h>
#include <stdint.h>
#include "hmac_sha256.h"

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIGMA0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIGMA1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define sigma0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define sigma1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t W[64];
    int i;

    for (i = 0; i < 16; i++) {
        W[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        W[i] = sigma1(W[i - 2]) + W[i - 7] + sigma0(W[i - 15]) + W[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t T1 = h + SIGMA1(e) + CH(e, f, g) + K[i] + W[i];
        uint32_t T2 = SIGMA0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
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

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t buf_idx = (size_t)(ctx->count & 0x3f);
    ctx->count += len;

    while (len > 0) {
        size_t to_fill = 64 - buf_idx;
        if (len < to_fill) {
            memcpy(&ctx->buffer[buf_idx], p, len);
            break;
        }
        memcpy(&ctx->buffer[buf_idx], p, to_fill);
        sha256_transform(ctx, ctx->buffer);
        p += to_fill;
        len -= to_fill;
        buf_idx = 0;
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t total_bits = ctx->count * 8;
    size_t buf_idx = (size_t)(ctx->count & 0x3f);
    int i;

    /* Padding byte 0x80 */
    ctx->buffer[buf_idx++] = 0x80;

    if (buf_idx > 56) {
        memset(&ctx->buffer[buf_idx], 0, 64 - buf_idx);
        sha256_transform(ctx, ctx->buffer);
        buf_idx = 0;
    }
    memset(&ctx->buffer[buf_idx], 0, 56 - buf_idx);

    /* 64-bit big-endian length */
    for (i = 7; i >= 0; i--) {
        ctx->buffer[56 + (7 - i)] = (uint8_t)((total_bits >> (i * 8)) & 0xff);
    }
    sha256_transform(ctx, ctx->buffer);

    for (i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)((ctx->state[i] >> 24) & 0xff);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xff);
    }

    memset(ctx, 0, sizeof(*ctx));
}

void sha256(const void *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const void *key, size_t key_len)
{
    uint8_t k[SHA256_BLOCK_SIZE];
    uint8_t k_ipad[SHA256_BLOCK_SIZE];
    uint8_t k_opad[SHA256_BLOCK_SIZE];
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
        k_ipad[i] = k[i] ^ 0x36;
        k_opad[i] = k[i] ^ 0x5c;
    }

    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, k_ipad, SHA256_BLOCK_SIZE);

    sha256_init(&ctx->outer);
    sha256_update(&ctx->outer, k_opad, SHA256_BLOCK_SIZE);

    memset(k, 0, sizeof(k));
    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
}

void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const void *data, size_t len)
{
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t inner_hash[SHA256_DIGEST_SIZE];
    sha256_final(&ctx->inner, inner_hash);
    sha256_update(&ctx->outer, inner_hash, SHA256_DIGEST_SIZE);
    sha256_final(&ctx->outer, out);
    memset(inner_hash, 0, sizeof(inner_hash));
}

void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_SIZE])
{
    hmac_sha256_ctx_t ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, data, data_len);
    hmac_sha256_final(&ctx, out);
}

void hkdf_sha256_extract(const void *salt, size_t salt_len,
                         const void *ikm, size_t ikm_len,
                         uint8_t prk[SHA256_DIGEST_SIZE])
{
    uint8_t default_salt[SHA256_DIGEST_SIZE];

    if (!salt || salt_len == 0) {
        memset(default_salt, 0, sizeof(default_salt));
        hmac_sha256(default_salt, sizeof(default_salt), ikm, ikm_len, prk);
    } else {
        hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    }
}

int hkdf_sha256_expand(const uint8_t prk[SHA256_DIGEST_SIZE],
                       const void *info, size_t info_len,
                       uint8_t *okm, size_t okm_len)
{
    uint8_t T[SHA256_DIGEST_SIZE];
    size_t T_len = 0;
    size_t where = 0;
    uint32_t N;
    uint8_t i;

    if (!okm)
        return -1;

    N = (uint32_t)((okm_len + SHA256_DIGEST_SIZE - 1) / SHA256_DIGEST_SIZE);
    if (N > 255)
        return -1;

    for (i = 1; i <= (uint8_t)N; i++) {
        hmac_sha256_ctx_t ctx;
        hmac_sha256_init(&ctx, prk, SHA256_DIGEST_SIZE);
        if (T_len > 0)
            hmac_sha256_update(&ctx, T, T_len);
        if (info && info_len > 0)
            hmac_sha256_update(&ctx, info, info_len);
        hmac_sha256_update(&ctx, &i, 1);
        hmac_sha256_final(&ctx, T);
        T_len = SHA256_DIGEST_SIZE;

        size_t to_copy = (okm_len - where > SHA256_DIGEST_SIZE) ?
                         SHA256_DIGEST_SIZE : (okm_len - where);
        memcpy(okm + where, T, to_copy);
        where += to_copy;
    }

    memset(T, 0, sizeof(T));
    return 0;
}

int hkdf_sha256(const void *salt, size_t salt_len,
                const void *ikm, size_t ikm_len,
                const void *info, size_t info_len,
                uint8_t *okm, size_t okm_len)
{
    uint8_t prk[SHA256_DIGEST_SIZE];
    int ret;

    hkdf_sha256_extract(salt, salt_len, ikm, ikm_len, prk);
    ret = hkdf_sha256_expand(prk, info, info_len, okm, okm_len);
    memset(prk, 0, sizeof(prk));
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
