/*
 * TestIavIoctlPlain.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Plain query ioctls. Each test checks that the call succeeds and records the fields of the
 * result that do not change while the pipeline runs, so the Envelope 5 comparison can check
 * that a virtualized run returns the same values.
 */

#include "IavTestSupport.hxx"

#include <CppUTest/TestHarness.h>

using namespace iavtest;

TEST_GROUP(IavIoctlPlain)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }
};

TEST(IavIoctlPlain, GetIavState)
{
    u32 state = 0xdeadbeef;

    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_GET_IAV_STATE, &state));
    CHECK_TRUE(state != 0xdeadbeef);
    note("state", state);
}

TEST(IavIoctlPlain, GetChipId)
{
    u32 id = 0;

    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_GET_CHIP_ID, &id));
    /* The real driver on N1-655 reports 0 here (observed), so the value is recorded, not judged. */
    note("chip_id", id);
}

TEST(IavIoctlPlain, QueryEncModeCap)
{
    struct iav_enc_mode_cap c;

    memset(&c, 0, sizeof(c));
    c.encode_mode = 0;
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_QUERY_ENC_MODE_CAP, &c));
    CHECK_TRUE(c.max_streams_num > 0);
    note("max_streams_num", c.max_streams_num);
    note("max_encode_MB", c.max_encode_MB);
}

TEST(IavIoctlPlain, QueryEncBufCap)
{
    struct iav_enc_buf_cap c;

    memset(&c, 0, sizeof(c));
    c.buf_id = 0;
    c.pass_id = 0;
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_QUERY_ENC_BUF_CAP, &c));
    CHECK_TRUE(c.max.width > 0);
    CHECK_TRUE(c.max.height > 0);
    note("max_width", c.max.width);
    note("max_height", c.max.height);
}

TEST(IavIoctlPlain, QueryIavCap)
{
    struct iav_capability c;

    memset(&c, 0, sizeof(c));
    c.cid = (enum iav_cap_id)0;
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_QUERY_IAV_CAP, &c));
    note("cid", (long long)c.cid);
}

TEST(IavIoctlPlain, GetSystemResource)
{
    struct iav_system_resource r;

    memset(&r, 0, sizeof(r));
    r.encode_mode = 0xff; /* the current encode mode */
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_GET_SYSTEM_RESOURCE, &r));
    CHECK_TRUE(r.chan_num >= 1);
    CHECK_TRUE(r.canvas_num >= 1);
    note("encode_mode", r.encode_mode);
    note("chan_num", r.chan_num);
    note("canvas_num", r.canvas_num);
    note("max_stream_num", r.max_stream_num);
}

TEST(IavIoctlPlain, GetChanConfig)
{
    struct iav_chan_cfg c;

    memset(&c, 0, sizeof(c));
    c.chan_id = 0;
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_GET_CHAN_CONFIG, &c));
    LONGS_EQUAL(0, c.chan_id);
    note("chan_id", c.chan_id);
    note("vsrc_id", c.vsrc_id);
}

TEST(IavIoctlPlain, GetCanvasConfig)
{
    struct iav_canvas_cfg c;

    memset(&c, 0, sizeof(c));
    c.canvas_id = 0;
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_GET_CANVAS_CONFIG, &c));
    LONGS_EQUAL(0, c.canvas_id);
    note("canvas_id", c.canvas_id);
}

TEST(IavIoctlPlain, DrvDspInfo)
{
    struct iav_driver_dsp_info d;

    memset(&d, 0, sizeof(d));
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_DRV_DSP_INFO, &d));
    /* The real driver on N1-655 reports dspid 0 (observed), so the value is recorded, not judged. */
    note("dspid", d.dspid);
}

TEST(IavIoctlPlain, QueryInfo)
{
    struct iav_queryinfo q;

    memset(&q, 0, sizeof(q));
    q.qid = IAV_INFO_DRIVER;
    LONGS_EQUAL(0, ioctlPtr(_f.fd(), IAV_IOC_QUERY_INFO, &q));
    note("qid", IAV_INFO_DRIVER);
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
