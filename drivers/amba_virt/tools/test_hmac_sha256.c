/*
 * test_hmac_sha256.c
 *
 * NIST and RFC verification vectors for SHA-256, HMAC-SHA-256, and HKDF-SHA-256.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "hmac_sha256.h"

static int hex2bin(const char *hex, uint8_t *bin, size_t bin_len)
{
    size_t i;
    for (i = 0; i < bin_len; i++) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%02x", &val) != 1)
            return -1;
        bin[i] = (uint8_t)val;
    }
    return 0;
}

static void test_sha256_nist_vectors(void)
{
    uint8_t digest[32];
    uint8_t expected[32];

    printf("[TEST] NIST SHA-256 Vectors...\n");

    /* 1. Empty string */
    sha256("", 0, digest);
    hex2bin("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", expected, 32);
    assert(memcmp(digest, expected, 32) == 0);
    printf("  [PASS] Empty string digest matches.\n");

    /* 2. "abc" */
    sha256("abc", 3, digest);
    hex2bin("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, 32);
    assert(memcmp(digest, expected, 32) == 0);
    printf("  [PASS] 'abc' digest matches.\n");

    /* 3. 56-byte message */
    const char *msg56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(msg56, strlen(msg56), digest);
    hex2bin("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", expected, 32);
    assert(memcmp(digest, expected, 32) == 0);
    printf("  [PASS] 56-byte message digest matches.\n");
}

static void test_hmac_sha256_rfc4231_vectors(void)
{
    uint8_t out[32];
    uint8_t expected[32];
    uint8_t key[131];

    printf("[TEST] RFC 4231 HMAC-SHA-256 Vectors...\n");

    /* Test Case 1: Key = 20 bytes of 0x0b, Data = "Hi There" */
    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi There", 8, out);
    hex2bin("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", expected, 32);
    assert(memcmp(out, expected, 32) == 0);
    printf("  [PASS] RFC 4231 Case 1 matches.\n");

    /* Test Case 2: Key = "Jefe", Data = "what do ya want for nothing?" */
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    hex2bin("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", expected, 32);
    assert(memcmp(out, expected, 32) == 0);
    printf("  [PASS] RFC 4231 Case 2 matches.\n");

    /* Test Case 6: Key = 131 bytes of 0xaa (Key > block size), Data = "Test Using Larger Than Block-Size Key - Hash Key First" */
    memset(key, 0xaa, 131);
    const char *data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(key, 131, data6, strlen(data6), out);
    hex2bin("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", expected, 32);
    assert(memcmp(out, expected, 32) == 0);
    printf("  [PASS] RFC 4231 Case 6 (key > 64B) matches.\n");

    /* Test Case 7: Key = 131 bytes of 0xaa, Data = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm." */
    const char *data7 = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.";
    hmac_sha256(key, 131, data7, strlen(data7), out);
    hex2bin("9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", expected, 32);
    assert(memcmp(out, expected, 32) == 0);
    printf("  [PASS] RFC 4231 Case 7 (key > 64B & large data) matches.\n");
}

static void test_hkdf_sha256_rfc5869_vectors(void)
{
    uint8_t ikm[22];
    uint8_t salt[13];
    uint8_t info[10];
    uint8_t prk[32];
    uint8_t okm[42];
    uint8_t expected_prk[32];
    uint8_t expected_okm[42];

    printf("[TEST] RFC 5869 HKDF-SHA-256 Vectors...\n");

    /* RFC 5869 Test Case 1 */
    memset(ikm, 0x0b, 22);
    hex2bin("000102030405060708090a0b0c", salt, 13);
    hex2bin("f0f1f2f3f4f5f6f7f8f9", info, 10);

    hex2bin("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", expected_prk, 32);
    hex2bin("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", expected_okm, 42);

    hkdf_sha256_extract(salt, 13, ikm, 22, prk);
    assert(memcmp(prk, expected_prk, 32) == 0);
    printf("  [PASS] RFC 5869 HKDF-Extract matches.\n");

    int ret = hkdf_sha256_expand(prk, info, 10, okm, 42);
    assert(ret == 0);
    assert(memcmp(okm, expected_okm, 42) == 0);
    printf("  [PASS] RFC 5869 HKDF-Expand matches.\n");

    /* Full one-shot HKDF */
    memset(okm, 0, sizeof(okm));
    ret = hkdf_sha256(salt, 13, ikm, 22, info, 10, okm, 42);
    assert(ret == 0);
    assert(memcmp(okm, expected_okm, 42) == 0);
    printf("  [PASS] RFC 5869 one-shot HKDF matches.\n");
}

static void test_session_protocol_simulation(void)
{
    uint8_t hmac_key[32];
    uint8_t client_nonce[32];
    uint8_t server_nonce[32];
    uint8_t nonce_material[64];
    uint8_t prk[32];
    uint8_t session_key[32];
    uint8_t tag[32];
    uint8_t verify_tag[32];
    uint64_t seq = 1;
    uint8_t body[48];
    uint8_t signed_payload[sizeof(uint64_t) + sizeof(body)];
    int ret;

    printf("[TEST] Ambarella QNX Session Authentication Protocol Simulation...\n");

    /* Setup mock bootstrap key and nonces */
    memset(hmac_key, 0x42, sizeof(hmac_key));
    memset(client_nonce, 0x11, sizeof(client_nonce));
    memset(server_nonce, 0x22, sizeof(server_nonce));

    /* 1. Both compute PRK = HMAC-SHA-256(hmac_key, client_nonce || server_nonce) */
    memcpy(nonce_material, client_nonce, 32);
    memcpy(nonce_material + 32, server_nonce, 32);
    hmac_sha256(hmac_key, 32, nonce_material, 64, prk);

    /* 2. Both derive session_key with HKDF-SHA-256, info = "amba-dma-session-v1" */
    ret = hkdf_sha256_expand(prk, "amba-dma-session-v1", strlen("amba-dma-session-v1"),
                             session_key, 32);
    assert(ret == 0);

    /* 3. Compute tag = HMAC-SHA-256(session_key, sequence || body) */
    memset(body, 0x77, sizeof(body));
    memcpy(signed_payload, &seq, sizeof(seq));
    memcpy(signed_payload + sizeof(seq), body, sizeof(body));

    hmac_sha256(session_key, 32, signed_payload, sizeof(signed_payload), tag);

    /* 4. Broker verifies tag */
    hmac_sha256(session_key, 32, signed_payload, sizeof(signed_payload), verify_tag);
    assert(memcmp(tag, verify_tag, 32) == 0);
    printf("  [PASS] Valid message tag verified successfully.\n");

    /* 5. Forged message body must fail verification */
    signed_payload[sizeof(seq) + 5] ^= 0xff;
    hmac_sha256(session_key, 32, signed_payload, sizeof(signed_payload), verify_tag);
    assert(memcmp(tag, verify_tag, 32) != 0);
    printf("  [PASS] Forged message body correctly rejected.\n");

    /* 6. Replayed / altered sequence number must fail verification */
    uint64_t bad_seq = 2;
    memcpy(signed_payload, &bad_seq, sizeof(bad_seq));
    memcpy(signed_payload + sizeof(bad_seq), body, sizeof(body));
    hmac_sha256(session_key, 32, signed_payload, sizeof(signed_payload), verify_tag);
    assert(memcmp(tag, verify_tag, 32) != 0);
    printf("  [PASS] Altered sequence number correctly rejected.\n");
}

int main(void)
{
    printf("====================================================\n");
    printf(" Portable HMAC-SHA-256 & HKDF Vector Test Suite\n");
    printf("====================================================\n");

    test_sha256_nist_vectors();
    test_hmac_sha256_rfc4231_vectors();
    test_hkdf_sha256_rfc5869_vectors();
    test_session_protocol_simulation();

    printf("\n=== All HMAC / HKDF Vectors PASSED ===\n");
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
