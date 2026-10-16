/*
 * IavRefScale.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, scale class: decodes a JPEG with the video DSP's hardware decoder and scales the decoded
 * picture with the DSP image scaler (IAV_IOC_SET_IMG_SCALE), input and output in the decode partition, through the
 * real ioctls. The layout is the demo's (plan_iav_usb_yolo, image inventory section 5): the JPEG at offset 0, the
 * decoder's output region [3 MiB, 6 MiB), a 640x640 NV12 output buffer at 0x100000 (Y) and 0x164000 (UV). The scaled
 * picture goes into the output window {0, 0, 640, 360}, the top-left letterbox of a 16:9 frame; the rest of the buffer
 * is the pad, which this program fills once with gray (Y 114, UV 128) and then checks is untouched after every scale.
 * Window access uses aligned accesses of at most 8 bytes (IavRefCommon.hxx).
 *
 *   IavRefScale --jpeg <file> [--repeat <n>] [--mode block|wait] [--out <file>]
 *   IavRefScale --jpeg <file> --probe bad          (in a guest only)
 *
 * Each pass prints the decoder's reported Y and UV offsets, the scale time and the hashes of the scaled window and of
 * the pad. --mode wait issues the scale non-blocking and then waits for its task with IAV_IOC_WAIT_IMG_SCALE. --out
 * writes the first scaled window as compact NV12 (640x360). Exit 0 when every pass scaled and left the pad as it was.
 *
 * `--probe bad` is the negative control for the primary guest: a physical address, a dma-buf fd, another partition,
 * offsets and planes outside the decode window (including the input UV plane the driver checks with the Y offset),
 * window sums that wrap, and the same for GDMA_COPY and SYNC_CACHE_MEM_PART. Each must be refused with the expected
 * errno before the driver sees it; then one good scale must still succeed. It refuses to run on the bare board, where
 * those calls would reach the driver.
 */

#include "IavRefCommon.hxx"

#include <algorithm>
#include <vector>

using namespace iavref;

namespace {

const unsigned long OutY = 0x100000;
const unsigned int OutPitch = 640;
const unsigned int OutHeight = 640;
const unsigned long OutUv = OutY + (unsigned long)OutPitch * OutHeight;
const unsigned int WinW = 640;
const unsigned int WinH = 360;
const unsigned char PadY = 114;
const unsigned char PadUv = 128;

bool readFile(const char *name, std::vector<unsigned char> &out)
{
    FILE *f = fopen(name, "rb");
    unsigned char buf[65536];
    size_t n;

    if (f == NULL) {
        fprintf(stderr, "open %s: %s\n", name, strerror(errno));
        return false;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    fclose(f);
    return !out.empty();
}

int jpegSize(const std::vector<unsigned char> &d, unsigned int *w, unsigned int *h)
{
    size_t i = 2;

    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return -1;
    }
    while (i + 9 < d.size()) {
        unsigned char m;
        size_t len;

        if (d[i] != 0xFF) {
            return -1;
        }
        m = d[i + 1];
        len = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            *h = ((unsigned int)d[i + 5] << 8) | d[i + 6];
            *w = ((unsigned int)d[i + 7] << 8) | d[i + 8];
            return 0;
        }
        if (m == 0xDA || len < 2) {
            return -1;
        }
        i += 2 + len;
    }
    return -1;
}

double percentile(std::vector<double> v, double p)
{
    std::sort(v.begin(), v.end());
    return v[(size_t)((double)(v.size() - 1) * p)];
}

/* Fill [off, off + n) of the window with one byte value, 8-byte stores where aligned. */
void windowFill(volatile unsigned char *win, unsigned long off, size_t n, unsigned char v)
{
    unsigned char buf[4096];
    size_t done = 0;

    memset(buf, v, sizeof(buf));
    while (done < n) {
        size_t c = std::min(sizeof(buf), n - done);

        windowWrite(win + off + done, buf, c);
        done += c;
    }
}

/* True if every byte of [off, off + n) is v. */
bool windowIs(const volatile unsigned char *win, unsigned long off, size_t n, unsigned char v)
{
    unsigned char buf[4096];
    size_t done = 0;

    while (done < n) {
        size_t c = std::min(sizeof(buf), n - done);

        windowCopy(buf, win + off + done, c);
        for (size_t i = 0; i < c; i++) {
            if (buf[i] != v) {
                return false;
            }
        }
        done += c;
    }
    return true;
}

/* The pad of the output buffer: Y rows WinH.. and UV rows WinH/2.., and the columns right of the window. */
bool padIntact(const volatile unsigned char *win)
{
    for (unsigned int r = 0; r < WinH; r++) {
        if (!windowIs(win, OutY + (unsigned long)r * OutPitch + WinW, OutPitch - WinW, PadY)) {
            return false;
        }
    }
    if (!windowIs(win, OutY + (unsigned long)WinH * OutPitch, (size_t)OutPitch * (OutHeight - WinH), PadY)) {
        return false;
    }
    return windowIs(win, OutUv + (unsigned long)(WinH / 2) * OutPitch, (size_t)OutPitch * (OutHeight / 2 - WinH / 2),
                    PadUv);
}

struct iav_img_scale scaleArg(const struct iav_yuv_cap &y)
{
    struct iav_img_scale s;

    memset(&s, 0, sizeof(s));
    s.format = IAV_YUV_FORMAT_YUV420;
    s.input.x = 0;
    s.input.y = 0;
    s.input.width = y.width;
    s.input.height = y.height;
    s.output.x = 0;
    s.output.y = 0;
    s.output.width = WinW;
    s.output.height = WinH;
    s.input_buf.pid = IAV_PART_DEC_BSB;
    s.input_buf.y_offset = y.y_addr_offset;
    s.input_buf.uv_offset = y.uv_addr_offset;
    s.input_buf.pitch = (u16)y.pitch;
    s.input_buf.height = (u16)y.height;
    s.output_buf.pid = IAV_PART_DEC_BSB;
    s.output_buf.y_offset = OutY;
    s.output_buf.uv_offset = OutUv;
    s.output_buf.pitch = OutPitch;
    s.output_buf.height = OutHeight;
    s.input_skip_cache_sync = 1;
    s.output_skip_cache_sync = 1;
    return s;
}

/* One probe: the call must fail with errno want. */
long probe(Device &dev, unsigned long cmd, void *arg, const char *name, const char *what, int want)
{
    int r = dev.call(cmd, arg, name, true);
    int e = errno;
    bool ok = r != 0 && e == want;

    Row("probe").add("call", name).add("case", what).add("rc", (long long)r).add("errno", (long long)(r != 0 ? e : 0))
        .add("expected_errno", (long long)want).add("refused_as_expected", (long long)(ok ? 1 : 0)).print();
    return ok ? 1 : 0;
}

long runProbes(Device &dev, const struct iav_yuv_cap &y, unsigned long decAddr, unsigned long decLen, long *cases)
{
    struct iav_img_scale s;
    struct iav_gdma_copy g;
    struct iav_sync_cache_mem_part c;
    long ok = 0, n = 0;

    s = scaleArg(y);
    s.input_buf.use_phys = 1;
    s.input_buf.y_addr = decAddr + y.y_addr_offset;
    s.input_buf.uv_addr = decAddr + y.uv_addr_offset;
    ok += probe(dev, IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE", "input_physical_address", EPERM); n++;
    s = scaleArg(y);
    s.output_buf.use_phys = 1;
    s.output_buf.y_addr = decAddr + OutY;
    s.output_buf.uv_addr = decAddr + OutUv;
    ok += probe(dev, IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE", "output_physical_address", EPERM); n++;
    s = scaleArg(y);
    s.input_buf.use_dma_buf_fd = 1;
    s.input_buf.dma_buf_fd = dev.fd();
    ok += probe(dev, IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE", "input_dma_buf_fd", EPERM); n++;
    s = scaleArg(y);
    s.output_buf.pid = IAV_PART_DSP;
    ok += probe(dev, IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE", "output_in_dsp_partition", EPERM); n++;
    s = scaleArg(y);
    s.output_buf.y_offset = decLen;
    s.output_buf.uv_offset = decLen + (unsigned long)OutPitch * OutHeight;
    ok += probe(dev, IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE", "output_past_window", EINVAL); n++;
    s = scaleArg(y);
    s.input_buf.uv_offset = decLen - (unsigned long)y.pitch * (y.height / 2) + y.pitch;
    ok += probe(dev, IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE", "input_uv_past_window", EINVAL); n++;
    s = scaleArg(y);
    s.output.x = 0xfffffffcu;
    s.output.width = 8;
    ok += probe(dev, IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE", "output_window_sum_wraps", EINVAL); n++;
    s = scaleArg(y);
    s.input.x = 0xfffffffeu;
    s.input.width = 4;
    ok += probe(dev, IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE", "input_window_sum_wraps", EINVAL); n++;

    memset(&g, 0, sizeof(g));
    g.src_mmap_type = IAV_PART_DEC_BSB;
    g.dst_mmap_type = IAV_PART_DSP;
    g.width = 64;
    g.height = 1;
    g.src_pitch = 64;
    g.dst_pitch = 64;
    ok += probe(dev, IAV_IOC_GDMA_COPY, &g, "IAV_IOC_GDMA_COPY", "into_dsp_partition", EPERM); n++;
    g.dst_mmap_type = IAV_PART_DEC_BSB;
    g.dst_offset = 0x200000;
    g.src_use_phys = 1;
    g.src_addr = decAddr;
    ok += probe(dev, IAV_IOC_GDMA_COPY, &g, "IAV_IOC_GDMA_COPY", "source_physical_address", EPERM); n++;
    g.src_use_phys = 0;
    g.src_offset = decLen - 32;
    ok += probe(dev, IAV_IOC_GDMA_COPY, &g, "IAV_IOC_GDMA_COPY", "source_past_window", EINVAL); n++;
    memset(&c, 0, sizeof(c));
    c.pid = IAV_PART_DSP;
    c.opt = IAV_CACHE_INVALIDATE;
    c.length = 4096;
    ok += probe(dev, IAV_IOC_SYNC_CACHE_MEM_PART, &c, "IAV_IOC_SYNC_CACHE_MEM_PART", "invalidate_dsp_partition", EPERM); n++;
    *cases = n;
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    const char *jpegName = argString(argc, argv, "--jpeg", NULL);
    const char *outName = argString(argc, argv, "--out", NULL);
    const bool waitMode = strcmp(argString(argc, argv, "--mode", "block"), "wait") == 0;
    const bool probeBad = strcmp(argString(argc, argv, "--probe", ""), "bad") == 0;
    const long repeat = probeBad ? 1 : argValue(argc, argv, "--repeat", 1);
    Device dev;
    Mapping part;
    std::vector<unsigned char> jpeg;
    unsigned long addr = 0, length = 0;
    unsigned int w = 0, h = 0;
    struct iav_decode_mode_config mode;
    struct iav_decoder_info info;
    std::vector<double> decodeUs, scaleUs;
    long bad = 0, padBroken = 0, probeFail = 0;
    bool entered = false, created = false;
    int rc = 1;

    if (jpegName == NULL || !readFile(jpegName, jpeg) || jpegSize(jpeg, &w, &h) != 0) {
        fprintf(stderr, "usage: IavRefScale --jpeg <baseline JPEG file> [--repeat n] [--mode block|wait] [--out file]\n");
        return 2;
    }
    if (probeBad && strcmp(environment(), "guest") != 0) {
        fprintf(stderr, "--probe bad runs only in a guest: on the bare board the calls would reach the driver\n");
        return 2;
    }
    if (!dev.open() || dev.partition(IAV_PART_DEC_BSB, &addr, &length) != 0) {
        return 1;
    }
    if (length < 0x600000 || jpeg.size() > OutY) {
        fprintf(stderr, "the decode partition (%lu bytes) or the JPEG (%zu bytes) does not fit the layout\n", length,
                jpeg.size());
        return 1;
    }
    if (!part.map(dev.fd(), addr, length, PROT_READ | PROT_WRITE)) {
        return 1;
    }
    volatile unsigned char *win = (volatile unsigned char *)part.bytes();

    windowFill(win, OutY, (size_t)OutPitch * OutHeight, PadY);
    windowFill(win, OutUv, (size_t)OutPitch * OutHeight / 2, PadUv);

    memset(&mode, 0, sizeof(mode));
    mode.num_decoder = 1;
    mode.decoder_type[0] = IAV_DEC_TYPE_JPEG;
    mode.max_width = w;
    mode.max_height = h;
    if (dev.call(IAV_IOC_ENTER_DECODE_MODE, &mode, "IAV_IOC_ENTER_DECODE_MODE") != 0) {
        return 1;
    }
    entered = true;
    memset(&info, 0, sizeof(info));
    info.decoder_id = 0;
    info.decoder_type = IAV_DEC_TYPE_JPEG;
    info.video_width = w;
    info.video_height = h;
    info.bsb_start_offset = 0;
    info.bsb_size = (u32)length;
    if (dev.call(IAV_IOC_CREATE_DECODER, &info, "IAV_IOC_CREATE_DECODER") != 0) {
        goto done;
    }
    created = true;

    for (long pass = 0; pass < repeat; pass++) {
        struct iav_decode_jpeg dj;
        struct iav_decode_status st;
        struct iav_img_scale s;
        long long t0, t1, t2;
        int tries;

        windowWrite(win, &jpeg[0], jpeg.size());
        t0 = nowNs();
        memset(&dj, 0, sizeof(dj));
        dj.decoder_id = 0;
        dj.is_dec_only = 1;
        dj.is_last_frame = 1;
        dj.jpeg_size = (u32)jpeg.size();
        dj.jpeg_start_offset = 0;
        dj.yuv_start_offset = 0x300000;
        dj.yuv_size = (u32)(length - 0x300000);
        if (dev.call(IAV_IOC_DECODE_JPEG, &dj, "IAV_IOC_DECODE_JPEG") != 0) {
            bad++;
            break;
        }
        for (tries = 0; tries < 2000; tries++) {
            memset(&st, 0, sizeof(st));
            st.decoder_id = 0;
            st.non_block_flag = 1;
            if (dev.call(IAV_IOC_QUERY_DECODE_STATUS, &st, "IAV_IOC_QUERY_DECODE_STATUS") != 0) {
                break;
            }
            if (st.decoded_still_pic_info.width != 0 && st.decoded_still_pic_info.height != 0) {
                break;
            }
            usleep(500);
        }
        t1 = nowNs();
        const struct iav_yuv_cap &y = st.decoded_still_pic_info;
        if (tries >= 2000 || y.width != w || y.height != h) {
            fprintf(stderr, "no decoded picture of %ux%u (got %ux%u after %d polls)\n", w, h, y.width, y.height, tries);
            bad++;
            break;
        }
        if (probeBad) {
            long cases = 0, refused = runProbes(dev, y, addr, length, &cases);

            Row("summary").add("probe", "bad").add("cases", (long long)cases).add("refused", (long long)refused).print();
            probeFail = refused == cases ? 0 : 1;
        }
        s = scaleArg(y);
        s.non_block_flag = waitMode ? 1 : 0;
        if (dev.call(IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE") != 0) {
            bad++;
            break;
        }
        if (waitMode) {
            struct iav_wait_img_scale ws;

            memset(&ws, 0, sizeof(ws));
            ws.task_id = s.task_id;
            if (dev.call(IAV_IOC_WAIT_IMG_SCALE, &ws, "IAV_IOC_WAIT_IMG_SCALE") != 0) {
                bad++;
                break;
            }
        }
        t2 = nowNs();
        {
            std::vector<unsigned char> out((size_t)WinW * WinH * 3 / 2);
            unsigned long long yHash, uvHash;
            bool pad = padIntact(win);

            for (unsigned int r = 0; r < WinH; r++) {
                windowCopy(&out[(size_t)r * WinW], win + OutY + (unsigned long)r * OutPitch, WinW);
            }
            for (unsigned int r = 0; r < WinH / 2; r++) {
                windowCopy(&out[(size_t)WinW * WinH + (size_t)r * WinW], win + OutUv + (unsigned long)r * OutPitch, WinW);
            }
            yHash = frameHash(&out[0], (size_t)WinW * WinH);
            uvHash = frameHash(&out[(size_t)WinW * WinH], (size_t)WinW * WinH / 2);
            padBroken += pad ? 0 : 1;
            if (pass == 0 && outName != NULL) {
                FILE *o = fopen(outName, "wb");

                if (o == NULL || fwrite(&out[0], 1, out.size(), o) != out.size()) {
                    fprintf(stderr, "write %s: %s\n", outName, strerror(errno));
                    bad++;
                }
                if (o != NULL) {
                    fclose(o);
                }
            }
            decodeUs.push_back((double)(t1 - t0) / 1000.0);
            scaleUs.push_back((double)(t2 - t1) / 1000.0);
            Row("scale").add("pass", (long long)pass).add("mode", waitMode ? "wait" : "block")
                .add("in_width", (long long)y.width).add("in_height", (long long)y.height)
                .add("in_pitch", (long long)y.pitch).add("in_y_offset", (long long)y.y_addr_offset)
                .add("in_uv_offset", (long long)y.uv_addr_offset).add("task_id", (long long)s.task_id)
                .add("out_width", (long long)WinW).add("out_height", (long long)WinH)
                .add("y_hash", (long long)yHash).add("uv_hash", (long long)uvHash).add("pad_intact", (long long)(pad ? 1 : 0))
                .add("decode_us", decodeUs.back()).add("scale_us", scaleUs.back()).print();
        }
    }
    rc = bad == 0 && padBroken == 0 && probeFail == 0 && (long)scaleUs.size() == repeat ? 0 : 1;
    if (!scaleUs.empty()) {
        Row("summary").add("passes", (long long)scaleUs.size()).add("failed", (long long)bad)
            .add("pad_broken", (long long)padBroken).add("scale_p50_us", percentile(scaleUs, 0.5))
            .add("scale_p99_us", percentile(scaleUs, 0.99)).add("decode_p50_us", percentile(decodeUs, 0.5)).print();
    }

done:
    if (created && dev.callValue(IAV_IOC_DESTROY_DECODER, 0, "IAV_IOC_DESTROY_DECODER") != 0) {
        rc = 1;
    }
    if (entered && dev.callValue(IAV_IOC_LEAVE_DECODE_MODE, 0, "IAV_IOC_LEAVE_DECODE_MODE") != 0) {
        rc = 1;
    }
    return rc;
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
