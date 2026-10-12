/*
 * IavRefCapture.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, consumer class (read only): captures YUV frames of canvas 0 through the canvas
 * descriptor and the DSP partition mapping. Each frame is identified by its time stamp (mono_pts) and
 * carries a hash of its Y plane, so a copy of this program running natively on the same board is the
 * witness: frames with the same time stamp must have the same hash.
 *
 *   IavRefCapture [--frames <n>] [--canvas <id>] [--out <file>]
 *
 * Exit 0 when n distinct frames were read and every check held.
 */

#include "IavRefCommon.hxx"

using namespace iavref;

int main(int argc, char **argv)
{
    const long wanted = argValue(argc, argv, "--frames", 24);
    const long canvas = argValue(argc, argv, "--canvas", 0);
    const char *outName = argString(argc, argv, "--out", NULL);
    Device dev;
    Mapping dsp;
    unsigned long addr = 0, length = 0;
    FILE *out = NULL;
    long got = 0, tries = 0, tooBig = 0;
    unsigned long long lastPts = 0, firstHash = 0;
    long distinct = 0;

    if (!dev.open() || dev.partition(IAV_PART_DSP, &addr, &length) != 0 || length == 0) {
        fprintf(stderr, "no DSP partition\n");
        return 1;
    }
    if (!dsp.map(dev.fd(), addr, length, PROT_READ)) {
        return 1;
    }
    if (outName != NULL && (out = fopen(outName, "wb")) == NULL) {
        fprintf(stderr, "open %s: %s\n", outName, strerror(errno));
        return 1;
    }
    while (got < wanted && tries < wanted * 40) {
        struct iav_querydesc q;
        const struct iav_yuv_cap &y = q.arg.canvas.yuv;
        size_t yBytes, uvBytes;

        tries++;
        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_CANVAS;
        q.arg.canvas.canvas_id = (u32)canvas;
        q.arg.canvas.non_block_flag = 0;
        if (dev.call(IAV_IOC_QUERY_DESC, &q, "IAV_IOC_QUERY_DESC", true) != 0) {
            if (errno == EAGAIN) {
                continue;
            }
            fprintf(stderr, "IAV_IOC_QUERY_DESC: %s (after %ld frames)\n", strerror(errno), got);
            return 1;
        }
        yBytes = (size_t)y.pitch * y.height;
        uvBytes = y.format == IAV_YUV_FORMAT_YUV420 ? yBytes / 2 : (y.format == IAV_YUV_FORMAT_YUV422 ? yBytes : 0);
        if (yBytes == 0 || y.y_addr_offset + yBytes > length || y.uv_addr_offset + uvBytes > length) {
            tooBig++;
            continue;
        }
        if (got > 0 && y.mono_pts == lastPts) {
            continue;
        }
        const volatile unsigned char *yp = dsp.bytes() + y.y_addr_offset;
        unsigned long long hash = frameHash(yp, yBytes);

        if (got == 0) {
            firstHash = hash;
            distinct = 1;
        } else if (hash != firstHash) {
            distinct = 2;
        }
        lastPts = y.mono_pts;
        if (out != NULL) {
            unsigned char buf[65536];
            size_t off = 0;

            while (off < yBytes) {
                size_t c = yBytes - off < sizeof(buf) ? yBytes - off : sizeof(buf);

                windowCopy(buf, yp + off, c);
                if (fwrite(buf, 1, c, out) != c) {
                    fprintf(stderr, "write %s: %s\n", outName, strerror(errno));
                    return 1;
                }
                off += c;
            }
        }
        Row("frame").add("seq", (long long)got).add("mono_pts", (long long)y.mono_pts).add("hash", (long long)hash)
            .add("width", (long long)y.width).add("height", (long long)y.height).add("pitch", (long long)y.pitch)
            .add("format", (long long)y.format).print();
        got++;
    }
    if (out != NULL) {
        fclose(out);
    }
    Row("summary").add("frames", (long long)got).add("tries", (long long)tries).add("outside", (long long)tooBig)
        .add("distinct_content", (long long)distinct).print();
    if (got < wanted) {
        fprintf(stderr, "read %ld of %ld frames\n", got, wanted);
        return 1;
    }
    return 0;
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
