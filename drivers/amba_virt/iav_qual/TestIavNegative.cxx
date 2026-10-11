/*
 * TestIavNegative.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Bad requests. The suite only sends requests that the driver is expected to refuse early:
 * unknown commands, null and out-of-range arguments, and invalid mappings. The results are
 * recorded so that a virtualized run can be compared with the native one.
 */

#include "IavTestSupport.hxx"

#include <CppUTest/TestHarness.h>

using namespace iavtest;

TEST_GROUP(IavNegative)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }

    int record(int rc)
    {
        int err = rc < 0 ? errno : 0;

        ctx().setResult(rc, err);
        errno = err;
        return rc;
    }
};

TEST(IavNegative, UnknownIoctlNumber)
{
    LONGS_EQUAL(-1, record(backend().ioctl(_f.fd(), _IO(IAVENCIOC_MAGIC, 0xfe), 0)));
    LONGS_EQUAL(ENOTTY, errno);
}

TEST(IavNegative, WrongMagic)
{
    LONGS_EQUAL(-1, record(backend().ioctl(_f.fd(), _IO('Z', 1), 0)));
    LONGS_EQUAL(ENOTTY, errno);
}

TEST(IavNegative, NullPointerArg)
{
    LONGS_EQUAL(-1, record(backend().ioctl(_f.fd(), IAV_IOC_GET_IAV_STATE, 0)));
    LONGS_EQUAL(EFAULT, errno);
}

/*
 * The real driver on N1-655 answers GET_CHAN_CONFIG for channel 200 with success and echoes the
 * channel id back (observed on the devkit), so it does not range-check the id. The test records
 * the outcome and checks only that the call returns promptly with a defined result. A virtualized
 * run must return the same outcome as the native one; this is a parity probe.
 */
TEST(IavNegative, InvalidChanId)
{
    struct iav_chan_cfg c;
    int rc;

    memset(&c, 0, sizeof(c));
    c.chan_id = 200;
    rc = record(ioctlPtr(_f.fd(), IAV_IOC_GET_CHAN_CONFIG, &c));
    CHECK_TRUE(rc == 0 || rc == -1);
    note("returned_chan_id", c.chan_id);
    note("returned_vsrc_id", c.vsrc_id);
}

TEST(IavNegative, InvalidCanvasId)
{
    struct iav_canvas_info c;

    memset(&c, 0, sizeof(c));
    LONGS_EQUAL(-1, record(queryCanvasInfo(_f.fd(), 200, &c)));
}

TEST(IavNegative, InvalidMemblockId)
{
    unsigned long addr = 0, length = 0;

    LONGS_EQUAL(-1, record(queryPartition(_f.fd(), 0xffff, &addr, &length)));
}

TEST(IavNegative, IoctlOnClosedFd)
{
    ScopedFd g;
    int fd;
    u32 state = 0;

    CHECK_TRUE(g.open());
    fd = g.release();
    LONGS_EQUAL(0, backend().close(fd));
    LONGS_EQUAL(-1, record(ioctlPtr(fd, IAV_IOC_GET_IAV_STATE, &state)));
    LONGS_EQUAL(EBADF, errno);
}

TEST(IavNegative, MapZeroLength)
{
    unsigned long addr = 0, length = 0;
    void *p;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_BSB, &addr, &length));
    p = backend().mmap(0, PROT_READ, MAP_SHARED, _f.fd(), (off_t)addr);
    CHECK_TRUE(p == MAP_FAILED);
    record(-1);
    LONGS_EQUAL(EINVAL, errno);
}

TEST(IavNegative, MapMisalignedOffset)
{
    unsigned long addr = 0, length = 0;
    void *p;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_BSB, &addr, &length));
    p = backend().mmap(4096, PROT_READ, MAP_SHARED, _f.fd(), (off_t)(addr + 1));
    CHECK_TRUE(p == MAP_FAILED);
    record(-1);
    LONGS_EQUAL(EINVAL, errno);
}

TEST(IavNegative, MapOffsetNotAPartition)
{
    void *p = backend().mmap(4096, PROT_READ, MAP_SHARED, _f.fd(), (off_t)0x1000);

    CHECK_TRUE(p == MAP_FAILED);
    record(-1);
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
