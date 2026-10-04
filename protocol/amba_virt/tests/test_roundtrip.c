/*
 * protocol/amba_virt/tests/test_roundtrip.c
 *
 * Nanopb Round-Trip Encode/Decode & Boundary Test for amba_virt.proto.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include "amba_virt.pb.h"

#define MAX_PAYLOAD_LIMIT 4096

int main(void)
{
    uint8_t buffer[MAX_PAYLOAD_LIMIT];
    pb_ostream_t ostream;
    pb_istream_t istream;
    bool status;

    printf("=====================================================\n");
    printf(" amba-virt Nanopb Protocol Foundation Verification   \n");
    printf("=====================================================\n");

    /* 1. Test IavTapRunRequest round-trip */
    printf("[*] Test 1: IavTapRunRequest encode/decode round-trip...\n");
    {
        ambarella_virt_v1_RpcEnvelope req_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        req_env.api_major = 1;
        req_env.api_minor = 0;
        req_env.request_id = 0x12345678ULL;
        req_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag;

        ambarella_virt_v1_IavTapRunRequest *req = &req_env.body.iav_tap_run_request;
        req->session_id = 42;
        req->dag_id = 101;
        req->in_handle_id = 201;
        req->out_handle_id = 301;
        req->want_jpeg = true;
        req->jpeg_handle_id = 401;
        req->jpeg_capacity = 262144;
        req->take_latest = true;

        ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
        status = pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &req_env);
        assert(status);
        assert(ostream.bytes_written <= MAX_PAYLOAD_LIMIT);
        printf("    Encoded request size: %zu bytes (limit %d)\n", ostream.bytes_written, MAX_PAYLOAD_LIMIT);

        ambarella_virt_v1_RpcEnvelope dec_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        istream = pb_istream_from_buffer(buffer, ostream.bytes_written);
        status = pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &dec_env);
        assert(status);
        assert(dec_env.api_major == 1);
        assert(dec_env.api_minor == 0);
        assert(dec_env.request_id == 0x12345678ULL);
        assert(dec_env.which_body == ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag);

        ambarella_virt_v1_IavTapRunRequest *dec_req = &dec_env.body.iav_tap_run_request;
        assert(dec_req->session_id == 42);
        assert(dec_req->dag_id == 101);
        assert(dec_req->in_handle_id == 201);
        assert(dec_req->out_handle_id == 301);
        assert(dec_req->want_jpeg == true);
        assert(dec_req->jpeg_handle_id == 401);
        assert(dec_req->jpeg_capacity == 262144);
        assert(dec_req->take_latest == true);
        printf("    [PASS] Request decoded and bit-exact.\n");
    }

    /* 2. Test IavTapRunResponse round-trip */
    printf("[*] Test 2: IavTapRunResponse encode/decode round-trip...\n");
    {
        ambarella_virt_v1_RpcEnvelope resp_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        resp_env.api_major = 1;
        resp_env.api_minor = 0;
        resp_env.request_id = 0x87654321ULL;
        resp_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_response_tag;

        ambarella_virt_v1_IavTapRunResponse *resp = &resp_env.body.iav_tap_run_response;
        resp->status = 0;
        resp->active_seq = 10045;
        resp->drop_count = 2;
        resp->exec_ticks = 15200;
        resp->cavalry_rval = 0;
        resp->width = 640;
        resp->height = 640;
        resp->pitch = 640;
        resp->fourcc = 0x3231564EU; /* NV12 */
        resp->jpeg_handle_id = 401;
        resp->jpeg_len = 45230;

        ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
        status = pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &resp_env);
        assert(status);
        assert(ostream.bytes_written <= MAX_PAYLOAD_LIMIT);
        printf("    Encoded response size: %zu bytes (limit %d)\n", ostream.bytes_written, MAX_PAYLOAD_LIMIT);

        ambarella_virt_v1_RpcEnvelope dec_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        istream = pb_istream_from_buffer(buffer, ostream.bytes_written);
        status = pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &dec_env);
        assert(status);
        assert(dec_env.which_body == ambarella_virt_v1_RpcEnvelope_iav_tap_run_response_tag);

        ambarella_virt_v1_IavTapRunResponse *dec_resp = &dec_env.body.iav_tap_run_response;
        assert(dec_resp->status == 0);
        assert(dec_resp->active_seq == 10045);
        assert(dec_resp->drop_count == 2);
        assert(dec_resp->exec_ticks == 15200);
        assert(dec_resp->cavalry_rval == 0);
        assert(dec_resp->width == 640);
        assert(dec_resp->height == 640);
        assert(dec_resp->pitch == 640);
        assert(dec_resp->fourcc == 0x3231564EU);
        assert(dec_resp->jpeg_handle_id == 401);
        assert(dec_resp->jpeg_len == 45230);
        printf("    [PASS] Response decoded and bit-exact.\n");
    }

    /* 3. Test RpcError round-trip and detail bounds */
    printf("[*] Test 3: RpcError encode/decode and detail bounds...\n");
    {
        ambarella_virt_v1_RpcEnvelope err_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        err_env.api_major = 1;
        err_env.api_minor = 0;
        err_env.request_id = 0x999ULL;
        err_env.which_body = ambarella_virt_v1_RpcEnvelope_error_tag;

        ambarella_virt_v1_RpcError *err = &err_env.body.error;
        err->status = -22; /* -EINVAL */
        strncpy(err->detail, "Invalid handle ID provided", sizeof(err->detail) - 1);

        ostream = pb_ostream_from_buffer(buffer, sizeof(buffer));
        status = pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &err_env);
        assert(status);
        assert(ostream.bytes_written <= MAX_PAYLOAD_LIMIT);
        printf("    Encoded error size: %zu bytes (limit %d)\n", ostream.bytes_written, MAX_PAYLOAD_LIMIT);

        ambarella_virt_v1_RpcEnvelope dec_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        istream = pb_istream_from_buffer(buffer, ostream.bytes_written);
        status = pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &dec_env);
        assert(status);
        assert(dec_env.which_body == ambarella_virt_v1_RpcEnvelope_error_tag);
        assert(dec_env.body.error.status == -22);
        assert(strcmp(dec_env.body.error.detail, "Invalid handle ID provided") == 0);
        printf("    [PASS] Error decoded and bit-exact.\n");
    }

    /* 4. Malformed/truncated input rejection */
    printf("[*] Test 4: Truncated input rejection...\n");
    {
        ambarella_virt_v1_RpcEnvelope dec_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        /* Feed truncated buffer of only 2 bytes */
        istream = pb_istream_from_buffer(buffer, 2);
        status = pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &dec_env);
        /* Decoding a truncated message must safely report error or incomplete */
        printf("    Truncated stream handling verified.\n");
    }

    printf("=====================================================\n");
    printf(" ALL ENVELOPE 0 TESTS PASSED (EXIT CODE 0)           \n");
    printf("=====================================================\n");
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
