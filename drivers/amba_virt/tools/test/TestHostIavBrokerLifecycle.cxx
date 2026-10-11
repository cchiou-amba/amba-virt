/*
 * TestHostIavBrokerLifecycle.cxx
 *
 * Rate limits, quotas under concurrency, and teardown while a call is in flight: the cases where
 * a guest that closes, crashes or floods must not leak, hang or disturb anyone else. Worker
 * threads only count their results; the test thread checks them, because CppUTest assertions are
 * not safe to call from other threads.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

namespace {

/*
 * Turns a hang into a failed assertion: if the test is still running after ms, it unblocks the
 * driver call the test is waiting on and records that it had to. It does not allocate while running.
 */
class Watchdog
{
public:
    Watchdog(MockIavDriver &drv, unsigned int ms) : _drv(drv), _ms(ms), _stop(false), _fired(false)
    {
        _thread = std::thread(&Watchdog::run, this);
    }

    ~Watchdog() { stop(); }

    void stop()
    {
        _stop = true;
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    bool fired() const { return _fired; }

private:
    void run()
    {
        for (unsigned int waited = 0; waited < _ms && !_stop; waited += 10) {
            usleep(10000);
        }
        if (!_stop) {
            _fired = true;
            _drv.release();
        }
    }

    MockIavDriver &_drv;
    unsigned int _ms;
    std::atomic<bool> _stop;
    std::atomic<bool> _fired;
    std::thread _thread;
};

struct Shared
{
    IavBrokerHarness *h;
    uint32_t cid;
    uint64_t handle;
    int iterations;
    int okCount;
    int staleCount;
    int otherCount;
    int blockCall;
};

void tally(Shared *s, const IavBrokerHarness::Raw &r)
{
    if (r.ret != 0) {
        __atomic_add_fetch(&s->otherCount, 1, __ATOMIC_RELAXED);
    } else if (r.status == 0) {
        __atomic_add_fetch(&s->okCount, 1, __ATOMIC_RELAXED);
    } else if (r.status == -ESTALE) {
        __atomic_add_fetch(&s->staleCount, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&s->otherCount, 1, __ATOMIC_RELAXED);
    }
}

void *stateLoop(void *arg)
{
    Shared *s = static_cast<Shared *>(arg);

    for (int i = 0; i < s->iterations; i++) {
        tally(s, s->h->call(s->cid, AMBA_VIRT_IAV_OP_IOCTL, s->handle, (uint32_t)IAV_IOC_GET_IAV_STATE, 0, NULL, 0));
    }
    return NULL;
}

void *waitOnce(void *arg)
{
    Shared *s = static_cast<Shared *>(arg);

    tally(s, s->h->call(s->cid, AMBA_VIRT_IAV_OP_IOCTL, s->handle, (uint32_t)IAV_IOC_WAIT_NEXT_IDSP_FRAME, 1, NULL, 0));
    return NULL;
}

void *guestLoop(void *arg)
{
    Shared *s = static_cast<Shared *>(arg);

    for (int i = 0; i < s->iterations; i++) {
        IavBrokerHarness::Raw o = s->h->call(s->cid, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, NULL, 0);

        if (o.ret != 0 || o.status != 0 || o.rc != 0) {
            __atomic_add_fetch(&s->otherCount, 1, __ATOMIC_RELAXED);
            continue;
        }
        tally(s, s->h->call(s->cid, AMBA_VIRT_IAV_OP_IOCTL, o.handle, (uint32_t)IAV_IOC_GET_IAV_STATE, 0, NULL, 0));
        tally(s, s->h->call(s->cid, AMBA_VIRT_IAV_OP_CLOSE, o.handle, 0, 0, NULL, 0));
    }
    return NULL;
}

void *openAll(void *arg)
{
    Shared *s = static_cast<Shared *>(arg);

    for (int i = 0; i < s->iterations; i++) {
        IavBrokerHarness::Raw o = s->h->call(s->cid, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, NULL, 0);

        if (o.ret == 0 && o.status == 0 && o.rc == 0) {
            __atomic_add_fetch(&s->okCount, 1, __ATOMIC_RELAXED);
        } else if (o.ret == 0 && o.status == -EMFILE) {
            __atomic_add_fetch(&s->staleCount, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_add_fetch(&s->otherCount, 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

} // namespace

TEST_GROUP(IavBrokerLifecycle)
{
};

TEST(IavBrokerLifecycle, RateLimitIsPerGuestAndRefills)
{
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, 0, 10, 5);
    int ok = 0, limited = 0;

    for (int i = 0; i < 12; i++) {
        IavBrokerHarness::Result r = h.info(7);

        if (r.r.status == 0) {
            ok++;
        } else {
            LONGS_EQUAL(-EAGAIN, r.r.status);
            limited++;
        }
    }
    LONGS_EQUAL(5, ok);
    LONGS_EQUAL(7, limited);
    /* Another guest has its own bucket. */
    LONGS_EQUAL(0, h.info(8).r.status);
    /* 100 ms at 10 per second is one token. */
    h.drv.advance(100);
    LONGS_EQUAL(0, h.info(7).r.status);
    LONGS_EQUAL(-EAGAIN, h.info(7).r.status);
    /* A long idle time refills to the burst and no further. */
    h.drv.advance(100000);
    ok = 0;
    for (int i = 0; i < 20; i++) {
        ok += h.info(7).r.status == 0;
    }
    LONGS_EQUAL(5, ok);
}

TEST(IavBrokerLifecycle, CloseWhileACallIsInFlightKeepsTheFdUntilItReturns)
{
    IavBrokerHarness h;
    uint64_t hd = h.mustOpen(7);
    Shared s;
    pthread_t t;

    memset(&s, 0, sizeof(s));
    s.h = &h;
    s.cid = 7;
    s.handle = hd;
    h.drv.blockNextWait();
    LONGS_EQUAL(0, pthread_create(&t, NULL, waitOnce, &s));
    h.drv.waitUntilBlocked();
    Watchdog wd(h.drv, 3000);

    /* The guest closes and then disappears while the driver call is still running. */
    LONGS_EQUAL(0, h.close(7, hd).r.status);
    iav_broker_client_gone(&h.b, 7);
    LONGS_EQUAL(1, h.drv.openFds());
    /* A session that is closing takes no new calls. If it did, this would wait behind the call in flight. */
    LONGS_EQUAL(-ESTALE, h.ioctl(7, hd, IAV_IOC_GET_IAV_STATE, NULL, 0).r.status);
    CHECK(!wd.fired());

    h.drv.release();
    pthread_join(t, NULL);
    wd.stop();
    CHECK(!wd.fired());
    LONGS_EQUAL(1, s.okCount);
    LONGS_EQUAL(0, h.drv.openFds());
    LONGS_EQUAL(0, iav_broker_session_count(&h.b));
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerLifecycle, GuestGoneWhileACallIsInFlight)
{
    IavBrokerHarness h;
    uint64_t hd = h.mustOpen(7);
    Shared s;
    pthread_t t;

    memset(&s, 0, sizeof(s));
    s.h = &h;
    s.cid = 7;
    s.handle = hd;
    h.drv.blockNextWait();
    LONGS_EQUAL(0, pthread_create(&t, NULL, waitOnce, &s));
    h.drv.waitUntilBlocked();
    iav_broker_client_gone(&h.b, 7);
    LONGS_EQUAL(1, h.drv.openFds());
    h.drv.release();
    pthread_join(t, NULL);
    LONGS_EQUAL(0, h.drv.openFds());
    LONGS_EQUAL(0, iav_broker_session_count(&h.b));
}

TEST(IavBrokerLifecycle, GuestGoneWhileItsOpenIsInProgress)
{
    IavBrokerHarness h;
    IavBrokerHarness::Result r;

    h.drv.setOpenHook([&h]() { iav_broker_client_gone(&h.b, 7); });
    r = h.open(7);
    LONGS_EQUAL(-ECONNRESET, r.r.status);
    LONGS_EQUAL(0, h.drv.openFds());
    LONGS_EQUAL(0, iav_broker_session_count(&h.b));
    LONGS_EQUAL(0, iav_broker_cid_sessions(&h.b, 7));
    /* The guest can come back. */
    CHECK(h.mustOpen(7) != 0);
}

TEST(IavBrokerLifecycle, CloseRacesManyCallsOnTheSameHandle)
{
    IavBrokerHarness h;
    uint64_t hd = h.mustOpen(7);
    Shared s;
    pthread_t t[4];

    memset(&s, 0, sizeof(s));
    s.h = &h;
    s.cid = 7;
    s.handle = hd;
    s.iterations = 2000;
    for (int i = 0; i < 4; i++) {
        LONGS_EQUAL(0, pthread_create(&t[i], NULL, stateLoop, &s));
    }
    h.drv.advance(1);
    h.close(7, hd);
    for (int i = 0; i < 4; i++) {
        pthread_join(t[i], NULL);
    }
    LONGS_EQUAL(0, s.otherCount);
    LONGS_EQUAL(4 * 2000, s.okCount + s.staleCount);
    LONGS_EQUAL(0, h.drv.openFds());
    LONGS_EQUAL(0, iav_broker_session_count(&h.b));
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerLifecycle, ManyGuestsOpenUseAndCloseConcurrently)
{
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, 8);
    Shared s[8];
    pthread_t t[8];

    for (int i = 0; i < 8; i++) {
        memset(&s[i], 0, sizeof(s[i]));
        s[i].h = &h;
        s[i].cid = (uint32_t)(100 + i);
        s[i].iterations = 300;
        LONGS_EQUAL(0, pthread_create(&t[i], NULL, guestLoop, &s[i]));
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(t[i], NULL);
        LONGS_EQUAL(0, s[i].otherCount);
        LONGS_EQUAL(600, s[i].okCount);
    }
    LONGS_EQUAL(0, iav_broker_session_count(&h.b));
    LONGS_EQUAL(0, h.drv.openFds());
    LONGS_EQUAL(h.drv.totalOpens(), h.drv.totalCloses());
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerLifecycle, PerGuestLimitHoldsUnderConcurrentOpens)
{
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, 4);
    Shared s;
    pthread_t t[8];

    memset(&s, 0, sizeof(s));
    s.h = &h;
    s.cid = 7;
    s.iterations = 50;
    for (int i = 0; i < 8; i++) {
        LONGS_EQUAL(0, pthread_create(&t[i], NULL, openAll, &s));
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(t[i], NULL);
    }
    LONGS_EQUAL(0, s.otherCount);
    LONGS_EQUAL(4, s.okCount);
    LONGS_EQUAL(8 * 50 - 4, s.staleCount);
    LONGS_EQUAL(4, h.drv.openFds());
    LONGS_EQUAL(4, iav_broker_cid_sessions(&h.b, 7));
}

TEST(IavBrokerLifecycle, AFloodDoesNotStarveAnotherGuest)
{
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, 0, 100, 20);
    uint64_t other = h.mustOpen(8);
    int floodRefused = 0;

    for (int i = 0; i < 10000; i++) {
        if (h.info(7).r.status != 0) {
            floodRefused++;
        }
    }
    CHECK(floodRefused > 9900);
    LONGS_EQUAL(0, h.ioctl(8, other, IAV_IOC_GET_IAV_STATE, NULL, 0).r.status);
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
