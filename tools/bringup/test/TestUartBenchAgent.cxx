/*
 * tools/bringup/test/TestUartBenchAgent.cxx
 *
 * CppUTest test suite for UART benchmark wire protocol, CRC-32 IEEE 802.3,
 * SHA-256 FIPS 180-4, PRBS-9 LFSR payload generation, and frame integrity.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include <stdint.h>
#include <string.h>
#include <vector>

#include "../uart_bench_protocol.h"

TEST_GROUP(UartBenchAgent)
{
    void setup() override
    {
    }

    void teardown() override
    {
    }
};

TEST(UartBenchAgent, Crc32NominalStandardVector)
{
    const uint8_t standard_vec[] = "123456789";
    uint32_t crc = crc32_ieee(standard_vec, 9);
    LONGS_EQUAL(0xCBF43926U, crc);
}

TEST(UartBenchAgent, Crc32BoundaryEmptyAndSingleByte)
{
    /* 0-byte buffer: ~0xFFFFFFFF = 0x00000000 */
    uint32_t crc_empty = crc32_ieee(nullptr, 0);
    LONGS_EQUAL(0x00000000U, crc_empty);

    const uint8_t zero_byte[1] = { 0x00 };
    uint32_t crc_zero = crc32_ieee(zero_byte, 1);
    LONGS_EQUAL(0xD202EF8DU, crc_zero);

    const uint8_t ff_byte[1] = { 0xFF };
    uint32_t crc_ff = crc32_ieee(ff_byte, 1);
    LONGS_EQUAL(0xFF000000U, crc_ff);
}

TEST(UartBenchAgent, Crc32BitFlipSensitivity)
{
    uint8_t buffer[256];
    generate_prbs_payload(12345, buffer, sizeof(buffer));

    uint32_t base_crc = crc32_ieee(buffer, sizeof(buffer));

    /* Flip every bit across multiple offsets and verify CRC always differs */
    for (size_t offset = 0; offset < sizeof(buffer); offset += 17) {
        for (int bit = 0; bit < 8; bit++) {
            buffer[offset] ^= (1U << bit);
            uint32_t corrupted_crc = crc32_ieee(buffer, sizeof(buffer));
            CHECK(corrupted_crc != base_crc);
            buffer[offset] ^= (1U << bit); /* restore */
        }
    }
}

TEST(UartBenchAgent, Sha256NistStandardVectors)
{
    sha256_ctx_t ctx;
    uint8_t hash[32];
    char hex[65];

    /* Test vector 1: Empty string */
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"", 0);
    sha256_final(&ctx, hash);
    sha256_hex(hash, hex);
    STRCMP_EQUAL("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);

    /* Test vector 2: "abc" */
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"abc", 3);
    sha256_final(&ctx, hash);
    sha256_hex(hash, hex);
    STRCMP_EQUAL("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);

    /* Test vector 3: Multi-block 56-byte message */
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)msg, strlen(msg));
    sha256_final(&ctx, hash);
    sha256_hex(hash, hex);
    STRCMP_EQUAL("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", hex);
}

TEST(UartBenchAgent, Sha256ChunkedIncrementalMatchesSinglePass)
{
    uint8_t payload[500];
    generate_prbs_payload(999, payload, sizeof(payload));

    /* Single-pass hash */
    sha256_ctx_t ctx1;
    uint8_t hash1[32];
    sha256_init(&ctx1);
    sha256_update(&ctx1, payload, sizeof(payload));
    sha256_final(&ctx1, hash1);

    /* Chunked hash in varied slice sizes */
    sha256_ctx_t ctx2;
    uint8_t hash2[32];
    sha256_init(&ctx2);
    sha256_update(&ctx2, payload, 53);
    sha256_update(&ctx2, payload + 53, 128);
    sha256_update(&ctx2, payload + 53 + 128, sizeof(payload) - 53 - 128);
    sha256_final(&ctx2, hash2);

    MEMCMP_EQUAL(hash1, hash2, 32);
}

TEST(UartBenchAgent, PrbsDeterminismAndDiversity)
{
    uint8_t buf1[256];
    uint8_t buf2[256];
    uint8_t buf3[256];

    generate_prbs_payload(42, buf1, sizeof(buf1));
    generate_prbs_payload(42, buf2, sizeof(buf2));
    generate_prbs_payload(43, buf3, sizeof(buf3));

    /* Identical seeds produce bit-exact payloads */
    MEMCMP_EQUAL(buf1, buf2, sizeof(buf1));

    /* Different seeds produce different payloads */
    CHECK(memcmp(buf1, buf3, sizeof(buf1)) != 0);

    /* Zero seed falls back to default LFSR non-zero state */
    uint8_t buf_zero[64];
    generate_prbs_payload(0, buf_zero, sizeof(buf_zero));
    bool all_zeros = true;
    for (size_t i = 0; i < sizeof(buf_zero); i++) {
        if (buf_zero[i] != 0) {
            all_zeros = false;
            break;
        }
    }
    CHECK_FALSE(all_zeros);
}

TEST(UartBenchAgent, FrameHeaderPackingAndValidation)
{
    /* Enforce 16-byte packed structure contract */
    LONGS_EQUAL(16, sizeof(struct bench_frame_hdr));

    struct bench_frame_hdr hdr;
    hdr.magic = BENCH_MAGIC;
    hdr.trial_id = 1;
    hdr.seq = 42;
    hdr.payload_len = 1024;

    /* Nominal valid header */
    LONGS_EQUAL(0, bench_frame_validate_hdr(&hdr, 4096));

    /* Corrupted magic number */
    hdr.magic = 0xDEADBEEFU;
    LONGS_EQUAL(-2, bench_frame_validate_hdr(&hdr, 4096));
    hdr.magic = BENCH_MAGIC;

    /* Oversized payload exceeding max allowed */
    hdr.payload_len = 8192;
    LONGS_EQUAL(-3, bench_frame_validate_hdr(&hdr, 4096));

    /* Null pointer */
    LONGS_EQUAL(-1, bench_frame_validate_hdr(nullptr, 4096));
}

TEST(UartBenchAgent, FrameCrcVerificationNominalAndFaults)
{
    uint8_t payload[128];
    generate_prbs_payload(777, payload, sizeof(payload));
    uint32_t expected_crc = crc32_ieee(payload, sizeof(payload));

    /* Nominal match */
    LONGS_EQUAL(0, bench_frame_verify_crc(payload, sizeof(payload), expected_crc));

    /* Corrupted expected CRC */
    LONGS_EQUAL(1, bench_frame_verify_crc(payload, sizeof(payload), expected_crc ^ 0x00000001U));

    /* Corrupted payload data */
    payload[10] ^= 0x80;
    LONGS_EQUAL(1, bench_frame_verify_crc(payload, sizeof(payload), expected_crc));

    /* Null payload with non-zero length */
    LONGS_EQUAL(-1, bench_frame_verify_crc(nullptr, sizeof(payload), expected_crc));
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
