/*
 * amba-virt-client.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "CppUTest/CommandLineTestRunner.h"
#include "CppUTest/TestHarness.h"

#include "amba_virt.h"
#include "amba_virt_dev.hxx"
#include "amba_virt_test.h"

// =============================================================================
// Helper Functions & Global Configuration
// =============================================================================
namespace {

uint64_t getMonotonicTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

struct LatencyStats {
    double _minUs{0.0};
    double _medianUs{0.0};
    double _meanUs{0.0};
    double _p90Us{0.0};
    double _p95Us{0.0};
    double _p99Us{0.0};
    double _maxUs{0.0};
    double _opsSec{0.0};
    size_t _count{0};
};

LatencyStats calculateLatencyStats(std::vector<double> &samples, double elapsedSec) {
    LatencyStats s;
    if (samples.empty())
        return s;

    std::sort(samples.begin(), samples.end());
    s._count = samples.size();
    s._minUs = samples.front();
    s._maxUs = samples.back();

    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    s._meanUs = sum / static_cast<double>(s._count);

    auto percentile = [&](double p) {
        size_t idx = static_cast<size_t>(p * (s._count - 1));
        return samples[idx];
    };

    s._medianUs = percentile(0.50);
    s._p90Us = percentile(0.90);
    s._p95Us = percentile(0.95);
    s._p99Us = percentile(0.99);
    s._opsSec = (elapsedSec > 0.0) ? (static_cast<double>(s._count) / elapsedSec) : 0.0;

    return s;
}

} // namespace

// =============================================================================
// TEST_GROUP(VFS): Linux VFS System Call Semantics on /dev/amba_virt
// =============================================================================
TEST_GROUP(VFS) {
    void setup() override {
        AmbaVirtDevice::ensureDevNode();
    }

    void teardown() override {
    }
};

TEST(VFS, OpenCloseFlags) {
    int fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
    CHECK_TRUE_TEXT(fd >= 0, "open(O_RDWR) failed");
    close(fd);

    fd = open(AMBA_VIRT_DEV_PATH, O_RDONLY);
    CHECK_TRUE_TEXT(fd >= 0, "open(O_RDONLY) failed");
    close(fd);

    fd = open(AMBA_VIRT_DEV_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    CHECK_TRUE_TEXT(fd >= 0, "open(O_RDWR | O_NONBLOCK | O_CLOEXEC) failed");
    close(fd);
}

TEST(VFS, ReadWriteDirectFallback) {
    int fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
    CHECK_TRUE(fd >= 0);

    char buf[64];
    ssize_t ret = read(fd, buf, sizeof(buf));
    // Driver implements messaging via ioctl, read() must fail cleanly without crash
    CHECK_TRUE(ret < 0);
    CHECK_EQUAL(EINVAL, errno);

    ret = write(fd, buf, sizeof(buf));
    CHECK_TRUE(ret < 0);
    CHECK_EQUAL(EINVAL, errno);

    close(fd);
}

TEST(VFS, LseekIllegalSeek) {
    int fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
    CHECK_TRUE(fd >= 0);

    off_t off = lseek(fd, 0, SEEK_SET);
    CHECK_EQUAL((off_t)-1, off);
    CHECK_EQUAL(ESPIPE, errno);

    close(fd);
}

TEST(VFS, IoctlGetInfo) {
    AmbaVirtDevice dev;
    LONGS_EQUAL(0, dev.openDevice());

    const auto &info = dev.getInfo();
    LONGS_EQUAL(AMBA_VIRT_PROTO, info.proto);
    LONGS_EQUAL(AMBA_VIRT_ROLE_GUEST, info.role);
    LONGS_EQUAL(AMBA_VIRT_VSOCK_CID, info.vsock_cid);
    LONGS_EQUAL(AMBA_VIRT_VSOCK_PORT, info.vsock_port);
    dev.closeDevice();
}

TEST(VFS, IoctlFaultHandling) {
    int fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
    CHECK_TRUE(fd >= 0);

    // Pass invalid memory pointer to ioctl
    int ret = ioctl(fd, AMBA_VIRT_IOC_GET_INFO, (void *)0x1);
    CHECK_EQUAL(-1, ret);
    CHECK_EQUAL(EFAULT, errno);

    close(fd);
}

TEST(VFS, IoctlInvalidMagic) {
    int fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
    CHECK_TRUE(fd >= 0);

    // Pass completely invalid ioctl command
    int ret = ioctl(fd, _IO('Z', 99));
    CHECK_EQUAL(-1, ret);
    CHECK_EQUAL(ENOTTY, errno);

    close(fd);
}

TEST(VFS, MmapBoundsAndOversize) {
    AmbaVirtDevice dev;
    LONGS_EQUAL(0, dev.openDevice());

    uint32_t shmSz = dev.getShmSize();
    if (shmSz > 0) {
        // Valid map of full size
        void *ptr = dev.mapShm(shmSz);
        CHECK_TRUE_TEXT(ptr != MAP_FAILED, "mmap full window failed");
        dev.unmapShm();

        // Valid map of single 4KB page
        ptr = dev.mapShm(4096);
        CHECK_TRUE_TEXT(ptr != MAP_FAILED, "mmap 4KB page failed");
        dev.unmapShm();

        // Over-allocation: request larger than window should fail with EINVAL
        void *badPtr = mmap(nullptr, (size_t)shmSz + 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, dev.getFd(), 0);
        CHECK_EQUAL(MAP_FAILED, badPtr);
        CHECK_EQUAL(EINVAL, errno);
    }
    dev.closeDevice();
}

TEST(VFS, MunmapAndMsync) {
    AmbaVirtDevice dev;
    LONGS_EQUAL(0, dev.openDevice());

    uint32_t shmSz = dev.getShmSize();
    if (shmSz >= 4096) {
        void *ptr = dev.mapShm(4096);
        CHECK_TRUE(ptr != MAP_FAILED);

        volatile uint8_t *b = static_cast<volatile uint8_t *>(ptr);
        b[0] = 0xAA;
        b[4095] = 0x55;

        int syncRet = msync(ptr, 4096, MS_SYNC);
        // ivshmem is physical MMIO (VM_IO); msync returns 0 or -1 with EINVAL/ENOMEM
        CHECK_TRUE(syncRet == 0 || errno == EINVAL || errno == ENOMEM);

        dev.unmapShm();
    }
    dev.closeDevice();
}

TEST(VFS, FcntlFlagsAndDup) {
    int fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
    CHECK_TRUE(fd >= 0);

    int flags = fcntl(fd, F_GETFL);
    CHECK_TRUE(flags >= 0);

    int ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    LONGS_EQUAL(0, ret);

    int newFlags = fcntl(fd, F_GETFL);
    CHECK_TRUE((newFlags & O_NONBLOCK) != 0);

    int dupFd = dup(fd);
    CHECK_TRUE(dupFd >= 0);
    close(dupFd);

    close(fd);
}

TEST(VFS, PollSelectReadiness) {
    int fd = open(AMBA_VIRT_DEV_PATH, O_RDWR | O_NONBLOCK);
    CHECK_TRUE(fd >= 0);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;

    // Driver poll check with 10ms timeout
    int ret = poll(&pfd, 1, 10);
    CHECK_TRUE(ret >= 0);

    close(fd);
}

// =============================================================================
// TEST_GROUP(Vsock): virt-vsock Protocol Corner Cases
// =============================================================================
TEST_GROUP(Vsock) {
    AmbaVirtDevice dev;

    void setup() override {
        LONGS_EQUAL(0, dev.openDevice());
        LONGS_EQUAL(0, dev.connect());
        dev.flushRx();
    }

    void teardown() override {
        dev.flushRx();
        dev.closeDevice();
    }
};

TEST(Vsock, ZeroBytePayloadRejection) {
    // 0-byte payload should be rejected with -EINVAL
    int ret = dev.send(nullptr, 0);
    CHECK_EQUAL(-EINVAL, ret);
}

TEST(Vsock, OversizedPayloadRejection) {
    std::vector<uint8_t> oversized(AMBA_VIRT_MAX_MSG + 1, 0x5A);
    int ret = dev.send(oversized.data(), oversized.size());
    CHECK_EQUAL(-EINVAL, ret);
}

TEST(Vsock, PingPongSequence) {
    struct amba_virt_msg msg;
    msg.type = AMBA_VIRT_MSG_PING;
    msg.seq = 1001;
    msg.shm_off = 0;
    msg.shm_len = 0;

    LONGS_EQUAL(0, dev.send(&msg, sizeof(msg)));

    struct amba_virt_msg resp;
    uint32_t rxLen = 0;
    LONGS_EQUAL(0, dev.recv(&resp, sizeof(resp), rxLen, 5000));
    CHECK_EQUAL(sizeof(resp), rxLen);
    LONGS_EQUAL(AMBA_VIRT_MSG_PONG, resp.type);
    LONGS_EQUAL(1001, resp.seq);
}

TEST(Vsock, BoundaryPayloadSizes) {
    const std::vector<uint32_t> sizes = {16, 64, 256, 512, 1024, 2048, AMBA_VIRT_MAX_MSG};

    for (uint32_t sz : sizes) {
        std::vector<uint8_t> txBuf(sz, 0xCC);
        struct amba_virt_msg *m = reinterpret_cast<struct amba_virt_msg *>(txBuf.data());
        m->type = AMBA_VIRT_MSG_ECHO_REQ;
        m->seq = sz;
        m->shm_off = 0;
        m->shm_len = sz;

        LONGS_EQUAL_TEXT(0, dev.send(txBuf.data(), sz), "Echo Send failed");

        std::vector<uint8_t> rxBuf(AMBA_VIRT_MAX_MSG, 0);
        uint32_t rxLen = 0;
        LONGS_EQUAL_TEXT(0, dev.recv(rxBuf.data(), rxBuf.size(), rxLen, 5000), "Echo Recv failed");
        LONGS_EQUAL(sz, rxLen);

        struct amba_virt_msg *resp = reinterpret_cast<struct amba_virt_msg *>(rxBuf.data());
        LONGS_EQUAL(AMBA_VIRT_MSG_ECHO_RESP, resp->type);
        LONGS_EQUAL(sz, resp->seq);
    }
}

TEST(Vsock, ChurnAndRapidCycles) {
    for (int i = 0; i < 50; ++i) {
        struct amba_virt_msg msg;
        msg.type = AMBA_VIRT_MSG_PING;
        msg.seq = static_cast<uint32_t>(i);
        msg.shm_off = 0;
        msg.shm_len = 0;

        LONGS_EQUAL(0, dev.send(&msg, sizeof(msg)));

        struct amba_virt_msg resp;
        uint32_t rxLen = 0;
        LONGS_EQUAL(0, dev.recv(&resp, sizeof(resp), rxLen, 3000));
        LONGS_EQUAL(AMBA_VIRT_MSG_PONG, resp.type);
        LONGS_EQUAL(i, resp.seq);
    }
}

// =============================================================================
// TEST_GROUP(SHM): ivshmem Shared Memory Integrity & Notification
// =============================================================================
TEST_GROUP(SHM) {
    AmbaVirtDevice dev;
    void *mapPtr{nullptr};
    uint32_t shmSz{0};

    void setup() override {
        LONGS_EQUAL(0, dev.openDevice());
        LONGS_EQUAL(0, dev.connect());
        dev.flushRx();
        shmSz = dev.getShmSize();
        if (shmSz > 0) {
            mapPtr = dev.mapShm(shmSz);
            CHECK_TRUE(mapPtr != MAP_FAILED);
        }
    }

    void teardown() override {
        dev.flushRx();
        dev.closeDevice();
    }
};

TEST(SHM, IncrementalCounterPattern) {
    if (shmSz < 4096)
        return;

    uint32_t testLen = 1024;
    uint8_t *buf = static_cast<uint8_t *>(mapPtr);
    for (uint32_t i = 0; i < testLen; ++i) {
        buf[i] = static_cast<uint8_t>((i + 0x42) & 0xFF);
    }

    struct amba_virt_msg msg;
    msg.type = AMBA_VIRT_MSG_SHM_NOTIFY;
    msg.seq = 777;
    msg.shm_off = 0;
    msg.shm_len = testLen;

    LONGS_EQUAL(0, dev.send(&msg, sizeof(msg)));

    struct amba_virt_msg resp;
    uint32_t rxLen = 0;
    LONGS_EQUAL(0, dev.recv(&resp, sizeof(resp), rxLen, 5000));
    LONGS_EQUAL(AMBA_VIRT_MSG_SHM_ACK, resp.type);
    LONGS_EQUAL(777, resp.seq);
    LONGS_EQUAL(testLen, resp.shm_len);
}

TEST(SHM, PRBS31PatternVerification) {
    if (shmSz < 65536)
        return;

    uint32_t testLen = 65536;
    uint32_t *words = static_cast<uint32_t *>(mapPtr);
    uint32_t state = 0x55AA55AA;

    for (size_t i = 0; i < testLen / sizeof(uint32_t); ++i) {
        state = amba_virt_prbs31_next(state);
        words[i] = state;
    }

    struct amba_virt_msg msg;
    msg.type = AMBA_VIRT_MSG_SHM_NOTIFY;
    msg.seq = 888;
    msg.shm_off = 0;
    msg.shm_len = testLen;

    LONGS_EQUAL(0, dev.send(&msg, sizeof(msg)));

    struct amba_virt_msg resp;
    uint32_t rxLen = 0;
    LONGS_EQUAL(0, dev.recv(&resp, sizeof(resp), rxLen, 5000));
    LONGS_EQUAL(AMBA_VIRT_MSG_SHM_ACK, resp.type);
}

TEST(SHM, AlignmentsAndOffsets) {
    if (shmSz < 8192)
        return;

    std::vector<uint32_t> offsets = {0, 64, 4096, 4097};
    if (shmSz >= 512) {
        offsets.push_back(shmSz - 256); // Tail boundary of the shared memory window
    }
    uint8_t *buf = static_cast<uint8_t *>(mapPtr);

    for (uint32_t off : offsets) {
        if (off + 256 > shmSz)
            continue;

        for (uint32_t i = 0; i < 256; ++i) {
            buf[off + i] = static_cast<uint8_t>((off + i) & 0xFF);
        }

        struct amba_virt_msg msg;
        msg.type = AMBA_VIRT_MSG_SHM_NOTIFY;
        msg.seq = off;
        msg.shm_off = off;
        msg.shm_len = 256;

        LONGS_EQUAL(0, dev.send(&msg, sizeof(msg)));

        struct amba_virt_msg resp;
        uint32_t rxLen = 0;
        LONGS_EQUAL(0, dev.recv(&resp, sizeof(resp), rxLen, 5000));
        LONGS_EQUAL(AMBA_VIRT_MSG_SHM_ACK, resp.type);
        LONGS_EQUAL(off, resp.shm_off);
    }
}

// =============================================================================
// TEST_GROUP(MultiFD): Multiple Descriptors & Multi-Thread Concurrency
// =============================================================================
TEST_GROUP(MultiFD) {
    void setup() override {
        AmbaVirtDevice::ensureDevNode();
        AmbaVirtDevice d;
        if (d.openDevice() == 0 && d.connect() == 0) {
            d.flushRx();
            d.closeDevice();
        }
    }
    void teardown() override {
        AmbaVirtDevice d;
        if (d.openDevice() == 0 && d.connect() == 0) {
            d.flushRx();
            d.closeDevice();
        }
    }
};

TEST(MultiFD, MultipleDescriptorsSingleProcess) {
    std::vector<AmbaVirtDevice> devs(4);
    for (size_t i = 0; i < devs.size(); ++i) {
        LONGS_EQUAL(0, devs[i].openDevice());
        LONGS_EQUAL(0, devs[i].connect());
        devs[i].flushRx();

        struct amba_virt_msg msg;
        msg.type = AMBA_VIRT_MSG_PING;
        msg.seq = static_cast<uint32_t>(i + 500);
        LONGS_EQUAL(0, devs[i].send(&msg, sizeof(msg)));

        struct amba_virt_msg resp;
        uint32_t rxLen = 0;
        LONGS_EQUAL(0, devs[i].recv(&resp, sizeof(resp), rxLen, 5000));
        LONGS_EQUAL(AMBA_VIRT_MSG_PONG, resp.type);
        LONGS_EQUAL(i + 500, resp.seq);
    }

    for (auto &d : devs)
        d.closeDevice();
}

TEST(MultiFD, ConcurrentThreads) {
    const int numThreads = 4;
    const int iterationsPerThread = 25;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    {
        AmbaVirtDevice initDev;
        if (initDev.openDevice() == 0 && initDev.connect() == 0) {
            initDev.flushRx();
        }
    }

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([t, iterationsPerThread, &successCount]() {
            AmbaVirtDevice d;
            if (d.openDevice() != 0 || d.connect() != 0)
                return;

            for (int i = 0; i < iterationsPerThread; ++i) {
                struct amba_virt_msg msg;
                msg.type = AMBA_VIRT_MSG_PING;
                msg.seq = static_cast<uint32_t>(t * 1000 + i);
                if (d.send(&msg, sizeof(msg)) != 0)
                    break;

                struct amba_virt_msg resp;
                uint32_t rxLen = 0;
                if (d.recv(&resp, sizeof(resp), rxLen, 5000) != 0)
                    break;

                if (resp.type == AMBA_VIRT_MSG_PONG)
                    successCount.fetch_add(1);
            }
            d.closeDevice();
        });
    }

    for (auto &th : threads)
        th.join();

    LONGS_EQUAL(numThreads * iterationsPerThread, successCount.load());
}

// =============================================================================
// Benchmark Runner Implementation
// =============================================================================
namespace {

void runBenchmarks(int iterations, int threadsNum, const std::string &jsonOut) {
    std::cout << "================================================================\n"
              << "  amba-virt IPC Performance Benchmark Suite\n"
              << "================================================================\n"
              << "Iterations: " << iterations << " | Threads: " << threadsNum << "\n\n";

    AmbaVirtDevice dev;
    if (dev.openDevice() != 0 || dev.connect() != 0) {
        std::cerr << "Error: Cannot open or connect to /dev/amba_virt for benchmarking.\n";
        return;
    }

    std::ofstream jfile;
    if (!jsonOut.empty())
        jfile.open(jsonOut);

    std::stringstream jsonStream;
    jsonStream << "{\n  \"benchmarks\": {\n";

    // 1. Vsock Latency Sweep
    std::cout << "--- 1. Vsock Round-Trip Latency (RTT) ---\n";
    const std::vector<uint32_t> payloadSizes = {16, 64, 256, 1024, 2048, 4096};

    for (size_t i = 0; i < payloadSizes.size(); ++i) {
        uint32_t sz = payloadSizes[i];
        std::vector<uint8_t> tx(sz, 0xEE);
        std::vector<uint8_t> rx(AMBA_VIRT_MAX_MSG);
        struct amba_virt_msg *m = reinterpret_cast<struct amba_virt_msg *>(tx.data());
        m->type = AMBA_VIRT_MSG_ECHO_REQ;

        std::vector<double> latenciesUs;
        latenciesUs.reserve(iterations);

        // Warmup
        for (int w = 0; w < 50; ++w) {
            m->seq = w;
            dev.send(tx.data(), sz);
            uint32_t rxLen;
            dev.recv(rx.data(), rx.size(), rxLen, 2000);
        }

        uint64_t startTotal = getMonotonicTimeNs();
        for (int it = 0; it < iterations; ++it) {
            m->seq = it;
            uint64_t t0 = getMonotonicTimeNs();
            dev.send(tx.data(), sz);
            uint32_t rxLen;
            dev.recv(rx.data(), rx.size(), rxLen, 5000);
            uint64_t t1 = getMonotonicTimeNs();
            latenciesUs.push_back(static_cast<double>(t1 - t0) / 1000.0);
        }
        uint64_t endTotal = getMonotonicTimeNs();
        double totalSec = static_cast<double>(endTotal - startTotal) / 1e9;

        LatencyStats st = calculateLatencyStats(latenciesUs, totalSec);

        std::cout << "Payload: " << std::setw(4) << sz << "B | "
                  << "Min: " << std::fixed << std::setprecision(1) << st._minUs << " us | "
                  << "Median: " << st._medianUs << " us | "
                  << "Mean: " << st._meanUs << " us | "
                  << "p95: " << st._p95Us << " us | "
                  << "p99: " << st._p99Us << " us | "
                  << "Max: " << st._maxUs << " us | "
                  << "Ops/sec: " << std::setprecision(0) << st._opsSec << "\n";

        jsonStream << "    \"vsock_rtt_" << sz << "B\": {\n"
                   << "      \"payload_bytes\": " << sz << ",\n"
                   << "      \"min_us\": " << st._minUs << ",\n"
                   << "      \"median_us\": " << st._medianUs << ",\n"
                   << "      \"mean_us\": " << st._meanUs << ",\n"
                   << "      \"p95_us\": " << st._p95Us << ",\n"
                   << "      \"p99_us\": " << st._p99Us << ",\n"
                   << "      \"max_us\": " << st._maxUs << ",\n"
                   << "      \"ops_sec\": " << st._opsSec << "\n"
                   << "    }" << ((i + 1 < payloadSizes.size()) ? "," : "") << "\n";
    }

    // 2. ivshmem Zero-Copy Handoff Throughput
    std::cout << "\n--- 2. ivshmem Zero-Copy Handoff Throughput ---\n";
    uint32_t shmSz = dev.getShmSize();
    if (shmSz > 0) {
        void *shmPtr = dev.mapShm(shmSz);
        if (shmPtr != MAP_FAILED) {
            const std::vector<uint32_t> shmTransferSizes = {
                4096,       // 4 KB   - baseline page
                65536,      // 64 KB  - control/metadata
                262144,     // 256 KB - intermediate buffer
                1048576,    // 1 MB   - small tensor / low-res frame
                4194304,    // 4 MB   - 1080p frame
                16777216,   // 16 MB  - uncompressed 4K video frame
                67108864,   // 64 MB  - 8K frame / 4-camera sync burst
                134217728,  // 128 MB - 8-camera rig burst / large NN batch
                268435456,  // 256 MB - stress ceiling (25% of 1 GB pool)
            };

            jsonStream << ",\n    \"shm_handoff\": [\n";

            for (size_t s = 0; s < shmTransferSizes.size(); ++s) {
                uint32_t bsz = shmTransferSizes[s];
                if (bsz > shmSz)
                    continue;

                int benchIters = std::max(50, iterations / 10);
                volatile uint32_t *p = static_cast<volatile uint32_t *>(shmPtr);
                uint32_t words = bsz / sizeof(uint32_t);

                uint64_t tStart = getMonotonicTimeNs();
                for (int it = 0; it < benchIters; ++it) {
                    // Client write phase
                    p[0] = it;
                    p[words - 1] = it + 1;

                    // Vsock signal phase
                    struct amba_virt_msg msg;
                    msg.type = AMBA_VIRT_MSG_SHM_NOTIFY;
                    msg.seq = it;
                    msg.shm_off = 0;
                    msg.shm_len = bsz;
                    dev.send(&msg, sizeof(msg));

                    struct amba_virt_msg ack;
                    uint32_t rxLen;
                    dev.recv(&ack, sizeof(ack), rxLen, 5000);
                }
                uint64_t tEnd = getMonotonicTimeNs();
                double sec = static_cast<double>(tEnd - tStart) / 1e9;
                double totalGb = (static_cast<double>(bsz) * benchIters) / (1024.0 * 1024.0 * 1024.0);
                double gbps = totalGb / sec;
                double handoffsSec = static_cast<double>(benchIters) / sec;

                std::cout << "Buffer Size: " << std::setw(8) << (bsz / 1024) << " KB | "
                          << "Handoffs/sec: " << std::fixed << std::setprecision(0) << handoffsSec << " | "
                          << "Effective Bandwidth: " << std::setprecision(2) << gbps << " GB/s\n";

                jsonStream << "      {\n"
                           << "        \"buffer_bytes\": " << bsz << ",\n"
                           << "        \"handoffs_sec\": " << handoffsSec << ",\n"
                           << "        \"effective_bandwidth_gbps\": " << gbps << "\n"
                           << "      }" << ((s + 1 < shmTransferSizes.size()) ? "," : "") << "\n";
            }
            jsonStream << "    ]\n";
            dev.unmapShm();
        }
    }

    jsonStream << "  }\n}\n";

    if (jfile.is_open()) {
        jfile << jsonStream.str();
        jfile.close();
        std::cout << "\nBenchmark results saved to: " << jsonOut << "\n";
    }

    dev.closeDevice();
}

} // namespace

// =============================================================================
// main Entry Point
// =============================================================================
int main(int argc, char **argv) {
    bool runBench = false;
    int iterations = 1000;
    int threads = 1;
    std::string jsonFile = "";

    std::vector<char *> filteredArgv;
    filteredArgv.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-bench") {
            runBench = true;
        } else if (arg == "-iterations" && i + 1 < argc) {
            iterations = std::atoi(argv[++i]);
        } else if (arg == "-threads" && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
        } else if (arg == "-json" && i + 1 < argc) {
            jsonFile = argv[++i];
        } else {
            filteredArgv.push_back(argv[i]);
        }
    }

    int testResult = 0;
    if (filteredArgv.size() > 1 || !runBench) {
        int cArgc = static_cast<int>(filteredArgv.size());
        testResult = CommandLineTestRunner::RunAllTests(cArgc, filteredArgv.data());
    }

    if (runBench) {
        runBenchmarks(iterations, threads, jsonFile);
    }

    return testResult;
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
