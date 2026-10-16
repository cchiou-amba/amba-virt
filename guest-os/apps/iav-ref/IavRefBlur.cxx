/*
 * IavRefBlur.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, blur class (plan_iav_usb_yolo Envelope F): privacy blur with the real ioctls. A normal blur
 * names no memory (the driver fills IAV_PART_BLUR, which amba-virt-server allocates at start, and canvas 0 has
 * blur_enable). An arbitrary blur reads a bitmap the application writes into IAV_PART_ARB_BLUR (mapped at the address
 * QUERY_MEMBLOCK reports): here the bits of the left half of the area are set. Observed on N1-655 (Envelope F): the
 * right half, whose bits are clear, is the half blurred; the set bits keep their pixels.
 *
 *   IavRefBlur [--arb] [--stream <id> | --canvas <id>] [--x n --y n --w n --h n] [--strength n] [--settle-ms ms]
 *              [--hold-ms ms]
 *
 * Canvas 0 is measured before, during and after (luma mean and variance of the area, and of each half for --arb);
 * --hold-ms keeps the blur on so the stream encoded from the canvas can be looked at meanwhile. GET_BLUR_INSERT reads
 * the blur back. The blur is always disabled and the bitmap restored before exit. Exit 0 when every call succeeded.
 */

#include "IavRefCommon.hxx"

using namespace iavref;

namespace {

void statsRow(const char *phase, const char *part, const RegionStats &s)
{
    Row("region").add("phase", phase).add("part", part).add("mono_pts", (long long)s.pts).add("mean", s.mean)
        .add("var", s.var).add("hash", (long long)s.hash).print();
}

} // namespace

int main(int argc, char **argv)
{
    const bool arb = argFlag(argc, argv, "--arb");
    const long stream = argValue(argc, argv, "--stream", -1);
    const long canvas = argValue(argc, argv, "--canvas", 0);
    const unsigned int x = (unsigned int)argValue(argc, argv, "--x", 256) & ~1u;
    const unsigned int y = (unsigned int)argValue(argc, argv, "--y", 128) & ~1u;
    const unsigned int w = (unsigned int)argValue(argc, argv, "--w", 512) & ~15u;
    const unsigned int h = (unsigned int)argValue(argc, argv, "--h", 256) & ~1u;
    const unsigned int strength = (unsigned int)argValue(argc, argv, "--strength", 3);
    const long settle = argValue(argc, argv, "--settle-ms", 1500);
    const long hold = argValue(argc, argv, "--hold-ms", 0);
    Device dev;
    Mapping dsp;
    unsigned long dspAddr = 0, dspLen = 0, arbAddr = 0, arbLen = 0;
    RegionStats st[3][3];	/* phase x (whole, left half, right half) */
    const char *phases[3] = { "before", "during", "after" };
    const char *parts[3] = { "area", "left", "right" };
    void *map = MAP_FAILED;
    unsigned char *saved = NULL;
    unsigned int pitch = 0;
    int rcOn, errOn, rcGet, errGet, rcOff, errOff;

    if (!dev.open() || dev.partition(IAV_PART_DSP, &dspAddr, &dspLen) != 0 ||
        !dsp.map(dev.fd(), dspAddr, dspLen, PROT_READ)) {
        return 1;
    }
    memset(st, 0, sizeof(st));
    if (arb) {
        if (dev.partition(IAV_PART_ARB_BLUR, &arbAddr, &arbLen) != 0 || arbLen == 0) {
            fprintf(stderr, "no IAV_PART_ARB_BLUR\n");
            return 1;
        }
        /* one bit per pixel, rows at a pitch that is a multiple of 64 bytes (LPDDR4_ALIGN) */
        pitch = (((w + 7) / 8) + 63) & ~63u;
        if ((unsigned long)pitch * h > arbLen) {
            fprintf(stderr, "the bitmap does not fit IAV_PART_ARB_BLUR\n");
            return 1;
        }
        map = mmap(NULL, arbLen, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)arbAddr);
        if (map == MAP_FAILED) {
            fprintf(stderr, "mmap ARB_BLUR: %s\n", strerror(errno));
            return 1;
        }
        saved = (unsigned char *)malloc((size_t)pitch * h);
        windowCopy(saved, (const volatile unsigned char *)map, (size_t)pitch * h);
        {
            unsigned char *bm = (unsigned char *)calloc(1, (size_t)pitch * h);

            for (unsigned int r = 0; r < h; r++) {
                memset(bm + (size_t)r * pitch, 0xff, w / 16);	/* the left half */
            }
            windowWrite((volatile unsigned char *)map, bm, (size_t)pitch * h);
            free(bm);
        }
    }
    if (canvasRegion(dev, dsp, (unsigned int)canvas, x, y, w, h, settle, &st[0][0]) != 0 ||
        canvasRegion(dev, dsp, (unsigned int)canvas, x, y, w / 2, h, 0, &st[0][1]) != 0 ||
        canvasRegion(dev, dsp, (unsigned int)canvas, x + w / 2, y, w / 2, h, 0, &st[0][2]) != 0) {
        return 1;
    }

    struct iav_blur_insert b;

    memset(&b, 0, sizeof(b));
    if (stream >= 0) {
        b.stream_id = (u8)stream;
    } else {
        b.canvas_id = (u8)canvas;
        b.id_for_canvas = 1;
    }
    b.enable = 1;
    b.type = IAV_BLUR_TYPE_PM;
    b.area[0].enable = 1;
    b.area[0].strength = strength;
    b.area[0].start_x = (u16)x;
    b.area[0].start_y = (u16)y;
    b.area[0].width = (u16)w;
    b.area[0].height = (u16)h;
    if (arb) {
        b.area[0].use_arb_blur = 1;
        b.area[0].arb_blur_pitch = (u16)pitch;
        b.area[0].arb_blur_offset = 0;
    }
    rcOn = dev.call(IAV_IOC_SET_BLUR_INSERT, &b, "IAV_IOC_SET_BLUR_INSERT", true);
    errOn = errno;
    for (int k = 0; k < 3; k++) {
        if (canvasRegion(dev, dsp, (unsigned int)canvas, x + (k == 2 ? w / 2 : 0), y, k ? w / 2 : w, h,
                         k ? 0 : settle, &st[1][k]) != 0) {
            memset(&st[1][k], 0, sizeof(st[1][k]));
        }
    }
    {
        struct iav_blur_insert g;

        memset(&g, 0, sizeof(g));
        g.stream_id = b.stream_id;
        g.id_for_canvas = b.id_for_canvas;
        rcGet = dev.call(IAV_IOC_GET_BLUR_INSERT, &g, "IAV_IOC_GET_BLUR_INSERT", true);
        errGet = errno;
        Row("readback").add("enable", (long long)g.enable).add("area0_enable", (long long)g.area[0].enable)
            .add("area0_width", (long long)g.area[0].width).add("area0_arb", (long long)g.area[0].use_arb_blur).print();
    }
    if (hold > 0) {
        usleep((useconds_t)hold * 1000);
    }
    b.enable = 0;
    rcOff = dev.call(IAV_IOC_SET_BLUR_INSERT, &b, "IAV_IOC_SET_BLUR_INSERT off", true);
    errOff = errno;
    if (map != MAP_FAILED) {
        windowWrite((volatile unsigned char *)map, saved, (size_t)pitch * h);
        munmap(map, arbLen);
        free(saved);
    }
    for (int k = 0; k < 3; k++) {
        if (canvasRegion(dev, dsp, (unsigned int)canvas, x + (k == 2 ? w / 2 : 0), y, k ? w / 2 : w, h,
                         k ? 0 : settle, &st[2][k]) != 0) {
            memset(&st[2][k], 0, sizeof(st[2][k]));
        }
    }
    callRow("call", 0, arb ? "set_blur_insert_arb_on" : "set_blur_insert_on", rcOn, errOn);
    callRow("call", 1, "get_blur_insert", rcGet, errGet);
    callRow("call", 2, "set_blur_insert_off", rcOff, errOff);
    for (int p = 0; p < 3; p++) {
        for (int k = 0; k < 3; k++) {
            statsRow(phases[p], parts[k], st[p][k]);
        }
    }
    Row("blur").add("arb", (long long)arb).add("stream", (long long)stream).add("canvas", (long long)canvas)
        .add("x", (long long)x).add("y", (long long)y).add("w", (long long)w).add("h", (long long)h)
        .add("pitch", (long long)pitch).print();
    return rcOn != 0 || rcOff != 0;
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
