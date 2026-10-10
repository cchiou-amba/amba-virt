/*
 * guest-os/apps/amba-virt-demo/AmbaVirtDemo.cxx
 *
 * Ambarella Virtualization Demo Hub Edge AI Service.
 * Modular C++ production service linking VisORC hardware acceleration,
 * Ambarella DSP hardware MJPEG streaming, and dual-tenant Web interface.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <endian.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <signal.h>
#include <poll.h>
#include <atomic>
#include <vector>
#include <string>
#include <thread>

extern "C" {
#include <cavalry_ioctl.h>
#include "cavalry_ioctl_path_b.h"
#include <cavalry_mem.h>
#include <nnctrl.h>
#include "amba_virt.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include "amba_virt.pb.h"
}

#include "PathBHelper.hxx"
#include "YoloGeometry.hxx"
#include "YoloPostprocess.hxx"
#include "HttpServer.hxx"
#include "StreamBroadcaster.hxx"
#include "DashboardHtml.hxx"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

#define DEFAULT_MODEL_PATH    "/opt/cavalry/n1-655_yolox_s_amba_optimized.bin"
#define DEFAULT_HTTP_BIND     "0.0.0.0:8080"
#define JPEG_HANDLE_CAPACITY  (2 * 1024 * 1024)

static const char *const COCO_CLASSES[80] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

struct JpegSidecarHandle {
    uint32_t handle_id{0};
    uint32_t bar_offset{0};
    uint32_t size{0};
    uint8_t *virt{nullptr};
};

struct YoloRuntime {
    int fd_cav{-1};
    int net_id{-1};
    struct net_cfg net_cf{};
    struct net_mem net_m{};
    struct net_input_cfg net_in{};
    struct net_output_cfg net_out{};
    pthread_mutex_t lock;

    char os_pretty_name[128]{"Linux"};
    char os_kernel[64]{"Unknown"};
    char os_machine[32]{"aarch64"};
};

/* Global application state */
static std::atomic<bool> g_running{true};
static std::atomic<float> g_live_conf_thresh{0.35f};
static std::atomic<float> g_live_nms_thresh{0.45f};

static struct stream_broadcaster g_broadcaster;
static JpegSidecarHandle g_jpeg_handles[2];
static uint32_t g_live_dag_id = 0;
static uint32_t g_live_in_handle = 0;
static uint32_t g_live_out_handle = 0;
static void *g_live_out_virt = nullptr;
static size_t g_live_out_size = 0;

static void SignalHandler(int signum)
{
    (void)signum;
    g_running.store(false);
    stream_broadcaster_shutdown(&g_broadcaster);
}

/* Worker thread: Queries host IAV tap over Nanopb vsock and broadcasts MJPEG frames */
static void *LiveTapWorkerThread(void *arg)
{
    auto *rt = static_cast<YoloRuntime *>(arg);
    int fd_virt = -1;
    uint32_t rpc_req_id = 0;
    uint64_t last_seq = 0;
    int current_jpeg_idx = 0;

    struct yolo_geometry_config geo_cfg{};
    yolo_geometry_init(&geo_cfg, 640, 640, 1920, 1080, false);

    struct yolo_postprocess_config post_cfg{};
    yolo_postprocess_default_config(&post_cfg);

    bool is_first_after_wake = true;
    const int kMaxStaleHandleErrors = 20;
    int stale_handle_errors = 0;

    while (g_running.load()) {
        /* Invariant: Demand-driven streaming. Sleep when 0 subscribers are connected */
        if (stream_broadcaster_wait_active(&g_broadcaster, 100) < 0) {
            is_first_after_wake = true;
            continue;
        }

        if (fd_virt < 0) {
            fd_virt = open("/dev/amba_virt", O_RDWR);
            if (fd_virt < 0) {
                usleep(100000);
                continue;
            }
        }

        if (g_live_dag_id == 0 || g_live_in_handle == 0 || g_live_out_handle == 0 ||
            g_jpeg_handles[0].handle_id == 0 || g_jpeg_handles[1].handle_id == 0) {
            usleep(100000);
            continue;
        }

        /* Ping-pong JPEG sidecar handle */
        current_jpeg_idx = 1 - current_jpeg_idx;
        auto &cur_handle = g_jpeg_handles[current_jpeg_idx];

        /* Prepare Nanopb RPC request */
        ambarella_virt_v1_RpcEnvelope req_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        req_env.api_major = 1;
        req_env.api_minor = 0;
        req_env.request_id = ++rpc_req_id;
        req_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag;

        auto *req = &req_env.body.iav_tap_run_request;
        req->session_id = 1;
        req->dag_id = g_live_dag_id;
        req->in_handle_id = g_live_in_handle;
        req->out_handle_id = g_live_out_handle;
        req->want_jpeg = true;
        req->jpeg_handle_id = cur_handle.handle_id;
        req->jpeg_capacity = cur_handle.size;
        req->take_latest = is_first_after_wake;
        is_first_after_wake = false;

        struct amba_virt_xfer xfer{};
        auto *msg = reinterpret_cast<struct amba_virt_msg *>(xfer.data);
        msg->type = AMBA_VIRT_MSG_IAV_TAP_REQ;
        msg->seq = static_cast<uint32_t>(req_env.request_id);

        pb_ostream_t ostream = pb_ostream_from_buffer(xfer.data + sizeof(*msg), sizeof(xfer.data) - sizeof(*msg));
        if (!pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &req_env)) {
            usleep(2000);
            continue;
        }
        xfer.len = sizeof(*msg) + ostream.bytes_written;
        xfer.timeout_ms = 2000;

        struct timespec t_start, t_rpc_done, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        /* Narrow lock strictly around VisORC execution and tensor copy */
        pthread_mutex_lock(&rt->lock);
        int io_ret = ioctl(fd_virt, AMBA_VIRT_IOC_RPC, &xfer);
        if (io_ret >= 0 && g_live_out_virt && g_live_out_virt != rt->net_out.out_desc[0].virt) {
            memcpy(rt->net_out.out_desc[0].virt, g_live_out_virt, g_live_out_size);
        }
        pthread_mutex_unlock(&rt->lock);
        clock_gettime(CLOCK_MONOTONIC, &t_rpc_done);

        if (io_ret < 0) {
            fprintf(stderr, "[ERROR] AMBA_VIRT_IOC_RPC ioctl failed: %s\n", strerror(errno));
            usleep(5000);
            continue;
        }

        /* Decode Nanopb response */
        pb_istream_t istream = pb_istream_from_buffer(xfer.data + sizeof(*msg), xfer.len - sizeof(*msg));
        ambarella_virt_v1_RpcEnvelope resp_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        if (!pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &resp_env)) {
            fprintf(stderr, "[ERROR] pb_decode failed: %s\n", PB_GET_ERROR(&istream));
            usleep(5000);
            continue;
        }

        if (resp_env.which_body != ambarella_virt_v1_RpcEnvelope_iav_tap_run_response_tag) {
            if (resp_env.which_body == ambarella_virt_v1_RpcEnvelope_error_tag) {
                int status = resp_env.body.error.status;

                fprintf(stderr, "[ERROR] Host tap RPC error: status=%d detail='%s'\n",
                        status, resp_env.body.error.detail);
                /*
                 * The host server restarted and freed this process's DAG and
                 * handles. They cannot be recovered in place; exit so the
                 * service manager restarts us and the model registers again.
                 */
                if (status == -ENOENT || status == -EACCES || status == -EPERM) {
                    if (++stale_handle_errors >= kMaxStaleHandleErrors) {
                        fprintf(stderr, "[FATAL] host no longer knows our handles; exiting to re-register\n");
                        exit(EXIT_FAILURE);
                    }
                } else {
                    stale_handle_errors = 0;
                }
            } else {
                fprintf(stderr, "[ERROR] Unexpected response body tag: %d\n", resp_env.which_body);
            }
            usleep(5000);
            continue;
        }

        stale_handle_errors = 0;
        auto *resp = &resp_env.body.iav_tap_run_response;
        if (resp->status != 0) {
            if (resp->status != -EAGAIN && resp->status != -EBUSY) {
                fprintf(stderr, "[ERROR] Tap run response status=%d\n", resp->status);
            }
            usleep(2000);
            continue;
        }

        if (resp->active_seq <= last_seq) {
            usleep(2000);
            continue;
        }
        last_seq = resp->active_seq;

        /* Postprocess detections outside rt->lock using modular YoloPostprocess */
        post_cfg.conf_thresh = g_live_conf_thresh.load();
        post_cfg.nms_thresh = g_live_nms_thresh.load();

        struct yolo_norm_box norm_dets[100];
        int det_cnt = yolo_postprocess(rt->net_out.out_desc[0].virt, &geo_cfg, &post_cfg, norm_dets, 100);
        if (det_cnt < 0) det_cnt = 0;

        uint32_t jpeg_len = resp->jpeg_len;
        if (jpeg_len == 0 || jpeg_len > cur_handle.size) {
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        double total_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0 + (t_end.tv_nsec - t_start.tv_nsec) / 1000000.0;
        double rpc_ms = (t_rpc_done.tv_sec - t_start.tv_sec) * 1000.0 + (t_rpc_done.tv_nsec - t_start.tv_nsec) / 1000000.0;
        double post_ms = (t_end.tv_sec - t_rpc_done.tv_sec) * 1000.0 + (t_end.tv_nsec - t_rpc_done.tv_nsec) / 1000000.0;
        double visorc_ms = resp->exec_ticks ? (static_cast<double>(resp->exec_ticks) / 24000.0)
                                            : ((t_rpc_done.tv_sec - t_start.tv_sec) * 1000.0 + (t_rpc_done.tv_nsec - t_start.tv_nsec) / 1000000.0);

        static double s_last_frame_s = 0.0;
        double now_s = t_end.tv_sec + t_end.tv_nsec / 1e9;
        double fps = 0.0;
        if (s_last_frame_s > 0.0) {
            double dt = now_s - s_last_frame_s;
            if (dt > 0.0001) fps = 1.0 / dt;
        }
        s_last_frame_s = now_s;

        /* Format strictly normalized JSON metadata */
        char json_buf[4096];
        int jlen = snprintf(json_buf, sizeof(json_buf),
                            "{\"seq\":%llu,\"drop_count\":%llu,"
                            "\"visorc_ms\":%.2f,\"rpc_ms\":%.2f,\"post_ms\":%.2f,\"total_ms\":%.2f,\"fps\":%.2f,"
                            "\"det_count\":%d,\"detections\":[",
                            static_cast<unsigned long long>(resp->active_seq),
                            static_cast<unsigned long long>(resp->drop_count),
                            visorc_ms, rpc_ms, post_ms, total_ms, fps, det_cnt);
        for (int d = 0; d < det_cnt && jlen < static_cast<int>(sizeof(json_buf)) - 160; d++) {
            const char *cname = (norm_dets[d].class_id >= 0 && norm_dets[d].class_id < 80)
                                    ? COCO_CLASSES[norm_dets[d].class_id] : "unknown";
            jlen += snprintf(json_buf + jlen, sizeof(json_buf) - jlen,
                             "%s{\"class\":\"%s\",\"confidence\":%.4f,"
                             "\"box\":[%.4f,%.4f,%.4f,%.4f]}",
                             (d > 0 ? "," : ""), cname, norm_dets[d].score,
                             norm_dets[d].x1, norm_dets[d].y1, norm_dets[d].x2, norm_dets[d].y2);
        }
        if (jlen < static_cast<int>(sizeof(json_buf)) - 2) {
            json_buf[jlen++] = ']';
            json_buf[jlen++] = '}';
            json_buf[jlen] = '\0';
        }

        uint32_t json_len_u32 = static_cast<uint32_t>(jlen);
        uint32_t net_json_len = htole32(json_len_u32);
        uint32_t net_jpeg_len = htole32(jpeg_len);
        size_t total_frame_bytes = sizeof(net_json_len) + sizeof(net_jpeg_len) + json_len_u32 + jpeg_len;

        std::vector<uint8_t> packet(total_frame_bytes);
        uint8_t *p = packet.data();
        memcpy(p, &net_json_len, sizeof(net_json_len));
        p += sizeof(net_json_len);
        memcpy(p, &net_jpeg_len, sizeof(net_jpeg_len));
        p += sizeof(net_jpeg_len);
        memcpy(p, json_buf, json_len_u32);
        p += json_len_u32;
        memcpy(p, cur_handle.virt, jpeg_len);

        uint64_t out_seq = 0;
        stream_broadcaster_publish(&g_broadcaster, packet.data(), total_frame_bytes, &out_seq);
    }

    if (fd_virt >= 0) close(fd_virt);
    return nullptr;
}

static void HandleLiveStream(int client_fd)
{
    int sub_id = -1;
    int reg_rc = stream_broadcaster_register(&g_broadcaster, &sub_id);
    if (reg_rc < 0) {
        char err_buf[256];
        int elen = http_format_503_service_unavailable(err_buf, sizeof(err_buf));
        http_send_all(client_fd, err_buf, elen);
        close(client_fd);
        return;
    }

    /* Send HTTP 200 Streaming Response Headers */
    const char *stream_header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n\r\n";
    if (http_send_all(client_fd, stream_header, strlen(stream_header)) < 0) {
        stream_broadcaster_unregister(&g_broadcaster, sub_id);
        close(client_fd);
        return;
    }

    std::vector<uint8_t> frame_buf(2 * 1024 * 1024);
    uint64_t client_last_seq = 0;

    while (g_running.load()) {
        size_t frame_len = 0;
        int fetch_rc = stream_broadcaster_fetch(&g_broadcaster, &client_last_seq,
                                               frame_buf.data(), frame_buf.size(),
                                               &frame_len, 2000);
        if (fetch_rc == -ETIMEDOUT) {
            /* No frame is flowing, so a send cannot detect a client that left. */
#ifdef POLLRDHUP
            const short hangup = POLLRDHUP | POLLHUP | POLLERR;
#else
            const short hangup = POLLHUP | POLLERR;
#endif
            struct pollfd pfd = { client_fd, static_cast<short>(POLLIN | hangup), 0 };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & hangup))
                break;
            continue;
        }
        if (fetch_rc < 0) {
            break;
        }

        /* Send binary packet to socket with timeout */
        ssize_t sent = http_send_all(client_fd, frame_buf.data(), frame_len);
        if (sent < 0) {
            /* Client disconnected or send timed out */
            break;
        }
    }

    stream_broadcaster_unregister(&g_broadcaster, sub_id);
    close(client_fd);
}

static void HandleStaticInfer(int client_fd, const struct http_request *req, YoloRuntime *rt)
{
    float conf_thresh = g_live_conf_thresh.load();
    float nms_thresh = g_live_nms_thresh.load();

    if (req->query[0] != '\0') {
        const char *p_t = strstr(req->query, "thresh=");
        if (p_t) {
            float t = strtof(p_t + 7, nullptr);
            if (t >= 0.01f && t <= 1.0f) conf_thresh = t;
        }
        const char *p_n = strstr(req->query, "nms=");
        if (p_n) {
            float n = strtof(p_n + 4, nullptr);
            if (n >= 0.01f && n <= 1.0f) nms_thresh = n;
        }
    }

    struct timespec ts_total_start, ts_total_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_total_start);

    int img_w = 0, img_h = 0, img_c = 0;
    uint8_t *img_rgb = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(req->body),
                                             static_cast<int>(req->body_len),
                                             &img_w, &img_h, &img_c, 3);
    if (!img_rgb || img_w <= 0 || img_h <= 0) {
        char err_buf[256];
        int elen = http_format_422_unprocessable(err_buf, sizeof(err_buf), "Failed to decode image");
        http_send_all(client_fd, err_buf, elen);
        close(client_fd);
        return;
    }

    /* Preprocess: letterbox scale into 640x640 planar RGB */
    std::vector<uint8_t> planar_in(3 * 640 * 640, 114);
    uint8_t *r_plane = planar_in.data();
    uint8_t *g_plane = r_plane + (640 * 640);
    uint8_t *b_plane = r_plane + (2 * 640 * 640);

    float scale = std::min(640.0f / img_w, 640.0f / img_h);
    int nw = static_cast<int>(img_w * scale);
    int nh = static_cast<int>(img_h * scale);
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            int src_x = std::min(static_cast<int>(x / scale), img_w - 1);
            int src_y = std::min(static_cast<int>(y / scale), img_h - 1);
            const uint8_t *sp = img_rgb + (src_y * img_w + src_x) * 3;
            int out_idx = y * 640 + x;
            r_plane[out_idx] = sp[0];
            g_plane[out_idx] = sp[1];
            b_plane[out_idx] = sp[2];
        }
    }
    stbi_image_free(img_rgb);

    struct timespec ts_inf_start, ts_inf_end;
    std::vector<uint8_t> out_tensor_copy(rt->net_out.out_desc[0].size);

    pthread_mutex_lock(&rt->lock);
    clock_gettime(CLOCK_MONOTONIC, &ts_inf_start);

    memcpy(rt->net_in.in_desc[0].virt, planar_in.data(), 3 * 640 * 640);
    int run_rc = nnctrl_run_net(rt->net_id, nullptr, nullptr, nullptr, nullptr);
    if (run_rc >= 0 && g_live_out_virt && g_live_out_virt != rt->net_out.out_desc[0].virt) {
        memcpy(out_tensor_copy.data(), g_live_out_virt, out_tensor_copy.size());
    } else if (run_rc >= 0) {
        memcpy(out_tensor_copy.data(), rt->net_out.out_desc[0].virt, out_tensor_copy.size());
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_inf_end);
    pthread_mutex_unlock(&rt->lock);

    if (run_rc < 0) {
        char err_buf[256];
        int elen = http_format_422_unprocessable(err_buf, sizeof(err_buf), "VisORC inference failed");
        http_send_all(client_fd, err_buf, elen);
        close(client_fd);
        return;
    }

    double visorc_ms = (ts_inf_end.tv_sec - ts_inf_start.tv_sec) * 1000.0 +
                       (ts_inf_end.tv_nsec - ts_inf_start.tv_nsec) / 1000000.0;

    struct yolo_geometry_config geo_cfg{};
    yolo_geometry_init(&geo_cfg, 640, 640, img_w, img_h, false);

    struct yolo_postprocess_config post_cfg{};
    yolo_postprocess_default_config(&post_cfg);
    post_cfg.conf_thresh = conf_thresh;
    post_cfg.nms_thresh = nms_thresh;

    struct yolo_norm_box norm_dets[100];
    int det_cnt = yolo_postprocess(out_tensor_copy.data(), &geo_cfg, &post_cfg, norm_dets, 100);
    if (det_cnt < 0) det_cnt = 0;

    clock_gettime(CLOCK_MONOTONIC, &ts_total_end);
    double total_ms = (ts_total_end.tv_sec - ts_total_start.tv_sec) * 1000.0 +
                      (ts_total_end.tv_nsec - ts_total_start.tv_nsec) / 1000000.0;

    char json_buf[8192];
    int jlen = snprintf(json_buf, sizeof(json_buf),
                        "{\"status\":\"success\",\"visorc_time_ms\":%.2f,\"total_time_ms\":%.2f,"
                        "\"det_count\":%d,\"detections\":[",
                        visorc_ms, total_ms, det_cnt);
    for (int d = 0; d < det_cnt && jlen < static_cast<int>(sizeof(json_buf)) - 160; d++) {
        const char *cname = (norm_dets[d].class_id >= 0 && norm_dets[d].class_id < 80)
                                ? COCO_CLASSES[norm_dets[d].class_id] : "unknown";
        jlen += snprintf(json_buf + jlen, sizeof(json_buf) - jlen,
                         "%s{\"class\":\"%s\",\"confidence\":%.4f,"
                         "\"box\":[%.4f,%.4f,%.4f,%.4f]}",
                         (d > 0 ? "," : ""), cname, norm_dets[d].score,
                         norm_dets[d].x1, norm_dets[d].y1, norm_dets[d].x2, norm_dets[d].y2);
    }
    if (jlen < static_cast<int>(sizeof(json_buf)) - 2) {
        json_buf[jlen++] = ']';
        json_buf[jlen++] = '}';
        json_buf[jlen] = '\0';
    }

    char resp_buf[10240];
    int rlen = http_format_200_json(resp_buf, sizeof(resp_buf), json_buf);
    http_send_all(client_fd, resp_buf, rlen);
    close(client_fd);
}

static void HandleClientConnection(int client_fd, YoloRuntime *rt)
{
    http_configure_socket_timeouts(client_fd, 2000);

    std::vector<char> req_buf(16384, 0);
    ssize_t recvd = recv(client_fd, req_buf.data(), req_buf.size() - 1, 0);
    if (recvd <= 0) {
        close(client_fd);
        return;
    }
    req_buf[recvd] = '\0';

    struct http_request req{};
    int parse_rc = http_parse_request(req_buf.data(), recvd, &req);
    if (parse_rc == -EAGAIN && req.content_length > 0) {
        if (req.content_length > 10 * 1024 * 1024) {
            char err_buf[256];
            int elen = http_format_413_payload_too_large(err_buf, sizeof(err_buf));
            http_send_all(client_fd, err_buf, elen);
            close(client_fd);
            return;
        }
        size_t header_len = req.body - req_buf.data();
        size_t total_expected = header_len + req.content_length;
        req_buf.resize(total_expected + 1);
        while (static_cast<size_t>(recvd) < total_expected) {
            ssize_t n = recv(client_fd, req_buf.data() + recvd, total_expected - recvd, 0);
            if (n <= 0) {
                close(client_fd);
                return;
            }
            recvd += n;
        }
        req_buf[recvd] = '\0';
        parse_rc = http_parse_request(req_buf.data(), recvd, &req);
    }

    if (parse_rc < 0 && parse_rc != -EAGAIN) {
        char err_buf[256];
        int elen = http_format_400_bad_request(err_buf, sizeof(err_buf), "Malformed HTTP request");
        http_send_all(client_fd, err_buf, elen);
        close(client_fd);
        return;
    }

    if (req.route == HTTP_ROUTE_ROOT) {
        size_t body_len = strlen(HTML_TEMPLATE);
        char header[256];
        int hlen = snprintf(header, sizeof(header),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n", body_len);
        http_send_all(client_fd, header, hlen);
        http_send_all(client_fd, HTML_TEMPLATE, body_len);
        close(client_fd);
    } else if (req.route == HTTP_ROUTE_API_INFO) {
        char json_buf[1024];
        snprintf(json_buf, sizeof(json_buf),
                 "{\n"
                 "  \"status\": \"online\",\n"
                 "  \"pretty_name\": \"%s\",\n"
                 "  \"kernel\": \"%s\",\n"
                 "  \"machine\": \"%s\",\n"
                 "  \"live_streaming\": true,\n"
                 "  \"mjpeg_hardware\": true,\n"
                 "  \"cavalry_path\": \"Path B\"\n"
                 "}\n",
                 rt->os_pretty_name, rt->os_kernel, rt->os_machine);
        char resp_buf[2048];
        int rlen = http_format_200_json(resp_buf, sizeof(resp_buf), json_buf);
        http_send_all(client_fd, resp_buf, rlen);
        close(client_fd);
    } else if (req.route == HTTP_ROUTE_LIVE_PARAMS) {
        struct http_live_params params{};
        int p_rc = http_parse_live_params(req.body, req.body_len, &params, true);
        if (p_rc < 0) {
            char err_buf[256];
            int elen = http_format_422_unprocessable(err_buf, sizeof(err_buf), "Invalid confidence or NMS threshold");
            http_send_all(client_fd, err_buf, elen);
        } else {
            g_live_conf_thresh.store(params.conf_thresh);
            g_live_nms_thresh.store(params.nms_thresh);
            char ok_json[128];
            snprintf(ok_json, sizeof(ok_json), "{\"status\":\"ok\",\"conf\":%.2f,\"nms\":%.2f}",
                     params.conf_thresh, params.nms_thresh);
            char resp_buf[256];
            int rlen = http_format_200_json(resp_buf, sizeof(resp_buf), ok_json);
            http_send_all(client_fd, resp_buf, rlen);
        }
        close(client_fd);
    } else if (req.route == HTTP_ROUTE_LIVE_STREAM) {
        HandleLiveStream(client_fd);
    } else if (req.route == HTTP_ROUTE_INFER) {
        HandleStaticInfer(client_fd, &req, rt);
    } else if (req.route == HTTP_ROUTE_OPTIONS) {
        char resp_buf[256];
        int rlen = http_format_options_response(resp_buf, sizeof(resp_buf));
        http_send_all(client_fd, resp_buf, rlen);
        close(client_fd);
    } else {
        char err_buf[256];
        int elen = http_format_404_not_found(err_buf, sizeof(err_buf));
        http_send_all(client_fd, err_buf, elen);
        close(client_fd);
    }
}

static void PopulateOsInfo(YoloRuntime *rt)
{
    struct utsname uts{};
    if (uname(&uts) == 0) {
        strncpy(rt->os_kernel, uts.release, sizeof(rt->os_kernel) - 1);
        rt->os_kernel[sizeof(rt->os_kernel) - 1] = '\0';
        strncpy(rt->os_machine, uts.machine, sizeof(rt->os_machine) - 1);
        rt->os_machine[sizeof(rt->os_machine) - 1] = '\0';
    }

    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                char *start = line + 12;
                if (*start == '\"') start++;
                char *end = strchr(start, '\"');
                if (!end) end = strchr(start, '\n');
                if (end) *end = '\0';
                strncpy(rt->os_pretty_name, start, sizeof(rt->os_pretty_name) - 1);
                rt->os_pretty_name[sizeof(rt->os_pretty_name) - 1] = '\0';
                break;
            }
        }
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN);

    const char *model_path = DEFAULT_MODEL_PATH;
    const char *bind_addr = DEFAULT_HTTP_BIND;
    float conf_thresh = 0.35f;
    float nms_thresh = 0.45f;

    static struct option long_options[] = {
        {"model", required_argument, nullptr, 'm'},
        {"bind",  required_argument, nullptr, 'b'},
        {"conf",  required_argument, nullptr, 'c'},
        {"nms",   required_argument, nullptr, 'n'},
        {"help",  no_argument,       nullptr, 'h'},
        {nullptr, 0,                 nullptr, 0}
    };

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "m:b:c:n:h", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'm': model_path = optarg; break;
        case 'b': bind_addr = optarg; break;
        case 'c': conf_thresh = strtof(optarg, nullptr); break;
        case 'n': nms_thresh = strtof(optarg, nullptr); break;
        case 'h':
        default:
            printf("Usage: %s [options]\n"
                   "  -m, --model <path>  Path to YOLOX model binary (default: %s)\n"
                   "  -b, --bind <ip:port> HTTP bind address (default: %s)\n"
                   "  -c, --conf <val>    Confidence threshold (default: %.2f)\n"
                   "  -n, --nms <val>     NMS IoU threshold (default: %.2f)\n",
                   argv[0], DEFAULT_MODEL_PATH, DEFAULT_HTTP_BIND, conf_thresh, nms_thresh);
            return (opt == 'h') ? 0 : 1;
        }
    }

    g_live_conf_thresh.store(conf_thresh);
    g_live_nms_thresh.store(nms_thresh);

    YoloRuntime yolo{};
    pthread_mutex_init(&yolo.lock, nullptr);
    PopulateOsInfo(&yolo);

    printf("===============================================================================\n");
    printf(" Ambarella Virtualization Demo Hub Edge AI Service (Modular C++)\n");
    printf(" Target OS: %s (%s)\n", yolo.os_pretty_name, yolo.os_machine);
    printf(" Model:     %s\n", model_path);
    printf("===============================================================================\n");

    /* 1. Open Cavalry device */
    yolo.fd_cav = open("/dev/cavalry", O_RDWR);
    if (yolo.fd_cav < 0) {
        fprintf(stderr, "Fatal: cannot open /dev/cavalry: %s\n", strerror(errno));
        return 1;
    }

    /* 2. Initialize memory and nnctrl */
    if (cavalry_mem_init(yolo.fd_cav, 0) < 0 || nnctrl_init(yolo.fd_cav, 0) < 0) {
        fprintf(stderr, "Fatal: failed to initialize cavalry memory or nnctrl\n");
        close(yolo.fd_cav);
        return 1;
    }

    /* 3. Initialize network */
    yolo.net_cf.net_file = const_cast<char *>(model_path);
    yolo.net_cf.no_chip_check = 1;
    yolo.net_id = nnctrl_init_net(&yolo.net_cf, nullptr, nullptr);
    if (yolo.net_id < 0) {
        fprintf(stderr, "Fatal: nnctrl_init_net failed for %s\n", model_path);
        nnctrl_exit();
        cavalry_mem_exit();
        close(yolo.fd_cav);
        return 1;
    }

    /* 4. Allocate model memory and load net */
    if (cavalry_mem_alloc(&yolo.net_cf.net_mem_total, &yolo.net_m.phy_addr,
                          reinterpret_cast<void **>(&yolo.net_m.virt_addr), 0) < 0) {
        fprintf(stderr, "Fatal: cavalry_mem_alloc failed\n");
        nnctrl_exit_net(yolo.net_id);
        nnctrl_exit();
        cavalry_mem_exit();
        close(yolo.fd_cav);
        return 1;
    }
    yolo.net_m.mem_size = yolo.net_cf.net_mem_total;

    if (nnctrl_load_net(yolo.net_id, &yolo.net_m, nullptr, nullptr) < 0 ||
        nnctrl_get_net_io_cfg(yolo.net_id, &yolo.net_in, &yolo.net_out) < 0) {
        fprintf(stderr, "Fatal: nnctrl_load_net or get_net_io_cfg failed\n");
        cavalry_mem_free(yolo.net_m.mem_size, yolo.net_m.phy_addr, yolo.net_m.virt_addr);
        nnctrl_exit_net(yolo.net_id);
        nnctrl_exit();
        cavalry_mem_exit();
        close(yolo.fd_cav);
        return 1;
    }

    struct path_b_info pb_info{};
    if (get_path_b_info(yolo.net_id, &pb_info) == 0 && pb_info.is_hvm) {
        g_live_dag_id = pb_info.dag_id;
        g_live_in_handle = pb_info.in_handle_id;
        g_live_out_handle = pb_info.out_handle_id;
        g_live_out_virt = pb_info.out_virt_addr;
        g_live_out_size = yolo.net_out.out_desc[0].size;
    }

    printf("[PASS] YOLOX model loaded and Path B registered successfully\n");

    /* 3. VisORC Warmup */
    printf("[*] Warming up VisORC coprocessor...\n");
    if (nnctrl_run_net(yolo.net_id, nullptr, nullptr, nullptr, nullptr) < 0) {
        fprintf(stderr, "Fatal: VisORC warmup execution failed\n");
        nnctrl_exit_net(yolo.net_id);
        nnctrl_exit();
        close(yolo.fd_cav);
        cavalry_mem_exit();
        return 1;
    }
    printf("[PASS] VisORC warmup complete\n");

    /* 5. Allocate two 2MB sidecar handles for ping-pong clean JPEG buffering */
    for (int i = 0; i < 2; i++) {
        struct cavalry_alloc_handle_user h{};
        h.size = JPEG_HANDLE_CAPACITY;
        if (ioctl(yolo.fd_cav, CAVALRY_IOC_ALLOC_HANDLE, &h) < 0) {
            g_jpeg_handles[i].handle_id = 0;
            g_jpeg_handles[i].virt = nullptr;
        } else {
            g_jpeg_handles[i].handle_id = h.handle_id;
            g_jpeg_handles[i].bar_offset = h.bar_offset;
            g_jpeg_handles[i].size = h.size;
            g_jpeg_handles[i].virt = static_cast<uint8_t *>(
                mmap(nullptr, h.size, PROT_READ | PROT_WRITE, MAP_SHARED, yolo.fd_cav, h.bar_offset));
            if (g_jpeg_handles[i].virt == MAP_FAILED) {
                g_jpeg_handles[i].virt = nullptr;
            } else {
                printf("[PASS] Allocated JPEG sidecar handle %d: hid=%u, offset=0x%08x, size=%u B\n",
                       i, h.handle_id, h.bar_offset, h.size);
            }
        }
    }

    /* 6. Initialize Multi-Subscriber Stream Broadcaster */
    stream_broadcaster_init(&g_broadcaster, 2 * 1024 * 1024);

    /* 7. Start Live Tap Worker Thread */
    pthread_t worker_tid;
    pthread_create(&worker_tid, nullptr, LiveTapWorkerThread, &yolo);

    /* 8. Setup HTTP Listener */
    char ip_str[64] = "0.0.0.0";
    int port = 8080;
    const char *colon = strchr(bind_addr, ':');
    if (colon) {
        size_t iplen = colon - bind_addr;
        if (iplen < sizeof(ip_str)) {
            strncpy(ip_str, bind_addr, iplen);
            ip_str[iplen] = '\0';
        }
        port = atoi(colon + 1);
    } else {
        port = atoi(bind_addr);
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        g_running.store(false);
    } else {
        int opt_val = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        inet_pton(AF_INET, ip_str, &sa.sin_addr);

        if (bind(sfd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) < 0) {
            perror("bind");
            g_running.store(false);
            close(sfd);
        } else if (listen(sfd, 16) < 0) {
            perror("listen");
            g_running.store(false);
            close(sfd);
        } else {
            printf("[PASS] HTTP Listener active on http://%s:%d/\n", ip_str, port);

            struct pollfd pfd{};
            pfd.fd = sfd;
            pfd.events = POLLIN;

            while (g_running.load()) {
                int prc = poll(&pfd, 1, 500);
                if (prc <= 0) {
                    continue;
                }
                struct sockaddr_in client_sa{};
                socklen_t slen = sizeof(client_sa);
                int client_fd = accept(sfd, reinterpret_cast<struct sockaddr *>(&client_sa), &slen);
                if (client_fd < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    break;
                }
                std::thread([client_fd, &yolo]() {
                    HandleClientConnection(client_fd, &yolo);
                }).detach();
            }
            close(sfd);
        }
    }

    /* Teardown */
    g_running.store(false);
    stream_broadcaster_shutdown(&g_broadcaster);
    pthread_join(worker_tid, nullptr);

    for (int i = 0; i < 2; i++) {
        if (g_jpeg_handles[i].virt) {
            munmap(g_jpeg_handles[i].virt, g_jpeg_handles[i].size);
            g_jpeg_handles[i].virt = nullptr;
        }
        if (g_jpeg_handles[i].handle_id != 0) {
            struct cavalry_free_handle_user free_h{};
            free_h.handle_id = g_jpeg_handles[i].handle_id;
            ioctl(yolo.fd_cav, CAVALRY_IOC_FREE_HANDLE, &free_h);
            g_jpeg_handles[i].handle_id = 0;
        }
    }

    stream_broadcaster_destroy(&g_broadcaster);
    cavalry_mem_free(yolo.net_m.mem_size, yolo.net_m.phy_addr, yolo.net_m.virt_addr);
    nnctrl_exit_net(yolo.net_id);
    nnctrl_exit();
    close(yolo.fd_cav);
    cavalry_mem_exit();
    pthread_mutex_destroy(&yolo.lock);

    return 0;
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
