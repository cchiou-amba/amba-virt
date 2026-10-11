/*
 * TestIavBench.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Benchmarks. They run only in bench mode and write one result row per sample. They measure
 * and set no thresholds: a test fails only if the harness itself cannot run the workload.
 * Every row carries a description of its workload so that two runs can be compared.
 */

#include "IavTestSupport.hxx"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <CppUTest/TestHarness.h>

using namespace iavtest;

namespace {

void emit(const char *bench, const char *unit, long long value, const char *workload)
{
    ctx().results().writeBench(bench, unit, value, workload);
}

} // namespace

TEST_GROUP(IavBench)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }
};

TEST(IavBench, IoctlLatencyGetState)
{
    const int samples = 1000;
    int errors = 0;

    for (int i = 0; i < samples; i++) {
        u32 state = 0;
        long long start = nowNs();
        int rc = ioctlPtr(_f.fd(), IAV_IOC_GET_IAV_STATE, &state);
        long long ns = nowNs() - start;

        if (rc != 0) {
            errors++;
        }
        emit("IoctlLatencyGetState", "ns", ns, "IAV_IOC_GET_IAV_STATE, one call per sample");
    }
    CHECK_TRUE(errors < samples);
}

TEST(IavBench, IoctlLatencyQueryCanvasInfo)
{
    const int samples = 1000;
    int errors = 0;

    for (int i = 0; i < samples; i++) {
        struct iav_canvas_info c;
        long long start, ns;
        int rc;

        memset(&c, 0, sizeof(c));
        start = nowNs();
        rc = queryCanvasInfo(_f.fd(), 0, &c);
        ns = nowNs() - start;
        if (rc != 0) {
            errors++;
        }
        emit("IoctlLatencyQueryCanvasInfo", "ns", ns,
             "IAV_IOC_QUERY_INFO IAV_INFO_CANVAS canvas 0, one call per sample");
    }
    CHECK_TRUE(errors < samples);
}

TEST(IavBench, IoctlLatencyQueryMemblock)
{
    const int samples = 1000;
    int errors = 0;

    for (int i = 0; i < samples; i++) {
        unsigned long addr, length;
        long long start = nowNs();
        int rc = queryPartition(_f.fd(), IAV_PART_DSP, &addr, &length);
        long long ns = nowNs() - start;

        if (rc != 0) {
            errors++;
        }
        emit("IoctlLatencyQueryMemblock", "ns", ns,
             "IAV_IOC_QUERY_MEMBLOCK IAV_PART_DSP, one call per sample");
    }
    CHECK_TRUE(errors < samples);
}

/*
 * A non-blocking canvas query, timed whether it returns a descriptor or reports that none is
 * ready. The sample is the cost of the call, not the wait for a frame.
 */
TEST(IavBench, QueryDescLatency)
{
    const int samples = 1000;
    int unexpected = 0;

    for (int i = 0; i < samples; i++) {
        struct iav_querydesc q;
        long long start = nowNs();
        int rc = queryCanvas(_f.fd(), false, &q);
        long long ns = nowNs() - start;

        if (rc != 0 && errno != EAGAIN) {
            unexpected++;
        }
        emit("QueryDescLatency", "ns", ns,
             "IAV_IOC_QUERY_DESC canvas 0 non-blocking, success or EAGAIN timed alike");
    }
    CHECK_TRUE(unexpected < samples);
}

/*
 * poll() on a file whose event is already pending, so the sample is the cost of the call and
 * not the wait for the next frame.
 */
TEST(IavBench, PollWakeLatency)
{
    const int samples = 500;
    struct iav_poll_event_config cfg;
    int failures = 0;

    canvasPollConfig(&cfg);
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
    for (int i = 0; i < samples; i++) {
        struct pollfd p;
        struct iav_querydesc q;
        long long start, ns;

        p.fd = _f.fd();
        p.events = POLLIN;
        p.revents = 0;
        if (backend().poll(&p, 1, 2000) != 1) {
            failures++;
            continue;
        }
        p.revents = 0;
        start = nowNs();
        backend().poll(&p, 1, 0);
        ns = nowNs() - start;
        emit("PollWakeLatency", "ns", ns,
             "poll() with a canvas event already pending, zero timeout, one call per sample");
        queryCanvas(_f.fd(), false, &q);
    }
    CHECK_TRUE(failures < samples);
}

TEST(IavBench, MmapSetupCost)
{
    const int samples = 100;
    unsigned long addr = 0, length = 0;
    char workload[160];

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_BSB, &addr, &length));
    snprintf(workload, sizeof(workload), "mmap plus munmap of IAV_PART_BSB, %lu bytes, read-only",
             length);
    for (int i = 0; i < samples; i++) {
        long long start = nowNs();
        void *p = backend().mmap(length, PROT_READ, MAP_SHARED, _f.fd(), (off_t)addr);
        long long ns;

        CHECK_TRUE(p != MAP_FAILED);
        backend().munmap(p, length);
        ns = nowNs() - start;
        emit("MmapSetupCost", "ns", ns, workload);
    }
}

TEST(IavBench, MmapReadBandwidth)
{
    const int samples = 50;
    unsigned long addr = 0, length = 0;
    ScopedMap map;
    size_t bytes;
    unsigned char *buf;
    char workload[160];

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_DSP, &addr, &length));
    CHECK_TRUE(map.map(_f.fd(), addr, length, PROT_READ));
    bytes = length < (16UL << 20) ? (size_t)length : (size_t)(16UL << 20);
    buf = (unsigned char *)malloc(bytes);
    CHECK_TRUE(buf != NULL);
    snprintf(workload, sizeof(workload),
             "memcpy of %zu bytes from the start of the IAV_PART_DSP mapping to private memory",
             bytes);
    for (int i = 0; i < samples; i++) {
        long long start = nowNs();
        long long ns;

        memcpy(buf, (const void *)map.bytes(), bytes);
        ns = nowNs() - start;
        emit("MmapReadBandwidth", "ns", ns, workload);
    }
    free(buf);
}

/* Reading one canvas frame (Y and UV) out of the mapping, after the descriptor says it is ready. */
TEST(IavBench, CanvasFrameReadCost)
{
    const int samples = 200;
    unsigned long addr = 0, length = 0;
    ScopedMap map;
    unsigned char *buf = NULL;
    size_t cap = 0;
    int failures = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_DSP, &addr, &length));
    CHECK_TRUE(map.map(_f.fd(), addr, length, PROT_READ));
    for (int i = 0; i < samples; i++) {
        struct iav_querydesc q;
        size_t yBytes, uvBytes;
        long long start, ns;
        char workload[160];

        if (queryCanvas(_f.fd(), true, &q) != 0) {
            failures++;
            continue;
        }
        yBytes = (size_t)q.arg.canvas.yuv.pitch * q.arg.canvas.yuv.height;
        uvBytes = (size_t)q.arg.canvas.yuv.pitch * (q.arg.canvas.yuv.height / 2);
        if (q.arg.canvas.yuv.y_addr_offset + yBytes > length ||
            q.arg.canvas.yuv.uv_addr_offset + uvBytes > length) {
            failures++;
            continue;
        }
        if (yBytes + uvBytes > cap) {
            free(buf);
            cap = yBytes + uvBytes;
            buf = (unsigned char *)malloc(cap);
            CHECK_TRUE(buf != NULL);
        }
        start = nowNs();
        memcpy(buf, (const void *)(map.bytes() + q.arg.canvas.yuv.y_addr_offset), yBytes);
        memcpy(buf + yBytes, (const void *)(map.bytes() + q.arg.canvas.yuv.uv_addr_offset), uvBytes);
        ns = nowNs() - start;
        snprintf(workload, sizeof(workload), "copy of one canvas 0 frame, %zu bytes, %ux%u",
                 yBytes + uvBytes, q.arg.canvas.yuv.width, q.arg.canvas.yuv.height);
        emit("CanvasFrameReadCost", "ns", ns, workload);
    }
    free(buf);
    CHECK_TRUE(failures < samples);
}

namespace {

struct ScaleWorker
{
    int batches;
    int callsPerBatch;
    int threads;
    int errors;
};

void *scaleThread(void *arg)
{
    ScaleWorker *w = (ScaleWorker *)arg;
    ScopedFd f;
    char workload[160];

    if (!f.open()) {
        __atomic_add_fetch(&w->errors, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    snprintf(workload, sizeof(workload),
             "mean ns per IAV_IOC_GET_IAV_STATE over %d calls, %d concurrent threads, one fd each",
             w->callsPerBatch, w->threads);
    for (int b = 0; b < w->batches; b++) {
        long long start = nowNs();

        for (int i = 0; i < w->callsPerBatch; i++) {
            u32 state = 0;

            if (ioctlPtr(f.fd(), IAV_IOC_GET_IAV_STATE, &state) != 0) {
                __atomic_add_fetch(&w->errors, 1, __ATOMIC_RELAXED);
            }
        }
        emit("ConcurrentScaling", "ns", (nowNs() - start) / w->callsPerBatch, workload);
    }
    return NULL;
}

} // namespace

/* 200 samples in total for each of 1, 2 and 4 threads. */
TEST(IavBench, ConcurrentScaling)
{
    static const int configs[3] = {1, 2, 4};

    for (int c = 0; c < 3; c++) {
        ScaleWorker w;
        pthread_t t[4];
        int k = configs[c];

        w.batches = 200 / k;
        w.callsPerBatch = 50;
        w.threads = k;
        w.errors = 0;
        for (int i = 0; i < k; i++) {
            LONGS_EQUAL(0, pthread_create(&t[i], NULL, scaleThread, &w));
        }
        for (int i = 0; i < k; i++) {
            pthread_join(t[i], NULL);
        }
        LONGS_EQUAL(0, w.errors);
    }
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
