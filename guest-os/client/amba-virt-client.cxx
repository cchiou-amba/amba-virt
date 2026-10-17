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
#include <random>
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

void storeBarrier() {
#if defined(__aarch64__)
    asm volatile("dsb st" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

void loadBarrier() {
#if defined(__aarch64__)
    asm volatile("dsb ld" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

uint32_t ensureFixtureExtent(AmbaVirtDevice &dev,
                             std::vector<uint32_t> &ownedExtents,
                             void *&mapPtr,
                             uint32_t &extentOffset,
                             uint32_t &extentSize,
                             uint32_t size) {
    struct amba_virt_mem_resp resp;
    int ret = dev.allocMemory(size, 0x200000, 0, resp);
    LONGS_EQUAL(0, ret);
    LONGS_EQUAL(0, resp.status);
    CHECK_TRUE(resp.allocated_size >= size);
    ownedExtents.push_back(resp.bar_offset);
    if (mapPtr == nullptr || mapPtr == MAP_FAILED) {
        mapPtr = dev.mapShm();
        CHECK_TRUE(mapPtr != MAP_FAILED);
    }
    extentOffset = resp.bar_offset;
    extentSize = resp.allocated_size;
    return resp.bar_offset;
}

void releaseFixtureExtents(AmbaVirtDevice &dev, std::vector<uint32_t> &ownedExtents) {
    for (auto it = ownedExtents.rbegin(); it != ownedExtents.rend(); ++it) {
        dev.freeMemory(*it);
    }
    ownedExtents.clear();
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

bool g_liveDemoMode = false;
bool g_verboseMode = false;

void detectRuntimeEnvironment() {
    AmbaVirtDevice::ensureDevNode();
    AmbaVirtDevice dev;
    if (dev.openDevice() != 0 || dev.connect() != 0) {
        g_liveDemoMode = false;
        return;
    }
    dev.flushRx();
    std::vector<struct amba_virt_dev_mem_desc> devs;
    if (dev.queryDevMem(devs) == 0) {
        for (const auto &d : devs) {
            if (d.dev_type == AMBA_VIRT_DEV_TYPE_CAVALRY) {
                g_liveDemoMode = true;
                break;
            }
        }
    }
    dev.flushRx();
    dev.closeDevice();

    if (g_liveDemoMode) {
        std::cout << "[Env] Live workload detected (CAVALRY active) — running in live-coexistence mode.\n";
    } else {
        std::cout << "[Env] Clean BAR pool detected — running in clean-pool mode.\n";
    }
}

} // namespace

// =============================================================================
// TEST_GROUP(VFS): Linux VFS System Call Semantics on /dev/amba_virt
// =============================================================================
TEST_GROUP(VFS) {
    AmbaVirtDevice _dev;
    std::vector<uint32_t> _ownedExtents;

    void setup() override {
        AmbaVirtDevice::ensureDevNode();
        _ownedExtents.clear();
    }

    void teardown() override {
        if (_dev.isOpen()) {
            _dev.unmapShm();
            releaseFixtureExtents(_dev, _ownedExtents);
            _dev.flushRx();
            _dev.closeDevice();
        }
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
    CHECK_TRUE(info.proto >= 2);
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

        // Over-allocation: request larger than window should fail with EINVAL (Linux) or ENXIO/EINVAL (QNX)
        void *badPtr = mmap(nullptr, (size_t)shmSz + 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, dev.getFd(), 0);
        CHECK_EQUAL(MAP_FAILED, badPtr);
#if defined(__QNX__) || defined(__QNXNTO__)
        CHECK_TRUE(errno == EINVAL || errno == ENXIO);
#else
        CHECK_EQUAL(EINVAL, errno);
#endif
    }
    dev.closeDevice();
}

TEST(VFS, MunmapAndMsync) {
    LONGS_EQUAL(0, _dev.openDevice());
    LONGS_EQUAL(0, _dev.connect());
    _dev.flushRx();

    struct amba_virt_mem_resp mresp;
    LONGS_EQUAL(0, _dev.allocMemory(4096, 4096, 0, mresp));
    LONGS_EQUAL(0, mresp.status);
    CHECK_TRUE(mresp.allocated_size >= 4096);
    _ownedExtents.push_back(mresp.bar_offset);

    void *ptr = _dev.mapShm(4096, PROT_READ | PROT_WRITE, MAP_SHARED, mresp.bar_offset);
    CHECK_TRUE(ptr != MAP_FAILED);

    volatile uint8_t *b = static_cast<volatile uint8_t *>(ptr);
    b[0] = 0xAA;
    b[4095] = 0x55;

    int syncRet = msync(ptr, 4096, MS_SYNC);
    // ivshmem is physical MMIO (VM_IO); msync returns 0 or -1 with EINVAL/ENOMEM
    CHECK_TRUE(syncRet == 0 || errno == EINVAL || errno == ENOMEM);

    _dev.unmapShm();
    LONGS_EQUAL(0, _dev.freeMemory(mresp.bar_offset));
    _ownedExtents.clear();
    _dev.closeDevice();
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
    std::vector<uint32_t> _ownedExtents;
    void *mapPtr{nullptr};
    uint32_t extentOffset{0};
    uint32_t extentSize{0};

    uint32_t ensureExtent(uint32_t size) {
        return ensureFixtureExtent(dev, _ownedExtents, mapPtr, extentOffset, extentSize, size);
    }

    void setup() override {
        LONGS_EQUAL(0, dev.openDevice());
        LONGS_EQUAL(0, dev.connect());
        dev.flushRx();
        _ownedExtents.clear();
        mapPtr = nullptr;
        extentOffset = 0;
        extentSize = 0;
        ensureExtent(65536);
    }

    void teardown() override {
        dev.unmapShm();
        mapPtr = nullptr;
        releaseFixtureExtents(dev, _ownedExtents);
        dev.flushRx();
        dev.closeDevice();
    }
};

TEST(SHM, IncrementalCounterPattern) {
    CHECK_TRUE(extentSize >= 4096);

    uint32_t testLen = 1024;
    uint8_t *buf = static_cast<uint8_t *>(mapPtr) + extentOffset;
    for (uint32_t i = 0; i < testLen; ++i) {
        buf[i] = static_cast<uint8_t>((i + 0x42) & 0xFF);
    }
    storeBarrier();

    struct amba_virt_msg msg;
    msg.type = AMBA_VIRT_MSG_SHM_NOTIFY;
    msg.seq = 777;
    msg.shm_off = extentOffset;
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
    CHECK_TRUE(extentSize >= 65536);

    uint32_t testLen = 65536;
    uint32_t *words = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(mapPtr) + extentOffset);
    uint32_t state = 0x55AA55AA;

    for (size_t i = 0; i < testLen / sizeof(uint32_t); ++i) {
        state = amba_virt_prbs31_next(state);
        words[i] = state;
    }
    storeBarrier();

    struct amba_virt_msg msg;
    msg.type = AMBA_VIRT_MSG_SHM_NOTIFY;
    msg.seq = 888;
    msg.shm_off = extentOffset;
    msg.shm_len = testLen;

    LONGS_EQUAL(0, dev.send(&msg, sizeof(msg)));

    struct amba_virt_msg resp;
    uint32_t rxLen = 0;
    LONGS_EQUAL(0, dev.recv(&resp, sizeof(resp), rxLen, 5000));
    LONGS_EQUAL(AMBA_VIRT_MSG_SHM_ACK, resp.type);
}

TEST(SHM, AlignmentsAndOffsets) {
    CHECK_TRUE(extentSize >= 8192);

    const std::vector<uint32_t> offsets = {0, 64, 4096, 4097, extentSize - 256};
    uint8_t *buf = static_cast<uint8_t *>(mapPtr) + extentOffset;

    for (uint32_t off : offsets) {
        CHECK_TRUE(off + 256 <= extentSize);

        for (uint32_t i = 0; i < 256; ++i) {
            buf[off + i] = static_cast<uint8_t>((off + i) & 0xFF);
        }
        storeBarrier();

        uint32_t targetOff = extentOffset + off;
        struct amba_virt_msg msg;
        msg.type = AMBA_VIRT_MSG_SHM_NOTIFY;
        msg.seq = off;
        msg.shm_off = targetOff;
        msg.shm_len = 256;

        LONGS_EQUAL(0, dev.send(&msg, sizeof(msg)));

        struct amba_virt_msg resp;
        uint32_t rxLen = 0;
        LONGS_EQUAL(0, dev.recv(&resp, sizeof(resp), rxLen, 5000));
        LONGS_EQUAL(AMBA_VIRT_MSG_SHM_ACK, resp.type);
        LONGS_EQUAL(targetOff, resp.shm_off);
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
// TEST_GROUP(InBandDeviceConfig): Dynamic Boundary Negotiation & Conflicts
// =============================================================================
TEST_GROUP(InBandDeviceConfig) {
    AmbaVirtDevice dev;
    std::vector<struct amba_virt_dev_mem_desc> _initialDevs;
    std::vector<uint32_t> _ownedDevs;
    std::vector<uint32_t> _ownedExtents;
    uint32_t _freeBase{0};
    uint32_t _availQuota{0};
    bool _liveCoexistence{false};

    void setup() override {
        LONGS_EQUAL(0, dev.openDevice());
        LONGS_EQUAL(0, dev.connect());
        dev.flushRx();
        _initialDevs.clear();
        _ownedDevs.clear();
        _ownedExtents.clear();
        LONGS_EQUAL(0, dev.queryDevMem(_initialDevs));

        uint64_t maxEnd = 0;
        uint64_t usedBytes = 0;
        bool cavPresent = false;
        for (const auto &d : _initialDevs) {
            if (d.dev_type == AMBA_VIRT_DEV_TYPE_CAVALRY) {
                cavPresent = true;
            }
            uint64_t devEnd = static_cast<uint64_t>(d.base_offset) + d.size;
            if (devEnd > maxEnd) {
                maxEnd = devEnd;
            }
            if (d.rpc_arena_size > 0) {
                uint64_t arenaEnd = static_cast<uint64_t>(d.rpc_arena_offset) + d.rpc_arena_size;
                if (arenaEnd > maxEnd) {
                    maxEnd = arenaEnd;
                }
            }
            usedBytes += static_cast<uint64_t>(d.size) + d.rpc_arena_size;
        }
        const uint64_t align2M = 0x200000ULL;
        _freeBase = static_cast<uint32_t>(((maxEnd + align2M - 1ULL) / align2M) * align2M);
        uint64_t shmSz = dev.getShmSize();
        _availQuota = (shmSz > usedBytes) ? static_cast<uint32_t>(shmSz - usedBytes) : 0U;
        _liveCoexistence = g_liveDemoMode || cavPresent;
    }

    void teardown() override {
        for (auto it = _ownedDevs.rbegin(); it != _ownedDevs.rend(); ++it) {
            dev.releaseDeviceBounds(*it);
        }
        _ownedDevs.clear();
        releaseFixtureExtents(dev, _ownedExtents);
        dev.flushRx();
        dev.closeDevice();
    }

    void fillUnalignedGapsBelowFreeBase() {
        uint64_t shmSz = dev.getShmSize();
        uint64_t tailBytes = (shmSz > _freeBase) ? (shmSz - _freeBase) : 0ULL;
        while (_availQuota > tailBytes) {
            uint32_t gap = static_cast<uint32_t>(_availQuota - tailBytes);
            struct amba_virt_mem_resp gresp;
            int gret = dev.allocMemory(gap, 4096, 0, gresp);
            LONGS_EQUAL(0, gret);
            LONGS_EQUAL(0, gresp.status);
            _ownedExtents.push_back(gresp.bar_offset);
            _availQuota = gresp.total_free;
        }
    }

    void requireUnregistered(uint32_t devType) {
        for (const auto &d : _initialDevs) {
            if (d.dev_type == devType) {
                FAIL("Target device type is already registered before test start");
            }
        }
    }

    void recordOwnedDev(uint32_t devType) {
        if (std::find(_ownedDevs.begin(), _ownedDevs.end(), devType) == _ownedDevs.end()) {
            _ownedDevs.push_back(devType);
        }
    }

    void forgetOwnedDev(uint32_t devType) {
        _ownedDevs.erase(std::remove(_ownedDevs.begin(), _ownedDevs.end(), devType),
                         _ownedDevs.end());
    }
};

TEST(InBandDeviceConfig, SuccessAutoOffset) {
    requireUnregistered(AMBA_VIRT_DEV_TYPE_SCRATCH);

    const uint32_t reqSize = _liveCoexistence ? (8 * 1024 * 1024) : (16 * 1024 * 1024);

    struct amba_virt_dev_bounds_req req;
    memset(&req, 0, sizeof(req));
    req.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
    req.requested_size = reqSize;
    req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
    req.flags = AMBA_VIRT_DEV_F_NONE;

    struct amba_virt_dev_bounds_resp resp;
    LONGS_EQUAL(0, dev.setDeviceBounds(req, resp));
    LONGS_EQUAL(0, resp.status);
    recordOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
    LONGS_EQUAL(AMBA_VIRT_ERR_NONE, resp.err_code);
    LONGS_EQUAL(reqSize, resp.granted_size);

    LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_SCRATCH, &resp));
    LONGS_EQUAL(0, resp.status);
    forgetOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
}

TEST(InBandDeviceConfig, CollisionReturnsSuggestedOffset) {
    if (_liveCoexistence) {
        requireUnregistered(AMBA_VIRT_DEV_TYPE_GDMA);
        requireUnregistered(AMBA_VIRT_DEV_TYPE_SCRATCH);

        // 1. Register GDMA at [_freeBase, _freeBase + 8 MiB)
        struct amba_virt_dev_bounds_req gdmaReq;
        memset(&gdmaReq, 0, sizeof(gdmaReq));
        gdmaReq.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
        gdmaReq.preferred_offset = _freeBase;
        gdmaReq.requested_size = 8 * 1024 * 1024;
        gdmaReq.flags = AMBA_VIRT_DEV_F_EXACT;

        struct amba_virt_dev_bounds_resp resp;
        LONGS_EQUAL(0, dev.setDeviceBounds(gdmaReq, resp));
        LONGS_EQUAL(0, resp.status);
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_GDMA);
        LONGS_EQUAL(_freeBase, resp.granted_offset);

        // 2. Request SCRATCH colliding at _freeBase + 4 MiB with 1 MiB RPC arena
        struct amba_virt_dev_bounds_req scratchReq;
        memset(&scratchReq, 0, sizeof(scratchReq));
        scratchReq.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
        scratchReq.preferred_offset = _freeBase + 0x00400000U;
        scratchReq.requested_size = 8 * 1024 * 1024;
        scratchReq.rpc_arena_size = 1024 * 1024;
        scratchReq.align = 0x200000U;
        scratchReq.flags = AMBA_VIRT_DEV_F_EXACT;

        int ret = dev.setDeviceBounds(scratchReq, resp);
        LONGS_EQUAL(-EEXIST, ret);
        LONGS_EQUAL(-EEXIST, resp.status);
        LONGS_EQUAL(AMBA_VIRT_ERR_OFFSET_COLLISION, resp.err_code);
        LONGS_EQUAL(AMBA_VIRT_DEV_TYPE_GDMA, resp.colliding_dev);
        LONGS_EQUAL(_freeBase + 0x00800000U, resp.suggested_offset);

        // 3. Retry SCRATCH at suggested_offset (_freeBase + 8 MiB)
        scratchReq.preferred_offset = resp.suggested_offset;
        LONGS_EQUAL(0, dev.setDeviceBounds(scratchReq, resp));
        LONGS_EQUAL(0, resp.status);
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
        LONGS_EQUAL(_freeBase + 0x00800000U, resp.granted_offset);
        LONGS_EQUAL(8 * 1024 * 1024, resp.granted_size);
        LONGS_EQUAL(_freeBase + 0x01000000U, resp.rpc_arena_offset);

        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_SCRATCH));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_GDMA));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_GDMA);
    } else {
        requireUnregistered(AMBA_VIRT_DEV_TYPE_GDMA);
        requireUnregistered(AMBA_VIRT_DEV_TYPE_CAVALRY);

        // 1. Register GDMA at [0x00000000, 0x04000000) (64 MiB)
        struct amba_virt_dev_bounds_req gdmaReq;
        memset(&gdmaReq, 0, sizeof(gdmaReq));
        gdmaReq.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
        gdmaReq.preferred_offset = 0x00000000;
        gdmaReq.requested_size = 64 * 1024 * 1024;
        gdmaReq.flags = AMBA_VIRT_DEV_F_EXACT;

        struct amba_virt_dev_bounds_resp resp;
        LONGS_EQUAL(0, dev.setDeviceBounds(gdmaReq, resp));
        LONGS_EQUAL(0, resp.status);
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_GDMA);
        LONGS_EQUAL(0x00000000, resp.granted_offset);

        // 2. Request Cavalry colliding at 0x02000000 with F_EXACT
        struct amba_virt_dev_bounds_req cavReq;
        memset(&cavReq, 0, sizeof(cavReq));
        cavReq.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
        cavReq.preferred_offset = 0x02000000;
        cavReq.requested_size = 64 * 1024 * 1024;
        cavReq.rpc_arena_size = 1024 * 1024;
        cavReq.flags = AMBA_VIRT_DEV_F_EXACT;

        int ret = dev.setDeviceBounds(cavReq, resp);
        LONGS_EQUAL(-EEXIST, ret);
        LONGS_EQUAL(-EEXIST, resp.status);
        LONGS_EQUAL(AMBA_VIRT_ERR_OFFSET_COLLISION, resp.err_code);
        LONGS_EQUAL(AMBA_VIRT_DEV_TYPE_GDMA, resp.colliding_dev);
        LONGS_EQUAL(0x04000000, resp.suggested_offset);

        // 3. Retry Cavalry at suggested_offset (0x04000000)
        cavReq.preferred_offset = resp.suggested_offset;
        LONGS_EQUAL(0, dev.setDeviceBounds(cavReq, resp));
        LONGS_EQUAL(0, resp.status);
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_CAVALRY);
        LONGS_EQUAL(0x04000000, resp.granted_offset);
        LONGS_EQUAL(64 * 1024 * 1024, resp.granted_size);
        LONGS_EQUAL(0x08000000, resp.rpc_arena_offset);

        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_CAVALRY));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_CAVALRY);
        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_GDMA));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_GDMA);
    }
}

TEST(InBandDeviceConfig, QuotaExceededExactFails) {
    if (_liveCoexistence) {
        requireUnregistered(AMBA_VIRT_DEV_TYPE_SCRATCH);

        // 1. Request exceeding 1 GiB BAR ceiling returns AMBA_VIRT_ERR_BAR_OVERFLOW
        struct amba_virt_dev_bounds_req barReq;
        memset(&barReq, 0, sizeof(barReq));
        barReq.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
        barReq.preferred_offset = _freeBase;
        barReq.requested_size = 0x60000000U; // 1536 MiB > 1 GiB BAR
        barReq.flags = AMBA_VIRT_DEV_F_EXACT;

        struct amba_virt_dev_bounds_resp resp;
        int ret = dev.setDeviceBounds(barReq, resp);
        LONGS_EQUAL(-ERANGE, ret);
        LONGS_EQUAL(-ERANGE, resp.status);
        LONGS_EQUAL(AMBA_VIRT_ERR_BAR_OVERFLOW, resp.err_code);

        // 2. Consume half of remaining tail quota then request exact beyond remaining quota
        fillUnalignedGapsBelowFreeBase();
        uint32_t reserveSize = (_availQuota / 2U) & ~0x1FFFFFU;
        CHECK_TRUE(reserveSize >= 0x200000U);

        struct amba_virt_mem_resp mresp;
        int allocRet = dev.allocMemory(reserveSize, 0x200000, 0, mresp);
        LONGS_EQUAL(0, allocRet);
        LONGS_EQUAL(0, mresp.status);
        _ownedExtents.push_back(mresp.bar_offset);
        uint32_t remQuota = mresp.total_free;

        struct amba_virt_dev_bounds_req scratchReq;
        memset(&scratchReq, 0, sizeof(scratchReq));
        scratchReq.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
        scratchReq.requested_size = (remQuota + 4 * 1024 * 1024) & ~0x1FFFFFU;
        scratchReq.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
        scratchReq.align = 0x200000U;
        scratchReq.flags = AMBA_VIRT_DEV_F_EXACT;

        int sRet = dev.setDeviceBounds(scratchReq, resp);
        LONGS_EQUAL(-ENOMEM, sRet);
        LONGS_EQUAL(-ENOMEM, resp.status);
        LONGS_EQUAL(AMBA_VIRT_ERR_QUOTA_EXCEEDED, resp.err_code);
        CHECK_TRUE(resp.max_avail_size <= remQuota);

        releaseFixtureExtents(dev, _ownedExtents);
    } else {
        requireUnregistered(AMBA_VIRT_DEV_TYPE_CAVALRY);

        // 1. Request exceeding 1 GiB BAR ceiling returns AMBA_VIRT_ERR_BAR_OVERFLOW
        struct amba_virt_dev_bounds_req barReq;
        memset(&barReq, 0, sizeof(barReq));
        barReq.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
        barReq.preferred_offset = 0;
        barReq.requested_size = 0x60000000U; // 1536 MiB > 1 GiB BAR
        barReq.flags = AMBA_VIRT_DEV_F_EXACT;

        struct amba_virt_dev_bounds_resp resp;
        int ret = dev.setDeviceBounds(barReq, resp);
        LONGS_EQUAL(-ERANGE, ret);
        LONGS_EQUAL(-ERANGE, resp.status);
        LONGS_EQUAL(AMBA_VIRT_ERR_BAR_OVERFLOW, resp.err_code);

        // 2. Consume quota with scratch extent then request exact beyond remaining quota
        struct amba_virt_mem_resp mresp;
        int allocRet = dev.allocMemory(768 * 1024 * 1024, 0x200000, 0, mresp);
        LONGS_EQUAL(0, allocRet);
        LONGS_EQUAL(0, mresp.status);
        _ownedExtents.push_back(mresp.bar_offset);

        struct amba_virt_dev_bounds_req cavReq;
        memset(&cavReq, 0, sizeof(cavReq));
        cavReq.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
        cavReq.requested_size = 512 * 1024 * 1024;
        cavReq.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
        cavReq.flags = AMBA_VIRT_DEV_F_EXACT;

        int cavRet = dev.setDeviceBounds(cavReq, resp);
        LONGS_EQUAL(-ENOMEM, cavRet);
        LONGS_EQUAL(-ENOMEM, resp.status);
        LONGS_EQUAL(AMBA_VIRT_ERR_QUOTA_EXCEEDED, resp.err_code);
        CHECK_TRUE(resp.max_avail_size <= 256 * 1024 * 1024);

        LONGS_EQUAL(0, dev.freeMemory(mresp.bar_offset));
        _ownedExtents.clear();
    }
}

TEST(InBandDeviceConfig, QuotaExceededBestEffortClamps) {
    requireUnregistered(AMBA_VIRT_DEV_TYPE_SCRATCH);

    if (_liveCoexistence) {
        fillUnalignedGapsBelowFreeBase();
        uint32_t reserveSize = (_availQuota / 2U) & ~0x1FFFFFU;
        CHECK_TRUE(reserveSize >= 0x200000U);

        struct amba_virt_mem_resp mresp;
        int allocRet = dev.allocMemory(reserveSize, 0x200000, 0, mresp);
        LONGS_EQUAL(0, allocRet);
        LONGS_EQUAL(0, mresp.status);
        _ownedExtents.push_back(mresp.bar_offset);
        uint32_t remQuota = mresp.total_free;

        struct amba_virt_dev_bounds_req scratchReq;
        memset(&scratchReq, 0, sizeof(scratchReq));
        scratchReq.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
        scratchReq.requested_size = (remQuota + 8 * 1024 * 1024) & ~0x1FFFFFU;
        scratchReq.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
        scratchReq.align = 0x200000U;
        scratchReq.flags = AMBA_VIRT_DEV_F_BEST_EFFORT;

        struct amba_virt_dev_bounds_resp resp;
        int sRet = dev.setDeviceBounds(scratchReq, resp);
        LONGS_EQUAL(0, sRet);
        LONGS_EQUAL(0, resp.status);
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
        CHECK_TRUE(resp.granted_size <= remQuota);
        CHECK_TRUE(resp.granted_size > 0);

        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_SCRATCH));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
        releaseFixtureExtents(dev, _ownedExtents);
    } else {
        struct amba_virt_mem_resp mresp;
        int allocRet = dev.allocMemory(768 * 1024 * 1024, 0x200000, 0, mresp);
        LONGS_EQUAL(0, allocRet);
        LONGS_EQUAL(0, mresp.status);
        _ownedExtents.push_back(mresp.bar_offset);

        struct amba_virt_dev_bounds_req scratchReq;
        memset(&scratchReq, 0, sizeof(scratchReq));
        scratchReq.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
        scratchReq.requested_size = 512 * 1024 * 1024;
        scratchReq.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
        scratchReq.flags = AMBA_VIRT_DEV_F_BEST_EFFORT;

        struct amba_virt_dev_bounds_resp resp;
        int sRet = dev.setDeviceBounds(scratchReq, resp);
        LONGS_EQUAL(0, sRet);
        LONGS_EQUAL(0, resp.status);
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
        CHECK_TRUE(resp.granted_size <= 256 * 1024 * 1024);
        CHECK_TRUE(resp.granted_size > 0);

        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_SCRATCH));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
        LONGS_EQUAL(0, dev.freeMemory(mresp.bar_offset));
        _ownedExtents.clear();
    }
}

TEST(InBandDeviceConfig, DuplicateWithoutReplaceFails) {
    if (_liveCoexistence) {
        requireUnregistered(AMBA_VIRT_DEV_TYPE_SCRATCH);

        struct amba_virt_dev_bounds_req req;
        memset(&req, 0, sizeof(req));
        req.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
        req.requested_size = 8 * 1024 * 1024;
        req.preferred_offset = _freeBase;
        req.align = 0x200000U;
        req.flags = AMBA_VIRT_DEV_F_EXACT;

        struct amba_virt_dev_bounds_resp resp;
        LONGS_EQUAL(0, dev.setDeviceBounds(req, resp));
        LONGS_EQUAL(0, resp.status);
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);

        // Register second time without F_REPLACE
        req.requested_size = 12 * 1024 * 1024;
        int ret = dev.setDeviceBounds(req, resp);
        LONGS_EQUAL(-EBUSY, ret);
        LONGS_EQUAL(-EBUSY, resp.status);
        LONGS_EQUAL(AMBA_VIRT_ERR_DEV_ALREADY_REG, resp.err_code);

        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_SCRATCH));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
    } else {
        requireUnregistered(AMBA_VIRT_DEV_TYPE_CAVALRY);

        struct amba_virt_dev_bounds_req req;
        memset(&req, 0, sizeof(req));
        req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
        req.requested_size = 32 * 1024 * 1024;
        req.preferred_offset = 0x08000000;
        req.flags = AMBA_VIRT_DEV_F_EXACT;

        struct amba_virt_dev_bounds_resp resp;
        LONGS_EQUAL(0, dev.setDeviceBounds(req, resp));
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_CAVALRY);

        // Register second time without F_REPLACE
        req.requested_size = 64 * 1024 * 1024;
        int ret = dev.setDeviceBounds(req, resp);
        LONGS_EQUAL(-EBUSY, ret);
        LONGS_EQUAL(-EBUSY, resp.status);
        LONGS_EQUAL(AMBA_VIRT_ERR_DEV_ALREADY_REG, resp.err_code);

        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_CAVALRY));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_CAVALRY);
    }
}

TEST(InBandDeviceConfig, DuplicateWithReplaceSucceeds) {
    if (_liveCoexistence) {
        requireUnregistered(AMBA_VIRT_DEV_TYPE_SCRATCH);

        struct amba_virt_dev_bounds_req req;
        memset(&req, 0, sizeof(req));
        req.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
        req.requested_size = 8 * 1024 * 1024;
        req.preferred_offset = _freeBase;
        req.align = 0x200000U;
        req.flags = AMBA_VIRT_DEV_F_EXACT;

        struct amba_virt_dev_bounds_resp resp;
        LONGS_EQUAL(0, dev.setDeviceBounds(req, resp));
        LONGS_EQUAL(0, resp.status);
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);

        // Re-register with F_REPLACE
        req.requested_size = 12 * 1024 * 1024;
        req.flags = AMBA_VIRT_DEV_F_REPLACE | AMBA_VIRT_DEV_F_EXACT;
        LONGS_EQUAL(0, dev.setDeviceBounds(req, resp));
        LONGS_EQUAL(0, resp.status);
        LONGS_EQUAL(12 * 1024 * 1024, resp.granted_size);

        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_SCRATCH));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_SCRATCH);
    } else {
        requireUnregistered(AMBA_VIRT_DEV_TYPE_CAVALRY);

        struct amba_virt_dev_bounds_req req;
        memset(&req, 0, sizeof(req));
        req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
        req.requested_size = 32 * 1024 * 1024;
        req.preferred_offset = 0x08000000;
        req.flags = AMBA_VIRT_DEV_F_EXACT;

        struct amba_virt_dev_bounds_resp resp;
        LONGS_EQUAL(0, dev.setDeviceBounds(req, resp));
        recordOwnedDev(AMBA_VIRT_DEV_TYPE_CAVALRY);

        // Re-register with F_REPLACE
        req.requested_size = 48 * 1024 * 1024;
        req.flags = AMBA_VIRT_DEV_F_REPLACE | AMBA_VIRT_DEV_F_EXACT;
        LONGS_EQUAL(0, dev.setDeviceBounds(req, resp));
        LONGS_EQUAL(0, resp.status);
        LONGS_EQUAL(48 * 1024 * 1024, resp.granted_size);

        LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_CAVALRY));
        forgetOwnedDev(AMBA_VIRT_DEV_TYPE_CAVALRY);
    }
}

TEST(InBandDeviceConfig, PermissionDeniedWithoutCap) {
    // Standard role lacks QUERY_PEERS cap; verifying ACL rejection over wire
    std::vector<struct amba_virt_peer_desc> peers;
    struct amba_virt_query_resp qresp;
    memset(&qresp, 0, sizeof(qresp));
    int ret = dev.queryPeers(peers, 0, &qresp);
    CHECK_TRUE(ret == -EPERM || qresp.status == -EPERM);
}

// =============================================================================
// TEST_GROUP(IntrospectionAPI): Guest & Topology Query Subsystem
// =============================================================================
TEST_GROUP(IntrospectionAPI) {
    AmbaVirtDevice dev;
    std::vector<uint32_t> _ownedDevs;

    void setup() override {
        LONGS_EQUAL(0, dev.openDevice());
        LONGS_EQUAL(0, dev.connect());
        dev.flushRx();
        _ownedDevs.clear();
    }

    void teardown() override {
        for (auto it = _ownedDevs.rbegin(); it != _ownedDevs.rend(); ++it) {
            dev.releaseDeviceBounds(*it);
        }
        _ownedDevs.clear();
        dev.flushRx();
        dev.closeDevice();
    }
};

TEST(IntrospectionAPI, QuerySelf) {
    struct amba_virt_peer_desc desc;
    memset(&desc, 0, sizeof(desc));
    LONGS_EQUAL(0, dev.querySelf(desc));
    CHECK_TRUE(desc.cid > 0);
    LONGS_EQUAL(1, desc.status); // ONLINE
    CHECK_TRUE((desc.caps & AMBA_VIRT_CAP_QUERY_SELF) != 0);
}

TEST(IntrospectionAPI, QueryDevMem) {
    std::vector<struct amba_virt_dev_mem_desc> devs;
    LONGS_EQUAL(0, dev.queryDevMem(devs, AMBA_VIRT_DEV_TYPE_SCRATCH));
    if (!devs.empty()) {
        FAIL("SCRATCH device already registered");
    }

    struct amba_virt_dev_bounds_req req;
    memset(&req, 0, sizeof(req));
    req.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
    req.requested_size = 4 * 1024 * 1024;
    req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
    req.flags = AMBA_VIRT_DEV_F_EXACT;

    struct amba_virt_dev_bounds_resp bresp;
    LONGS_EQUAL(0, dev.setDeviceBounds(req, bresp));
    LONGS_EQUAL(0, bresp.status);
    _ownedDevs.push_back(AMBA_VIRT_DEV_TYPE_SCRATCH);

    devs.clear();
    LONGS_EQUAL(0, dev.queryDevMem(devs, AMBA_VIRT_DEV_TYPE_SCRATCH));
    CHECK_TRUE(devs.size() >= 1);

    bool found = false;
    for (const auto &d : devs) {
        if (d.dev_type == AMBA_VIRT_DEV_TYPE_SCRATCH) {
            found = true;
            LONGS_EQUAL(bresp.granted_offset, d.base_offset);
            LONGS_EQUAL(4 * 1024 * 1024, d.size);
        }
    }
    CHECK_TRUE(found);

    LONGS_EQUAL(0, dev.releaseDeviceBounds(AMBA_VIRT_DEV_TYPE_SCRATCH));
    _ownedDevs.clear();
}

TEST(IntrospectionAPI, QueryTopology) {
    struct amba_virt_topo_desc topo;
    memset(&topo, 0, sizeof(topo));
    struct amba_virt_query_resp qresp;
    memset(&qresp, 0, sizeof(qresp));
    int ret = dev.queryTopology(topo, &qresp);
    if (ret == 0) {
        LONGS_EQUAL(0x655, topo.chip_id);
        LONGS_EQUAL(4, topo.npu_core_cnt);
        LONGS_EQUAL(0, topo.host_phys_addr); // HPA must be masked for security
    } else {
        LONGS_EQUAL(-EPERM, ret);
    }
}

TEST(IntrospectionAPI, QueryPeers) {
    std::vector<struct amba_virt_peer_desc> peers;
    struct amba_virt_query_resp qresp;
    memset(&qresp, 0, sizeof(qresp));
    int ret = dev.queryPeers(peers, 0, &qresp);
    if (ret == 0) {
        CHECK_TRUE(peers.size() >= 1);
    } else {
        LONGS_EQUAL(-EPERM, ret);
    }
}

// =============================================================================
// TEST_GROUP(DynamicMemory): Extent Slicing & Memory Management
// =============================================================================
TEST_GROUP(DynamicMemory) {
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

TEST(DynamicMemory, AllocAndFreeExtent) {
    struct amba_virt_mem_resp resp;
    LONGS_EQUAL(0, dev.allocMemory(4 * 1024 * 1024, 0x200000, 0, resp));
    LONGS_EQUAL(0, resp.status);
    LONGS_EQUAL(4 * 1024 * 1024, resp.allocated_size);

    LONGS_EQUAL(0, dev.freeMemory(resp.bar_offset));
}

TEST(DynamicMemory, OverBarAllocFails) {
    struct amba_virt_mem_resp resp;
    int ret = dev.allocMemory(0x60000000U, 0x200000, 0, resp); // 1536 MiB > 1 GiB BAR
    CHECK_TRUE(ret == -ERANGE || resp.status == -ERANGE);
}

// =============================================================================
// TEST_GROUP(AccessControlList): Capability Enforcement
// =============================================================================
TEST_GROUP(AccessControlList) {
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

TEST(AccessControlList, StandardCapabilitiesVerify) {
    struct amba_virt_peer_desc desc;
    memset(&desc, 0, sizeof(desc));
    LONGS_EQUAL(0, dev.querySelf(desc));
    CHECK_TRUE((desc.caps & AMBA_VIRT_CAP_PING) != 0);
    CHECK_TRUE((desc.caps & AMBA_VIRT_CAP_QUERY_SELF) != 0);
    CHECK_TRUE((desc.caps & AMBA_VIRT_CAP_DEV_CONFIG) != 0);
    CHECK_TRUE((desc.caps & AMBA_VIRT_CAP_MEM_ALLOC) != 0);
}

// =============================================================================
// TEST_GROUP(GDMA): GDMA Hardware Linear & 2D Pitch Copy Verification
// =============================================================================
TEST_GROUP(GDMA) {
    AmbaVirtDevice dev;
    std::vector<uint32_t> _ownedExtents;
    void *mapPtr{nullptr};
    uint32_t extentOffset{0};
    uint32_t extentSize{0};

    uint32_t ensureExtent(uint32_t size) {
        return ensureFixtureExtent(dev, _ownedExtents, mapPtr, extentOffset, extentSize, size);
    }

    void setup() override {
        LONGS_EQUAL(0, dev.openDevice());
        LONGS_EQUAL(0, dev.connect());
        dev.flushRx();
        _ownedExtents.clear();
        mapPtr = nullptr;
        extentOffset = 0;
        extentSize = 0;
        ensureExtent(2 * 1024 * 1024);
    }

    void teardown() override {
        dev.unmapShm();
        mapPtr = nullptr;
        releaseFixtureExtents(dev, _ownedExtents);
        dev.flushRx();
        dev.closeDevice();
    }
};

TEST(GDMA, LinearCopyPattern) {
    const uint32_t copyLen = 65536;
    const uint32_t guardLen = 4096;
    const uint32_t dstDelta = 0x100000U;
    CHECK_TRUE(extentSize >= dstDelta + copyLen + guardLen);

    uint8_t *src = static_cast<uint8_t *>(mapPtr) + extentOffset;
    uint8_t *dst = src + dstDelta;

    uint32_t *srcWords = reinterpret_cast<uint32_t *>(src);
    uint32_t state = 0x13579BDFU;
    for (size_t i = 0; i < copyLen / sizeof(uint32_t); ++i) {
        state = amba_virt_prbs31_next(state);
        srcWords[i] = state;
    }

    std::memset(dst, 0xA5, copyLen + guardLen);
    storeBarrier();

    struct amba_virt_gdma_copy resp;
    int ret = dev.gdmaCopy(extentOffset, extentOffset + dstDelta, copyLen, &resp);
    LONGS_EQUAL(0, ret);
    LONGS_EQUAL(0, resp.status);
    loadBarrier();

    LONGS_EQUAL(0, std::memcmp(src, dst, copyLen));
    for (uint32_t i = 0; i < guardLen; ++i) {
        LONGS_EQUAL(0xA5, dst[copyLen + i]);
    }
}

TEST(GDMA, Pitch2DCopyPattern) {
    const uint16_t width = 64;
    const uint16_t height = 64;
    const uint16_t srcPitch = 512;
    const uint16_t dstPitch = 256;
    const uint32_t dstDelta = 0x100000U;
    CHECK_TRUE(extentSize >= dstDelta + static_cast<uint32_t>(height) * dstPitch);

    uint8_t *src = static_cast<uint8_t *>(mapPtr) + extentOffset;
    uint8_t *dst = src + dstDelta;

    for (uint32_t r = 0; r < height; ++r) {
        for (uint32_t c = 0; c < srcPitch; ++c) {
            uint8_t v = static_cast<uint8_t>((r * 37U + c * 13U + 0x5AU) & 0xFFU);
            if (v == 0x3CU) {
                v = 0x3DU;
            }
            src[r * srcPitch + c] = v;
        }
    }
    std::memset(dst, 0x3C, static_cast<size_t>(height) * dstPitch);
    storeBarrier();

    struct amba_virt_gdma_copy resp;
    int ret = dev.gdmaPitch(extentOffset, extentOffset + dstDelta,
                            width, height, srcPitch, dstPitch, &resp);
    LONGS_EQUAL(0, ret);
    LONGS_EQUAL(0, resp.status);
    loadBarrier();

    for (uint32_t r = 0; r < height; ++r) {
        LONGS_EQUAL(0, std::memcmp(src + r * srcPitch, dst + r * dstPitch, width));
        for (uint32_t c = width; c < dstPitch; ++c) {
            LONGS_EQUAL(0x3C, dst[r * dstPitch + c]);
        }
    }
}

TEST(GDMA, RejectsInvalid) {
    const uint32_t dstOff = extentOffset + 0x100000U;
    const uint32_t usableSz = dev.getShmSize();
    CHECK_TRUE(usableSz >= 256);

    LONGS_EQUAL(-EINVAL, dev.gdmaCopy(extentOffset, dstOff, 0));
    LONGS_EQUAL(-EINVAL, dev.gdmaCopy(extentOffset, dstOff, 65));
    LONGS_EQUAL(-EINVAL, dev.gdmaCopy(extentOffset, extentOffset + 64, 256));
    LONGS_EQUAL(-ERANGE, dev.gdmaCopy(usableSz - 64, dstOff, 128));

    LONGS_EQUAL(-EINVAL, dev.gdmaPitch(extentOffset, dstOff, 0, 16, 64, 64));
    LONGS_EQUAL(-EINVAL, dev.gdmaPitch(extentOffset, dstOff, 128, 16, 64, 128));
    LONGS_EQUAL(-EINVAL, dev.gdmaPitch(extentOffset, dstOff, 4097, 2, 8192, 8192));
    LONGS_EQUAL(-ERANGE, dev.gdmaPitch(usableSz - 128, dstOff, 64, 4, 64, 64));
}

// =============================================================================
// TEST_GROUP(CavalryPathB): Cavalry Path B RPC Verification
// =============================================================================
TEST_GROUP(CavalryPathB) {
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

TEST(CavalryPathB, GetVersion) {
    struct amba_virt_cavalry_rpc req;
    struct amba_virt_cavalry_rpc resp;
    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_GET_VERSION;

    LONGS_EQUAL(0, dev.cavalryRpc(req, resp));
    LONGS_EQUAL(0, resp.status);
}

TEST(CavalryPathB, GetChipId) {
    struct amba_virt_cavalry_rpc req;
    struct amba_virt_cavalry_rpc resp;
    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_GET_CHIP_ID;

    LONGS_EQUAL(0, dev.cavalryRpc(req, resp));
    LONGS_EQUAL(0, resp.status);
    CHECK_TRUE(resp.chip_id != 0);
    if (g_verboseMode) {
        std::cout << "  [CavalryPathB] chip_id=0x" << std::hex << resp.chip_id << std::dec << "\n";
    }
}

TEST(CavalryPathB, GetStatus) {
    struct amba_virt_cavalry_rpc req;
    struct amba_virt_cavalry_rpc resp;
    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_GET_STATUS;

    LONGS_EQUAL(0, dev.cavalryRpc(req, resp));
    LONGS_EQUAL(0, resp.status);
    CHECK_TRUE(resp.rval == 0 || resp.rval == 1);
    if (g_verboseMode) {
        std::cout << "  [CavalryPathB] cavalry_started=" << resp.rval << "\n";
    }
}

TEST(CavalryPathB, RunRegisteredDagRejects) {
    struct amba_virt_cavalry_rpc req;
    struct amba_virt_cavalry_rpc resp;
    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_RUN_REGISTERED_DAG;
    req.arena_len = 0;

    LONGS_EQUAL(-EMSGSIZE, dev.cavalryRpc(req, resp));

    memset(&req, 0, sizeof(req));
    req.opcode = VCAV_OP_RUN_REGISTERED_DAG;
    req.arena_len = sizeof(struct amba_virt_cavalry_run_reg_desc);
    req.bar_offset = 0x7FFFF000U;

    LONGS_EQUAL(-EINVAL, dev.cavalryRpc(req, resp));
}

// =============================================================================
// TEST_GROUP(DriverCaps): Virtual Driver Capabilities & Host Module Readiness
// =============================================================================
TEST_GROUP(DriverCaps) {
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

TEST(DriverCaps, QueryDriverCaps) {
    struct amba_virt_driver_caps_resp caps;
    memset(&caps, 0, sizeof(caps));
    LONGS_EQUAL(0, dev.queryDriverCaps(caps));
    CHECK_TRUE(caps.count <= 8);

    for (uint32_t i = 0; i < caps.count; ++i) {
        CHECK_TRUE(std::memchr(caps.entries[i].dev_name, '\0',
                               sizeof(caps.entries[i].dev_name)) != nullptr);
        CHECK_TRUE(caps.entries[i].state == AMBA_VIRT_DEV_STATE_ONLINE ||
                   caps.entries[i].state == AMBA_VIRT_DEV_STATE_OFFLINE);
    }
}

TEST(DriverCaps, StateMatchesMask) {
    struct amba_virt_driver_caps_resp caps;
    memset(&caps, 0, sizeof(caps));
    LONGS_EQUAL(0, dev.queryDriverCaps(caps));
    CHECK_TRUE(caps.count <= 8);

    for (uint32_t i = 0; i < caps.count; ++i) {
        if (caps.entries[i].state == AMBA_VIRT_DEV_STATE_ONLINE) {
            LONGS_EQUAL(caps.entries[i].module_mask,
                        caps.host_mod_mask & caps.entries[i].module_mask);
        }
    }
}

// =============================================================================
// TEST_GROUP(DynamicSlice): Dynamic Shared-Memory Slice Claim Rejection
// =============================================================================
TEST_GROUP(DynamicSlice) {
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

TEST(DynamicSlice, ClaimRejectsBadNonce) {
    struct amba_virt_slice_claim_resp resp;
    LONGS_EQUAL(-ENOENT, dev.claimSlice(0ULL, resp));
    LONGS_EQUAL(-ENOENT, dev.claimSlice(0xFFFFFFFFFFFFFFFFULL, resp));
}

TEST(DynamicSlice, ExistingBindingUntouched) {
    struct amba_virt_peer_desc beforeSelf;
    memset(&beforeSelf, 0, sizeof(beforeSelf));
    LONGS_EQUAL(0, dev.querySelf(beforeSelf));
    uint32_t beforeShm = dev.getInfo().shm_size;

    struct amba_virt_slice_claim_resp resp;
    LONGS_EQUAL(-ENOENT, dev.claimSlice(0ULL, resp));
    LONGS_EQUAL(-ENOENT, dev.claimSlice(0xFFFFFFFFFFFFFFFFULL, resp));

    LONGS_EQUAL(0, dev.refreshInfo());
    struct amba_virt_peer_desc afterSelf;
    memset(&afterSelf, 0, sizeof(afterSelf));
    LONGS_EQUAL(0, dev.querySelf(afterSelf));

    LONGS_EQUAL(beforeSelf.tenant_idx, afterSelf.tenant_idx);
    LONGS_EQUAL(beforeShm, dev.getInfo().shm_size);
}

// =============================================================================
// TEST_GROUP(DynamicMemoryStress): Extent Fragmentation & Quota Exhaustion
// =============================================================================
TEST_GROUP(DynamicMemoryStress) {
    AmbaVirtDevice dev;
    std::vector<uint32_t> _ownedExtents;

    void setup() override {
        LONGS_EQUAL(0, dev.openDevice());
        LONGS_EQUAL(0, dev.connect());
        dev.flushRx();
        _ownedExtents.clear();
    }

    void teardown() override {
        releaseFixtureExtents(dev, _ownedExtents);
        dev.flushRx();
        dev.closeDevice();
    }
};

TEST(DynamicMemoryStress, FragmentationAndChurn) {
    struct amba_virt_peer_desc beforeSelf;
    memset(&beforeSelf, 0, sizeof(beforeSelf));
    LONGS_EQUAL(0, dev.querySelf(beforeSelf));

    std::mt19937 rng(0xC0FFEEU);
    const uint32_t reqSizes[3] = {
        1 * 1024 * 1024,
        4 * 1024 * 1024,
        16 * 1024 * 1024,
    };

    for (int round = 0; round < 200; ++round) {
        for (uint32_t sz : reqSizes) {
            struct amba_virt_mem_resp resp;
            int ret = dev.allocMemory(sz, 0x200000, 0, resp);
            if (ret == 0) {
                _ownedExtents.push_back(resp.bar_offset);
            } else {
                LONGS_EQUAL(-ENOMEM, ret);
            }
        }
        std::shuffle(_ownedExtents.begin(), _ownedExtents.end(), rng);
        for (uint32_t off : _ownedExtents) {
            LONGS_EQUAL(0, dev.freeMemory(off));
        }
        _ownedExtents.clear();
    }

    struct amba_virt_peer_desc afterSelf;
    memset(&afterSelf, 0, sizeof(afterSelf));
    LONGS_EQUAL(0, dev.querySelf(afterSelf));
    LONGS_EQUAL(beforeSelf.mem_allocated_mb, afterSelf.mem_allocated_mb);
}

TEST(DynamicMemoryStress, HighWaterQuotaExhaustion) {
    struct amba_virt_peer_desc beforeSelf;
    memset(&beforeSelf, 0, sizeof(beforeSelf));
    LONGS_EQUAL(0, dev.querySelf(beforeSelf));

    uint64_t shmSize = dev.getShmSize();
    CHECK_TRUE(shmSize > 0);

    std::vector<struct amba_virt_dev_mem_desc> initDevs;
    LONGS_EQUAL(0, dev.queryDevMem(initDevs));

    uint64_t maxEnd = 0;
    uint64_t usedBytes = 0;
    for (const auto &d : initDevs) {
        uint64_t devEnd = static_cast<uint64_t>(d.base_offset) + d.size;
        if (devEnd > maxEnd) {
            maxEnd = devEnd;
        }
        if (d.rpc_arena_size > 0) {
            uint64_t arenaEnd = static_cast<uint64_t>(d.rpc_arena_offset) + d.rpc_arena_size;
            if (arenaEnd > maxEnd) {
                maxEnd = arenaEnd;
            }
        }
        usedBytes += static_cast<uint64_t>(d.size) + d.rpc_arena_size;
    }

    const uint64_t align2M = 2ULL * 1024ULL * 1024ULL;
    uint64_t freeBase = ((maxEnd + align2M - 1ULL) / align2M) * align2M;
    uint64_t availQuota = (shmSize > usedBytes) ? (shmSize - usedBytes) : 0ULL;
    uint64_t tailBytes = (shmSize > freeBase) ? (shmSize - freeBase) : 0ULL;
    uint64_t unallocatableGap = (availQuota > tailBytes) ? (availQuota - tailBytes) : 0ULL;

    uint64_t ceilDiv16 = (tailBytes + 15ULL) / 16ULL;
    uint64_t chunk64 = ((ceilDiv16 + align2M - 1ULL) / align2M) * align2M;
    if (chunk64 == 0) {
        chunk64 = align2M;
    }
    const uint32_t chunkSize = static_cast<uint32_t>(chunk64);

    struct amba_virt_mem_resp lastResp;
    memset(&lastResp, 0, sizeof(lastResp));
    int lastRet = 0;
    size_t chunkCount = 0;

    for (;;) {
        struct amba_virt_mem_resp resp;
        int ret = dev.allocMemory(chunkSize, 0x200000, 0, resp);
        lastResp = resp;
        lastRet = ret;
        if (ret == 0) {
            _ownedExtents.push_back(resp.bar_offset);
            ++chunkCount;
        } else {
            break;
        }
    }

    LONGS_EQUAL(-ENOMEM, lastRet);
    CHECK_TRUE(chunkCount > 0);
    CHECK_TRUE(lastResp.total_free >= unallocatableGap);
    CHECK_TRUE((lastResp.total_free - unallocatableGap) < chunkSize);
    if (g_verboseMode) {
        std::cout << "    [Stress] HighWaterQuotaExhaustion ("
                  << (g_liveDemoMode ? "live-coexistence" : "clean-pool")
                  << "): chunk_size=" << chunkSize
                  << " allocated " << chunkCount << " chunks"
                  << " (unallocatable_gap=" << unallocatableGap
                  << "), terminal total_free=" << lastResp.total_free
                  << " bytes" << std::endl;
    }

    for (uint32_t off : _ownedExtents) {
        LONGS_EQUAL(0, dev.freeMemory(off));
    }
    _ownedExtents.clear();

    struct amba_virt_peer_desc afterSelf;
    memset(&afterSelf, 0, sizeof(afterSelf));
    LONGS_EQUAL(0, dev.querySelf(afterSelf));
    LONGS_EQUAL(beforeSelf.mem_allocated_mb, afterSelf.mem_allocated_mb);
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
        uint32_t grantedExtentSize = 0;
        uint32_t extentOff = 0;
        for (uint32_t trySz = 256U * 1024U * 1024U; trySz >= 4096U; trySz >>= 1) {
            uint32_t align = (trySz >= 0x200000U) ? 0x200000U : 4096U;
            struct amba_virt_mem_resp mresp;
            if (dev.allocMemory(trySz, align, 0, mresp) == 0 && mresp.allocated_size >= trySz) {
                grantedExtentSize = mresp.allocated_size;
                extentOff = mresp.bar_offset;
                break;
            }
        }

        if (grantedExtentSize > 0) {
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

                uint32_t effectiveShmSz = (dev.getMappedSize() > 0)
                    ? static_cast<uint32_t>(dev.getMappedSize())
                    : shmSz;
                bool firstEntry = true;
                for (size_t s = 0; s < shmTransferSizes.size(); ++s) {
                    uint32_t bsz = shmTransferSizes[s];
                    if (bsz > grantedExtentSize || extentOff + bsz > effectiveShmSz)
                        continue;

                    int benchIters = std::max(50, iterations / 10);
                    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(
                        static_cast<uint8_t *>(shmPtr) + extentOff);
                    uint32_t words = bsz / sizeof(uint32_t);

                    uint64_t tStart = getMonotonicTimeNs();
                    for (int it = 0; it < benchIters; ++it) {
                        // Client write phase inside owned extent
                        p[0] = it;
                        p[words - 1] = it + 1;
                        storeBarrier();

                        // Vsock signal phase
                        struct amba_virt_msg msg;
                        msg.type = AMBA_VIRT_MSG_SHM_NOTIFY;
                        msg.seq = it;
                        msg.shm_off = extentOff;
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

                    if (!firstEntry)
                        jsonStream << ",\n";
                    firstEntry = false;
                    jsonStream << "      {\n"
                               << "        \"buffer_bytes\": " << bsz << ",\n"
                               << "        \"handoffs_sec\": " << handoffsSec << ",\n"
                               << "        \"effective_bandwidth_gbps\": " << gbps << "\n"
                               << "      }";
                }
                if (!firstEntry)
                    jsonStream << "\n";
                jsonStream << "    ]\n";
                dev.unmapShm();
            }
            dev.freeMemory(extentOff);
        } else {
            std::cerr << "Warning: Could not allocate owned extent for shm_handoff benchmark.\n";
        }
    }

    // 3. GDMA Hardware Copy & 2D Pitch Throughput
    std::cout << "\n--- 3. GDMA Hardware Copy & 2D Pitch Throughput ---\n";
    struct amba_virt_mem_resp gdmaMemResp;
    if (dev.allocMemory(2 * 1024 * 1024, 0x200000, 0, gdmaMemResp) == 0 &&
        gdmaMemResp.allocated_size >= 2 * 1024 * 1024) {
        uint32_t gdmaSrcOff = gdmaMemResp.bar_offset;
        uint32_t gdmaDstOff = gdmaSrcOff + 0x100000U;
        int benchIters = std::max(50, iterations / 10);

        const std::vector<uint32_t> linearSizes = {4096, 65536, 1048576};
        jsonStream << ",\n    \"gdma_linear\": [\n";
        bool firstLinear = true;
        for (uint32_t bsz : linearSizes) {
            std::vector<double> latenciesUs;
            latenciesUs.reserve(benchIters);
            int copyErr = 0;

            uint64_t tStart = getMonotonicTimeNs();
            for (int it = 0; it < benchIters; ++it) {
                uint64_t t0 = getMonotonicTimeNs();
                int ret = dev.gdmaCopy(gdmaSrcOff, gdmaDstOff, bsz);
                uint64_t t1 = getMonotonicTimeNs();
                if (ret < 0) {
                    copyErr = ret;
                    break;
                }
                latenciesUs.push_back(static_cast<double>(t1 - t0) / 1000.0);
            }
            uint64_t tEnd = getMonotonicTimeNs();
            if (copyErr < 0) {
                std::cerr << "GDMA linear copy (" << bsz << " B) failed: " << copyErr << "\n";
                continue;
            }
            double totalSec = static_cast<double>(tEnd - tStart) / 1e9;
            LatencyStats st = calculateLatencyStats(latenciesUs, totalSec);
            double mbps = (totalSec > 0.0)
                ? ((static_cast<double>(bsz) * latenciesUs.size()) / (1024.0 * 1024.0) / totalSec)
                : 0.0;

            std::cout << "Linear " << std::setw(7) << bsz << " B | "
                      << "Min: " << std::fixed << std::setprecision(1) << st._minUs << " us | "
                      << "Median: " << st._medianUs << " us | "
                      << "p99: " << st._p99Us << " us | "
                      << "Throughput: " << std::setprecision(2) << mbps << " MB/s\n";

            if (!firstLinear)
                jsonStream << ",\n";
            firstLinear = false;
            jsonStream << "      {\n"
                       << "        \"bytes\": " << bsz << ",\n"
                       << "        \"min_us\": " << st._minUs << ",\n"
                       << "        \"median_us\": " << st._medianUs << ",\n"
                       << "        \"p99_us\": " << st._p99Us << ",\n"
                       << "        \"throughput_mbps\": " << mbps << "\n"
                       << "      }";
        }
        if (!firstLinear)
            jsonStream << "\n";
        jsonStream << "    ],\n";

        struct PitchCase {
            const char *label;
            uint16_t width;
            uint16_t height;
            uint16_t srcPitch;
            uint16_t dstPitch;
        };
        const std::vector<PitchCase> pitchCases = {
            {"64x64", 64, 64, 64, 64},
            {"1024x256", 1024, 256, 2048, 1024},
        };

        jsonStream << "    \"gdma_pitch\": [\n";
        bool firstPitch = true;
        for (const auto &pc : pitchCases) {
            std::vector<double> latenciesUs;
            latenciesUs.reserve(benchIters);
            int pitchErr = 0;

            uint64_t tStart = getMonotonicTimeNs();
            for (int it = 0; it < benchIters; ++it) {
                uint64_t t0 = getMonotonicTimeNs();
                int ret = dev.gdmaPitch(gdmaSrcOff, gdmaDstOff,
                                        pc.width, pc.height, pc.srcPitch, pc.dstPitch);
                uint64_t t1 = getMonotonicTimeNs();
                if (ret < 0) {
                    pitchErr = ret;
                    break;
                }
                latenciesUs.push_back(static_cast<double>(t1 - t0) / 1000.0);
            }
            uint64_t tEnd = getMonotonicTimeNs();
            if (pitchErr < 0) {
                std::cerr << "GDMA pitch copy (" << pc.label << ") failed: " << pitchErr << "\n";
                continue;
            }
            double totalSec = static_cast<double>(tEnd - tStart) / 1e9;
            LatencyStats st = calculateLatencyStats(latenciesUs, totalSec);
            uint32_t xferBytes = static_cast<uint32_t>(pc.width) * pc.height;
            double mbps = (totalSec > 0.0)
                ? ((static_cast<double>(xferBytes) * latenciesUs.size()) / (1024.0 * 1024.0) / totalSec)
                : 0.0;

            std::cout << "Pitch  " << std::setw(8) << pc.label << "  | "
                      << "Min: " << std::fixed << std::setprecision(1) << st._minUs << " us | "
                      << "Median: " << st._medianUs << " us | "
                      << "p99: " << st._p99Us << " us | "
                      << "Throughput: " << std::setprecision(2) << mbps << " MB/s\n";

            if (!firstPitch)
                jsonStream << ",\n";
            firstPitch = false;
            jsonStream << "      {\n"
                       << "        \"case\": \"" << pc.label << "\",\n"
                       << "        \"width\": " << pc.width << ",\n"
                       << "        \"height\": " << pc.height << ",\n"
                       << "        \"src_pitch\": " << pc.srcPitch << ",\n"
                       << "        \"dst_pitch\": " << pc.dstPitch << ",\n"
                       << "        \"min_us\": " << st._minUs << ",\n"
                       << "        \"median_us\": " << st._medianUs << ",\n"
                       << "        \"p99_us\": " << st._p99Us << ",\n"
                       << "        \"throughput_mbps\": " << mbps << "\n"
                       << "      }";
        }
        if (!firstPitch)
            jsonStream << "\n";
        jsonStream << "    ]\n";

        dev.freeMemory(gdmaSrcOff);
    } else {
        std::cerr << "Warning: Could not allocate 2 MiB extent for GDMA benchmark.\n";
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
        bool listOnly = false;
        g_verboseMode = false;
        for (size_t i = 1; i < filteredArgv.size(); ++i) {
            std::string a = filteredArgv[i];
            if (a == "-lg" || a == "-ln") {
                listOnly = true;
            } else if (a == "-v") {
                g_verboseMode = true;
            }
        }
        if (!listOnly) {
            detectRuntimeEnvironment();
        }
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
