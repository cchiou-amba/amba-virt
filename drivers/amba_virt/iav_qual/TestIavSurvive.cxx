/*
 * TestIavSurvive.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Runs only in --mode survive. One process keeps one descriptor open and calls a cheap ioctl for
 * --seconds while something outside restarts the Dom0 server. The process must not be restarted,
 * must not be handed a failure it cannot recover from, and must be served again after the outage:
 * the calls fail while the server is away and succeed again once it is back, on the same
 * descriptor, with the poll configuration it set before the restart still in force.
 */

#include "IavTestSupport.hxx"

#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include <CppUTest/TestHarness.h>

using namespace iavtest;

TEST_GROUP(IavSurvive)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }
};

TEST(IavSurvive, TheDescriptorIsServedAgainAfterTheServerRestarts)
{
    const long long endNs = nowNs() + (long long)ctx().config()._seconds * 1000000000LL;
    struct iav_poll_event_config cfg;
    long long firstFail = -1, lastFail = -1, start = nowNs();
    long okBefore = 0, okAfter = 0, failures = 0, calls = 0, firstErrno = 0, lastErrno = 0;

    canvasPollConfig(&cfg);
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
    while (nowNs() < endNs) {
        int state = -1;
        int rc = ioctlPtr(_f.fd(), IAV_IOC_GET_IAV_STATE, &state);

        calls++;
        if (rc == 0) {
            if (lastFail < 0) {
                okBefore++;
            } else {
                okAfter++;
            }
        } else {
            long long t = nowNs() - start;

            failures++;
            lastErrno = errno;
            if (firstFail < 0) {
                firstFail = t;
                firstErrno = errno;
            }
            lastFail = t;
            okAfter = 0;
        }
        usleep(50 * 1000);
    }
    note("calls", calls);
    note("ok_before_first_failure", okBefore);
    note("failures", failures);
    note("first_failure_ms", firstFail < 0 ? -1 : firstFail / 1000000LL);
    note("last_failure_ms", lastFail < 0 ? -1 : lastFail / 1000000LL);
    note("outage_ms", firstFail < 0 ? 0 : (lastFail - firstFail) / 1000000LL);
    note("ok_after_last_failure", okAfter);
    note("first_errno", firstErrno);
    note("last_errno", lastErrno);
    CHECK_TEXT(okAfter >= 20, "the descriptor was not served again after the outage");
    CHECK_TEXT(okBefore + okAfter > 100, "too few successful calls");
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
