/*
 * IavRefStream.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, consumer class (read only): reads the encoded frames of one stream through the frame
 * descriptors and the bitstream partition mapping, writes the bitstream to a file, and checks what it read:
 * every frame lies inside the partition, time stamps rise, and an MJPEG frame is a JPEG whose size matches
 * the descriptor. Runs unchanged on the bare board and in a guest.
 *
 *   IavRefStream [--stream <id>] [--frames <n>] [--timeout-ms <ms>] [--out <file>]
 *
 * Exit 0 when the requested number of frames was read and every check held.
 */

#include "IavRefCommon.hxx"

using namespace iavref;

namespace {

/* Width and height from the first SOF marker of a JPEG, or -1. */
int jpegSize(const volatile unsigned char *p, size_t n, unsigned int *w, unsigned int *h)
{
    size_t i = 2;

    while (i + 9 < n) {
        if (p[i] != 0xFF) {
            return -1;
        }
        unsigned char m = p[i + 1];
        size_t len = ((size_t)p[i + 2] << 8) | p[i + 3];

        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            *h = ((unsigned int)p[i + 5] << 8) | p[i + 6];
            *w = ((unsigned int)p[i + 7] << 8) | p[i + 8];
            return 0;
        }
        if (m == 0xDA || len < 2) {
            return -1;
        }
        i += 2 + len;
    }
    return -1;
}

bool jpegEnds(const volatile unsigned char *p, size_t n)
{
    for (size_t i = n; i >= 2 && i + 8 >= n; i--) {
        if (p[i - 2] == 0xFF && p[i - 1] == 0xD9) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    const long stream = argValue(argc, argv, "--stream", 1);
    const long wanted = argValue(argc, argv, "--frames", 30);
    const long timeoutMs = argValue(argc, argv, "--timeout-ms", 2000);
    const char *outName = argString(argc, argv, "--out", NULL);
    Device dev;
    Mapping bsb;
    unsigned long addr = 0, length = 0;
    FILE *out = NULL;
    long got = 0, eagain = 0, badOrder = 0, badJpeg = 0, outside = 0;
    unsigned long long firstPts = 0, lastPts = 0, bytes = 0, firstHash = 0;
    long distinct = 0;
    long long t0 = 0;

    if (!dev.open() || dev.partition(IAV_PART_BSB, &addr, &length) != 0 || length == 0) {
        fprintf(stderr, "no bitstream partition\n");
        return 1;
    }
    /* The bitstream is a ring: a frame may wrap past the end, so the partition is mapped twice back to back. */
    if (!bsb.map(dev.fd(), addr, length * 2, PROT_READ)) {
        return 1;
    }
    if (outName != NULL && (out = fopen(outName, "wb")) == NULL) {
        fprintf(stderr, "open %s: %s\n", outName, strerror(errno));
        return 1;
    }
    t0 = nowNs();
    while (got < wanted) {
        struct iav_querydesc q;
        const struct iav_framedesc &f = q.arg.frame;

        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_FRAME;
        q.arg.frame.id = (u32)stream;
        q.arg.frame.time_ms = (u32)timeoutMs;
        if (dev.call(IAV_IOC_QUERY_DESC, &q, "IAV_IOC_QUERY_DESC", true) != 0) {
            if (errno == EAGAIN && ++eagain < 20) {
                continue;
            }
            fprintf(stderr, "IAV_IOC_QUERY_DESC: %s (after %ld frames)\n", strerror(errno), got);
            return 1;
        }
        if (f.size == 0 || f.data_addr_offset + f.size > length * 2) {
            outside++;
            fprintf(stderr, "frame %ld lies outside the bitstream mapping (offset %lu size %u of %lu)\n", got,
                    f.data_addr_offset, f.size, length * 2);
            return 1;
        }
        const volatile unsigned char *data = bsb.bytes() + f.data_addr_offset;
        unsigned long long hash = frameHash(data, f.size);
        unsigned int w = 0, h = 0;
        int jpeg = -1;

        if (f.stream_type == IAV_STREAM_TYPE_MJPEG) {
            jpeg = (f.size > 4 && data[0] == 0xFF && data[1] == 0xD8 && jpegSize(data, f.size, &w, &h) == 0 &&
                    w == f.reso.width && h == f.reso.height && jpegEnds(data, f.size)) ? 1 : 0;
            if (jpeg != 1) {
                badJpeg++;
            }
        }
        if (got > 0 && f.arm_pts <= lastPts) {
            badOrder++;
        }
        if (got == 0) {
            firstPts = f.arm_pts;
            firstHash = hash;
            distinct = 1;
        } else if (hash != firstHash) {
            distinct = 2;
        }
        lastPts = f.arm_pts;
        bytes += f.size;
        if (out != NULL) {
            unsigned char buf[65536];
            size_t off = 0;

            while (off < f.size) {
                size_t c = f.size - off < sizeof(buf) ? f.size - off : sizeof(buf);

                windowCopy(buf, data + off, c);
                if (fwrite(buf, 1, c, out) != c) {
                    fprintf(stderr, "write %s: %s\n", outName, strerror(errno));
                    return 1;
                }
                off += c;
            }
        }
        Row("frame").add("seq", (long long)got).add("stream", (long long)f.id).add("frame_num", (long long)f.frame_num)
            .add("arm_pts", (long long)f.arm_pts).add("size", (long long)f.size).add("pic_type", (long long)f.pic_type)
            .add("width", (long long)f.reso.width).add("height", (long long)f.reso.height).add("jpeg_ok", (long long)jpeg)
            .add("hash", (long long)hash).print();
        got++;
    }
    if (out != NULL) {
        fclose(out);
    }
    {
        double secs = (double)(nowNs() - t0) / 1e9;

        Row("summary").add("frames", (long long)got).add("bytes", (long long)bytes).add("bad_order", (long long)badOrder)
            .add("bad_jpeg", (long long)badJpeg).add("outside", (long long)outside).add("distinct_content", (long long)distinct)
            .add("pts_span", (long long)(lastPts - firstPts)).add("seconds", secs)
            .add("pts_fps", lastPts > firstPts && got > 1 ? (double)(got - 1) * 90000.0 / (double)(lastPts - firstPts) : 0.0)
            .print();
    }
    if (badOrder != 0 || badJpeg != 0) {
        fprintf(stderr, "checks failed: %ld out-of-order time stamps, %ld bad JPEG frames\n", badOrder, badJpeg);
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
