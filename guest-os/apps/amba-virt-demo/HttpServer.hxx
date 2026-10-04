/*
 * HttpServer.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_HTTP_SERVER_HXX
#define AMBA_VIRT_HTTP_SERVER_HXX

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_MAX_METHOD_LEN     16
#define HTTP_MAX_URI_LEN        256
#define HTTP_MAX_PAYLOAD_SIZE   (64 * 1024)   /* 64 KB limit for standard payloads */
#define HTTP_DEFAULT_TIMEOUT_MS 2000

enum http_method {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_OPTIONS
};

enum http_route {
    HTTP_ROUTE_UNKNOWN = 0,
    HTTP_ROUTE_ROOT,         /* GET / or /index.html */
    HTTP_ROUTE_API_INFO,     /* GET /api/info or /health */
    HTTP_ROUTE_LIVE_STREAM,  /* GET /live/stream */
    HTTP_ROUTE_LIVE_PARAMS,  /* POST /live/params */
    HTTP_ROUTE_INFER,        /* POST /infer */
    HTTP_ROUTE_OPTIONS       /* OPTIONS * */
};

struct http_request {
    enum http_method method;
    char method_str[HTTP_MAX_METHOD_LEN];
    char uri[HTTP_MAX_URI_LEN];
    char query[HTTP_MAX_URI_LEN];
    enum http_route route;
    size_t content_length;
    const char *body;
    size_t body_len;
    bool headers_complete;
};

struct http_live_params {
    float conf_thresh;
    float nms_thresh;
};

/*
 * Parse an HTTP request buffer.
 * Populates req structure with method, route, uri, content_length, body pointer.
 * Returns 0 on success,
 * -EAGAIN if request is incomplete (needs more TCP chunks),
 * -EBADMSG or -EINVAL on malformed request / verb,
 * -EMSGSIZE on payload exceeding maximum size limit.
 */
int http_parse_request(const char *buf, size_t len, struct http_request *req);

/*
 * Parse and validate JSON parameters for POST /live/params.
 * Validates conf in [0.01, 1.0] and nms in [0.01, 1.0].
 * Returns 0 on success,
 * -EBADMSG on malformed JSON,
 * -ERANGE on out-of-bounds parameters (or clamps depending on policy).
 */
int http_parse_live_params(const char *json_body, size_t body_len,
                           struct http_live_params *params, bool strict_range);

/*
 * Configure socket timeouts for send and receive (SO_SNDTIMEO, SO_RCVTIMEO).
 * Sets both to timeout_ms (default 2000 ms).
 * Returns 0 on success, negative errno on failure.
 */
int http_configure_socket_timeouts(int fd, uint32_t timeout_ms);

/*
 * Send all data to socket with timeout handling.
 * Returns bytes sent on success,
 * -EPIPE / -ECONNRESET on client disconnect,
 * -ETIMEDOUT on socket send timeout.
 */
ssize_t http_send_all(int fd, const void *buf, size_t len);

/* Helper to format standard responses */
int http_format_options_response(char *out_buf, size_t max_len);
int http_format_400_bad_request(char *out_buf, size_t max_len, const char *msg);
int http_format_404_not_found(char *out_buf, size_t max_len);
int http_format_413_payload_too_large(char *out_buf, size_t max_len);
int http_format_422_unprocessable(char *out_buf, size_t max_len, const char *msg);
int http_format_503_service_unavailable(char *out_buf, size_t max_len);
int http_format_200_json(char *out_buf, size_t max_len, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* AMBA_VIRT_HTTP_SERVER_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
