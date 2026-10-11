/*
 * TestIavOpen.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavTestSupport.hxx"

#include <CppUTest/TestHarness.h>

using namespace iavtest;

TEST_GROUP(IavOpen)
{
};

TEST(IavOpen, OpenCloseSingle)
{
    ScopedFd f;

    CHECK_TRUE(f.open());
    CHECK_TRUE(f.fd() >= 0);
    f.close();
    LONGS_EQUAL(-1, f.fd());
}

TEST(IavOpen, OpenCloseRepeated)
{
    const int cycles = 50;
    int ok = 0;

    for (int i = 0; i < cycles; i++) {
        ScopedFd f;

        if (f.open()) {
            ok++;
        }
    }
    note("cycles", cycles);
    LONGS_EQUAL(cycles, ok);
}

TEST(IavOpen, OpenMultipleFds)
{
    ScopedFd f[4];
    u32 first = 0;

    for (int i = 0; i < 4; i++) {
        u32 state = 0;

        CHECK_TRUE(f[i].open());
        LONGS_EQUAL(0, ioctlPtr(f[i].fd(), IAV_IOC_GET_IAV_STATE, &state));
        if (i == 0) {
            first = state;
        }
        LONGS_EQUAL(first, state);
    }
    note("fds", 4);
}

TEST(IavOpen, DupKeepsFdUsable)
{
    ScopedFd f;
    int copy;
    u32 state = 0;

    CHECK_TRUE(f.open());
    copy = backend().dup(f.fd());
    CHECK_TRUE(copy >= 0);
    f.close();
    LONGS_EQUAL(0, ioctlPtr(copy, IAV_IOC_GET_IAV_STATE, &state));
    LONGS_EQUAL(0, backend().close(copy));
}

TEST(IavOpen, ReopenAfterClose)
{
    ScopedFd f;
    u32 state = 0;

    CHECK_TRUE(f.open());
    f.close();
    CHECK_TRUE(f.open());
    LONGS_EQUAL(0, ioctlPtr(f.fd(), IAV_IOC_GET_IAV_STATE, &state));
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
