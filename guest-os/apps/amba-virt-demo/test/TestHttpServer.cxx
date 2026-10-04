/*
 * TestHttpServer.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include "../HttpServer.hxx"
#include "../StreamBroadcaster.hxx"
#include "stb/stb_image.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <vector>
#include <string>

TEST_GROUP(HttpServer)
{
    void setup() override {}
    void teardown() override {}
};

TEST(HttpServer, NominalRouteDispatch)
{
    struct http_request req;

    /* GET /live/stream */
    const char *get_stream = "GET /live/stream HTTP/1.1\r\n"
                             "Host: localhost:8080\r\n"
                             "User-Agent: test\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(get_stream, strlen(get_stream), &req));
    LONGS_EQUAL(HTTP_METHOD_GET, req.method);
    LONGS_EQUAL(HTTP_ROUTE_LIVE_STREAM, req.route);
    STRCMP_EQUAL("/live/stream", req.uri);

    /* GET /api/info */
    const char *get_info = "GET /api/info HTTP/1.1\r\n"
                           "Host: localhost:8080\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(get_info, strlen(get_info), &req));
    LONGS_EQUAL(HTTP_METHOD_GET, req.method);
    LONGS_EQUAL(HTTP_ROUTE_API_INFO, req.route);

    /* POST /live/params */
    const char *post_params = "POST /live/params HTTP/1.1\r\n"
                              "Host: localhost:8080\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: 25\r\n\r\n"
                              "{\"conf\":0.40,\"nms\":0.50}\n";
    LONGS_EQUAL(0, http_parse_request(post_params, strlen(post_params), &req));
    LONGS_EQUAL(HTTP_METHOD_POST, req.method);
    LONGS_EQUAL(HTTP_ROUTE_LIVE_PARAMS, req.route);
    LONGS_EQUAL(25, req.content_length);
    CHECK(req.body != nullptr);

    char resp[512];
    int len = http_format_200_json(resp, sizeof(resp), "{\"status\":\"ok\"}\n");
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 200 OK") != nullptr);
    CHECK(strstr(resp, "Content-Type: application/json") != nullptr);
}

TEST(HttpServer, OptionsCorsHeaders)
{
    struct http_request req;
    const char *options_req = "OPTIONS /live/stream HTTP/1.1\r\n"
                              "Host: localhost:8080\r\n"
                              "Origin: http://localhost:3000\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(options_req, strlen(options_req), &req));
    LONGS_EQUAL(HTTP_METHOD_OPTIONS, req.method);
    LONGS_EQUAL(HTTP_ROUTE_OPTIONS, req.route);

    char resp[512];
    int len = http_format_options_response(resp, sizeof(resp));
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 204 No Content") != nullptr);
    CHECK(strstr(resp, "Access-Control-Allow-Origin: *") != nullptr);
    CHECK(strstr(resp, "Access-Control-Allow-Methods: GET, POST, OPTIONS") != nullptr);
}

TEST(HttpServer, ParameterBoundaryValues)
{
    struct http_live_params params;
    params.conf_thresh = 0.30f;
    params.nms_thresh = 0.45f;

    /* Boundary minimum: conf = 0.01, nms = 0.01 */
    const char *min_json = "{\"conf\": 0.01, \"nms\": 0.01}";
    LONGS_EQUAL(0, http_parse_live_params(min_json, strlen(min_json), &params, true));
    DOUBLES_EQUAL(0.01, (double)params.conf_thresh, 1e-4);
    DOUBLES_EQUAL(0.01, (double)params.nms_thresh, 1e-4);

    /* Boundary maximum: conf = 1.0, nms = 1.0 */
    const char *max_json = "{\"conf\": 1.0, \"nms\": 1.0}";
    LONGS_EQUAL(0, http_parse_live_params(max_json, strlen(max_json), &params, true));
    DOUBLES_EQUAL(1.0, (double)params.conf_thresh, 1e-4);
    DOUBLES_EQUAL(1.0, (double)params.nms_thresh, 1e-4);
}

TEST(HttpServer, ChunkedTcpFragmentParsing)
{
    struct http_request req;
    const char *full_msg = "POST /live/params HTTP/1.1\r\n"
                           "Host: localhost:8080\r\n"
                           "Content-Length: 14\r\n\r\n"
                           "{\"conf\": 0.55}";
    size_t full_len = strlen(full_msg);

    char fragment[256] = { 0 };

    /* Feed byte by byte up to headers end */
    for (size_t i = 1; i < 40; i++) {
        memcpy(fragment, full_msg, i);
        int rc = http_parse_request(fragment, i, &req);
        LONGS_EQUAL(-EAGAIN, rc);
    }

    /* Feed complete header but incomplete body */
    size_t header_len = strstr(full_msg, "\r\n\r\n") - full_msg + 4;
    memcpy(fragment, full_msg, header_len + 5);
    LONGS_EQUAL(-EAGAIN, http_parse_request(fragment, header_len + 5, &req));

    /* Feed full message */
    LONGS_EQUAL(0, http_parse_request(full_msg, full_len, &req));
    LONGS_EQUAL(HTTP_ROUTE_LIVE_PARAMS, req.route);
    LONGS_EQUAL(14, req.content_length);
    STRCMP_EQUAL("{\"conf\": 0.55}", req.body);
}

TEST(HttpServer, FaultInjectionMalformedRequests)
{
    struct http_request req;

    /* Invalid HTTP verb */
    const char *bad_verb = "DELETE /live/stream HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(-EBADMSG, http_parse_request(bad_verb, strlen(bad_verb), &req));

    /* Missing HTTP version */
    const char *bad_proto = "GET /live/stream\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(-EBADMSG, http_parse_request(bad_proto, strlen(bad_proto), &req));

    /* Random garbage binary */
    const uint8_t garbage[] = { 0xFF, 0xFE, 0x00, 0x12, 0x34, 0x56, '\r', '\n', '\r', '\n' };
    LONGS_EQUAL(-EBADMSG, http_parse_request((const char *)garbage, sizeof(garbage), &req));

    char resp[512];
    int len = http_format_400_bad_request(resp, sizeof(resp), "Malformed request line");
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 400 Bad Request") != nullptr);
}

TEST(HttpServer, FaultInjectionNotFoundRoutes)
{
    struct http_request req;
    const char *not_found_req = "GET /nonexistent HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(not_found_req, strlen(not_found_req), &req));
    LONGS_EQUAL(HTTP_ROUTE_UNKNOWN, req.route);

    char resp[512];
    int len = http_format_404_not_found(resp, sizeof(resp));
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 404 Not Found") != nullptr);
}

TEST(HttpServer, FaultInjectionOversizedPayload)
{
    struct http_request req;
    const char *oversized_req = "POST /live/params HTTP/1.1\r\n"
                                "Host: test\r\n"
                                "Content-Length: 100000\r\n\r\n";
    LONGS_EQUAL(-EMSGSIZE, http_parse_request(oversized_req, strlen(oversized_req), &req));

    char resp[512];
    int len = http_format_413_payload_too_large(resp, sizeof(resp));
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 413 Payload Too Large") != nullptr);
}

TEST(HttpServer, FaultInjectionCorruptJsonParams)
{
    struct http_live_params params;
    params.conf_thresh = 0.30f;
    params.nms_thresh = 0.45f;

    /* Missing value */
    const char *corrupt_1 = "{\"conf\": invalid}";
    LONGS_EQUAL(-EBADMSG, http_parse_live_params(corrupt_1, strlen(corrupt_1), &params, false));

    /* Missing colon */
    const char *corrupt_2 = "{\"conf\" 0.5}";
    LONGS_EQUAL(-EBADMSG, http_parse_live_params(corrupt_2, strlen(corrupt_2), &params, false));

    /* Missing closing brace */
    const char *corrupt_3 = "{\"conf\": 0.5";
    LONGS_EQUAL(-EBADMSG, http_parse_live_params(corrupt_3, strlen(corrupt_3), &params, false));

    /* Not JSON */
    const char *corrupt_4 = "plain string without braces";
    LONGS_EQUAL(-EBADMSG, http_parse_live_params(corrupt_4, strlen(corrupt_4), &params, false));
}

TEST(HttpServer, FaultInjectionOutOfBoundsParams)
{
    struct http_live_params params;
    params.conf_thresh = 0.30f;
    params.nms_thresh = 0.45f;

    /* Strict range checks: out of bounds rejected with -ERANGE */
    const char *out_upper = "{\"conf\": 5.0}";
    LONGS_EQUAL(-ERANGE, http_parse_live_params(out_upper, strlen(out_upper), &params, true));

    const char *out_lower = "{\"nms\": -0.5}";
    LONGS_EQUAL(-ERANGE, http_parse_live_params(out_lower, strlen(out_lower), &params, true));

    /* Lenient mode: clamped into [0.01, 1.0] */
    LONGS_EQUAL(0, http_parse_live_params(out_upper, strlen(out_upper), &params, false));
    DOUBLES_EQUAL(1.0, (double)params.conf_thresh, 1e-4);

    LONGS_EQUAL(0, http_parse_live_params(out_lower, strlen(out_lower), &params, false));
    DOUBLES_EQUAL(0.01, (double)params.nms_thresh, 1e-4);

    char resp[512];
    int len = http_format_422_unprocessable(resp, sizeof(resp), "Parameters must be in range [0.01, 1.0]");
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 422 Unprocessable") != nullptr);
}

TEST(HttpServer, SocketSendTimeoutReap)
{
    int fds[2];
    LONGS_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    /* Configure 2KB send buffer and 50ms send timeout on sender */
    int sndbuf = 2048;
    LONGS_EQUAL(0, setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)));
    LONGS_EQUAL(0, http_configure_socket_timeouts(fds[0], 50));

    /* Saturate socket send buffer without reading on fds[1] */
    size_t big_size = 512 * 1024;
    char *big_buf = (char *)malloc(big_size);
    CHECK(big_buf != nullptr);
    memset(big_buf, 'Z', big_size);

    ssize_t res = http_send_all(fds[0], big_buf, big_size);
    LONGS_EQUAL(-ETIMEDOUT, res);

    free(big_buf);
    close(fds[0]);
    close(fds[1]);
}

/*
 * =========================================================================
 * 1. Unit Testing: HTTP_ROUTE_INFER & Response Formatters
 * =========================================================================
 */
TEST(HttpServer, InferRouteAndQueryParamParsing)
{
    struct http_request req;

    /* Base POST /infer without query string */
    const char *post_infer = "POST /infer HTTP/1.1\r\n"
                             "Host: localhost:8080\r\n"
                             "Content-Length: 10\r\n\r\n"
                             "0123456789";
    LONGS_EQUAL(0, http_parse_request(post_infer, strlen(post_infer), &req));
    LONGS_EQUAL(HTTP_METHOD_POST, req.method);
    LONGS_EQUAL(HTTP_ROUTE_INFER, req.route);
    STRCMP_EQUAL("/infer", req.uri);
    STRCMP_EQUAL("", req.query);
    LONGS_EQUAL(10, req.content_length);
    LONGS_EQUAL(10, req.body_len);
    MEMCMP_EQUAL("0123456789", req.body, 10);

    /* POST /infer with both thresh and nms */
    const char *post_query = "POST /infer?thresh=0.25&nms=0.50 HTTP/1.1\r\n"
                             "Host: localhost:8080\r\n"
                             "Content-Length: 0\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(post_query, strlen(post_query), &req));
    LONGS_EQUAL(HTTP_ROUTE_INFER, req.route);
    STRCMP_EQUAL("/infer", req.uri);
    STRCMP_EQUAL("thresh=0.25&nms=0.50", req.query);

    /* POST /infer with reversed parameter order */
    const char *post_query_rev = "POST /infer?nms=0.60&thresh=0.15 HTTP/1.1\r\n"
                                 "Host: localhost:8080\r\n"
                                 "Content-Length: 0\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(post_query_rev, strlen(post_query_rev), &req));
    LONGS_EQUAL(HTTP_ROUTE_INFER, req.route);
    STRCMP_EQUAL("/infer", req.uri);
    STRCMP_EQUAL("nms=0.60&thresh=0.15", req.query);

    /* POST /infer with single query parameter */
    const char *post_query_single = "POST /infer?thresh=0.40 HTTP/1.1\r\n"
                                    "Host: localhost:8080\r\n"
                                    "Content-Length: 0\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(post_query_single, strlen(post_query_single), &req));
    LONGS_EQUAL(HTTP_ROUTE_INFER, req.route);
    STRCMP_EQUAL("thresh=0.40", req.query);
}

TEST(HttpServer, Format503ServiceUnavailable)
{
    char resp[512];
    int len = http_format_503_service_unavailable(resp, sizeof(resp));
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 503 Service Unavailable") != nullptr);
    CHECK(strstr(resp, "Content-Type: text/plain") != nullptr);
    CHECK(strstr(resp, "Access-Control-Allow-Origin: *") != nullptr);
    CHECK(strstr(resp, "Connection: close") != nullptr);
    CHECK(strstr(resp, "Maximum stream subscribers reached") != nullptr);
}

TEST(HttpServer, AllResponseFormattersParity)
{
    char buf[512];

    CHECK(http_format_options_response(buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "204 No Content") != nullptr);

    CHECK(http_format_400_bad_request(buf, sizeof(buf), "Test error") > 0);
    CHECK(strstr(buf, "400 Bad Request") != nullptr);
    CHECK(strstr(buf, "Test error") != nullptr);

    CHECK(http_format_404_not_found(buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "404 Not Found") != nullptr);

    CHECK(http_format_413_payload_too_large(buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "413 Payload Too Large") != nullptr);

    CHECK(http_format_422_unprocessable(buf, sizeof(buf), "Invalid entity") > 0);
    CHECK(strstr(buf, "422 Unprocessable") != nullptr);
    CHECK(strstr(buf, "Invalid entity") != nullptr);

    CHECK(http_format_503_service_unavailable(buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "503 Service Unavailable") != nullptr);

    CHECK(http_format_200_json(buf, sizeof(buf), "{\"k\":\"v\"}") > 0);
    CHECK(strstr(buf, "200 OK") != nullptr);
    CHECK(strstr(buf, "{\"k\":\"v\"}") != nullptr);
}

/*
 * =========================================================================
 * 2. Coverage Testing: All HTTP Methods, Routes & Delimiters
 * =========================================================================
 */
TEST(HttpServer, AllHttpMethodsAndRoutesCoverage)
{
    struct http_request req;

    /* GET / */
    const char *get_root = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(get_root, strlen(get_root), &req));
    LONGS_EQUAL(HTTP_ROUTE_ROOT, req.route);

    /* GET /index.html */
    const char *get_index = "GET /index.html HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(get_index, strlen(get_index), &req));
    LONGS_EQUAL(HTTP_ROUTE_ROOT, req.route);

    /* GET /health */
    const char *get_health = "GET /health HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(get_health, strlen(get_health), &req));
    LONGS_EQUAL(HTTP_ROUTE_API_INFO, req.route);

    /* GET /live/stream */
    const char *get_stream = "GET /live/stream HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(get_stream, strlen(get_stream), &req));
    LONGS_EQUAL(HTTP_ROUTE_LIVE_STREAM, req.route);

    /* Unsupported HTTP methods */
    const char *put_req = "PUT /live/params HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(-EBADMSG, http_parse_request(put_req, strlen(put_req), &req));

    const char *head_req = "HEAD /live/stream HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(-EBADMSG, http_parse_request(head_req, strlen(head_req), &req));

    const char *patch_req = "PATCH /api/info HTTP/1.1\r\nHost: test\r\n\r\n";
    LONGS_EQUAL(-EBADMSG, http_parse_request(patch_req, strlen(patch_req), &req));

    /* Unknown route under POST */
    const char *post_unknown = "POST /unhandled HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(post_unknown, strlen(post_unknown), &req));
    LONGS_EQUAL(HTTP_ROUTE_UNKNOWN, req.route);
}

TEST(HttpServer, HttpHeaderDelimiterAndLengthCoverage)
{
    struct http_request req;

    /* Single LF delimiter (\n\n) */
    const char *lf_delim = "GET /api/info HTTP/1.1\nHost: test\n\n";
    LONGS_EQUAL(0, http_parse_request(lf_delim, strlen(lf_delim), &req));
    LONGS_EQUAL(HTTP_ROUTE_API_INFO, req.route);
    CHECK(req.headers_complete);

    /* Case-insensitive and padded Content-Length */
    const char *padded_cl = "POST /live/params HTTP/1.1\r\n"
                            "Host: test\r\n"
                            "cOnTeNt-LeNgTh:   \t  16   \r\n\r\n"
                            "{\"conf\": 0.35}\n\n";
    LONGS_EQUAL(0, http_parse_request(padded_cl, strlen(padded_cl), &req));
    LONGS_EQUAL(16, req.content_length);

    /* Excessive header without delimiter rejected when > 8192 bytes */
    std::vector<char> big_header(9000, 'X');
    LONGS_EQUAL(-EBADMSG, http_parse_request(big_header.data(), big_header.size(), &req));
}

TEST(HttpServer, ClientDisconnectAndSocketResetCoverage)
{
    int fds[2];
    LONGS_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    /* Close receiving end to trigger immediate EPIPE / ECONNRESET on sender */
    close(fds[1]);

    const char test_data[] = "SAMPLE_PACKET_CONTENT";
    ssize_t sent = http_send_all(fds[0], test_data, sizeof(test_data));
    CHECK(sent < 0);
    CHECK(sent == -EPIPE || sent == -ECONNRESET);

    close(fds[0]);
}

/*
 * =========================================================================
 * 3. Boundary Testing: Payloads, Thresholds & Image Extremes
 * =========================================================================
 */
TEST(HttpServer, PayloadLengthBoundaries)
{
    struct http_request req;

    /* Content-Length 0 */
    const char *zero_len = "POST /live/params HTTP/1.1\r\n"
                           "Host: test\r\n"
                           "Content-Length: 0\r\n\r\n";
    LONGS_EQUAL(0, http_parse_request(zero_len, strlen(zero_len), &req));
    LONGS_EQUAL(0, req.content_length);
    LONGS_EQUAL(0, req.body_len);

    /* Content-Length exact 64KB for /live/params -> allowed (EAGAIN waiting for data) */
    char exact_64k[256];
    snprintf(exact_64k, sizeof(exact_64k),
             "POST /live/params HTTP/1.1\r\nHost: test\r\nContent-Length: %d\r\n\r\n",
             HTTP_MAX_PAYLOAD_SIZE);
    LONGS_EQUAL(-EAGAIN, http_parse_request(exact_64k, strlen(exact_64k), &req));
    LONGS_EQUAL(HTTP_MAX_PAYLOAD_SIZE, req.content_length);

    /* Content-Length 64KB + 1 for /live/params -> rejected with -EMSGSIZE */
    char over_64k[256];
    snprintf(over_64k, sizeof(over_64k),
             "POST /live/params HTTP/1.1\r\nHost: test\r\nContent-Length: %d\r\n\r\n",
             HTTP_MAX_PAYLOAD_SIZE + 1);
    LONGS_EQUAL(-EMSGSIZE, http_parse_request(over_64k, strlen(over_64k), &req));

    /* Content-Length exact 10MB for /infer -> allowed (EAGAIN waiting for data) */
    char exact_10mb[256];
    snprintf(exact_10mb, sizeof(exact_10mb),
             "POST /infer HTTP/1.1\r\nHost: test\r\nContent-Length: %d\r\n\r\n",
             10 * 1024 * 1024);
    LONGS_EQUAL(-EAGAIN, http_parse_request(exact_10mb, strlen(exact_10mb), &req));
    LONGS_EQUAL(10 * 1024 * 1024, req.content_length);

    /* Content-Length 10MB + 1 for /infer -> rejected with -EMSGSIZE */
    char over_10mb[256];
    snprintf(over_10mb, sizeof(over_10mb),
             "POST /infer HTTP/1.1\r\nHost: test\r\nContent-Length: %d\r\n\r\n",
             10 * 1024 * 1024 + 1);
    LONGS_EQUAL(-EMSGSIZE, http_parse_request(over_10mb, strlen(over_10mb), &req));
}

TEST(HttpServer, QueryParamBoundaryClamping)
{
    struct http_live_params params{};

    /* Exactly on lower bound 0.01 */
    const char *min_val = "{\"conf\": 0.01, \"nms\": 0.01}";
    LONGS_EQUAL(0, http_parse_live_params(min_val, strlen(min_val), &params, true));
    DOUBLES_EQUAL(0.01, (double)params.conf_thresh, 1e-4);
    DOUBLES_EQUAL(0.01, (double)params.nms_thresh, 1e-4);

    /* Exactly on upper bound 1.0 */
    const char *max_val = "{\"conf\": 1.0, \"nms\": 1.0}";
    LONGS_EQUAL(0, http_parse_live_params(max_val, strlen(max_val), &params, true));
    DOUBLES_EQUAL(1.0, (double)params.conf_thresh, 1e-4);
    DOUBLES_EQUAL(1.0, (double)params.nms_thresh, 1e-4);

    /* Strict mode: 0.009 rejected */
    const char *sub_min = "{\"conf\": 0.009}";
    LONGS_EQUAL(-ERANGE, http_parse_live_params(sub_min, strlen(sub_min), &params, true));

    /* Strict mode: 1.001 rejected */
    const char *super_max = "{\"nms\": 1.001}";
    LONGS_EQUAL(-ERANGE, http_parse_live_params(super_max, strlen(super_max), &params, true));
}

/*
 * =========================================================================
 * 4. Error Handling & Fault Injection: Chunked TCP & Corrupt Binaries
 * =========================================================================
 */
TEST(HttpServer, ChunkedTcpBinaryReassembly)
{
    struct http_request req;

    /* Build simulated 150 KB JPEG message header */
    const size_t image_size = 150 * 1024;
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "POST /infer?thresh=0.30 HTTP/1.1\r\n"
                        "Host: localhost:8080\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: %zu\r\n\r\n", image_size);

    std::vector<char> full_stream(hlen + image_size);
    memcpy(full_stream.data(), header, hlen);
    memset(full_stream.data() + hlen, 0xAA, image_size);

    /* Feed stream in 1 KB fragments */
    const size_t chunk_size = 1024;
    size_t current_len = 0;
    while (current_len + chunk_size < full_stream.size()) {
        current_len += chunk_size;
        int rc = http_parse_request(full_stream.data(), current_len, &req);
        LONGS_EQUAL(-EAGAIN, rc);
    }

    /* Final fragment completes message */
    LONGS_EQUAL(0, http_parse_request(full_stream.data(), full_stream.size(), &req));
    LONGS_EQUAL(HTTP_ROUTE_INFER, req.route);
    LONGS_EQUAL(image_size, req.content_length);
    LONGS_EQUAL(image_size, req.body_len);
    STRCMP_EQUAL("thresh=0.30", req.query);
    CHECK(req.body != nullptr);
    LONGS_EQUAL(0xAA, (uint8_t)req.body[0]);
    LONGS_EQUAL(0xAA, (uint8_t)req.body[image_size - 1]);
}

TEST(HttpServer, OversizedPayloadRejection413)
{
    struct http_request req;
    const char *over = "POST /infer HTTP/1.1\r\n"
                       "Host: localhost:8080\r\n"
                       "Content-Length: 15000000\r\n\r\n";
    LONGS_EQUAL(-EMSGSIZE, http_parse_request(over, strlen(over), &req));

    char resp[512];
    int len = http_format_413_payload_too_large(resp, sizeof(resp));
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 413 Payload Too Large") != nullptr);
}

TEST(HttpServer, CorruptedImageBytesRejection422)
{
    /* Corrupted 512 bytes of ASCII text pretending to be an image */
    const uint8_t corrupt_bytes[512] = "THIS IS NOT A VALID JPEG OR PNG IMAGE STREAM AT ALL!";
    int img_w = 0, img_h = 0, img_c = 0;
    uint8_t *img = stbi_load_from_memory(corrupt_bytes, sizeof(corrupt_bytes),
                                         &img_w, &img_h, &img_c, 3);
    CHECK(img == nullptr);
    LONGS_EQUAL(0, img_w);
    LONGS_EQUAL(0, img_h);

    char resp[512];
    int len = http_format_422_unprocessable(resp, sizeof(resp), "Failed to decode image");
    CHECK(len > 0);
    CHECK(strstr(resp, "HTTP/1.1 422 Unprocessable") != nullptr);
    CHECK(strstr(resp, "Failed to decode image") != nullptr);
}

/*
 * =========================================================================
 * 5. Integration Testing: Loopback Socketpair End-to-End Flows
 * =========================================================================
 */
TEST(HttpServer, EndToEndLoopbackApiInfo)
{
    int fds[2];
    LONGS_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    http_configure_socket_timeouts(fds[0], 2000);
    http_configure_socket_timeouts(fds[1], 2000);

    /* Client sends GET /api/info */
    const char *client_req = "GET /api/info HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    LONGS_EQUAL((ssize_t)strlen(client_req), send(fds[0], client_req, strlen(client_req), 0));

    /* Server receives and parses */
    char srv_buf[1024];
    ssize_t n = recv(fds[1], srv_buf, sizeof(srv_buf) - 1, 0);
    CHECK(n > 0);
    srv_buf[n] = '\0';

    struct http_request req;
    LONGS_EQUAL(0, http_parse_request(srv_buf, n, &req));
    LONGS_EQUAL(HTTP_ROUTE_API_INFO, req.route);

    /* Server formats response and sends */
    const char *json_body = "{\"status\":\"online\",\"machine\":\"aarch64\"}\n";
    char resp_buf[1024];
    int rlen = http_format_200_json(resp_buf, sizeof(resp_buf), json_body);
    LONGS_EQUAL(rlen, http_send_all(fds[1], resp_buf, rlen));

    /* Client receives and verifies */
    char client_recv[1024];
    ssize_t cr = recv(fds[0], client_recv, sizeof(client_recv) - 1, 0);
    CHECK(cr > 0);
    client_recv[cr] = '\0';
    CHECK(strstr(client_recv, "HTTP/1.1 200 OK") != nullptr);
    CHECK(strstr(client_recv, "Content-Type: application/json") != nullptr);
    CHECK(strstr(client_recv, "\"status\":\"online\"") != nullptr);

    close(fds[0]);
    close(fds[1]);
}

TEST(HttpServer, EndToEndLoopbackLiveParams)
{
    int fds[2];
    LONGS_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    http_configure_socket_timeouts(fds[0], 2000);
    http_configure_socket_timeouts(fds[1], 2000);

    /* Client sends POST /live/params */
    const char *params_req = "POST /live/params HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "Content-Length: 25\r\n\r\n"
                             "{\"conf\":0.40,\"nms\":0.50}\n";
    LONGS_EQUAL((ssize_t)strlen(params_req), send(fds[0], params_req, strlen(params_req), 0));

    /* Server reads and parses */
    char srv_buf[1024];
    ssize_t n = recv(fds[1], srv_buf, sizeof(srv_buf) - 1, 0);
    CHECK(n > 0);
    srv_buf[n] = '\0';

    struct http_request req;
    LONGS_EQUAL(0, http_parse_request(srv_buf, n, &req));
    LONGS_EQUAL(HTTP_ROUTE_LIVE_PARAMS, req.route);

    struct http_live_params params{};
    LONGS_EQUAL(0, http_parse_live_params(req.body, req.body_len, &params, true));
    DOUBLES_EQUAL(0.40, (double)params.conf_thresh, 1e-4);
    DOUBLES_EQUAL(0.50, (double)params.nms_thresh, 1e-4);

    char ok_resp[256];
    int rlen = http_format_200_json(ok_resp, sizeof(ok_resp), "{\"status\":\"ok\"}\n");
    LONGS_EQUAL(rlen, http_send_all(fds[1], ok_resp, rlen));

    char client_recv[512];
    ssize_t cr = recv(fds[0], client_recv, sizeof(client_recv) - 1, 0);
    CHECK(cr > 0);
    client_recv[cr] = '\0';
    CHECK(strstr(client_recv, "HTTP/1.1 200 OK") != nullptr);

    close(fds[0]);
    close(fds[1]);
}

TEST(HttpServer, EndToEndLoopbackInferCorrupt)
{
    int fds[2];
    LONGS_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    http_configure_socket_timeouts(fds[0], 2000);
    http_configure_socket_timeouts(fds[1], 2000);

    /* Client sends POST /infer with corrupted non-image content */
    const char *corrupt_req = "POST /infer HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "Content-Length: 12\r\n\r\n"
                              "INVALID_JPEG";
    LONGS_EQUAL((ssize_t)strlen(corrupt_req), send(fds[0], corrupt_req, strlen(corrupt_req), 0));

    /* Server receives and parses */
    char srv_buf[1024];
    ssize_t n = recv(fds[1], srv_buf, sizeof(srv_buf) - 1, 0);
    CHECK(n > 0);

    struct http_request req;
    LONGS_EQUAL(0, http_parse_request(srv_buf, n, &req));
    LONGS_EQUAL(HTTP_ROUTE_INFER, req.route);

    int img_w = 0, img_h = 0, img_c = 0;
    uint8_t *img = stbi_load_from_memory((const uint8_t *)req.body, req.body_len,
                                         &img_w, &img_h, &img_c, 3);
    CHECK(img == nullptr);

    /* Format 422 Unprocessable and send */
    char err_resp[256];
    int elen = http_format_422_unprocessable(err_resp, sizeof(err_resp), "Failed to decode image");
    LONGS_EQUAL(elen, http_send_all(fds[1], err_resp, elen));

    char client_recv[512];
    ssize_t cr = recv(fds[0], client_recv, sizeof(client_recv) - 1, 0);
    CHECK(cr > 0);
    client_recv[cr] = '\0';
    CHECK(strstr(client_recv, "HTTP/1.1 422 Unprocessable") != nullptr);
    CHECK(strstr(client_recv, "Failed to decode image") != nullptr);

    close(fds[0]);
    close(fds[1]);
}

TEST(HttpServer, EndToEndLoopbackStreamSaturation)
{
    /* Initialize broadcaster and fill 4 slots */
    struct stream_broadcaster sb;
    LONGS_EQUAL(0, stream_broadcaster_init(&sb, 1024 * 1024));

    int subs[4];
    for (int i = 0; i < 4; i++) {
        LONGS_EQUAL(0, stream_broadcaster_register(&sb, &subs[i]));
    }
    LONGS_EQUAL(4, sb.active_subscribers);

    /* 5th client arrives over socketpair */
    int fds[2];
    LONGS_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    http_configure_socket_timeouts(fds[0], 2000);
    http_configure_socket_timeouts(fds[1], 2000);

    int fifth_sub = -1;
    int reg_rc = stream_broadcaster_register(&sb, &fifth_sub);
    LONGS_EQUAL(-EBUSY, reg_rc);

    char err_resp[256];
    int elen = http_format_503_service_unavailable(err_resp, sizeof(err_resp));
    LONGS_EQUAL(elen, http_send_all(fds[1], err_resp, elen));

    char client_recv[512];
    ssize_t cr = recv(fds[0], client_recv, sizeof(client_recv) - 1, 0);
    CHECK(cr > 0);
    client_recv[cr] = '\0';
    CHECK(strstr(client_recv, "HTTP/1.1 503 Service Unavailable") != nullptr);
    CHECK(strstr(client_recv, "Maximum stream subscribers reached") != nullptr);

    close(fds[0]);
    close(fds[1]);
    for (int i = 0; i < 4; i++) stream_broadcaster_unregister(&sb, subs[i]);
    stream_broadcaster_destroy(&sb);
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
