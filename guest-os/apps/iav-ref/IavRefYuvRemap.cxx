/*
 * IavRefYuvRemap.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, YUV remap class (plan_iav_usb_yolo Envelope F): a colour table the application writes into
 * IAV_PART_YUV_REMAP (which amba-virt-server allocates at start; mapped at the address QUERY_MEMBLOCK reports), named by
 * SET_YUV_REMAP_INSERT for an area of a stream. The table maps every luma value to one value (--y-value, default 16,
 * black), so the area of the encoded stream turns uniform. The remap is a stream post-process: it shows in the stream,
 * not in the canvas, so --hold-ms keeps it on while something else looks at the stream (the Dom0 frame tap, through a
 * guest demo); the stream must be encoding.
 *
 *   IavRefYuvRemap [--stream <id>] [--x n --y n --w n --h n] [--y-value n] [--hold-ms ms]
 *
 * GET_YUV_REMAP_INSERT reads it back. The remap is always disabled and the table restored before exit.
 */

#include "IavRefCommon.hxx"

using namespace iavref;

int main(int argc, char **argv)
{
    const long stream = argValue(argc, argv, "--stream", 1);
    const unsigned int x = (unsigned int)argValue(argc, argv, "--x", 256) & ~1u;
    const unsigned int y = (unsigned int)argValue(argc, argv, "--y", 128) & ~3u;
    const unsigned int w = (unsigned int)argValue(argc, argv, "--w", 512) & ~31u;
    const unsigned int h = (unsigned int)argValue(argc, argv, "--h", 256) & ~3u;
    const unsigned int yv = (unsigned int)argValue(argc, argv, "--y-value", 16);
    const long hold = argValue(argc, argv, "--hold-ms", 3000);
    const unsigned int clutBytes = 1024;
    Device dev;
    unsigned long addr = 0, len = 0;
    unsigned char saved[1024], clut[1024];
    int rcOn, errOn, rcGet, errGet, rcOff, errOff;

    if (!dev.open() || dev.partition(IAV_PART_YUV_REMAP, &addr, &len) != 0 || len < clutBytes) {
        fprintf(stderr, "no IAV_PART_YUV_REMAP\n");
        return 1;
    }
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)addr);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap YUV_REMAP: %s\n", strerror(errno));
        return 1;
    }
    windowCopy(saved, (const volatile unsigned char *)p, clutBytes);
    /* 256 entries of V, U, Y, alpha: every input luma to yv, chroma neutral */
    for (unsigned int i = 0; i < 256; i++) {
        clut[4 * i + 0] = 128;
        clut[4 * i + 1] = 128;
        clut[4 * i + 2] = (unsigned char)yv;
        clut[4 * i + 3] = 255;
    }
    windowWrite((volatile unsigned char *)p, clut, clutBytes);

    struct iav_yuv_remap r;

    memset(&r, 0, sizeof(r));
    r.id = (u32)stream;
    r.enable = 1;
    r.area[0].enable = 1;
    r.area[0].yuv_remap_en = 0;
    r.area[0].start_x = (u16)x;
    r.area[0].start_y = (u16)y;
    r.area[0].width = (u16)w;
    r.area[0].height = (u16)h;
    r.area[0].clut_addr_offset = 0;
    rcOn = dev.call(IAV_IOC_SET_YUV_REMAP_INSERT, &r, "IAV_IOC_SET_YUV_REMAP_INSERT", true);
    errOn = errno;
    {
        struct iav_yuv_remap g;

        memset(&g, 0, sizeof(g));
        g.id = (u32)stream;
        rcGet = dev.call(IAV_IOC_GET_YUV_REMAP_INSERT, &g, "IAV_IOC_GET_YUV_REMAP_INSERT", true);
        errGet = errno;
        Row("readback").add("enable", (long long)g.enable).add("area0_enable", (long long)g.area[0].enable)
            .add("area0_width", (long long)g.area[0].width).add("area0_clut", (long long)g.area[0].clut_addr_offset)
            .print();
    }
    Row("hold").add("start_ns", nowNs()).add("hold_ms", (long long)hold).print();
    if (hold > 0) {
        usleep((useconds_t)hold * 1000);
    }
    r.enable = 0;
    rcOff = dev.call(IAV_IOC_SET_YUV_REMAP_INSERT, &r, "IAV_IOC_SET_YUV_REMAP_INSERT off", true);
    errOff = errno;
    windowWrite((volatile unsigned char *)p, saved, clutBytes);
    munmap(p, len);
    callRow("call", 0, "set_yuv_remap_insert_on", rcOn, errOn);
    callRow("call", 1, "get_yuv_remap_insert", rcGet, errGet);
    callRow("call", 2, "set_yuv_remap_insert_off", rcOff, errOff);
    Row("remap").add("stream", (long long)stream).add("x", (long long)x).add("y", (long long)y).add("w", (long long)w)
        .add("h", (long long)h).add("y_value", (long long)yv).print();
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
