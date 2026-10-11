/*
 * TestIavDescriptors.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavTestSupport.hxx"

#include <CppUTest/TestHarness.h>

using namespace iavtest;

TEST_GROUP(IavDescriptors)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }
};

TEST(IavDescriptors, QueryCanvasInfo)
{
    struct iav_canvas_info c;
    unsigned long addr = 0, length = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_DSP, &addr, &length));
    memset(&c, 0, sizeof(c));
    LONGS_EQUAL(0, queryCanvasInfo(_f.fd(), 0, &c));
    CHECK_TRUE(c.width > 0);
    CHECK_TRUE(c.height > 0);
    CHECK_TRUE(c.yuv_pitch >= c.width);
    /* The canvas lives in the DSP partition. */
    CHECK_TEXT(c.mem_phys_addr == addr, "canvas memory is not the DSP partition");
    CHECK_TEXT(c.mem_size == length, "canvas memory size differs from the DSP partition");
    note("width", c.width);
    note("height", c.height);
    note("yuv_pitch", c.yuv_pitch);
}

TEST(IavDescriptors, QueryDescCanvas)
{
    struct iav_querydesc q;

    LONGS_EQUAL(0, queryCanvas(_f.fd(), true, &q));
    CHECK_TRUE(q.arg.canvas.yuv.width > 0);
    CHECK_TRUE(q.arg.canvas.yuv.height > 0);
    CHECK_TRUE(q.arg.canvas.yuv.pitch >= q.arg.canvas.yuv.width);
    note("width", q.arg.canvas.yuv.width);
    note("height", q.arg.canvas.yuv.height);
    note("pitch", q.arg.canvas.yuv.pitch);
}

TEST(IavDescriptors, QueryDescPyramid)
{
    struct iav_querydesc q;

    LONGS_EQUAL(0, queryPyramid(_f.fd(), true, &q));
    CHECK_TRUE(q.arg.pyramid.layers[0].width > 0);
    CHECK_TRUE(q.arg.pyramid.layers[0].height > 0);
    note("layer0_width", q.arg.pyramid.layers[0].width);
    note("layer0_height", q.arg.pyramid.layers[0].height);
}

TEST(IavDescriptors, QueryDescFrame)
{
    struct iav_querydesc q;
    unsigned long addr = 0, length = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_BSB, &addr, &length));
    memset(&q, 0, sizeof(q));
    q.qid = IAV_DESC_FRAME;
    q.arg.frame.id = 0xffffffffU; /* every stream */
    q.arg.frame.time_ms = 2000;
    {
        int rc = ioctlPtr(_f.fd(), IAV_IOC_QUERY_DESC, &q);

        ctx().setResult(rc, rc == 0 ? 0 : errno);
        LONGS_EQUAL(0, rc);
    }
    CHECK_TRUE(q.arg.frame.size > 0);
    CHECK_TEXT(q.arg.frame.data_addr_offset < length, "frame offset is outside the bitstream buffer");
    note("size_positive", q.arg.frame.size > 0);
}

TEST(IavDescriptors, CanvasSequenceMonotonic)
{
    struct iav_querydesc q;
    u32 prevSeq = 0;
    u32 prevPts = 0;

    for (int i = 0; i < 10; i++) {
        LONGS_EQUAL(0, queryCanvas(_f.fd(), true, &q));
        if (i > 0) {
            CHECK_TEXT(q.arg.canvas.yuv.seq_num > prevSeq, "canvas sequence number did not increase");
            CHECK_TEXT((int)(q.arg.canvas.yuv.dsp_pts - prevPts) >= 0, "canvas dsp pts went backwards");
        }
        prevSeq = q.arg.canvas.yuv.seq_num;
        prevPts = q.arg.canvas.yuv.dsp_pts;
    }
    note("queries", 10);
}

TEST(IavDescriptors, DescOffsetsInsidePartition)
{
    struct iav_querydesc q;
    unsigned long addr = 0, length = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_DSP, &addr, &length));

    LONGS_EQUAL(0, queryCanvas(_f.fd(), true, &q));
    {
        const struct iav_yuv_cap &y = q.arg.canvas.yuv;
        unsigned long yBytes = (unsigned long)y.pitch * y.height;
        unsigned long uvBytes = (unsigned long)y.pitch * (y.height / 2);

        CHECK_TEXT(y.y_addr_offset + yBytes <= length, "canvas Y data is outside the DSP partition");
        CHECK_TEXT(y.uv_addr_offset + uvBytes <= length, "canvas UV data is outside the DSP partition");
    }

    LONGS_EQUAL(0, queryPyramid(_f.fd(), true, &q));
    {
        const struct iav_yuv_cap &y = q.arg.pyramid.layers[0];
        unsigned long yBytes = (unsigned long)y.pitch * y.height;
        unsigned long uvBytes = (unsigned long)y.pitch * (y.height / 2);

        CHECK_TEXT(y.y_addr_offset + yBytes <= length, "pyramid Y data is outside the DSP partition");
        CHECK_TEXT(y.uv_addr_offset + uvBytes <= length, "pyramid UV data is outside the DSP partition");
    }
}

TEST(IavDescriptors, QueryDescTimeoutHonored)
{
    struct iav_querydesc q;
    long long start, elapsedMs;
    int rc;

    memset(&q, 0, sizeof(q));
    q.qid = IAV_DESC_FRAME;
    q.arg.frame.id = 0xffffffffU;
    q.arg.frame.time_ms = 1;
    start = nowNs();
    rc = ioctlPtr(_f.fd(), IAV_IOC_QUERY_DESC, &q);
    elapsedMs = (nowNs() - start) / 1000000;
    ctx().setResult(rc, rc == 0 ? 0 : errno);
    CHECK_TEXT(elapsedMs < 500, "a 1 ms timeout took longer than 500 ms");
    CHECK_TRUE(rc == 0 || errno == EAGAIN || errno == ETIMEDOUT);
    note("returned_ok", rc == 0);
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
