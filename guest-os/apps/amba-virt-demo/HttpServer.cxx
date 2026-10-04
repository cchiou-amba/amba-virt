/*
 * HttpServer.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "HttpServer.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>

int http_parse_request(const char *buf, size_t len, struct http_request *req)
{
    if (!buf || !req)
        return -EINVAL;

    memset(req, 0, sizeof(*req));

    if (len == 0)
        return -EAGAIN;

    /* A valid HTTP request starts with an uppercase method name */
    if (!isupper((unsigned char)buf[0]))
        return -EBADMSG;

    /* Check for end of headers */
    const char *hdr_end = (const char *)memmem(buf, len, "\r\n\r\n", 4);
    size_t hdr_delim_len = 4;
    if (!hdr_end) {
        hdr_end = (const char *)memmem(buf, len, "\n\n", 2);
        hdr_delim_len = 2;
    }

    if (!hdr_end) {
        if (len > 8192)
            return -EBADMSG; /* Headers exceed max permissible size */
        return -EAGAIN;      /* Need more TCP data */
    }

    req->headers_complete = true;
    size_t header_len = (hdr_end - buf);

    /* Parse request line */
    const char *line_end = (const char *)memmem(buf, header_len, "\r\n", 2);
    if (!line_end) line_end = (const char *)memmem(buf, header_len, "\n", 1);
    if (!line_end || line_end > hdr_end)
        return -EBADMSG;

    char req_line[512];
    size_t req_line_len = line_end - buf;
    if (req_line_len >= sizeof(req_line))
        return -EBADMSG;

    memcpy(req_line, buf, req_line_len);
    req_line[req_line_len] = '\0';

    char full_uri[HTTP_MAX_URI_LEN] = { 0 };
    char proto[32] = { 0 };
    int matched = sscanf(req_line, "%15s %255s %31s", req->method_str, full_uri, proto);
    if (matched < 3)
        return -EBADMSG;

    /* Validate HTTP version */
    if (strncasecmp(proto, "HTTP/1.", 7) != 0 && strncasecmp(proto, "HTTP/2", 6) != 0)
        return -EBADMSG;

    /* Parse Method */
    if (strcmp(req->method_str, "GET") == 0) {
        req->method = HTTP_METHOD_GET;
    } else if (strcmp(req->method_str, "POST") == 0) {
        req->method = HTTP_METHOD_POST;
    } else if (strcmp(req->method_str, "OPTIONS") == 0) {
        req->method = HTTP_METHOD_OPTIONS;
    } else {
        req->method = HTTP_METHOD_UNKNOWN;
        return -EBADMSG;
    }

    /* Separate URI and query string */
    char *q = strchr(full_uri, '?');
    if (q) {
        *q = '\0';
        snprintf(req->uri, sizeof(req->uri), "%s", full_uri);
        snprintf(req->query, sizeof(req->query), "%s", q + 1);
    } else {
        snprintf(req->uri, sizeof(req->uri), "%s", full_uri);
        req->query[0] = '\0';
    }

    /* Route Resolution */
    if (req->method == HTTP_METHOD_OPTIONS) {
        req->route = HTTP_ROUTE_OPTIONS;
    } else if (req->method == HTTP_METHOD_GET) {
        if (strcmp(req->uri, "/") == 0 || strcmp(req->uri, "/index.html") == 0) {
            req->route = HTTP_ROUTE_ROOT;
        } else if (strcmp(req->uri, "/api/info") == 0 || strcmp(req->uri, "/health") == 0) {
            req->route = HTTP_ROUTE_API_INFO;
        } else if (strcmp(req->uri, "/live/stream") == 0) {
            req->route = HTTP_ROUTE_LIVE_STREAM;
        } else {
            req->route = HTTP_ROUTE_UNKNOWN;
        }
    } else if (req->method == HTTP_METHOD_POST) {
        if (strcmp(req->uri, "/live/params") == 0) {
            req->route = HTTP_ROUTE_LIVE_PARAMS;
        } else if (strcmp(req->uri, "/infer") == 0) {
            req->route = HTTP_ROUTE_INFER;
        } else {
            req->route = HTTP_ROUTE_UNKNOWN;
        }
    }

    /* Content-Length extraction */
    const char *cl_pos = strcasestr(buf, "Content-Length:");
    if (cl_pos && cl_pos < hdr_end) {
        cl_pos += 15;
        while (*cl_pos == ' ' || *cl_pos == '\t') cl_pos++;
        req->content_length = strtoul(cl_pos, NULL, 10);
    } else {
        req->content_length = 0;
    }

    if (req->route == HTTP_ROUTE_INFER) {
        if (req->content_length > 10 * 1024 * 1024) {
            return -EMSGSIZE;
        }
    } else if (req->content_length > HTTP_MAX_PAYLOAD_SIZE) {
        return -EMSGSIZE;
    }

    /* Body pointers */
    req->body = hdr_end + hdr_delim_len;
    size_t total_body_recvd = len - (header_len + hdr_delim_len);
    req->body_len = total_body_recvd;

    if (req->content_length > 0 && req->body_len < req->content_length) {
        return -EAGAIN; /* Incomplete body payload */
    }

    return 0;
}

static const char *skip_whitespace(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

int http_parse_live_params(const char *json_body, size_t body_len,
                           struct http_live_params *params, bool strict_range)
{
    if (!json_body || !params || body_len == 0)
        return -EINVAL;

    const char *start = skip_whitespace(json_body);
    if (*start != '{')
        return -EBADMSG;

    bool found_any = false;
    float parsed_conf = params->conf_thresh;
    float parsed_nms = params->nms_thresh;

    /* Parse "conf": <val> */
    const char *p_conf = strstr(start, "\"conf\"");
    if (p_conf) {
        p_conf = strchr(p_conf + 6, ':');
        if (!p_conf)
            return -EBADMSG;
        p_conf = skip_whitespace(p_conf + 1);
        char *endptr = NULL;
        float val = strtof(p_conf, &endptr);
        if (endptr == p_conf || isnan(val) || isinf(val))
            return -EBADMSG;

        if (strict_range && (val < 0.01f || val > 1.0f))
            return -ERANGE;

        parsed_conf = fmaxf(0.01f, fminf(val, 1.0f));
        found_any = true;
    }

    /* Parse "nms": <val> */
    const char *p_nms = strstr(start, "\"nms\"");
    if (p_nms) {
        p_nms = strchr(p_nms + 5, ':');
        if (!p_nms)
            return -EBADMSG;
        p_nms = skip_whitespace(p_nms + 1);
        char *endptr = NULL;
        float val = strtof(p_nms, &endptr);
        if (endptr == p_nms || isnan(val) || isinf(val))
            return -EBADMSG;

        if (strict_range && (val < 0.01f || val > 1.0f))
            return -ERANGE;

        parsed_nms = fmaxf(0.01f, fminf(val, 1.0f));
        found_any = true;
    }

    /* Verify JSON ends with closing brace */
    const char *close_brace = strrchr(start, '}');
    if (!close_brace)
        return -EBADMSG;

    if (!found_any)
        return -EBADMSG;

    params->conf_thresh = parsed_conf;
    params->nms_thresh = parsed_nms;
    return 0;
}

int http_configure_socket_timeouts(int fd, uint32_t timeout_ms)
{
    if (fd < 0)
        return -EBADF;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        return -errno;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -errno;

    return 0;
}

ssize_t http_send_all(int fd, const void *buf, size_t len)
{
    if (fd < 0 || !buf)
        return -EINVAL;

    const char *p = (const char *)buf;
    size_t total_sent = 0;

    while (total_sent < len) {
        ssize_t sent = send(fd, p + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (sent > 0) {
            total_sent += (size_t)sent;
            continue;
        }

        if (sent < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -ETIMEDOUT;
            if (errno == EPIPE || errno == ECONNRESET)
                return -EPIPE;
            return -errno;
        }

        /* sent == 0, connection closed */
        return -EPIPE;
    }

    return (ssize_t)total_sent;
}

int http_format_options_response(char *out_buf, size_t max_len)
{
    return snprintf(out_buf, max_len,
                    "HTTP/1.1 204 No Content\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type\r\n"
                    "Connection: close\r\n\r\n");
}

int http_format_400_bad_request(char *out_buf, size_t max_len, const char *msg)
{
    const char *detail = msg ? msg : "Bad Request";
    return snprintf(out_buf, max_len,
                    "HTTP/1.1 400 Bad Request\r\n"
                    "Content-Type: text/plain\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n"
                    "400 Bad Request: %s\n", detail);
}

int http_format_404_not_found(char *out_buf, size_t max_len)
{
    return snprintf(out_buf, max_len,
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/plain\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n"
                    "404 Not Found\n");
}

int http_format_413_payload_too_large(char *out_buf, size_t max_len)
{
    return snprintf(out_buf, max_len,
                    "HTTP/1.1 413 Payload Too Large\r\n"
                    "Content-Type: text/plain\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n"
                    "413 Payload Too Large\n");
}

int http_format_422_unprocessable(char *out_buf, size_t max_len, const char *msg)
{
    const char *detail = msg ? msg : "Unprocessable Entity";
    return snprintf(out_buf, max_len,
                    "HTTP/1.1 422 Unprocessable Entity\r\n"
                    "Content-Type: text/plain\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n"
                    "422 Unprocessable: %s\n", detail);
}

int http_format_503_service_unavailable(char *out_buf, size_t max_len)
{
    return snprintf(out_buf, max_len,
                    "HTTP/1.1 503 Service Unavailable\r\n"
                    "Content-Type: text/plain\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n"
                    "503 Service Unavailable: Maximum stream subscribers reached\n");
}

int http_format_200_json(char *out_buf, size_t max_len, const char *json)
{
    const char *body = json ? json : "{\"status\":\"ok\"}\n";
    return snprintf(out_buf, max_len,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n\r\n"
                    "%s", strlen(body), body);
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
