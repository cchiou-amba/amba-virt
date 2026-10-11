/*
 * TestHostIavBrokerOps.cxx
 *
 * The operations: window information and mapping authorization, descriptor queries with bounded
 * waits, event configuration, poll, and the pass-through of results and errors.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdint.h>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

TEST_GROUP(IavBrokerOps)
{
    IavBrokerHarness *h;
    uint64_t hd;

    void setup()
    {
        h = new IavBrokerHarness();
        hd = h->mustOpen(7);
        CHECK(hd != 0);
    }

    void teardown()
    {
        CHECK(h->drv.violations().empty());
        delete h;
    }

    void refused(const IavBrokerHarness::Result &r, int err)
    {
        LONGS_EQUAL(0, r.ret);
        LONGS_EQUAL(-err, r.r.status);
        LONGS_EQUAL(0, r.r.out_len);
    }

    struct amba_virt_iav_map mapOut(const IavBrokerHarness::Result &r)
    {
        struct amba_virt_iav_map m;

        memset(&m, 0, sizeof(m));
        LONGS_EQUAL(0, r.r.status);
        LONGS_EQUAL(sizeof(m), r.out.size());
        memcpy(&m, &r.out[0], sizeof(m));
        return m;
    }
};

TEST(IavBrokerOps, InfoListsTheWindows)
{
    IavBrokerHarness::Result r = h->info(7);
    struct amba_virt_iav_window w[2];

    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(2, r.r.value);
    LONGS_EQUAL(sizeof(w), r.out.size());
    memcpy(w, &r.out[0], sizeof(w));
    LONGS_EQUAL(AMBA_VIRT_IAV_WIN_DSP, w[0].id);
    CHECK(w[0].base == MockIavDriver::DspBase);
    CHECK(w[0].size == MockIavDriver::DspLen);
    CHECK(w[0].part_len == MockIavDriver::DspLen);
    LONGS_EQUAL(AMBA_VIRT_IAV_WIN_BSB, w[1].id);
    CHECK(w[1].base == MockIavDriver::BsbBase);
    CHECK(w[1].size == MockIavDriver::BsbWindow);
    CHECK(w[1].part_len == MockIavDriver::BsbLen);
}

TEST(IavBrokerOps, InfoTakesNoArgument)
{
    uint8_t extra[8] = {0};
    std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, extra, 8);

    refused(h->send(7, &v[0], v.size()), EMSGSIZE);
}

TEST(IavBrokerOps, MapAuthorizesTheBaseOfAWindow)
{
    struct amba_virt_iav_map m;

    m = mapOut(h->map(7, hd, MockIavDriver::DspBase, MockIavDriver::DspLen, PROT_READ));
    LONGS_EQUAL(AMBA_VIRT_IAV_WIN_DSP, m.window);
    CHECK(m.window_offset == 0);
    CHECK(m.length == MockIavDriver::DspLen);
    LONGS_EQUAL(PROT_READ, m.prot);
    m = mapOut(h->map(7, hd, MockIavDriver::DspBase, 4096, PROT_READ | PROT_WRITE));
    CHECK(m.length == 4096);
    LONGS_EQUAL(PROT_READ | PROT_WRITE, m.prot);
    m = mapOut(h->map(7, hd, MockIavDriver::BsbBase, MockIavDriver::BsbLen, PROT_READ));
    LONGS_EQUAL(AMBA_VIRT_IAV_WIN_BSB, m.window);
}

TEST(IavBrokerOps, MapAllowsTheBitstreamRingTwiceButNoMore)
{
    struct amba_virt_iav_map m = mapOut(h->map(7, hd, MockIavDriver::BsbBase,
                                               2 * MockIavDriver::BsbLen, PROT_READ));

    CHECK(m.length == 2 * MockIavDriver::BsbLen);
    refused(h->map(7, hd, MockIavDriver::BsbBase, 2 * MockIavDriver::BsbLen + 4096, PROT_READ), EINVAL);
    /* A partition that is not a ring cannot be mapped twice. */
    refused(h->map(7, hd, MockIavDriver::DspBase, 2 * MockIavDriver::DspLen, PROT_READ), EINVAL);
    refused(h->map(7, hd, MockIavDriver::DspBase, MockIavDriver::DspLen + 4096, PROT_READ), EINVAL);
}

TEST(IavBrokerOps, MapRefusesWhatIsNotAWindowBase)
{
    uint64_t addrs[] = {0, 0x1000, MockIavDriver::DspBase + 4096, MockIavDriver::DspBase - 4096,
                        MockIavDriver::BsbBase + MockIavDriver::BsbLen, 0x100000000ull,
                        0xffffffffffff0000ull, MockIavDriver::DspBase + MockIavDriver::DspLen};

    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        refused(h->map(7, hd, addrs[i], 4096, PROT_READ), EFAULT);
    }
}

TEST(IavBrokerOps, MapRefusesBadLengthsAndAlignment)
{
    refused(h->map(7, hd, MockIavDriver::DspBase, 0, PROT_READ), EINVAL);
    refused(h->map(7, hd, MockIavDriver::DspBase, 100, PROT_READ), EINVAL);
    refused(h->map(7, hd, MockIavDriver::DspBase + 1, 4096, PROT_READ), EINVAL);
    refused(h->map(7, hd, MockIavDriver::DspBase, UINT64_MAX, PROT_READ), EINVAL);
    refused(h->map(7, hd, MockIavDriver::DspBase, 0xfffffffffffff000ull, PROT_READ), EINVAL);
    refused(h->map(7, hd, MockIavDriver::DspBase, 0x8000000000000000ull, PROT_READ), EINVAL);
}

TEST(IavBrokerOps, MapRefusesExecutableAndUnknownProtections)
{
    refused(h->map(7, hd, MockIavDriver::DspBase, 4096, PROT_READ | PROT_EXEC), EPERM);
    refused(h->map(7, hd, MockIavDriver::DspBase, 4096, PROT_EXEC), EPERM);
    refused(h->map(7, hd, MockIavDriver::DspBase, 4096, PROT_WRITE), EPERM);
    refused(h->map(7, hd, MockIavDriver::DspBase, 4096, 0), EPERM);
    refused(h->map(7, hd, MockIavDriver::DspBase, 4096, 0x80000000u | PROT_READ), EPERM);
}

TEST(IavBrokerOps, MapNeedsAnOpenFile)
{
    refused(h->map(7, 0, MockIavDriver::DspBase, 4096, PROT_READ), EBADF);
    h->close(7, hd);
    refused(h->map(7, hd, MockIavDriver::DspBase, 4096, PROT_READ), ESTALE);
}

TEST(IavBrokerOps, MemblockPassesThroughUnchanged)
{
    struct iav_querymem q;
    IavBrokerHarness::Result r;

    memset(&q, 0, sizeof(q));
    q.mid = IAV_MEM_PARTITION;
    q.arg.partition.pid = IAV_PART_BSB;
    r = h->ioctl(7, hd, IAV_IOC_QUERY_MEMBLOCK, &q, sizeof(q));
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(0, r.r.rc);
    LONGS_EQUAL(sizeof(q), r.out.size());
    memcpy(&q, &r.out[0], sizeof(q));
    CHECK(q.arg.partition.mem.addr == MockIavDriver::BsbBase);
    CHECK(q.arg.partition.mem.length == MockIavDriver::BsbLen);

    /* An invalid partition id is the driver's error, passed back, not a broker refusal. */
    q.arg.partition.pid = 0xffff;
    r = h->ioctl(7, hd, IAV_IOC_QUERY_MEMBLOCK, &q, sizeof(q));
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(-1, r.r.rc);
    LONGS_EQUAL(EINVAL, r.r.err);
    LONGS_EQUAL(0, r.r.out_len);
}

TEST(IavBrokerOps, DriverErrorsAreCarriedBackWithNoOutput)
{
    IavBrokerHarness::Result r;

    h->drv.failIoctl(IAV_IOC_GET_CHIP_ID, EIO);
    r = h->ioctl(7, hd, IAV_IOC_GET_CHIP_ID, NULL, 0);
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(-1, r.r.rc);
    LONGS_EQUAL(EIO, r.r.err);
    LONGS_EQUAL(0, r.r.out_len);
    h->drv.failIoctl(IAV_IOC_GET_CHIP_ID, 0);
    r = h->ioctl(7, hd, IAV_IOC_GET_CHIP_ID, NULL, 0);
    LONGS_EQUAL(0, r.r.rc);
    LONGS_EQUAL(sizeof(u32), r.out.size());
}

TEST(IavBrokerOps, ValueIoctlReturnsThePositiveCount)
{
    IavBrokerHarness::Result r = h->ioctl(7, hd, IAV_IOC_WAIT_NEXT_IDSP_FRAME, NULL, 0, 1);

    LONGS_EQUAL(0, r.r.status);
    CHECK(r.r.rc >= 500);
    LONGS_EQUAL(0, r.r.err);
    LONGS_EQUAL(0, r.r.out_len);
    LONGS_EQUAL(r.r.rc + 1, h->ioctl(7, hd, IAV_IOC_WAIT_NEXT_IDSP_FRAME, NULL, 0, 1).r.rc);
}

TEST(IavBrokerOps, BlockingCanvasQueryIsForwardedAsBlocking)
{
    IavBrokerHarness::Result r = h->canvas(7, hd, true);

    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(0, r.r.rc);
    LONGS_EQUAL(sizeof(struct iav_querydesc), r.out.size());
    LONGS_EQUAL(1, h->drv.reached(IAV_IOC_QUERY_DESC));
}

TEST(IavBrokerOps, BlockingCanvasQueryTimeoutIsTheDriversAndIsNotRetried)
{
    uint64_t t0 = h->drv.now();
    IavBrokerHarness::Result r;

    h->drv.setEagainCount(1000000);
    r = h->canvas(7, hd, true);
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(-1, r.r.rc);
    LONGS_EQUAL(EAGAIN, r.r.err);
    LONGS_EQUAL(1, h->drv.reached(IAV_IOC_QUERY_DESC));
    LONGS_EQUAL(MockIavDriver::BlockMaxMs, h->drv.now() - t0);
}

TEST(IavBrokerOps, NonBlockingCanvasQueryAsksTheDriverOnce)
{
    uint64_t t0 = h->drv.now();
    IavBrokerHarness::Result r;

    h->drv.setEagainCount(1);
    r = h->canvas(7, hd, false);
    LONGS_EQUAL(-1, r.r.rc);
    LONGS_EQUAL(EAGAIN, r.r.err);
    LONGS_EQUAL(1, h->drv.reached(IAV_IOC_QUERY_DESC));
    LONGS_EQUAL(0, h->drv.now() - t0);
}

TEST(IavBrokerOps, BlockingPyramidQueryTimeoutIsTheDriversAndIsNotRetried)
{
    struct iav_querydesc q;
    IavBrokerHarness::Result r;

    memset(&q, 0, sizeof(q));
    q.qid = IAV_DESC_PYRAMID;
    q.arg.pyramid.chan_id = 0;
    q.arg.pyramid.layers_map = 1;
    h->drv.setEagainCount(1000000);
    r = h->ioctl(7, hd, IAV_IOC_QUERY_DESC, &q, sizeof(q));
    LONGS_EQUAL(-1, r.r.rc);
    LONGS_EQUAL(EAGAIN, r.r.err);
    LONGS_EQUAL(1, h->drv.reached(IAV_IOC_QUERY_DESC));
}

TEST(IavBrokerOps, DescriptorQueriesNeverCarryADmaBufFd)
{
    struct iav_querydesc q;

    memset(&q, 0, sizeof(q));
    q.qid = IAV_DESC_CANVAS;
    q.arg.canvas.yuv_use_dma_buf_fd = 1;
    refused(h->ioctl(7, hd, IAV_IOC_QUERY_DESC, &q, sizeof(q)), EPERM);
    q.arg.canvas.yuv_use_dma_buf_fd = 0;
    q.arg.canvas.me_use_dma_buf_fd = 1;
    refused(h->ioctl(7, hd, IAV_IOC_QUERY_DESC, &q, sizeof(q)), EPERM);
    q.arg.canvas.me_use_dma_buf_fd = 0;
    q.arg.canvas.mo_use_dma_buf_fd = 1;
    refused(h->ioctl(7, hd, IAV_IOC_QUERY_DESC, &q, sizeof(q)), EPERM);
    memset(&q, 0, sizeof(q));
    q.qid = IAV_DESC_PYRAMID;
    q.arg.pyramid.use_dma_buf_fd = 1;
    refused(h->ioctl(7, hd, IAV_IOC_QUERY_DESC, &q, sizeof(q)), EPERM);
    LONGS_EQUAL(0, h->drv.reached(IAV_IOC_QUERY_DESC));
}

TEST(IavBrokerOps, OnlyFrameCanvasAndPyramidDescriptors)
{
    struct iav_querydesc q;
    int qids[] = {IAV_DESC_STATIS, IAV_DESC_RAW, IAV_DESC_SLICE, IAV_DESC_Y12, IAV_DESC_POLL,
                  IAV_DESC_MOTION, IAV_DESC_NUM, 99, -1, 0x7fffffff};

    for (size_t i = 0; i < sizeof(qids) / sizeof(qids[0]); i++) {
        memset(&q, 0, sizeof(q));
        q.qid = (enum iav_desc_id)qids[i];
        refused(h->ioctl(7, hd, IAV_IOC_QUERY_DESC, &q, sizeof(q)), EPERM);
    }
    LONGS_EQUAL(0, h->drv.reached(IAV_IOC_QUERY_DESC));
}

TEST(IavBrokerOps, FrameQueryWaitIsClamped)
{
    struct iav_querydesc q;
    u32 times[] = {0, 1, 2000, 2001, 100000, 0xfffffffeU, 0xffffffffU};

    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        IavBrokerHarness::Result r;

        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_FRAME;
        q.arg.frame.id = 0xffffffffU;
        q.arg.frame.time_ms = times[i];
        r = h->ioctl(7, hd, IAV_IOC_QUERY_DESC, &q, sizeof(q));
        LONGS_EQUAL(0, r.r.status);
        LONGS_EQUAL(0, r.r.rc);
    }
    /* The mock records a violation if any wait reached it unbounded; teardown checks. */
}

TEST(IavBrokerOps, PollConfigLimits)
{
    struct iav_poll_event_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.valid_num = MAX_POLL_EVENT;
    for (int i = 0; i < MAX_POLL_EVENT; i++) {
        cfg.desc_event[i].qid = IAV_DESC_CANVAS;
    }
    LONGS_EQUAL(0, h->ioctl(7, hd, IAV_IOC_SET_POLL_EVENT_CFG, &cfg, sizeof(cfg)).r.status);
    cfg.valid_num = MAX_POLL_EVENT + 1;
    refused(h->ioctl(7, hd, IAV_IOC_SET_POLL_EVENT_CFG, &cfg, sizeof(cfg)), EINVAL);
    cfg.valid_num = 255;
    refused(h->ioctl(7, hd, IAV_IOC_SET_POLL_EVENT_CFG, &cfg, sizeof(cfg)), EINVAL);
    cfg.valid_num = 1;
    cfg.desc_event[0].qid = IAV_DESC_STATIS;
    refused(h->ioctl(7, hd, IAV_IOC_SET_POLL_EVENT_CFG, &cfg, sizeof(cfg)), EPERM);
    /* An event past valid_num is not looked at. */
    cfg.desc_event[0].qid = IAV_DESC_CANVAS;
    cfg.desc_event[1].qid = IAV_DESC_STATIS;
    LONGS_EQUAL(0, h->ioctl(7, hd, IAV_IOC_SET_POLL_EVENT_CFG, &cfg, sizeof(cfg)).r.status);
}

TEST(IavBrokerOps, PollWaitIsASliceAndEventsAreFiltered)
{
    struct iav_poll_event_config cfg;
    IavBrokerHarness::Result r;
    uint64_t t0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.valid_num = 1;
    cfg.desc_event[0].qid = IAV_DESC_CANVAS;
    LONGS_EQUAL(0, h->ioctl(7, hd, IAV_IOC_SET_POLL_EVENT_CFG, &cfg, sizeof(cfg)).r.status);

    r = h->poll(7, hd, POLLIN, 50);
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(1, r.r.rc);
    LONGS_EQUAL(POLLIN, r.r.value);

    /* Not ready: the wait is the slice at most, however long the guest asked. */
    h->drv.setPollReady(false);
    t0 = h->drv.now();
    r = h->poll(7, hd, POLLIN, 1000000);
    LONGS_EQUAL(0, r.r.rc);
    CHECK(h->drv.now() - t0 <= AMBA_VIRT_IAV_POLL_SLICE_MS);
    r = h->poll(7, hd, 0xffffffffu, 0xffffffffffffffffull);
    LONGS_EQUAL(0, r.r.status);
    r = h->poll(7, hd, POLLIN, 10);
    LONGS_EQUAL(0, r.r.status);
}

TEST(IavBrokerOps, PollRefusesAnArgument)
{
    uint8_t extra[4] = {0};
    std::vector<uint8_t> v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_POLL, hd, POLLIN, 10, 0, extra, 4);

    refused(h->send(7, &v[0], v.size()), EMSGSIZE);
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
