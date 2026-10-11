/*
 * TestIavLifecycle.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavTestSupport.hxx"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CppUTest/TestHarness.h>

using namespace iavtest;

TEST_GROUP(IavLifecycle)
{
    /*
     * Poll an open file and return poll()'s result. A file with no poll configuration must
     * time out with 0. The wait is longer than a frame period, so a configuration left behind
     * by an earlier file shows up as a wakeup.
     */
    int pollQuiet(int fd)
    {
        struct pollfd p;

        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        return backend().poll(&p, 1, 100);
    }
};

/* A file that was closed must not leave its poll configuration for the next opener. */
TEST(IavLifecycle, CloseReleasesPollConfig)
{
    struct iav_poll_event_config cfg;
    ScopedFd a, b;

    CHECK_TRUE(a.open());
    canvasPollConfig(&cfg);
    LONGS_EQUAL(0, ioctlPtr(a.fd(), IAV_IOC_SET_POLL_EVENT_CFG, &cfg));
    a.close();

    CHECK_TRUE(b.open());
    LONGS_EQUAL(0, pollQuiet(b.fd()));
}

TEST(IavLifecycle, MappingSurvivesFdClose)
{
    ScopedFd f;
    ScopedMap map;
    unsigned long addr = 0, length = 0;
    volatile unsigned char v;

    CHECK_TRUE(f.open());
    LONGS_EQUAL(0, queryPartition(f.fd(), IAV_PART_BSB, &addr, &length));
    CHECK_TRUE(map.map(f.fd(), addr, length, PROT_READ));
    f.close();
    for (unsigned long off = 0; off < length; off += 4096) {
        v = map.bytes()[off];
    }
    (void)v;
    note("pages_read_after_close", (long long)(length / 4096));
}

/* A process that exits without closing its file must not leave state behind. */
TEST(IavLifecycle, ProcessExitReleasesFd)
{
    struct iav_poll_event_config cfg;
    ScopedFd f;
    pid_t pid;
    int status = 0;
    u32 state = 0;

    pid = fork();
    CHECK_TRUE(pid >= 0);
    if (pid == 0) {
        int fd = backend().open(ctx().config()._device.c_str(), O_RDWR);

        if (fd < 0) {
            _exit(3);
        }
        canvasPollConfig(&cfg);
        if (ioctlPtr(fd, IAV_IOC_SET_POLL_EVENT_CFG, &cfg) != 0) {
            _exit(4);
        }
        _exit(0);
    }
    LONGS_EQUAL(pid, waitpid(pid, &status, 0));
    CHECK_TRUE(WIFEXITED(status));
    LONGS_EQUAL(0, WEXITSTATUS(status));

    CHECK_TRUE(f.open());
    LONGS_EQUAL(0, ioctlPtr(f.fd(), IAV_IOC_GET_IAV_STATE, &state));
    LONGS_EQUAL(0, pollQuiet(f.fd()));
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
