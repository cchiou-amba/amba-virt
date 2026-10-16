/*
 * IavRefImage.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, image configuration class: one well-formed call of each image ioctl that the virtual
 * /dev/iav serves against the existing windows or with scalars only (plan_iav_usb_yolo, image inventory class W),
 * other than the scaler (IavRefScale). Each row carries the call's return code, errno and a 64-bit digest of what it
 * returned, so a run on the bare board and a run in the primary guest compare call by call. The calls are chosen to
 * leave the pipeline as it is: reads, a blur disable on a stream and a canvas that have none, a post-processing scale
 * written back as it was read, a GDMA copy and cache operations inside the decode partition, and manual-feed calls
 * on buffers that are not in manual feed (the driver refuses them the same way on both sides).
 *
 *   IavRefImage [--stream 1] [--canvas 0] [--chan 0]
 *
 * The GDMA row also checks the copy bit for bit: the bytes inside the copied width equal the source pattern, the
 * bytes between the width and the pitch are untouched. The ISP-NN row reports whether the returned configuration
 * carried any physical address (the broker clears them) and digests the configuration without them. Exit 0 when the
 * GDMA copy was exact; the comparison with the other side is the gate's.
 */

#include "IavRefCommon.hxx"

#include <vector>

using namespace iavref;

namespace {

unsigned long long digest(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    unsigned long long h = 1469598103934665603ULL;

    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void report(const char *name, const char *what, int rc, int err, unsigned long long dg, long long extra = -1)
{
    Row r("call");

    r.add("name", name).add("case", what).add("rc", (long long)rc).add("errno", (long long)(rc < 0 ? err : 0))
        .add("digest", (long long)dg);
    if (extra >= 0) {
        r.add("value", extra);
    }
    r.print();
}

/* A pointer call: the raw return value of ioctl (a positive count is kept), and the digest of the argument after it. */
int call(Device &dev, unsigned long cmd, void *arg, size_t n, const char *name, const char *what)
{
    int rc = ioctl(dev.fd(), cmd, arg);
    int e = errno;

    report(name, what, rc, e, rc < 0 ? 0 : digest(arg, n));
    errno = e;
    return rc;
}

} // namespace

int main(int argc, char **argv)
{
    const unsigned int stream = (unsigned int)argValue(argc, argv, "--stream", 1);
    const unsigned int canvas = (unsigned int)argValue(argc, argv, "--canvas", 0);
    const unsigned int chan = (unsigned int)argValue(argc, argv, "--chan", 0);
    Device dev;
    Mapping part;
    unsigned long addr = 0, length = 0;
    int gdmaExact = 0;

    if (!dev.open()) {
        return 1;
    }

    {
        static struct iav_mem_layout_info m;

        memset(&m, 0, sizeof(m));
        call(dev, IAV_IOC_QUERY_MEM_LAYOUT_INFO, &m, sizeof(m), "IAV_IOC_QUERY_MEM_LAYOUT_INFO", "layout");
    }
    {
        struct iav_video_proc v;
        const struct {
            int cid;
            const char *what;
        } cids[] = {{IAV_VIDEO_PROC_DPTZ, "dptz"}, {IAV_VIDEO_PROC_MASK, "mask"},
                    {IAV_VIDEO_PROC_PYRAMID_ROI, "pyramid_roi"}, {IAV_VIDEO_PROC_CANVAS_FPS, "canvas_fps"},
                    {IAV_VIDEO_PROC_VSRC_UPDATE, "vsrc_update"}, {IAV_VIDEO_PROC_CANVAS_ORDER, "canvas_order"},
                    {IAV_VIDEO_PROC_POSTP_SCALE, "postp_scale"}, {IAV_VIDEO_PROC_CAWARP, "cawarp"}};

        for (size_t i = 0; i < sizeof(cids) / sizeof(cids[0]); i++) {
            memset(&v, 0, sizeof(v));
            v.cid = (enum iav_video_proc_id)cids[i].cid;
            switch (cids[i].cid) {
            case IAV_VIDEO_PROC_DPTZ:
                v.arg.dptz.channel_id = (u8)chan;
                break;
            case IAV_VIDEO_PROC_MASK:
                v.arg.mask.channel_id = (u8)chan;
                break;
            case IAV_VIDEO_PROC_PYRAMID_ROI:
                v.arg.pyramid_roi.chan_id = chan;
                break;
            case IAV_VIDEO_PROC_CANVAS_FPS:
                v.arg.canvas_fps.id = canvas;
                break;
            case IAV_VIDEO_PROC_VSRC_UPDATE:
                v.arg.vsrc_update.chan_id = chan;
                break;
            case IAV_VIDEO_PROC_CANVAS_ORDER:
                v.arg.canvas_order.id = canvas;
                break;
            case IAV_VIDEO_PROC_POSTP_SCALE:
                v.arg.postp_scale.canvas_id = canvas;
                break;
            default:
                v.arg.cawarp.chan_id = (u8)chan;
                break;
            }
            call(dev, IAV_IOC_GET_VIDEO_PROC, &v, sizeof(v), "IAV_IOC_GET_VIDEO_PROC", cids[i].what);
        }
    }
    {
        struct iav_vin_idsp_config c;

        memset(&c, 0, sizeof(c));
        c.idsp_cfg.chan_id = chan;
        c.vin_cfg.vsrc_id = 0;
        /* the shutter and gain change with every frame (3A): the gate compares the return code only */
        call(dev, IAV_IOC_GET_VIN_IDSP_CFG, &c, sizeof(c), "IAV_IOC_GET_VIN_IDSP_CFG", "chan_vsrc0");
    }
    {
        int rc = ioctl(dev.fd(), IAV_IOC_WAIT_SKIP_IDSP_CFG, (unsigned long)chan);
        int e = errno;

        /* a positive return is the time left of the driver's bound; the gate compares its sign */
        report("IAV_IOC_WAIT_SKIP_IDSP_CFG", "chan", rc > 0 ? 1 : rc, e, 0, rc);
    }
    {
        struct iav_warp_ctrl wc;

        memset(&wc, 0, sizeof(wc));
        wc.cid = IAV_WARP_CTRL_MAIN;
        wc.channel_id = (u16)chan;
        call(dev, IAV_IOC_GET_WARP_CTRL, &wc, sizeof(wc), "IAV_IOC_GET_WARP_CTRL", "main");
    }
    {
        struct iav_blur_insert b;

        memset(&b, 0, sizeof(b));
        b.stream_id = (u8)stream;
        call(dev, IAV_IOC_GET_BLUR_INSERT, &b, sizeof(b), "IAV_IOC_GET_BLUR_INSERT", "stream");
        if (!b.enable) {
            memset(&b, 0, sizeof(b));
            b.stream_id = (u8)stream;
            call(dev, IAV_IOC_SET_BLUR_INSERT, &b, sizeof(b), "IAV_IOC_SET_BLUR_INSERT", "stream_disable");
        } else {
            report("IAV_IOC_SET_BLUR_INSERT", "stream_disable_skipped_blur_active", 0, 0, 0);
        }
    }
    {
        struct iav_yuv_remap y;

        memset(&y, 0, sizeof(y));
        y.id = stream;
        call(dev, IAV_IOC_GET_YUV_REMAP_INSERT, &y, sizeof(y), "IAV_IOC_GET_YUV_REMAP_INSERT", "stream");
    }
    {
        static struct iav_stream_cfg s;

        memset(&s, 0, sizeof(s));
        s.id = stream;
        s.cid = IAV_STMCFG_FPS;
        call(dev, IAV_IOC_GET_FRAME_SYNC_PROC, &s, sizeof(s), "IAV_IOC_GET_FRAME_SYNC_PROC", "fps");
        memset(&s, 0, sizeof(s));
        s.id = stream;
        s.cid = IAV_STMCFG_FORMAT;
        call(dev, IAV_IOC_GET_STREAM_CONFIG, &s, sizeof(s), "IAV_IOC_GET_STREAM_CONFIG", "format");
        memset(&s, 0, sizeof(s));
        s.id = stream;
        s.cid = IAV_STMCFG_FPS;
        call(dev, IAV_IOC_GET_STREAM_CONFIG, &s, sizeof(s), "IAV_IOC_GET_STREAM_CONFIG", "fps");
    }
    {
        static struct iav_canvas_sync_cfg c;

        memset(&c, 0, sizeof(c));
        c.id = canvas;
        c.cid = IAV_CANVAS_SYNC_CFG_BLUR;
        call(dev, IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c), "IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC", "blur");
        if (!c.arg.blur.enable) {
            memset(&c, 0, sizeof(c));
            c.id = canvas;
            c.cid = IAV_CANVAS_SYNC_CFG_BLUR;
            c.arg.blur.canvas_id = (u8)canvas;
            c.arg.blur.id_for_canvas = 1;
            call(dev, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c), "IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC",
                 "blur_disable");
        } else {
            report("IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC", "blur_disable_skipped_blur_active", 0, 0, 0);
        }
        memset(&c, 0, sizeof(c));
        c.id = canvas;
        c.cid = IAV_CANVAS_SYNC_CFG_POSTP_SCALE;
        if (call(dev, IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c), "IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC",
                 "postp_scale") == 0) {
            c.id = canvas;
            c.cid = IAV_CANVAS_SYNC_CFG_POSTP_SCALE;
            call(dev, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, &c, sizeof(c), "IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC",
                 "postp_scale_as_read");
        }
    }
    {
        struct iav_isp_nn_params p;
        int rc;
        int e;
        long long hadAddress = 0;

        memset(&p, 0, sizeof(p));
        p.cfg_type = IAV_ISP_NN_CFG;
        p.arg.nn_cfg.chan_id = (u8)chan;
        rc = ioctl(dev.fd(), IAV_IOC_GET_ISP_NN_CFG, &p);
        e = errno;
        hadAddress = p.arg.nn_cfg.fifo_addr != 0;
        p.arg.nn_cfg.fifo_addr = 0;
        for (unsigned int i = 0; i < IAV_ISP_NN_TOTAL_MAX; i++) {
            hadAddress |= p.arg.nn_cfg.nn_info[i].dag_addr_phys_addr != 0 ||
                          p.arg.nn_cfg.nn_info[i].port_map_phys_addr != 0 ||
                          p.arg.nn_cfg.nn_info[i].port_pos_map_phys_addr != 0;
            p.arg.nn_cfg.nn_info[i].dag_addr_phys_addr = 0;
            p.arg.nn_cfg.nn_info[i].port_map_phys_addr = 0;
            p.arg.nn_cfg.nn_info[i].port_pos_map_phys_addr = 0;
        }
        report("IAV_IOC_GET_ISP_NN_CFG", "chan_addresses_removed", rc, e, rc < 0 ? 0 : digest(&p, sizeof(p)), hadAddress);
    }
    {
        struct iav_efm_get_pool_info pi;
        struct iav_efm_release_all_frame ra;

        memset(&pi, 0, sizeof(pi));
        pi.stream_id = stream;
        call(dev, IAV_IOC_EFM_GET_POOL_INFO, &pi, sizeof(pi), "IAV_IOC_EFM_GET_POOL_INFO", "stream");
        memset(&ra, 0, sizeof(ra));
        ra.stream_id = (u8)stream;
        call(dev, IAV_IOC_EFM_RELEASE_ALL_FRAMES, &ra, sizeof(ra), "IAV_IOC_EFM_RELEASE_ALL_FRAMES", "stream");
    }
    {
        struct iav_feed_canvas fc;
        struct iav_canvasdesc cd;
        struct iav_feed_pyramid fp;
        struct iav_pyramiddesc pd;
        struct iav_feed_raw fr;
        struct iav_rawbufdesc rd;

        memset(&fc, 0, sizeof(fc));
        fc.canvas_id = canvas;
        call(dev, IAV_IOC_FEED_CANVAS_BUF, &fc, sizeof(fc), "IAV_IOC_FEED_CANVAS_BUF", "not_manual_feed");
        memset(&cd, 0, sizeof(cd));
        cd.canvas_id = canvas;
        cd.feed_seq_num = 0x7fffffffu;      /* a sequence no buffer has: nothing is released */
        cd.yuv.seq_num = 0x7fffffffu;
        call(dev, IAV_IOC_RELEASE_CANVAS_BUF, &cd, sizeof(cd), "IAV_IOC_RELEASE_CANVAS_BUF", "unknown_seq");
        memset(&fp, 0, sizeof(fp));
        fp.chan_id = chan;
        call(dev, IAV_IOC_FEED_PYRAMID_BUF, &fp, sizeof(fp), "IAV_IOC_FEED_PYRAMID_BUF", "not_manual_feed");
        memset(&pd, 0, sizeof(pd));
        pd.chan_id = chan;
        pd.seq_num = 0x7fffffffu;
        call(dev, IAV_IOC_RELEASE_PYRAMID_BUF, &pd, sizeof(pd), "IAV_IOC_RELEASE_PYRAMID_BUF", "unknown_seq");
        memset(&fr, 0, sizeof(fr));
        fr.chan_id = chan;
        call(dev, IAV_IOC_FEED_RAW_BUF, &fr, sizeof(fr), "IAV_IOC_FEED_RAW_BUF", "not_manual_feed");
        memset(&rd, 0, sizeof(rd));
        rd.vin_id = 0;
        rd.feed_seq_num = 0x7fffffffu;
        call(dev, IAV_IOC_RELEASE_RAW_BUF, &rd, sizeof(rd), "IAV_IOC_RELEASE_RAW_BUF", "not_manual_feed");
    }
    {
        struct iav_wait_img_scale ws;

        memset(&ws, 0, sizeof(ws));
        ws.task_id = 0;     /* a task that is already done: returns at once */
        call(dev, IAV_IOC_WAIT_IMG_SCALE, &ws, sizeof(ws), "IAV_IOC_WAIT_IMG_SCALE", "done_task");
    }

    {
        /*
         * One scale of whatever the decoder region holds, inside the decode partition, with no mapping of it: the
         * return code compares (IavRefScale checks the picture). The task id is global to the driver, so the row
         * has no digest.
         */
        struct iav_img_scale s;
        int rc, e;

        memset(&s, 0, sizeof(s));
        s.format = IAV_YUV_FORMAT_YUV420;
        s.input.width = 640;
        s.input.height = 360;
        s.output.width = 320;
        s.output.height = 180;
        s.input_buf.pid = IAV_PART_DEC_BSB;
        s.input_buf.y_offset = 0x300000;
        s.input_buf.uv_offset = 0x300000 + 640 * 360;
        s.input_buf.pitch = 640;
        s.input_buf.height = 360;
        s.output_buf.pid = IAV_PART_DEC_BSB;
        s.output_buf.y_offset = 0x100000;
        s.output_buf.uv_offset = 0x100000 + 384 * 180;
        s.output_buf.pitch = 384;
        s.output_buf.height = 180;
        s.input_skip_cache_sync = 1;
        s.output_skip_cache_sync = 1;
        rc = ioctl(dev.fd(), IAV_IOC_SET_IMG_SCALE, &s);
        e = errno;
        report("IAV_IOC_SET_IMG_SCALE", "decode_region_640x360_to_320x180", rc, e, 0);
    }

    /* GDMA and the cache, inside the decode partition: the range of the demo's map that nothing else uses. */
    if (dev.partition(IAV_PART_DEC_BSB, &addr, &length) != 0 || length < 0x300000 ||
        !part.map(dev.fd(), addr, length, PROT_READ | PROT_WRITE)) {
        report("IAV_IOC_GDMA_COPY", "no_decode_partition", -1, ENODEV, 0);
        return 1;
    }
    {
        volatile unsigned char *win = (volatile unsigned char *)part.bytes();
        const unsigned long src = 0x196000, dst = 0x200000;
        const unsigned int width = 200, height = 16, pitch = 256;
        std::vector<unsigned char> pattern((size_t)pitch * height), zero((size_t)pitch * height, 0), back(pattern.size());
        struct iav_gdma_copy g;
        struct iav_sync_cache_mem_part c;
        int rc, e;

        for (size_t i = 0; i < pattern.size(); i++) {
            pattern[i] = (unsigned char)((i * 7 + 3) ^ (i >> 8));
        }
        windowWrite(win + src, &pattern[0], pattern.size());
        windowWrite(win + dst, &zero[0], zero.size());
        memset(&g, 0, sizeof(g));
        g.src_mmap_type = IAV_PART_DEC_BSB;
        g.dst_mmap_type = IAV_PART_DEC_BSB;
        g.src_offset = src;
        g.dst_offset = dst;
        g.src_pitch = pitch;
        g.dst_pitch = pitch;
        g.width = width;
        g.height = height;
        rc = ioctl(dev.fd(), IAV_IOC_GDMA_COPY, &g);
        e = errno;
        windowCopy(&back[0], win + dst, back.size());
        gdmaExact = rc == 0;
        for (unsigned int r = 0; r < height && gdmaExact; r++) {
            for (unsigned int x = 0; x < pitch; x++) {
                unsigned char want = x < width ? pattern[(size_t)r * pitch + x] : 0;

                if (back[(size_t)r * pitch + x] != want) {
                    gdmaExact = 0;
                    break;
                }
            }
        }
        report("IAV_IOC_GDMA_COPY", "decode_to_decode_2d", rc, e, digest(&back[0], back.size()), gdmaExact);

        memset(&c, 0, sizeof(c));
        c.pid = IAV_PART_DEC_BSB;
        c.offset = dst;
        c.length = 4096;
        c.opt = IAV_CACHE_CLEAN;
        call(dev, IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c), "IAV_IOC_SYNC_CACHE_MEM_PART", "clean_decode");
        c.opt = IAV_CACHE_INVALIDATE;
        call(dev, IAV_IOC_SYNC_CACHE_MEM_PART, &c, sizeof(c), "IAV_IOC_SYNC_CACHE_MEM_PART", "invalidate_decode");
    }
    Row("summary").add("gdma_exact", (long long)gdmaExact).print();
    return gdmaExact ? 0 : 1;
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
