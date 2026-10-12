/*
 * IavRefDecode.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, decode class: decodes a JPEG with the video DSP's hardware decoder through the real
 * ioctls and reads the decoded frame straight out of the mapped decode partition. The sequence is the one an
 * application uses: ENTER_DECODE_MODE, CREATE_DECODER, the JPEG copied into the partition, DECODE_JPEG, the status
 * polled until the still picture is ready, the frame read from the mapping, DESTROY_DECODER, LEAVE_DECODE_MODE.
 * Writes and reads of the window use aligned accesses only (IavRefCommon.hxx).
 *
 *   IavRefDecode --jpeg <file> [--repeat <n>] [--out <file>] [--readout 0|1]
 *   IavRefDecode --jpeg <file> --probe bad
 *
 * Each pass prints the time to copy the JPEG in, to decode, and to read the frame out, and the hashes of the
 * decoded planes (rows without pitch padding, so a native run and a guest run compare). --out writes the first
 * decoded frame as compact NV12. Exit 0 when every pass decoded a picture of the JPEG's size.
 *
 * `--probe bad` is a negative control for the primary guest: with a decoder created, it issues decode calls whose
 * offsets and lengths lie outside the decode partition, overflow, or name a decoder that does not exist, and expects
 * every one to fail with EINVAL (refused before the driver sees it); then one good decode must still succeed.
 */

#include "IavRefCommon.hxx"

#include <algorithm>
#include <vector>

using namespace iavref;

namespace {

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

/* Width and height from the first SOF marker, or -1. */
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

} // namespace

int main(int argc, char **argv)
{
    const char *jpegName = argString(argc, argv, "--jpeg", NULL);
    const char *outName = argString(argc, argv, "--out", NULL);
    const long repeat = argValue(argc, argv, "--repeat", 1);
    const bool readout = argValue(argc, argv, "--readout", 1) != 0;
    const bool probeBad = strcmp(argString(argc, argv, "--probe", ""), "bad") == 0;
    long bad_ = 0;
    Device dev;
    Mapping part;
    std::vector<unsigned char> jpeg;
    unsigned long addr = 0, length = 0;
    unsigned int w = 0, h = 0;
    struct iav_decode_mode_config mode;
    struct iav_decoder_info info;
    std::vector<double> copyUs, decodeUs, readUs;
    long bad = 0;
    bool entered = false, created = false;
    int rc = 1;

    if (jpegName == NULL || !readFile(jpegName, jpeg) || jpegSize(jpeg, &w, &h) != 0) {
        fprintf(stderr, "usage: IavRefDecode --jpeg <baseline JPEG file>\n");
        return 2;
    }
    if (!dev.open() || dev.partition(IAV_PART_DEC_BSB, &addr, &length) != 0) {
        return 1;
    }
    if (length == 0) {
        fprintf(stderr, "there is no decode partition: the pipeline has no decoder channel\n");
        return 1;
    }
    if (jpeg.size() * 2 > length) {
        fprintf(stderr, "the JPEG (%zu bytes) does not fit the decode partition (%lu bytes)\n", jpeg.size(), length);
        return 1;
    }
    if (!part.map(dev.fd(), addr, length, PROT_READ | PROT_WRITE)) {
        return 1;
    }
    volatile unsigned char *win = (volatile unsigned char *)part.bytes();

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

    if (probeBad) {
        struct iav_decode_jpeg bad[5];
        struct iav_decoder_info big;
        struct iav_decode_wait_eos eos;
        struct iav_decode_video v;
        const unsigned long long huge = 0xfffffffffffffff0ULL;
        long refused = 0, expected = 0;

        memset(bad, 0, sizeof(bad));
        for (int i = 0; i < 5; i++) {
            bad[i].decoder_id = 0;
            bad[i].is_dec_only = 1;
            bad[i].jpeg_size = (u32)jpeg.size();
            bad[i].jpeg_start_offset = 0;
            bad[i].yuv_start_offset = length / 2;
            bad[i].yuv_size = (u32)(length - length / 2);
        }
        bad[0].jpeg_start_offset = (unsigned long)length;          /* input past the end */
        bad[1].yuv_start_offset = (unsigned long)(length - 16);   /* output runs past the end */
        bad[2].jpeg_start_offset = (unsigned long)huge;            /* an offset that wraps */
        bad[3].decoder_id = 200;                                   /* a decoder that cannot exist */
        bad[4].jpeg_size = 0;                                      /* nothing to decode */
        for (int i = 0; i < 5; i++) {
            int r = dev.call(IAV_IOC_DECODE_JPEG, &bad[i], "IAV_IOC_DECODE_JPEG", true);
            int e = errno;

            expected++;
            refused += (r != 0 && e == EINVAL) ? 1 : 0;
            Row("probe").add("call", "IAV_IOC_DECODE_JPEG").add("case", (long long)i).add("rc", (long long)r)
                .add("errno", (long long)(r != 0 ? e : 0)).add("expected_einval", (long long)(r != 0 && e == EINVAL)).print();
        }
        memset(&big, 0, sizeof(big));
        big.decoder_type = IAV_DEC_TYPE_JPEG;
        big.video_width = w;
        big.video_height = h;
        big.bsb_size = (u32)length + 1;
        {
            int r = dev.call(IAV_IOC_CREATE_DECODER, &big, "IAV_IOC_CREATE_DECODER", true);
            int e = errno;

            expected++;
            refused += (r != 0 && e == EINVAL) ? 1 : 0;
            Row("probe").add("call", "IAV_IOC_CREATE_DECODER").add("case", 0LL).add("rc", (long long)r)
                .add("errno", (long long)(r != 0 ? e : 0)).add("expected_einval", (long long)(r != 0 && e == EINVAL)).print();
        }
        memset(&v, 0, sizeof(v));
        v.start_ptr_offset = (unsigned long)length;
        v.end_ptr_offset = (unsigned long)length + 4096;
        {
            int r = dev.call(IAV_IOC_DECODE_VIDEO, &v, "IAV_IOC_DECODE_VIDEO", true);
            int e = errno;

            expected++;
            refused += (r != 0 && e == EINVAL) ? 1 : 0;
            Row("probe").add("call", "IAV_IOC_DECODE_VIDEO").add("case", 0LL).add("rc", (long long)r)
                .add("errno", (long long)(r != 0 ? e : 0)).add("expected_einval", (long long)(r != 0 && e == EINVAL)).print();
        }
        memset(&eos, 0, sizeof(eos));
        eos.decoder_id = 200;
        {
            int r = dev.call(IAV_IOC_WAIT_PLAYBACK_EOS, &eos, "IAV_IOC_WAIT_PLAYBACK_EOS", true);
            int e = errno;

            expected++;
            refused += (r != 0 && e == EINVAL) ? 1 : 0;
            Row("probe").add("call", "IAV_IOC_WAIT_PLAYBACK_EOS").add("case", 0LL).add("rc", (long long)r)
                .add("errno", (long long)(r != 0 ? e : 0)).add("expected_einval", (long long)(r != 0 && e == EINVAL)).print();
        }
        Row("summary").add("probe", "bad").add("cases", (long long)expected).add("refused", (long long)refused).print();
        if (refused != expected) {
            bad_ = 1;
        }
    }
    for (long pass = 0; pass < (probeBad ? 1 : repeat); pass++) {
        struct iav_decode_jpeg dj;
        struct iav_decode_status st;
        long long t0, t1, t2, t3;
        int tries;

        t0 = nowNs();
        windowWrite(win, &jpeg[0], jpeg.size());
        t1 = nowNs();
        memset(&dj, 0, sizeof(dj));
        dj.decoder_id = 0;
        dj.is_dec_only = 1;
        dj.is_last_frame = 1;
        dj.jpeg_size = (u32)jpeg.size();
        dj.jpeg_start_offset = 0;
        dj.yuv_start_offset = length / 2;
        dj.yuv_size = (u32)(length - length / 2);
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
        t2 = nowNs();
        if (tries >= 2000 || st.decoded_still_pic_info.width != w || st.decoded_still_pic_info.height != h) {
            fprintf(stderr, "no decoded picture of %ux%u (got %ux%u after %d polls)\n", w, h,
                    st.decoded_still_pic_info.width, st.decoded_still_pic_info.height, tries);
            bad++;
            break;
        }
        {
            const struct iav_yuv_cap &y = st.decoded_still_pic_info;
            unsigned long long yHash = 0, uvHash = 0;
            std::vector<unsigned char> yuv;

            if (readout) {
                size_t yBytes = (size_t)y.pitch * y.height, uvBytes = (size_t)y.pitch * (y.height / 2);

                if (y.y_addr_offset + yBytes > length || y.uv_addr_offset + uvBytes > length || y.pitch < y.width) {
                    fprintf(stderr, "the decoded picture lies outside the partition\n");
                    bad++;
                    break;
                }
                yuv.resize((size_t)y.width * y.height * 3 / 2);
                for (unsigned int r = 0; r < y.height; r++) {
                    windowCopy(&yuv[(size_t)r * y.width], win + y.y_addr_offset + (size_t)r * y.pitch, y.width);
                }
                for (unsigned int r = 0; r < y.height / 2; r++) {
                    windowCopy(&yuv[(size_t)y.width * y.height + (size_t)r * y.width],
                               win + y.uv_addr_offset + (size_t)r * y.pitch, y.width);
                }
                yHash = frameHash(&yuv[0], (size_t)y.width * y.height);
                uvHash = frameHash(&yuv[(size_t)y.width * y.height], (size_t)y.width * y.height / 2);
                if (pass == 0 && outName != NULL) {
                    FILE *o = fopen(outName, "wb");

                    if (o == NULL || fwrite(&yuv[0], 1, yuv.size(), o) != yuv.size()) {
                        fprintf(stderr, "write %s: %s\n", outName, strerror(errno));
                        bad++;
                    }
                    if (o != NULL) {
                        fclose(o);
                    }
                }
            }
            t3 = nowNs();
            copyUs.push_back((double)(t1 - t0) / 1000.0);
            decodeUs.push_back((double)(t2 - t1) / 1000.0);
            readUs.push_back((double)(t3 - t2) / 1000.0);
            Row("decode").add("pass", (long long)pass).add("width", (long long)y.width).add("height", (long long)y.height)
                .add("pitch", (long long)y.pitch).add("format", (long long)y.format).add("jpeg_bytes", (long long)jpeg.size())
                .add("y_hash", (long long)yHash).add("uv_hash", (long long)uvHash).add("copy_us", copyUs.back())
                .add("decode_us", decodeUs.back()).add("read_us", readUs.back()).print();
        }
    }
    rc = bad == 0 && bad_ == 0 && (long)decodeUs.size() == (probeBad ? 1 : repeat) ? 0 : 1;
    if (!decodeUs.empty()) {
        Row("summary").add("passes", (long long)decodeUs.size()).add("failed", (long long)bad)
            .add("decode_p50_us", percentile(decodeUs, 0.5)).add("decode_p99_us", percentile(decodeUs, 0.99))
            .add("copy_p50_us", percentile(copyUs, 0.5)).add("read_p50_us", percentile(readUs, 0.5)).print();
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
