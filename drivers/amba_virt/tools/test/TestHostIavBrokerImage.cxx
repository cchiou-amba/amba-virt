/*
 * drivers/amba_virt/tools/test/TestHostIavBrokerImage.cxx
 *
 * The image calls served against the existing windows (plan_iav_usb_yolo, Envelope D; inventory class W): one good
 * call of each kind reaches the driver, and every hostile shape is refused before it: a physical address, a dma-buf
 * fd, another partition, an offset or a plane outside the window, sums that wrap, the driver defects the handlers
 * have (the input UV range checked with the Y offset, unbounded shifts, NULL partitions, unbounded feed-table
 * indexes), and a consumer guest. The mock driver judges every argument that reaches it on its own.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

namespace {

const uint32_t Primary = 7;
const uint32_t Consumer = 9;
const uint64_t Dec = MockIavDriver::DecLen;

/* The demo's map (inventory section 5): a 1080p NV12 picture at 0x300000, a 640x640 output at 0x100000. */
struct iav_img_scale goodScale()
{
    struct iav_img_scale s;

    memset(&s, 0, sizeof(s));
    s.format = IAV_YUV_FORMAT_YUV420;
    s.input.width = 1920;
    s.input.height = 1080;
    s.output.width = 640;
    s.output.height = 360;
    s.input_buf.pid = IAV_PART_DEC_BSB;
    s.input_buf.y_offset = 0x300000;
    s.input_buf.uv_offset = 0x300000 + 1920 * 1080;
    s.input_buf.pitch = 1920;
    s.input_buf.height = 1080;
    s.output_buf.pid = IAV_PART_DEC_BSB;
    s.output_buf.y_offset = 0x100000;
    s.output_buf.uv_offset = 0x100000 + 640 * 640;
    s.output_buf.pitch = 640;
    s.output_buf.height = 640;
    s.input_skip_cache_sync = 1;
    s.output_skip_cache_sync = 1;
    return s;
}

struct iav_gdma_copy gdma(uint32_t src, uint64_t srcOff, uint32_t dst, uint64_t dstOff, uint16_t w, uint32_t h,
                          uint16_t pitch)
{
    struct iav_gdma_copy g;

    memset(&g, 0, sizeof(g));
    g.src_mmap_type = src;
    g.src_offset = srcOff;
    g.dst_mmap_type = dst;
    g.dst_offset = dstOff;
    g.width = w;
    g.height = h;
    g.src_pitch = pitch;
    g.dst_pitch = pitch;
    return g;
}

struct iav_sync_cache_mem_part sync(uint32_t pid, int opt, uint64_t off, uint64_t len)
{
    struct iav_sync_cache_mem_part c;

    memset(&c, 0, sizeof(c));
    c.pid = pid;
    c.opt = (enum iav_sync_cache_mem_part_opt)opt;
    c.offset = off;
    c.length = len;
    return c;
}

} // namespace

TEST_GROUP(IavBrokerImage)
{
    IavBrokerHarness *h;
    uint64_t hd;

    void setup() override
    {
        h = new IavBrokerHarness(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
        h->addOverlayWindow();
        hd = h->mustOpen(Primary);
    }

    void teardown() override
    {
        std::vector<std::string> v = h->drv.violations();

        for (size_t i = 0; i < v.size(); i++) {
            FAIL(v[i].c_str());
        }
        delete h;
    }

    int32_t scale(const struct iav_img_scale &s)
    {
        return h->ioctl(Primary, hd, IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)).r.status;
    }

    /* A call refused by the broker: the status, and no driver call made. */
    int32_t refused(unsigned long cmd, const void *arg, uint32_t len)
    {
        unsigned long before = h->drv.ioctlCalls();
        int32_t st = h->ioctl(Primary, hd, cmd, arg, len).r.status;

        UNSIGNED_LONGS_EQUAL_TEXT(before, h->drv.ioctlCalls(), "a refused call reached the driver");
        return st;
    }
};

/* ---- SET_IMG_SCALE / WAIT_IMG_SCALE ---- */

TEST(IavBrokerImage, AScaleInsideTheDecodeWindowReachesTheDriverAndReturnsItsTask)
{
    struct iav_img_scale s = goodScale();
    struct iav_img_scale out;
    struct iav_wait_img_scale w;
    IavBrokerHarness::Result r = h->ioctl(Primary, hd, IAV_IOC_SET_IMG_SCALE, &s, sizeof(s));

    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(0, r.r.rc);
    LONGS_EQUAL(sizeof(out), r.out.size());
    memcpy(&out, &r.out[0], sizeof(out));
    LONGS_EQUAL(77, (long)out.task_id);
    memset(&w, 0, sizeof(w));
    w.task_id = out.task_id;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_WAIT_IMG_SCALE, &w, sizeof(w)).r.status);
    /* 720p input, high-precision crop window, a 4:2:2 and a monochrome output */
    s.input.width = 1280;
    s.input.height = 720;
    s.input_buf.pitch = 1280;
    s.input_buf.height = 720;
    s.input_buf.uv_offset = 0x300000 + 1280 * 720;
    LONGS_EQUAL(0, scale(s));
    s.input_win_format = IAV_INPUT_WIN_FORMAT_U24_8;
    s.input.x = 0x80;
    s.input.width = 1270u << 8;
    s.input.height = 700u << 8;
    LONGS_EQUAL(0, scale(s));
    s = goodScale();
    s.output_format_enable = 1;
    s.output_format = IAV_YUV_FORMAT_YUV422;
    s.output_buf.uv_offset = 0x100000 + 640 * 640;
    LONGS_EQUAL(0, scale(s));
    s.output_format = IAV_YUV_FORMAT_YUV400;
    s.output_buf.uv_offset = 0;
    LONGS_EQUAL(0, scale(s));
}

TEST(IavBrokerImage, AScaleNamingAPhysicalAddressADmaBufFdOrAnotherPartitionIsRefused)
{
    struct iav_img_scale s;

    s = goodScale();
    s.input_buf.use_phys = 1;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.output_buf.use_phys = 1;
    s.output_buf.y_addr = 0x40000000;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.input_buf.use_dma_buf_fd = 1;
    s.input_buf.dma_buf_fd = 3;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.output_buf.use_dma_buf_fd = 1;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.output_buf.pid = IAV_PART_DSP;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.input_buf.pid = IAV_PART_OVERLAY;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
}

TEST(IavBrokerImage, EveryPlaneIsCheckedWithItsOwnOffset)
{
    struct iav_img_scale s;

    /* the driver checks the input UV plane with y_offset: a UV offset past the window must still be refused */
    s = goodScale();
    s.input_buf.uv_offset = Dec;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.input_buf.uv_offset = Dec - 1920 * 540 + 1;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.input_buf.y_offset = Dec - 1920 * 1080 + 128;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.output_buf.y_offset = 0xffffffffffff0000ull;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.output_buf.uv_offset = Dec;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    /* a 4:2:2 output plane is twice the 4:2:0 one */
    s = goodScale();
    s.output_format_enable = 1;
    s.output_format = IAV_YUV_FORMAT_YUV422;
    s.output_buf.y_offset = Dec - 640 * 640 * 2 + 640 * 320;
    s.output_buf.uv_offset = s.output_buf.y_offset + 640 * 640;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    /* an unknown format is not guessed at */
    s = goodScale();
    s.format = (enum iav_yuv_format)7;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
}

TEST(IavBrokerImage, TheLastByteOfTheWindowIsAllowedAndTheNextIsNot)
{
    struct iav_img_scale s = goodScale();

    s.output_buf.y_offset = Dec - 640 * 960;
    s.output_buf.uv_offset = Dec - 640 * 320;
    LONGS_EQUAL(0, scale(s));
    s.output_buf.y_offset += 128;
    s.output_buf.uv_offset += 128;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
}

TEST(IavBrokerImage, PlaneSizesAndWindowSumsDoNotWrap)
{
    struct iav_img_scale s;

    s = goodScale();
    s.input_buf.pitch = 0xff80;
    s.input_buf.height = 0xffff;
    s.input_buf.y_offset = 0;
    s.input_buf.uv_offset = 0;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    /* x + width wraps in 32 bits: the DSP would read at x */
    s = goodScale();
    s.input.x = 0xfffffffeu;
    s.input.width = 4;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.input.y = 0xfffffffcu;
    s.input.height = 8;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.input_win_format = IAV_INPUT_WIN_FORMAT_U24_8;
    s.input.x = 0xffffff00u;
    s.input.width = 0x200;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    /* the DSP writes at output.x: a wrapping sum would put it anywhere */
    s = goodScale();
    s.output.x = 0xfffffffcu;
    s.output.width = 8;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.output.y = 0xfffffffeu;
    s.output.height = 4;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    /* a window the alignment pushes past the buffer: 638 + 3 rounds to 4 */
    s = goodScale();
    s.output.x = 638;
    s.output.width = 1;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.input.x = 3;              /* aligned down to 2, 1917 + 1 rounded up to 1920: 2 + 1920 > 1920 */
    s.input.width = 1917;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.input_win_format = 5;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    s = goodScale();
    s.output_buf.pitch = 0;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
}

TEST(IavBrokerImage, WithoutADecodeWindowNoScaleIsPossible)
{
    IavBrokerHarness g(IAV_PROFILE_CONSUMER, 0, 0, 0, 0, Primary);
    uint64_t gd = g.mustOpen(Primary);
    struct iav_img_scale s = goodScale();

    LONGS_EQUAL(-EPERM, g.ioctl(Primary, gd, IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)).r.status);
    LONGS_EQUAL(0, g.drv.ioctlCalls());
}

/* ---- GDMA_COPY ---- */

TEST(IavBrokerImage, GdmaCopiesFromAnyWindowIntoTheGuestsOwnContent)
{
    struct iav_gdma_copy g;

    g = gdma(IAV_PART_DEC_BSB, 0x196000, IAV_PART_DEC_BSB, 0x200000, 200, 16, 256);
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_GDMA_COPY, &g, sizeof(g)).r.status);
    g = gdma(IAV_PART_DSP, 0, IAV_PART_DEC_BSB, 0x200000, 4096, 1, 4096);
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_GDMA_COPY, &g, sizeof(g)).r.status);
    g = gdma(IAV_PART_BSB, 0, IAV_PART_OVERLAY, 0, 128, 2, 128);
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_GDMA_COPY, &g, sizeof(g)).r.status);
    /* the last byte of the window */
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DEC_BSB, Dec - 4096, 4096, 1, 4096);
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_GDMA_COPY, &g, sizeof(g)).r.status);
}

TEST(IavBrokerImage, GdmaCopiesThatLeaveTheWindowsAreRefused)
{
    struct iav_gdma_copy g;

    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DSP, 0, 64, 1, 64);
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));     /* into the DSP's memory */
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_BSB, 0, 64, 1, 64);
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));     /* into the encoder's bitstream */
    g = gdma(IAV_PART_IMG, 0, IAV_PART_DEC_BSB, 0, 64, 1, 64);
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));     /* from a partition with no window */
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DEC_BSB, 0x200000, 64, 1, 64);
    g.src_use_phys = 1;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));
    g.src_use_phys = 0;
    g.dst_use_phys = 1;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));
    g.dst_use_phys = 0;
    g.src_use_dma_buf_fd = 1;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));
    g.src_use_dma_buf_fd = 0;
    g.dst_use_dma_buf_fd = 1;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DEC_BSB, Dec - 4095, 4096, 1, 4096);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));    /* one byte past the end */
    g = gdma(IAV_PART_DEC_BSB, 0xfffffffffffff000ull, IAV_PART_DEC_BSB, 0, 4096, 1, 4096);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));    /* an offset that wraps */
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DEC_BSB, 0x200000, 4096, 0xffffffffu, 4096);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));    /* pitch x height past the window */
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DEC_BSB, 0x200000, 256, 1, 128);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));    /* width past the pitch */
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DEC_BSB, 0x200000, 0, 1, 128);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));    /* the driver's zero-size NULL path */
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DEC_BSB, 0x200000, 64, 0, 128);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));
    g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_OVERLAY, MockIavDriver::OvlLen - 64, 128, 1, 128);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));    /* past the overlay content */
}

/* ---- SYNC_CACHE_MEM_PART ---- */

TEST(IavBrokerImage, CacheOperationsStayInsideTheWindowsAndInvalidateOnlyTheGuestsOwnContent)
{
    struct iav_sync_cache_mem_part c;

    c = sync(IAV_PART_DEC_BSB, IAV_CACHE_INVALIDATE, 0, Dec);
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)).r.status);
    c = sync(IAV_PART_DSP, IAV_CACHE_CLEAN, 0, 4096);
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)).r.status);
    c = sync(IAV_PART_DSP, IAV_CACHE_INVALIDATE, 0, 4096);
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    c = sync(IAV_PART_BSB, IAV_CACHE_INVALIDATE, 0, 4096);
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    c = sync(IAV_PART_IMG, IAV_CACHE_CLEAN, 0, 4096);
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    c = sync(IAV_PART_DEC_BSB, IAV_CACHE_CLEAN, 1, Dec);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    c = sync(IAV_PART_DEC_BSB, IAV_CACHE_CLEAN, 0, 0);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    c = sync(IAV_PART_DEC_BSB, IAV_CACHE_CLEAN, 0xffffffffffffff00ull, 0x200);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    c = sync(IAV_PART_DEC_BSB, 9, 0, 64);
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
}

/* ---- blur ---- */

TEST(IavBrokerImage, AnEnabledBlurNeedsItsPartitionAndNoGuestData)
{
    struct iav_blur_insert b;
    struct iav_canvas_sync_cfg c;

    memset(&b, 0, sizeof(b));
    b.stream_id = 1;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)).r.status);   /* disabling */
    b.enable = 1;
    b.area[0].enable = 1;
    b.area[0].width = 64;
    b.area[0].height = 64;
    /* the boards have no blur partition: the driver would dereference NULL; refused after one query */
    {
        unsigned long before = h->drv.ioctlCalls();

        LONGS_EQUAL(-EINVAL, h->ioctl(Primary, hd, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)).r.status);
        UNSIGNED_LONGS_EQUAL(before + 1, h->drv.ioctlCalls());
    }
    h->drv.setBlurPartitions(0x100000, 0);
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)).r.status);
    b.type = IAV_BLUR_TYPE_SMOOTH;
    LONGS_EQUAL(-EINVAL, h->ioctl(Primary, hd, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)).r.status);
    b.type = 3;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));
    b.type = IAV_BLUR_TYPE_PM;
    b.area[MAX_NUM_BLUR_AREA - 1].enable = 1;
    b.area[MAX_NUM_BLUR_AREA - 1].use_arb_blur = 1;
    b.area[MAX_NUM_BLUR_AREA - 1].arb_blur_offset = 0x1000;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)));

    /* the same rules in the canvas sync configuration; a canvas overlay has no window */
    memset(&c, 0, sizeof(c));
    c.cid = IAV_CANVAS_SYNC_CFG_BLUR;
    c.arg.blur = b;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)));
    c.arg.blur.area[MAX_NUM_BLUR_AREA - 1].use_arb_blur = 0;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)).r.status);
    h->drv.setBlurPartitions(0, 0);
    LONGS_EQUAL(-EINVAL, h->ioctl(Primary, hd, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)).r.status);
    c.cid = IAV_CANVAS_SYNC_CFG_OVERLAY;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)));
    c.cid = (enum iav_canvas_sync_cfg_id)9;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)));
    memset(&c, 0, sizeof(c));
    c.cid = IAV_CANVAS_SYNC_CFG_POSTP_SCALE;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c)).r.status);
}

TEST(IavBrokerImage, ABlurOfUnknownPartitionStateIsRefused)
{
    struct iav_blur_insert b;

    memset(&b, 0, sizeof(b));
    b.enable = 1;
    b.area[0].enable = 1;
    h->drv.setBlurPartitions(0x100000, 0x100000);
    h->drv.failIoctl(IAV_IOC_QUERY_MEMBLOCK, EIO);
    LONGS_EQUAL(-EINVAL, h->ioctl(Primary, hd, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b)).r.status);
    h->drv.failIoctl(IAV_IOC_QUERY_MEMBLOCK, 0);
}

/* ---- configuration reads and their guards ---- */

TEST(IavBrokerImage, VideoProcessingReadsExceptTheTwoThatCopyInsidePartitionsWithoutAWindow)
{
    struct iav_video_proc v;

    memset(&v, 0, sizeof(v));
    v.cid = IAV_VIDEO_PROC_MASK;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_GET_VIDEO_PROC, &v, sizeof(v)).r.status);
    v.cid = IAV_VIDEO_PROC_DPTZ;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_GET_VIDEO_PROC, &v, sizeof(v)).r.status);
    v.cid = IAV_VIDEO_PROC_MULTI_BLEND;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GET_VIDEO_PROC, &v, sizeof(v)));
    v.cid = IAV_VIDEO_PROC_BLEND_WARP;
    LONGS_EQUAL(-EPERM, refused(IAV_IOC_GET_VIDEO_PROC, &v, sizeof(v)));
}

TEST(IavBrokerImage, VinIdspIdsTheDriverShiftsByAreBounded)
{
    struct iav_vin_idsp_config c;

    memset(&c, 0, sizeof(c));
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_GET_VIN_IDSP_CFG, &c, sizeof(c)).r.status);
    c.idsp_cfg.chan_id = 31;
    c.vin_cfg.vsrc_id = 31;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_GET_VIN_IDSP_CFG, &c, sizeof(c)).r.status);
    c.idsp_cfg.chan_id = 32;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GET_VIN_IDSP_CFG, &c, sizeof(c)));
    c.idsp_cfg.chan_id = 0;
    c.vin_cfg.vsrc_id = 33;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_GET_VIN_IDSP_CFG, &c, sizeof(c)));
}

TEST(IavBrokerImage, TheIspNnConfigurationReturnsNoPhysicalAddress)
{
    struct iav_isp_nn_params p, out;
    IavBrokerHarness::Result r;

    memset(&p, 0, sizeof(p));
    p.cfg_type = IAV_ISP_NN_CFG;
    r = h->ioctl(Primary, hd, IAV_IOC_GET_ISP_NN_CFG, &p, sizeof(p));
    LONGS_EQUAL(0, r.r.status);
    LONGS_EQUAL(sizeof(out), r.out.size());
    memcpy(&out, &r.out[0], sizeof(out));
    LONGS_EQUAL(0, out.arg.nn_cfg.fifo_addr);
    for (unsigned int i = 0; i < IAV_ISP_NN_TOTAL_MAX; i++) {
        LONGS_EQUAL(0, (long)out.arg.nn_cfg.nn_info[i].dag_addr_phys_addr);
        LONGS_EQUAL(0, (long)out.arg.nn_cfg.nn_info[i].port_map_phys_addr);
        LONGS_EQUAL(0, (long)out.arg.nn_cfg.nn_info[i].port_pos_map_phys_addr);
    }
}

TEST(IavBrokerImage, EfmCallsNameOnlyStreamsWithAContext)
{
    struct iav_efm_get_pool_info p;
    struct iav_efm_release_all_frame a;

    memset(&p, 0, sizeof(p));
    p.stream_id = 1;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_EFM_GET_POOL_INFO, &p, sizeof(p)).r.status);
    p.stream_id = 3;
    LONGS_EQUAL(-EINVAL, h->ioctl(Primary, hd, IAV_IOC_EFM_GET_POOL_INFO, &p, sizeof(p)).r.status);
    p.stream_id = 0xffffffffu;
    LONGS_EQUAL(-EINVAL, h->ioctl(Primary, hd, IAV_IOC_EFM_GET_POOL_INFO, &p, sizeof(p)).r.status);
    h->drv.setStreams(3, 0x5);
    p.stream_id = 1;
    LONGS_EQUAL(-EINVAL, h->ioctl(Primary, hd, IAV_IOC_EFM_GET_POOL_INFO, &p, sizeof(p)).r.status);
    p.stream_id = 2;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_EFM_GET_POOL_INFO, &p, sizeof(p)).r.status);
    /* the release takes the header's one byte from a legacy guest */
    memset(&a, 0, sizeof(a));
    a.stream_id = 2;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_EFM_RELEASE_ALL_FRAMES, &a, 1).r.status);
    a.stream_id = 1;
    LONGS_EQUAL(-EINVAL, h->ioctl(Primary, hd, IAV_IOC_EFM_RELEASE_ALL_FRAMES, &a, 1).r.status);
    h->drv.failIoctl(IAV_IOC_GET_SYSTEM_RESOURCE, EIO);
    a.stream_id = 2;
    LONGS_EQUAL(-EPERM, h->ioctl(Primary, hd, IAV_IOC_EFM_RELEASE_ALL_FRAMES, &a, 1).r.status);
    h->drv.failIoctl(IAV_IOC_GET_SYSTEM_RESOURCE, 0);
}

TEST(IavBrokerImage, ManualFeedReleasesStayInsideTheDriversFeedTables)
{
    struct iav_pyramiddesc p;
    struct iav_canvasdesc c;
    uint8_t raw[4];

    memset(&p, 0, sizeof(p));
    p.chan_id = CONFIG_AMBARELLA_MAX_CHANNEL_NUM - 1;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_RELEASE_PYRAMID_BUF, &p, sizeof(p)).r.status);
    p.chan_id = CONFIG_AMBARELLA_MAX_CHANNEL_NUM;
    p.seq_num = (u64)-1;        /* release all: the driver would clear past the table */
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_RELEASE_PYRAMID_BUF, &p, sizeof(p)));
    p.chan_id = 255;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_RELEASE_PYRAMID_BUF, &p, sizeof(p)));
    memset(&c, 0, sizeof(c));
    c.canvas_id = IAV_MAX_CANVAS_BUF_NUM - 1;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_RELEASE_CANVAS_BUF, &c, sizeof(c)).r.status);
    c.canvas_id = IAV_MAX_CANVAS_BUF_NUM;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_RELEASE_CANVAS_BUF, &c, sizeof(c)));
    /* RELEASE_RAW_BUF from a legacy guest: the header's 4 bytes, vin_id in the low byte */
    memset(raw, 0, sizeof(raw));
    raw[0] = VIN_CONTROLLER_NUM - 1;
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_RELEASE_RAW_BUF, raw, sizeof(raw)).r.status);
    raw[0] = VIN_CONTROLLER_NUM;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_RELEASE_RAW_BUF, raw, sizeof(raw)));
}

TEST(IavBrokerImage, TheAuditedWireRowsReachTheDriverWhole)
{
    struct amba_virt_iav_hello hl;
    std::vector<uint8_t> v;
    struct iav_rawbufdesc d;
    struct iav_efm_release_all_frame a;
    std::vector<uint8_t> seen;

    memset(&hl, 0, sizeof(hl));
    hl.revision = AMBA_VIRT_IAV_REVISION;
    hl.page_size = 4096;
    hl.features = AMBA_VIRT_IAV_F_SIZE_OVERRIDE;
    hl.max_window_id = 3;
    v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, &hl, sizeof(hl));
    LONGS_EQUAL(0, h->send(Primary, &v[0], v.size()).r.status);

    memset(&d, 0, sizeof(d));
    d.vin_id = 1;
    d.feed_seq_num = 0x1122334455667788ull;
    h->drv.captureArgOf(IAV_IOC_RELEASE_RAW_BUF, sizeof(d));
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_RELEASE_RAW_BUF, &d, sizeof(d)).r.status);
    seen = h->drv.capturedArg();
    LONGS_EQUAL(sizeof(d), seen.size());
    MEMCMP_EQUAL(&d, &seen[0], sizeof(d));
    d.vin_id = VIN_CONTROLLER_NUM;
    LONGS_EQUAL(-EINVAL, refused(IAV_IOC_RELEASE_RAW_BUF, &d, sizeof(d)));

    memset(&a, 0, sizeof(a));
    a.stream_id = 1;
    a.no_prefetch_flag = 1;
    h->drv.captureArgOf(IAV_IOC_EFM_RELEASE_ALL_FRAMES, sizeof(a));
    LONGS_EQUAL(0, h->ioctl(Primary, hd, IAV_IOC_EFM_RELEASE_ALL_FRAMES, &a, sizeof(a)).r.status);
    seen = h->drv.capturedArg();
    LONGS_EQUAL(sizeof(a), seen.size());
    MEMCMP_EQUAL(&a, &seen[0], sizeof(a));
}

/* ---- the consumer, and the set as a whole ---- */

namespace {

const unsigned long ImageSet[] = {
    IAV_IOC_SET_IMG_SCALE,       IAV_IOC_WAIT_IMG_SCALE,          IAV_IOC_GET_VIDEO_PROC,
    IAV_IOC_GET_VIN_IDSP_CFG,    IAV_IOC_WAIT_SKIP_IDSP_CFG,      IAV_IOC_GET_WARP_CTRL,
    IAV_IOC_SET_BLUR_INSERT,     IAV_IOC_GET_BLUR_INSERT,         IAV_IOC_GET_YUV_REMAP_INSERT,
    IAV_IOC_GET_FRAME_SYNC_PROC, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC,
    IAV_IOC_GET_STREAM_CONFIG,   IAV_IOC_QUERY_MEM_LAYOUT_INFO,   IAV_IOC_GDMA_COPY,
    IAV_IOC_SYNC_CACHE_MEM_PART, IAV_IOC_GET_ISP_NN_CFG,          IAV_IOC_EFM_GET_POOL_INFO,
    IAV_IOC_EFM_RELEASE_ALL_FRAMES, IAV_IOC_FEED_CANVAS_BUF,      IAV_IOC_RELEASE_CANVAS_BUF,
    IAV_IOC_FEED_PYRAMID_BUF,    IAV_IOC_RELEASE_PYRAMID_BUF,     IAV_IOC_FEED_RAW_BUF,
    IAV_IOC_RELEASE_RAW_BUF,
};

} // namespace

TEST(IavBrokerImage, TheSetIsTheInventorysClassWAndPrimaryOnly)
{
    LONGS_EQUAL(25, sizeof(ImageSet) / sizeof(ImageSet[0]));
    for (size_t i = 0; i < sizeof(ImageSet) / sizeof(ImageSet[0]); i++) {
        const struct iav_policy_entry *e = iav_policy_lookup((unsigned int)ImageSet[i]);

        CHECK(e != NULL);
        LONGS_EQUAL_TEXT(0, e->profiles & IAV_PROFILE_BIT(IAV_PROFILE_CONSUMER), e->name);
        CHECK_TEXT((e->profiles & IAV_PROFILE_BIT(IAV_PROFILE_PRIMARY)) != 0, e->name);
    }
}

TEST(IavBrokerImage, AConsumerGuestIsRefusedEveryImageCall)
{
    uint64_t cd = h->mustOpen(Consumer);
    uint8_t arg[AMBA_VIRT_MAX_MSG];
    unsigned long before = h->drv.ioctlCalls();

    memset(arg, 0, sizeof(arg));
    for (size_t i = 0; i < sizeof(ImageSet) / sizeof(ImageSet[0]); i++) {
        const struct iav_policy_entry *e = iav_policy_lookup((unsigned int)ImageSet[i]);
        struct iav_img_scale s = goodScale();

        if (e->cmd == IAV_IOC_SET_IMG_SCALE) {
            memcpy(arg, &s, sizeof(s));
        }
        LONGS_EQUAL_TEXT(-EPERM, h->ioctl(Consumer, cd, e->cmd, e->kind == IAV_ARG_PTR ? arg : NULL,
                                          e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W) ? (uint32_t)e->size : 0)
                                     .r.status, e->name);
    }
    UNSIGNED_LONGS_EQUAL(before, h->drv.ioctlCalls());
}

TEST(IavBrokerImage, StaleWindowsStopTheWindowBackedImageCalls)
{
    struct iav_img_scale s = goodScale();
    struct iav_gdma_copy g = gdma(IAV_PART_DEC_BSB, 0, IAV_PART_DEC_BSB, 0x200000, 64, 1, 64);
    struct iav_sync_cache_mem_part c = sync(IAV_PART_DEC_BSB, IAV_CACHE_CLEAN, 0, 64);

    h->b.windows_stale = 1;
    LONGS_EQUAL(-EIO, refused(IAV_IOC_SET_IMG_SCALE, &s, sizeof(s)));
    LONGS_EQUAL(-EIO, refused(IAV_IOC_GDMA_COPY, &g, sizeof(g)));
    LONGS_EQUAL(-EIO, refused(IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c)));
    h->b.windows_stale = 0;
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
