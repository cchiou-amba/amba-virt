/*
 * hmac_sha256.h
 *
 * Portable SHA-256, HMAC-SHA-256, and HKDF-SHA-256.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_HMAC_SHA256_H
#define AMBA_HMAC_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[SHA256_BLOCK_SIZE];
} sha256_ctx_t;

typedef struct {
    sha256_ctx_t inner;
    sha256_ctx_t outer;
} hmac_sha256_ctx_t;

/* SHA-256 APIs */
void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256(const void *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

/* HMAC-SHA-256 APIs */
void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const void *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

/* HKDF-SHA-256 APIs (RFC 5869) */
void hkdf_sha256_extract(const void *salt, size_t salt_len,
                         const void *ikm, size_t ikm_len,
                         uint8_t prk[SHA256_DIGEST_SIZE]);

int hkdf_sha256_expand(const uint8_t prk[SHA256_DIGEST_SIZE],
                       const void *info, size_t info_len,
                       uint8_t *okm, size_t okm_len);

int hkdf_sha256(const void *salt, size_t salt_len,
                const void *ikm, size_t ikm_len,
                const void *info, size_t info_len,
                uint8_t *okm, size_t okm_len);

#endif /* AMBA_HMAC_SHA256_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
