/*
 * IavRefOverlay.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, overlay class: blends a picture into an encoded stream with the real ioctls. The colour
 * table and the pixels are written into the mapped overlay partition (aligned accesses only, IavRefCommon.hxx),
 * IAV_IOC_SET_OVERLAY_INSERT names them by offset, and the video DSP blends them into the stream. The picture is a
 * solid white box with a black border, drawn with an 8-bit colour table (index 0 transparent, 1 white, 2 black).
 * A colour table entry is four bytes in the order V, U, Y, alpha.
 *
 *   IavRefOverlay [--stream <id>] [--x <n>] [--y <n>] [--w <n>] [--h <n>] [--settle-ms <ms>]
 *                 --before <file> --after <file>
 *   IavRefOverlay --stream <id> --probe bad
 *   IavRefOverlay [--stream <id>] [--x ..] [--y ..] [--w ..] [--h ..] --no-frames --hold-ms <ms>
 *
 * With --no-frames the application reads no frames (a Dom0 reader may own the stream's frame descriptors, so a guest
 * cannot read them): it sets the overlay, holds it for --hold-ms, and disables it, so that something else can look at
 * the stream meanwhile.
 *
 * It saves one frame of the stream before the overlay and one after it was set, so the pictures can be compared
 * outside the application. The frame queue of a stream holds the frames made since its last reader, so a frame is
 * only fresh once the queue has drained: the application reads for `--settle-ms` (default 1500) and keeps the last
 * frame, which arrives in real time (the box is there, the rest of the frame is not changed), and it
 * reads the overlay back with IAV_IOC_GET_OVERLAY_INSERT. The overlay is always disabled again before exit. The
 * stream must be MJPEG so that a frame is a picture on its own.
 *
 * `--probe bad` is a negative control for the primary guest: overlays whose colour table or pixels lie outside the
 * overlay partition, wrap around, or are shorter than their pitch times height, and a canvas overlay, must be
 * refused with EINVAL or EPERM before the driver sees them; then a good overlay must still be accepted.
 *
 * Exit 0 when every step held.
 */

#include "IavRefCommon.hxx"

using namespace iavref;

namespace {

const unsigned int ClutBytes = 1024;
const unsigned int ClutEntries = 256;

/* One frame of the stream, copied to a file. Returns the frame's size, or -1. */
long grabFrame(Device &dev, const Mapping &bsb, unsigned long bsbLength, long stream, const char *name,
               unsigned int *width, unsigned int *height)
{
    struct iav_querydesc q;
    long tries = 0;

    for (;;) {
        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_FRAME;
        q.arg.frame.id = (u32)stream;
        q.arg.frame.time_ms = 2000;
        if (dev.call(IAV_IOC_QUERY_DESC, &q, "IAV_IOC_QUERY_DESC", true) == 0) {
            break;
        }
        if (errno != EAGAIN || ++tries >= 20) {
            fprintf(stderr, "IAV_IOC_QUERY_DESC: %s\n", strerror(errno));
            return -1;
        }
    }
    const struct iav_framedesc &f = q.arg.frame;

    if (f.size == 0 || f.data_addr_offset + f.size > bsbLength * 2) {
        fprintf(stderr, "frame outside the bitstream mapping\n");
        return -1;
    }
    if (f.stream_type != IAV_STREAM_TYPE_MJPEG) {
        fprintf(stderr, "stream %ld is not MJPEG (type %u)\n", stream, (unsigned int)f.stream_type);
        return -1;
    }
    *width = f.reso.width;
    *height = f.reso.height;
    if (name != NULL) {
        FILE *out = fopen(name, "wb");
        unsigned char buf[65536];
        size_t off = 0;

        if (out == NULL) {
            fprintf(stderr, "open %s: %s\n", name, strerror(errno));
            return -1;
        }
        while (off < f.size) {
            size_t c = f.size - off < sizeof(buf) ? f.size - off : sizeof(buf);

            windowCopy(buf, bsb.bytes() + f.data_addr_offset + off, c);
            if (fwrite(buf, 1, c, out) != c) {
                fprintf(stderr, "write %s: %s\n", name, strerror(errno));
                fclose(out);
                return -1;
            }
            off += c;
        }
        fclose(out);
    }
    return (long)f.size;
}

/* Read frames for settleMs of wall time and keep the last one in the file: after the queue has drained it is fresh. */
long grabFresh(Device &dev, const Mapping &bsb, unsigned long bsbLength, long stream, const char *name, long settleMs,
               unsigned int *width, unsigned int *height)
{
    const long long end = nowNs() + (long long)settleMs * 1000000LL;
    long size = -1;

    do {
        size = grabFrame(dev, bsb, bsbLength, stream, name, width, height);
        if (size < 0) {
            return -1;
        }
    } while (nowNs() < end);
    return size;
}

void fillArea(struct iav_overlay_area &a, unsigned int x, unsigned int y, unsigned int w, unsigned int h,
              unsigned int pitch, unsigned long clutOff, unsigned long dataOff, unsigned int total)
{
    memset(&a, 0, sizeof(a));
    a.enable = 1;
    a.start_x = (u16)x;
    a.start_y = (u16)y;
    a.width = (u16)w;
    a.pitch = (u16)pitch;
    a.height = (u16)h;
    a.total_size = total;
    a.clut_addr_offset = clutOff;
    a.data_addr_offset = dataOff;
}

void fillInsert(struct iav_overlay_insert &o, long stream, const struct iav_overlay_area &a)
{
    memset(&o, 0, sizeof(o));
    o.id = (u32)stream;
    o.enable = 1;
    o.osd_insert_always = 1;
    o.osd_num_regions = 1;
    o.area[0] = a;
}

/* The refusals a primary guest must get. Returns the number of overlays that were not refused as expected. */
int probeBad(Device &dev, long stream, unsigned long len)
{
    struct iav_overlay_insert o;
    struct iav_overlay_area a;
    int wrong = 0;
    struct Case {
        const char *name;
        unsigned long clut;
        unsigned long data;
        unsigned int total;
        int canvas;
    } cases[] = {
        { "pixels past the end", 0, len - 4096, 128 * 64 + 4096, 0 },
        { "pixels start outside", 0, len, 128 * 64, 0 },
        { "colour table past the end", len - 512, 4096, 128 * 64, 0 },
        { "offset wraps", 0, ~0UL - 100, 128 * 64, 0 },
        { "total_size short of pitch x height", 0, len - 4096, 16, 0 },
        { "canvas overlay", 0, 4096, 128 * 64, 1 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc;

        fillArea(a, 64, 64, 128, 64, 128, cases[i].clut, cases[i].data, cases[i].total);
        fillInsert(o, stream, a);
        o.id_for_canvas = (u32)cases[i].canvas;
        rc = dev.call(IAV_IOC_SET_OVERLAY_INSERT, &o, "SET_OVERLAY_INSERT", true);
        Row("probe").add("case", cases[i].name).add("rc", (long long)rc).add("errno", (long long)(rc < 0 ? errno : 0))
            .print();
        if (rc == 0 || (errno != EINVAL && errno != EPERM)) {
            wrong++;
        }
    }
    return wrong;
}

} // namespace

int main(int argc, char **argv)
{
    const long stream = argValue(argc, argv, "--stream", 1);
    const unsigned int x = (unsigned int)argValue(argc, argv, "--x", 64);
    const unsigned int y = (unsigned int)argValue(argc, argv, "--y", 64);
    const unsigned int w = (unsigned int)argValue(argc, argv, "--w", 128);
    const unsigned int h = (unsigned int)argValue(argc, argv, "--h", 64);
    const long settleMs = argValue(argc, argv, "--settle-ms", 1500);
    const long holdMs = argValue(argc, argv, "--hold-ms", 0);
    bool noFrames = false;
    const char *beforeName = argString(argc, argv, "--before", NULL);
    const char *afterName = argString(argc, argv, "--after", NULL);
    const char *probe = argString(argc, argv, "--probe", NULL);
    /* The buffer pitch is the width rounded up to the DSP's 128-byte line alignment (8 bits per pixel). */
    const unsigned int pitch = (w + 127) & ~127u;
    const unsigned long clutOff = 0;
    const unsigned long dataOff = 4096;
    const unsigned int total = pitch * h;
    Device dev;
    Mapping bsb, ovl;
    unsigned long bsbAddr = 0, bsbLen = 0, ovlAddr = 0, ovlLen = 0;
    struct iav_overlay_insert o, back;
    struct iav_overlay_area a;
    unsigned char clut[ClutBytes];
    unsigned char *pix;
    unsigned int fw = 0, fh = 0, i, j;
    long sizeBefore, sizeAfter = -1;
    int failed = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-frames") == 0) {
            noFrames = true;
        }
    }
    if (!dev.open() || dev.partition(IAV_PART_BSB, &bsbAddr, &bsbLen) != 0 || bsbLen == 0) {
        fprintf(stderr, "no bitstream partition\n");
        return 1;
    }
    if (dev.partition(IAV_PART_OVERLAY, &ovlAddr, &ovlLen) != 0 || ovlLen == 0) {
        fprintf(stderr, "no overlay partition\n");
        return 1;
    }
    if (!bsb.map(dev.fd(), bsbAddr, bsbLen * 2, PROT_READ) || !ovl.map(dev.fd(), ovlAddr, ovlLen, PROT_READ | PROT_WRITE)) {
        return 1;
    }
    Row("overlay_partition").add("length", (long long)ovlLen).print();

    if (probe != NULL && strcmp(probe, "bad") == 0) {
        int wrong = probeBad(dev, stream, ovlLen);

        fillArea(a, x, y, w, h, pitch, clutOff, dataOff, total);
        fillInsert(o, stream, a);
        if (dev.call(IAV_IOC_SET_OVERLAY_INSERT, &o, "SET_OVERLAY_INSERT (good)") != 0) {
            wrong++;
        }
        memset(&o, 0, sizeof(o));
        o.id = (u32)stream;
        (void)dev.call(IAV_IOC_SET_OVERLAY_INSERT, &o, "SET_OVERLAY_INSERT (disable)");
        Row("summary").add("probe", "bad").add("wrong", (long long)wrong).print();
        return wrong == 0 ? 0 : 1;
    }

    /* The frame before the overlay. */
    if (!noFrames) {
        sizeBefore = grabFresh(dev, bsb, bsbLen, stream, beforeName, settleMs, &fw, &fh);
        if (sizeBefore < 0) {
            return 1;
        }
        Row("frame").add("when", "before").add("size", (long long)sizeBefore).add("width", (long long)fw)
            .add("height", (long long)fh).print();
        if (x + w > fw || y + h > fh) {
            fprintf(stderr, "the box (%u,%u %ux%u) does not fit the %ux%u frame\n", x, y, w, h, fw, fh);
            return 1;
        }
    }

    /* The colour table: index 0 transparent, 1 opaque white, 2 opaque black; entries are V, U, Y, alpha. */
    memset(clut, 0, sizeof(clut));
    clut[1 * 4 + 0] = 128;
    clut[1 * 4 + 1] = 128;
    clut[1 * 4 + 2] = 235;
    clut[1 * 4 + 3] = 255;
    clut[2 * 4 + 0] = 128;
    clut[2 * 4 + 1] = 128;
    clut[2 * 4 + 2] = 16;
    clut[2 * 4 + 3] = 255;
    (void)ClutEntries;
    pix = (unsigned char *)calloc(total, 1);
    if (pix == NULL) {
        return 1;
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            pix[j * pitch + i] = (i < 4 || j < 4 || i >= w - 4 || j >= h - 4) ? 2 : 1;
        }
    }
    windowWrite((volatile unsigned char *)ovl.bytes() + clutOff, clut, sizeof(clut));
    windowWrite((volatile unsigned char *)ovl.bytes() + dataOff, pix, total);
    {
        /* The window must hold exactly what was written: the first ground truth, before the DSP reads it. */
        unsigned char *rb = (unsigned char *)malloc(total);
        unsigned char cb[ClutBytes];
        bool same;

        if (rb == NULL) {
            return 1;
        }
        windowCopy(rb, ovl.bytes() + dataOff, total);
        windowCopy(cb, ovl.bytes() + clutOff, sizeof(cb));
        same = memcmp(rb, pix, total) == 0 && memcmp(cb, clut, sizeof(cb)) == 0;
        Row("window_readback").add("bytes", (long long)(total + sizeof(cb))).add("same", (long long)same).print();
        if (!same) {
            failed = 1;
        }
        free(rb);
    }

    fillArea(a, x, y, w, h, pitch, clutOff, dataOff, total);
    fillInsert(o, stream, a);
    if (dev.call(IAV_IOC_SET_OVERLAY_INSERT, &o, "IAV_IOC_SET_OVERLAY_INSERT") != 0) {
        free(pix);
        return 1;
    }
    memset(&back, 0, sizeof(back));
    back.id = (u32)stream;
    if (dev.call(IAV_IOC_GET_OVERLAY_INSERT, &back, "IAV_IOC_GET_OVERLAY_INSERT") == 0) {
        Row("get_overlay").add("enable", (long long)back.enable).add("x", (long long)back.area[0].start_x)
            .add("y", (long long)back.area[0].start_y).add("w", (long long)back.area[0].width)
            .add("h", (long long)back.area[0].height).add("pitch", (long long)back.area[0].pitch)
            .add("data_offset", (long long)back.area[0].data_addr_offset).print();
        if (!back.enable || back.area[0].start_x != x || back.area[0].start_y != y || back.area[0].width != w ||
            back.area[0].height != h || back.area[0].data_addr_offset != dataOff) {
            failed = 1;
        }
    } else {
        failed = 1;
    }

    /* The frame after, once the queue has drained and the overlay has reached the encoder. */
    if (noFrames) {
        Row("hold").add("ms", (long long)holdMs).print();
        usleep((useconds_t)holdMs * 1000);
    } else {
        sizeAfter = grabFresh(dev, bsb, bsbLen, stream, afterName, settleMs, &fw, &fh);
        if (sizeAfter < 0) {
            failed = 1;
        } else {
            Row("frame").add("when", "after").add("size", (long long)sizeAfter).add("width", (long long)fw)
                .add("height", (long long)fh).print();
        }
    }

    /* Always take the overlay off again. */
    memset(&o, 0, sizeof(o));
    o.id = (u32)stream;
    if (dev.call(IAV_IOC_SET_OVERLAY_INSERT, &o, "IAV_IOC_SET_OVERLAY_INSERT (disable)") != 0) {
        failed = 1;
    }
    free(pix);
    Row("summary").add("box_x", (long long)x).add("box_y", (long long)y).add("box_w", (long long)w)
        .add("box_h", (long long)h).add("failed", (long long)failed).print();
    return failed ? 1 : 0;
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
