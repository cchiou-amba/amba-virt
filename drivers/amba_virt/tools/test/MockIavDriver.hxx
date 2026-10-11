/*
 * MockIavDriver.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBAVIRT_TEST_MOCKIAVDRIVER_HXX
#define AMBAVIRT_TEST_MOCKIAVDRIVER_HXX

#include <stdint.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "iav_broker.h"
}

/*
 * A stand-in for the real IAV driver that doubles as the oracle of the broker tests. It records a
 * violation when anything reaches it that the broker must never forward: an ioctl that is not on
 * the allow-list, a call on a descriptor that is not open, a wait with no bound, a dma-buf fd
 * flag, or a poll event outside the allowed set. A test that sees a violation has found a hole
 * in the broker's checks. Time is a fake clock that only sleep_ms and advance() move, so the
 * bounded waits are deterministic.
 */
class MockIavDriver
{
public:
    MockIavDriver();

    /* The ops table to give the broker. */
    struct iav_broker_ops ops();

    /* What the next calls do. */
    void failOpensWith(int err);                 /* 0: opens succeed */
    void setEagainCount(int n);                  /* non-blocking descriptor queries fail n times */
    void failIoctl(unsigned long cmd, int err);  /* the ioctl returns -1 with err; err 0 clears */
    void setPollReady(bool ready);
    void advance(uint64_t ms);
    void setSeed(uint32_t seed);
    /* Run something inside the next open, before it returns, to race it against a disconnect. */
    void setOpenHook(std::function<void()> hook);

    /* Make the next WAIT_NEXT_IDSP_FRAME block until release() is called. */
    void blockNextWait();
    void waitUntilBlocked();
    void release();

    /* Observations. */
    int openFds();
    unsigned long totalOpens();
    unsigned long totalCloses();
    unsigned long ioctlCalls();
    unsigned long reached(unsigned long cmd);
    uint64_t now();
    std::vector<std::string> violations();
    void clearViolations();

    /* The window layout the broker is told about, matching QUERY_MEMBLOCK here. */
    static const uint64_t DspBase = 0x40000000ull;
    static const uint64_t DspLen = 0x400000ull;
    static const uint64_t BsbBase = 0x84300000ull;
    static const uint64_t BsbLen = 0xa00000ull;
    static const uint64_t BsbWindow = 0x1000000ull;
    static const unsigned int BlockMaxMs = 2000;

private:
    static int cOpen(void *ctx);
    static int cClose(void *ctx, int fd);
    static int cIoctl(void *ctx, int fd, unsigned long cmd, unsigned long arg);
    static int cPoll(void *ctx, int fd, int events, int timeoutMs, int *revents);
    static void cSleep(void *ctx, unsigned int ms);
    static uint32_t cRandom(void *ctx);
    static uint64_t cNow(void *ctx);

    int doOpen();
    int doClose(int fd);
    int doIoctl(int fd, unsigned long cmd, unsigned long arg);
    int doPoll(int fd, int events, int timeoutMs, int *revents);
    void violation(const std::string &what);
    bool fdOpen(int fd);

    std::mutex _lock;
    std::vector<int> _fds;
    std::vector<unsigned int> _pollConfigured;
    int _nextFd;
    int _openErr;
    int _eagain;
    unsigned long _failCmd;
    int _failErr;
    bool _pollReady;
    uint64_t _now;
    unsigned long _opens, _closes, _ioctls;
    static const int ReachedSlots = 16;
    unsigned long _reachedCount[ReachedSlots];
    static int slotOf(unsigned long cmd);
    std::vector<std::string> _violations;
    unsigned int _waitCounter;
    uint32_t _rnd;
    std::function<void()> _openHook;
    std::mutex _blockMutex;
    std::condition_variable _blockCv;
    bool _blockNext;
    bool _blocked;
    bool _released;
};

#endif /* AMBAVIRT_TEST_MOCKIAVDRIVER_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
