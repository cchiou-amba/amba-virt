/*
 * TestIavConcurrency.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Worker threads count failures and the test thread checks them after the join: CppUTest
 * assertions are not safe to call from other threads.
 */

#include "IavTestSupport.hxx"

#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <CppUTest/TestHarness.h>

using namespace iavtest;

namespace {

struct Worker
{
    int iterations;
    long long runMs;
    int errors;
    int done;
};

void addError(Worker *w)
{
    __atomic_add_fetch(&w->errors, 1, __ATOMIC_RELAXED);
}

void *getStateThread(void *arg)
{
    Worker *w = (Worker *)arg;
    ScopedFd f;

    if (!f.open()) {
        addError(w);
        return NULL;
    }
    for (int i = 0; i < w->iterations; i++) {
        u32 state = 0;

        if (ioctlPtr(f.fd(), IAV_IOC_GET_IAV_STATE, &state) != 0) {
            addError(w);
        }
    }
    return NULL;
}

void *openCloseThread(void *arg)
{
    Worker *w = (Worker *)arg;

    for (int i = 0; i < w->iterations; i++) {
        ScopedFd f;

        if (!f.open()) {
            addError(w);
        }
    }
    return NULL;
}

void *pollThread(void *arg)
{
    Worker *w = (Worker *)arg;
    ScopedFd f;
    struct iav_poll_event_config cfg;
    long long end = nowNs() + w->runMs * 1000000LL;

    if (!f.open()) {
        addError(w);
        return NULL;
    }
    canvasPollConfig(&cfg);
    if (ioctlPtr(f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg) != 0) {
        addError(w);
        return NULL;
    }
    while (nowNs() < end) {
        struct pollfd p;
        int n;

        p.fd = f.fd();
        p.events = POLLIN;
        p.revents = 0;
        n = backend().poll(&p, 1, 100);
        if (n < 0 && errno != EINTR) {
            addError(w);
        } else if (n == 1) {
            struct iav_querydesc q;

            if (queryCanvas(f.fd(), false, &q) != 0 && errno != EAGAIN) {
                addError(w);
            }
        }
    }
    return NULL;
}

void *infoThread(void *arg)
{
    Worker *w = (Worker *)arg;
    ScopedFd f;
    long long end = nowNs() + w->runMs * 1000000LL;

    if (!f.open()) {
        addError(w);
        return NULL;
    }
    while (nowNs() < end) {
        struct iav_canvas_info c;

        memset(&c, 0, sizeof(c));
        if (queryCanvasInfo(f.fd(), 0, &c) != 0) {
            addError(w);
        }
    }
    return NULL;
}

} // namespace

TEST_GROUP(IavConcurrency)
{
};

TEST(IavConcurrency, ParallelGetState)
{
    const int threads = 4;
    Worker w[4];
    pthread_t t[4];

    for (int i = 0; i < threads; i++) {
        w[i].iterations = 200;
        w[i].runMs = 0;
        w[i].errors = 0;
        w[i].done = 0;
        LONGS_EQUAL(0, pthread_create(&t[i], NULL, getStateThread, &w[i]));
    }
    for (int i = 0; i < threads; i++) {
        pthread_join(t[i], NULL);
        LONGS_EQUAL(0, w[i].errors);
    }
    note("threads", threads);
}

TEST(IavConcurrency, ParallelOpenClose)
{
    const int threads = 4;
    Worker w[4];
    pthread_t t[4];

    for (int i = 0; i < threads; i++) {
        w[i].iterations = 50;
        w[i].runMs = 0;
        w[i].errors = 0;
        w[i].done = 0;
        LONGS_EQUAL(0, pthread_create(&t[i], NULL, openCloseThread, &w[i]));
    }
    for (int i = 0; i < threads; i++) {
        pthread_join(t[i], NULL);
        LONGS_EQUAL(0, w[i].errors);
    }
    note("threads", threads);
}

TEST(IavConcurrency, ParallelPollAndQuery)
{
    Worker a, b;
    pthread_t ta, tb;

    a.iterations = b.iterations = 0;
    a.runMs = b.runMs = 1000;
    a.errors = b.errors = 0;
    a.done = b.done = 0;
    LONGS_EQUAL(0, pthread_create(&ta, NULL, pollThread, &a));
    LONGS_EQUAL(0, pthread_create(&tb, NULL, infoThread, &b));
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    LONGS_EQUAL(0, a.errors);
    LONGS_EQUAL(0, b.errors);
    note("run_ms", 1000);
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
