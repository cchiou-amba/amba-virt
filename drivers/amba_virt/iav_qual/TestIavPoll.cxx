/*
 * TestIavPoll.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavTestSupport.hxx"

#include <poll.h>

#include <CppUTest/TestHarness.h>

using namespace iavtest;

TEST_GROUP(IavPoll)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }

    int pollOnce(int timeoutMs)
    {
        struct pollfd p;

        p.fd = _f.fd();
        p.events = POLLIN;
        p.revents = 0;
        return backend().poll(&p, 1, timeoutMs) == 1 ? (int)p.revents : -1;
    }
};

TEST(IavPoll, SetPollConfig)
{
    struct iav_poll_event_config cfg;

    canvasPollConfig(&cfg);
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
}

TEST(IavPoll, ClearPollConfig)
{
    struct iav_poll_event_config cfg;
    struct pollfd p;

    canvasPollConfig(&cfg);
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
    LONGS_EQUAL(0, backend().ioctl(_f.fd(), IAV_IOC_CLEAR_POLL_EVENT_CFG, 0));
    p.fd = _f.fd();
    p.events = POLLIN;
    p.revents = 0;
    LONGS_EQUAL(0, backend().poll(&p, 1, 0));
}

TEST(IavPoll, PollZeroTimeoutNonBlocking)
{
    struct iav_poll_event_config cfg;
    struct pollfd p;
    long long start, elapsedMs;

    canvasPollConfig(&cfg);
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
    p.fd = _f.fd();
    p.events = POLLIN;
    p.revents = 0;
    start = nowNs();
    CHECK_TRUE(backend().poll(&p, 1, 0) >= 0);
    elapsedMs = (nowNs() - start) / 1000000;
    CHECK_TEXT(elapsedMs < 50, "a zero-timeout poll took longer than 50 ms");
}

TEST(IavPoll, PollWakesOnFrame)
{
    struct iav_poll_event_config cfg;
    struct iav_querydesc q;
    long long start, waitMs;
    int revents;

    canvasPollConfig(&cfg);
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
    start = nowNs();
    revents = pollOnce(2000);
    waitMs = (nowNs() - start) / 1000000;
    CHECK_TEXT(revents >= 0 && (revents & POLLIN), "poll did not report a frame within 2 s");
    LONGS_EQUAL(0, queryCanvas(_f.fd(), false, &q));
    note("woke", 1);
    note("wait_under_2s", waitMs < 2000);
}

TEST(IavPoll, PollMultipleEvents)
{
    struct iav_poll_event_config cfg;
    int revents;

    memset(&cfg, 0, sizeof(cfg));
    cfg.valid_num = 2;
    cfg.desc_event[0].qid = IAV_DESC_CANVAS;
    cfg.desc_event[0].arg.canvas.canvas_id = 0;
    cfg.desc_event[1].qid = IAV_DESC_PYRAMID;
    cfg.desc_event[1].arg.pyramid.chan_id = 0;
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
    revents = pollOnce(2000);
    CHECK_TEXT(revents >= 0 && (revents & POLLIN), "poll did not report an event within 2 s");
    note("events", 2);
}

TEST(IavPoll, PollSustainedWakeups)
{
    struct iav_poll_event_config cfg;
    struct iav_querydesc q;

    canvasPollConfig(&cfg);
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
    for (int i = 0; i < 20; i++) {
        int revents = pollOnce(2000);

        CHECK_TEXT(revents >= 0 && (revents & POLLIN), "poll stopped reporting frames");
        LONGS_EQUAL(0, queryCanvas(_f.fd(), false, &q));
    }
    note("wakeups", 20);
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
