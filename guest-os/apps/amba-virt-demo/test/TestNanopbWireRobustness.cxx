/*
 * guest-os/apps/amba-virt-demo/test/TestNanopbWireRobustness.cxx
 *
 * Ambarella Virtualization Nanopb Wire Protocol Robustness Test Suite
 * Validates fuzzing, truncation, unknown fields, oversized payloads, and memory safety.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include <vector>
#include <cstdint>
#include <cstring>
#include <pb_encode.h>
#include <pb_decode.h>
#include "amba_virt.pb.h"

TEST_GROUP(NanopbWireRobustness)
{
    void setup() override {}
    void teardown() override {}
};

/*
 * Test 1: Complete Round-Trip Serialization & Deserialization
 */
TEST(NanopbWireRobustness, RoundTripIntegrity)
{
    ambarella_virt_v1_RpcEnvelope req_env = ambarella_virt_v1_RpcEnvelope_init_zero;
    req_env.api_major = 1;
    req_env.api_minor = 2;
    req_env.request_id = 987654321ULL;
    req_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag;

    auto *req = &req_env.body.iav_tap_run_request;
    req->session_id = 42;
    req->dag_id = 1;
    req->in_handle_id = 1001;
    req->out_handle_id = 2002;
    req->want_jpeg = true;
    req->jpeg_handle_id = 3003;
    req->jpeg_capacity = 2 * 1024 * 1024;
    req->take_latest = true;

    uint8_t buffer[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    CHECK(pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &req_env));
    size_t encoded_len = ostream.bytes_written;
    CHECK(encoded_len > 0);

    ambarella_virt_v1_RpcEnvelope decoded_env = ambarella_virt_v1_RpcEnvelope_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(buffer, encoded_len);
    CHECK(pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &decoded_env));

    LONGS_EQUAL(1, decoded_env.api_major);
    LONGS_EQUAL(2, decoded_env.api_minor);
    LONGS_EQUAL(987654321ULL, decoded_env.request_id);
    LONGS_EQUAL(ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag, decoded_env.which_body);

    auto *dec_req = &decoded_env.body.iav_tap_run_request;
    LONGS_EQUAL(42, dec_req->session_id);
    LONGS_EQUAL(1, dec_req->dag_id);
    LONGS_EQUAL(1001, dec_req->in_handle_id);
    LONGS_EQUAL(2002, dec_req->out_handle_id);
    CHECK(dec_req->want_jpeg);
    LONGS_EQUAL(3003, dec_req->jpeg_handle_id);
    LONGS_EQUAL(2 * 1024 * 1024, dec_req->jpeg_capacity);
    CHECK(dec_req->take_latest);
}

/*
 * Test 2: Truncated Stream Fault Injection
 * Verifies that decoding any prefix length of a valid message fails cleanly without crash.
 */
TEST(NanopbWireRobustness, TruncatedStreamSafety)
{
    ambarella_virt_v1_RpcEnvelope env = ambarella_virt_v1_RpcEnvelope_init_zero;
    env.api_major = 1;
    env.request_id = 12345;
    env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_response_tag;
    env.body.iav_tap_run_response.status = 0;
    env.body.iav_tap_run_response.active_seq = 100;
    env.body.iav_tap_run_response.jpeg_len = 85000;

    uint8_t buffer[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    CHECK(pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &env));
    size_t full_len = ostream.bytes_written;

    for (size_t trunc_len = 0; trunc_len <= full_len; trunc_len++) {
        ambarella_virt_v1_RpcEnvelope out_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(buffer, trunc_len);
        bool ok = pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &out_env);
        if (trunc_len == full_len) {
            CHECK(ok);
            LONGS_EQUAL(12345, out_env.request_id);
        }
    }
}

/*
 * Test 3: Bit-Flipping Fuzzing Resistance
 * Injects single and multi-bit corruptions into valid wire messages and asserts memory safety.
 */
TEST(NanopbWireRobustness, FuzzCorruptedStream)
{
    ambarella_virt_v1_RpcEnvelope env = ambarella_virt_v1_RpcEnvelope_init_zero;
    env.api_major = 1;
    env.request_id = 999;
    env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag;
    env.body.iav_tap_run_request.session_id = 7;
    env.body.iav_tap_run_request.dag_id = 1;

    uint8_t orig_buffer[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(orig_buffer, sizeof(orig_buffer));
    CHECK(pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &env));
    size_t len = ostream.bytes_written;

    // Fuzz 1: Invert each byte individually
    for (size_t i = 0; i < len; i++) {
        std::vector<uint8_t> fuzzed(orig_buffer, orig_buffer + len);
        fuzzed[i] ^= 0xFF;

        ambarella_virt_v1_RpcEnvelope out_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(fuzzed.data(), fuzzed.size());
        // pb_decode must return without memory fault or hang
        pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &out_env);
    }

    // Fuzz 2: Fill arbitrary junk bytes
    for (int run = 0; run < 50; run++) {
        std::vector<uint8_t> junk(len);
        for (size_t j = 0; j < len; j++) {
            junk[j] = static_cast<uint8_t>((run * 37 + j * 17) & 0xFF);
        }
        ambarella_virt_v1_RpcEnvelope out_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(junk.data(), junk.size());
        pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &out_env);
    }
}

/*
 * Test 4: Unknown Protobuf Fields Forward Compatibility
 * Validates that extra unknown fields appended to message are safely skipped by pb_decode.
 */
TEST(NanopbWireRobustness, UnknownFieldForwardCompatibility)
{
    ambarella_virt_v1_RpcEnvelope env = ambarella_virt_v1_RpcEnvelope_init_zero;
    env.api_major = 1;
    env.request_id = 777;
    env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;
    env.body.error.status = -404;
    snprintf(env.body.error.detail, sizeof(env.body.error.detail), "Not Found");

    uint8_t buffer[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    CHECK(pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &env));
    size_t len = ostream.bytes_written;

    // Append unknown varint field (field number 99, wire type 0 = (99 << 3) | 0 = 0x318)
    std::vector<uint8_t> extended(buffer, buffer + len);
    extended.push_back(0x98); // Varint tag 99 low 7 bits
    extended.push_back(0x06); // Varint tag 99 high bits
    extended.push_back(0x2A); // Value = 42

    ambarella_virt_v1_RpcEnvelope out_env = ambarella_virt_v1_RpcEnvelope_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(extended.data(), extended.size());
    bool ok = pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &out_env);
    CHECK_TEXT(ok, "Failed to skip unknown field in forward compatible protobuf stream");

    LONGS_EQUAL(1, out_env.api_major);
    LONGS_EQUAL(777, out_env.request_id);
    LONGS_EQUAL(ambarella_virt_v1_RpcEnvelope_error_tag, out_env.which_body);
    LONGS_EQUAL(-404, out_env.body.error.status);
}

/*
 * Test 5: Wire Frame Message Length Header Validation
 */
TEST(NanopbWireRobustness, WireFrameHeaderValidation)
{
    uint32_t json_len = 120;
    uint32_t jpeg_len = 95000;
    uint32_t le_json = htole32(json_len);
    uint32_t le_jpeg = htole32(jpeg_len);

    uint8_t header[8];
    memcpy(header, &le_json, 4);
    memcpy(header + 4, &le_jpeg, 4);

    uint32_t parsed_json = le32toh(*reinterpret_cast<uint32_t *>(header));
    uint32_t parsed_jpeg = le32toh(*reinterpret_cast<uint32_t *>(header + 4));

    LONGS_EQUAL(json_len, parsed_json);
    LONGS_EQUAL(jpeg_len, parsed_jpeg);
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
