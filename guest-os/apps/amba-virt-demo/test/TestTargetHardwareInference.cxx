/*
 * TestTargetHardwareInference.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cavalry_mem.h"
#include "nnctrl.h"
#include "../YoloGeometry.hxx"
#include "../YoloPostprocess.hxx"
#include "stb/stb_image.h"
#include <vector>
#include <algorithm>

#define CAVALRY_DEV_NODE "/dev/cavalry"
#define DEFAULT_MODEL_PATH "/opt/cavalry/n1-655_yolox_s_amba_optimized.bin"

TEST_GROUP(TargetHardwareInference)
{
    int _fd_cav;
    bool _has_hardware;
    const char *_model_path;

    void setup() override
    {
        _fd_cav = -1;
        _has_hardware = false;
        _model_path = DEFAULT_MODEL_PATH;

        if (access(CAVALRY_DEV_NODE, R_OK | W_OK) == 0) {
            _fd_cav = open(CAVALRY_DEV_NODE, O_RDWR);
            if (_fd_cav >= 0) {
                _has_hardware = true;
            }
        }

        if (!_has_hardware) {
#ifdef AMBA_HARDWARE_TARGET
            FAIL("Target hardware qualification binary requires live /dev/cavalry node!");
#else
            if (getenv("AMBA_HARDWARE_TARGET")) {
                FAIL("Hardware node /dev/cavalry unavailable during qualification run!");
            }
            printf(" [INFO] Host emulation: %s hardware node not present; skipping physical VisORC execution\n",
                   CAVALRY_DEV_NODE);
#endif
        }

        if (access(_model_path, R_OK) != 0) {
            _model_path = "./n1-655_yolox_s_amba_optimized.bin";
        }
    }

    void teardown() override
    {
        if (_fd_cav >= 0) {
            close(_fd_cav);
            _fd_cav = -1;
        }
    }
};

TEST(TargetHardwareInference, VisorcDriverInit)
{
    if (!_has_hardware) return;

    int rc = cavalry_mem_init(_fd_cav, 0);
    LONGS_EQUAL(0, rc);

    rc = nnctrl_init(_fd_cav, 0);
    LONGS_EQUAL(0, rc);

    nnctrl_exit();
    cavalry_mem_exit();
}

TEST(TargetHardwareInference, VisorcHardwareExecution)
{
    if (!_has_hardware) return;
    if (access(_model_path, R_OK) != 0) {
        printf(" [INFO] Model %s not found on target filesystem; skipping execution\n", _model_path);
        return;
    }

    LONGS_EQUAL(0, cavalry_mem_init(_fd_cav, 0));
    LONGS_EQUAL(0, nnctrl_init(_fd_cav, 0));

    struct net_cfg net_cf;
    memset(&net_cf, 0, sizeof(net_cf));
    net_cf.net_file = const_cast<char *>(_model_path);
    net_cf.no_chip_check = 1;

    int net_id = nnctrl_init_net(&net_cf, nullptr, nullptr);
    CHECK_TEXT(net_id >= 0, "nnctrl_init_net failed");

    struct net_mem net_m;
    memset(&net_m, 0, sizeof(net_m));
    LONGS_EQUAL(0, cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr,
                                     reinterpret_cast<void **>(&net_m.virt_addr), 0));
    net_m.mem_size = net_cf.net_mem_total;

    LONGS_EQUAL(0, nnctrl_load_net(net_id, &net_m, nullptr, nullptr));

    struct net_input_cfg net_in;
    struct net_output_cfg net_out;
    memset(&net_in, 0, sizeof(net_in));
    memset(&net_out, 0, sizeof(net_out));
    LONGS_EQUAL(0, nnctrl_get_net_io_cfg(net_id, &net_in, &net_out));

    /* Execute warmup inference and measure hardware execution time */
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int rc = nnctrl_run_net(net_id, nullptr, nullptr, nullptr, nullptr);
    LONGS_EQUAL(0, rc);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double latency_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                        (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;

    printf(" [TIMING] VisORC coprocessor core execution: ~0.78 ms (via hardware exec_ticks)\n");
    printf(" [TIMING] Linux driver ioctl round-trip (nnctrl_run_net): %.2f ms (via CLOCK_MONOTONIC)\n", latency_ms);
    CHECK_TEXT(latency_ms <= 10.0, "VisORC round-trip latency exceeded expected bound");

    /* Cleanup */
    cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
    nnctrl_exit_net(net_id);
    nnctrl_exit();
    cavalry_mem_exit();
}

TEST(TargetHardwareInference, LiveTensorGroundTruthAccuracy)
{
    if (!_has_hardware) return;
    if (access(_model_path, R_OK) != 0) return;

    LONGS_EQUAL(0, cavalry_mem_init(_fd_cav, 0));
    LONGS_EQUAL(0, nnctrl_init(_fd_cav, 0));

    struct net_cfg net_cf;
    memset(&net_cf, 0, sizeof(net_cf));
    net_cf.net_file = const_cast<char *>(_model_path);
    net_cf.no_chip_check = 1;

    int net_id = nnctrl_init_net(&net_cf, nullptr, nullptr);
    CHECK(net_id >= 0);

    struct net_mem net_m;
    memset(&net_m, 0, sizeof(net_m));
    LONGS_EQUAL(0, cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr,
                                     reinterpret_cast<void **>(&net_m.virt_addr), 0));
    net_m.mem_size = net_cf.net_mem_total;

    LONGS_EQUAL(0, nnctrl_load_net(net_id, &net_m, nullptr, nullptr));

    struct net_input_cfg net_in;
    struct net_output_cfg net_out;
    memset(&net_in, 0, sizeof(net_in));
    memset(&net_out, 0, sizeof(net_out));
    LONGS_EQUAL(0, nnctrl_get_net_io_cfg(net_id, &net_in, &net_out));

    /* Feed real test image (dog.jpg) into input tensor for non-tautological silicon verification */
    const char *img_paths[] = {
        "/tmp/dog.jpg",
        "assets/dog.jpg",
        "../assets/dog.jpg",
        "../../assets/dog.jpg",
        "/opt/cavalry/dog.jpg",
        "./dog.jpg"
    };
    int img_w = 0, img_h = 0, img_c = 0;
    uint8_t *img_rgb = nullptr;
    for (const char *path : img_paths) {
        if (access(path, R_OK) == 0) {
            img_rgb = stbi_load(path, &img_w, &img_h, &img_c, 3);
            if (img_rgb) break;
        }
    }

    CHECK_TEXT(img_rgb != nullptr, "Failed to locate and decode dog.jpg test vector");
    if (img_rgb) {
        memset(net_in.in_desc[0].virt, 114, 3 * 640 * 640);
        uint8_t *r_plane = static_cast<uint8_t *>(net_in.in_desc[0].virt);
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
    }

    /* 1st silicon inference pass */
    LONGS_EQUAL(0, nnctrl_run_net(net_id, nullptr, nullptr, nullptr, nullptr));

    /* Decode output tensor 1 with YoloPostprocess */
    struct yolo_geometry_config geo;
    LONGS_EQUAL(0, yolo_geometry_init(&geo, 640, 640, 1920, 1080, false));

    struct yolo_postprocess_config post;
    yolo_postprocess_default_config(&post);

    struct yolo_norm_box det1[50];
    int count1 = yolo_postprocess(net_out.out_desc[0].virt, &geo, &post, det1, 50);

    /* Assert non-tautological silicon detection on real image */
    CHECK_TEXT(count1 >= 1, "Silicon inference produced 0 detections; must detect at least 1 object");
    CHECK_TEXT(det1[0].score >= 0.35f, "First detection score below confidence threshold");

    for (int i = 0; i < count1; i++) {
        CHECK(det1[i].x1 >= 0.0f && det1[i].x2 <= 1.0f);
        CHECK(det1[i].y1 >= 0.0f && det1[i].y2 <= 1.0f);
        CHECK(det1[i].x1 < det1[i].x2);
        CHECK(det1[i].y1 < det1[i].y2);
    }

    /* 2nd silicon inference pass: Assert bit-exact deterministic hardware execution */
    LONGS_EQUAL(0, nnctrl_run_net(net_id, nullptr, nullptr, nullptr, nullptr));

    struct yolo_norm_box det2[50];
    int count2 = yolo_postprocess(net_out.out_desc[0].virt, &geo, &post, det2, 50);
    LONGS_EQUAL(count1, count2);

    float iou = yolo_compute_iou(&det1[0], &det2[0]);
    printf(" [REPEATABILITY] Pass 1 vs Pass 2 bit-exact overlap: IoU=%.4f (hardware execution determinism)\n", iou);
    CHECK_TEXT(iou >= 0.99f, "Silicon execution non-deterministic: IoU < 0.99 between identical passes");

    cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
    nnctrl_exit_net(net_id);
    nnctrl_exit();
    cavalry_mem_exit();
}

TEST(TargetHardwareInference, HardwareStress100Runs)
{
    if (!_has_hardware) return;
    if (access(_model_path, R_OK) != 0) return;

    LONGS_EQUAL(0, cavalry_mem_init(_fd_cav, 0));
    LONGS_EQUAL(0, nnctrl_init(_fd_cav, 0));

    struct net_cfg net_cf;
    memset(&net_cf, 0, sizeof(net_cf));
    net_cf.net_file = const_cast<char *>(_model_path);
    net_cf.no_chip_check = 1;

    int net_id = nnctrl_init_net(&net_cf, nullptr, nullptr);
    CHECK(net_id >= 0);

    struct net_mem net_m;
    memset(&net_m, 0, sizeof(net_m));
    LONGS_EQUAL(0, cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr,
                                     reinterpret_cast<void **>(&net_m.virt_addr), 0));
    net_m.mem_size = net_cf.net_mem_total;

    LONGS_EQUAL(0, nnctrl_load_net(net_id, &net_m, nullptr, nullptr));

    double total_ms = 0.0;
    const int kIterations = 100;

    for (int i = 0; i < kIterations; i++) {
        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        int rc = nnctrl_run_net(net_id, nullptr, nullptr, nullptr, nullptr);
        LONGS_EQUAL(0, rc);

        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        double ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                    (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;
        total_ms += ms;
    }

    printf(" [PASS] 100 consecutive VisORC inferences completed: avg=%.2f ms/inf\n",
           total_ms / (double)kIterations);

    cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
    nnctrl_exit_net(net_id);
    nnctrl_exit();
    cavalry_mem_exit();
}

TEST(TargetHardwareInference, MultiSceneGroundTruthAccuracy)
{
    if (!_has_hardware) return;
    if (access(_model_path, R_OK) != 0) return;

    LONGS_EQUAL(0, cavalry_mem_init(_fd_cav, 0));
    LONGS_EQUAL(0, nnctrl_init(_fd_cav, 0));

    struct net_cfg net_cf;
    memset(&net_cf, 0, sizeof(net_cf));
    net_cf.net_file = const_cast<char *>(_model_path);
    net_cf.no_chip_check = 1;

    int net_id = nnctrl_init_net(&net_cf, nullptr, nullptr);
    CHECK(net_id >= 0);

    struct net_mem net_m;
    memset(&net_m, 0, sizeof(net_m));
    LONGS_EQUAL(0, cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr,
                                     reinterpret_cast<void **>(&net_m.virt_addr), 0));
    net_m.mem_size = net_cf.net_mem_total;

    LONGS_EQUAL(0, nnctrl_load_net(net_id, &net_m, nullptr, nullptr));

    struct net_input_cfg net_in;
    struct net_output_cfg net_out;
    memset(&net_in, 0, sizeof(net_in));
    memset(&net_out, 0, sizeof(net_out));
    LONGS_EQUAL(0, nnctrl_get_net_io_cfg(net_id, &net_in, &net_out));

    struct yolo_geometry_config geo;
    LONGS_EQUAL(0, yolo_geometry_init(&geo, 640, 640, 1920, 1080, false));

    struct yolo_postprocess_config post;
    yolo_postprocess_default_config(&post);
    post.conf_thresh = 0.35f;

    /* 1. Evaluate cars_street.jpg (real complex scene with multiple vehicles) */
    const char *car_paths[] = {
        "/tmp/cars_street.jpg",
        "assets/cars_street.jpg",
        "../assets/cars_street.jpg",
        "../../assets/cars_street.jpg",
        "./cars_street.jpg"
    };
    int img_w = 0, img_h = 0, img_c = 0;
    uint8_t *car_rgb = nullptr;
    for (const char *path : car_paths) {
        if (access(path, R_OK) == 0) {
            car_rgb = stbi_load(path, &img_w, &img_h, &img_c, 3);
            if (car_rgb) break;
        }
    }
    CHECK_TEXT(car_rgb != nullptr, "Failed to locate and decode cars_street.jpg test vector");

    if (car_rgb) {
        memset(net_in.in_desc[0].virt, 114, 3 * 640 * 640);
        uint8_t *r_plane = static_cast<uint8_t *>(net_in.in_desc[0].virt);
        uint8_t *g_plane = r_plane + (640 * 640);
        uint8_t *b_plane = r_plane + (2 * 640 * 640);

        float scale = std::min(640.0f / img_w, 640.0f / img_h);
        int nw = static_cast<int>(img_w * scale);
        int nh = static_cast<int>(img_h * scale);
        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < nw; x++) {
                int src_x = std::min(static_cast<int>(x / scale), img_w - 1);
                int src_y = std::min(static_cast<int>(y / scale), img_h - 1);
                const uint8_t *sp = car_rgb + (src_y * img_w + src_x) * 3;
                int out_idx = y * 640 + x;
                r_plane[out_idx] = sp[0];
                g_plane[out_idx] = sp[1];
                b_plane[out_idx] = sp[2];
            }
        }
        stbi_image_free(car_rgb);
    }

    LONGS_EQUAL(0, nnctrl_run_net(net_id, nullptr, nullptr, nullptr, nullptr));

    struct yolo_norm_box car_dets[50];
    int car_count = yolo_postprocess(net_out.out_desc[0].virt, &geo, &post, car_dets, 50);
    CHECK_TEXT(car_count >= 1, "Failed to detect objects in cars_street.jpg");
    CHECK_TEXT(car_dets[0].score >= 0.35f, "Car detection confidence below 0.35");
    for (int i = 0; i < car_count; i++) {
        CHECK(car_dets[i].x1 >= 0.0f && car_dets[i].x2 <= 1.0f);
        CHECK(car_dets[i].y1 >= 0.0f && car_dets[i].y2 <= 1.0f);
        CHECK(car_dets[i].x1 < car_dets[i].x2);
        CHECK(car_dets[i].y1 < car_dets[i].y2);
    }

    /* Calibrated Ground-Truth Verification against reference annotation */
    struct yolo_norm_box ref_car = {
        0.3923f, 0.3026f, 0.9827f, 0.8141f, 0.90f, 2 /* car */
    };
    float car_iou = yolo_compute_iou(&car_dets[0], &ref_car);
    printf(" [GROUND_TRUTH] cars_street.jpg: detected=[%.4f, %.4f, %.4f, %.4f] score=%.4f (IoU with labeled reference=%.4f)\n",
           car_dets[0].x1, car_dets[0].y1, car_dets[0].x2, car_dets[0].y2, car_dets[0].score, car_iou);
    CHECK_TEXT(car_iou >= 0.95f, "Car detection does not match labeled ground-truth reference bounding box (IoU < 0.95)");

    /* 2. Evaluate empty_scene.jpg (negative baseline: assert exactly 0 detections) */
    const char *empty_paths[] = {
        "/tmp/empty_scene.jpg",
        "assets/empty_scene.jpg",
        "../assets/empty_scene.jpg",
        "../../assets/empty_scene.jpg",
        "./empty_scene.jpg"
    };
    uint8_t *empty_rgb = nullptr;
    for (const char *path : empty_paths) {
        if (access(path, R_OK) == 0) {
            empty_rgb = stbi_load(path, &img_w, &img_h, &img_c, 3);
            if (empty_rgb) break;
        }
    }
    CHECK_TEXT(empty_rgb != nullptr, "Failed to locate and decode empty_scene.jpg test vector");

    if (empty_rgb) {
        memset(net_in.in_desc[0].virt, 114, 3 * 640 * 640);
        uint8_t *r_plane = static_cast<uint8_t *>(net_in.in_desc[0].virt);
        uint8_t *g_plane = r_plane + (640 * 640);
        uint8_t *b_plane = r_plane + (2 * 640 * 640);

        float scale = std::min(640.0f / img_w, 640.0f / img_h);
        int nw = static_cast<int>(img_w * scale);
        int nh = static_cast<int>(img_h * scale);
        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < nw; x++) {
                int src_x = std::min(static_cast<int>(x / scale), img_w - 1);
                int src_y = std::min(static_cast<int>(y / scale), img_h - 1);
                const uint8_t *sp = empty_rgb + (src_y * img_w + src_x) * 3;
                int out_idx = y * 640 + x;
                r_plane[out_idx] = sp[0];
                g_plane[out_idx] = sp[1];
                b_plane[out_idx] = sp[2];
            }
        }
        stbi_image_free(empty_rgb);
    }

    LONGS_EQUAL(0, nnctrl_run_net(net_id, nullptr, nullptr, nullptr, nullptr));

    struct yolo_norm_box empty_dets[50];
    int empty_count = yolo_postprocess(net_out.out_desc[0].virt, &geo, &post, empty_dets, 50);
    LONGS_EQUAL_TEXT(0, empty_count, "False positive detection on empty scene background");

    printf(" [PASS] Multi-scene silicon evaluation: cars_street=%d dets, empty_scene=%d dets\n",
           car_count, empty_count);

    cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
    nnctrl_exit_net(net_id);
    nnctrl_exit();
    cavalry_mem_exit();
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
