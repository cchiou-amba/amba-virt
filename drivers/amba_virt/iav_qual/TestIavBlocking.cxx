/*
 * TestIavBlocking.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavTestSupport.hxx"

#include <CppUTest/TestHarness.h>

using namespace iavtest;

TEST_GROUP(IavBlocking)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }
};

/*
 * IAV_IOC_WAIT_NEXT_IDSP_FRAME takes a channel map as its argument value. On success the real
 * driver returns a positive frame count (observed 497, then 498 on the next call), so success is
 * a non-negative return.
 */
TEST(IavBlocking, WaitNextIdspFrame)
{
    long long start = nowNs();
    long long elapsedMs;
    int rc = backend().ioctl(_f.fd(), IAV_IOC_WAIT_NEXT_IDSP_FRAME, 1);

    elapsedMs = (nowNs() - start) / 1000000;
    ctx().setResult(rc, rc >= 0 ? 0 : errno);
    CHECK_TRUE(rc >= 0);
    CHECK_TEXT(elapsedMs < 2000, "waiting for the next IDSP frame took longer than 2 s");
}

TEST(IavBlocking, WaitNextIdspFrameTwice)
{
    int counts[2] = {0, 0};

    for (int i = 0; i < 2; i++) {
        long long start = nowNs();
        long long elapsedMs;

        counts[i] = backend().ioctl(_f.fd(), IAV_IOC_WAIT_NEXT_IDSP_FRAME, 1);
        elapsedMs = (nowNs() - start) / 1000000;
        CHECK_TRUE(counts[i] >= 0);
        CHECK_TEXT(elapsedMs < 2000, "waiting for the next IDSP frame took longer than 2 s");
    }
    /* The returned counts are recorded, not ordered: they were not strictly increasing on the devkit. */
    note("count_first_nonnegative", counts[0] >= 0);
    note("count_second_nonnegative", counts[1] >= 0);
    note("waits", 2);
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
