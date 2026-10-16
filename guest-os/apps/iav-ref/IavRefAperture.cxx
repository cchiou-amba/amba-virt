/*
 * IavRefAperture.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, aux aperture class (plan_iav_usb_yolo Envelope F): the partitions an application maps beyond
 * the four legacy windows (MASK, WARP, QPMATRIX, CE_GO_MAP, CANVAS_OVERLAY, ARB_BLUR, YUV_REMAP), and the calls that use
 * them, with the bare-metal pattern only: QUERY_MEMBLOCK, mmap of /dev/iav at the reported address, the real ioctls.
 * The same static binary runs on the bare board and in a guest.
 *
 *   IavRefAperture --map                  map each partition, write a word at its first and last 8 bytes and read it
 *                                         back, restore what was there; a map of twice its length (a ring) is tried
 *                                         and reported; a map of more than twice must fail
 *   IavRefAperture --mask [--x n --y n --w n --h n --y-value n --settle-ms ms]
 *                                         privacy mask a rectangle of channel 0 (written into its MASK slot, applied
 *                                         with CFG_VIDEO_PROC and APPLY_VIDEO_PROC), measure the rectangle on canvas 0
 *                                         before, during and after; the mask and the slot are restored
 *   IavRefAperture --canvas-overlay [--x n --y n --w n --h n --settle-ms ms --hold-ms ms]
 *                                         a white box overlaid on canvas 0 from the CANVAS_OVERLAY partition, measured
 *                                         on canvas 0 before, during and after; held --hold-ms with the box on so the
 *                                         streams encoded from canvas 0 can be looked at; disabled and restored
 *   IavRefAperture --misc                 IMG_GET_MISC_INFO without and with the extra-raw query (the call whose user
 *                                         pointers the virtual driver marshals inline)
 *   IavRefAperture --probe consumer       every call the aperture unlocks must fail with EPERM and no aperture
 *                                         partition may be mapped (a consumer guest)
 *   IavRefAperture --probe bad            the primary guest's hostile calls are refused before the driver with the
 *                                         expected errno, then a good call still succeeds (guest only: natively the
 *                                         calls would reach the driver). An out-of-range offset into a partition the
 *                                         aperture carries is EINVAL; into one it does not carry, EPERM
 *
 * Results are JSON rows on stdout. Exit 0 when every step of the mode held.
 */

#include "IavRefCommon.hxx"

#include <ambas_imgproc_ioctl_arch.h>

using namespace iavref;

namespace {

struct Part
{
    unsigned int pid;
    const char *name;
};

const Part Parts[] = {
    { IAV_PART_MASK, "mask" },           { IAV_PART_WARP, "warp" },
    { IAV_PART_QPMATRIX, "qpmatrix" },   { IAV_PART_CE_GO_MAP, "ce_go_map" },
    { IAV_PART_CANVAS_OVERLAY, "canvas_overlay" }, { IAV_PART_ARB_BLUR, "arb_blur" },
    { IAV_PART_YUV_REMAP, "yuv_remap" },
};
const unsigned int NParts = sizeof(Parts) / sizeof(Parts[0]);

uint64_t readWord(const volatile unsigned char *p)
{
    return *(const volatile uint64_t *)p;
}

void writeWord(volatile unsigned char *p, uint64_t v)
{
    *(volatile uint64_t *)p = v;
}

/* ---- --map ---- */

int runMap(Device &dev)
{
    int bad = 0;

    for (unsigned int i = 0; i < NParts; i++) {
        unsigned long addr = 0, len = 0;
        long long mapped = 0, readback = 0, ring = 0, ringMapped = 0, overRefused = 0, mapErr = 0;

        if (dev.partition(Parts[i].pid, &addr, &len) != 0 || len == 0) {
            Row("map").add("part", Parts[i].name).add("pid", (long long)Parts[i].pid).add("length", 0LL)
                .add("present", 0LL).print();
            continue;
        }
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)addr);
        if (p == MAP_FAILED) {
            mapErr = errno;
        } else {
            volatile unsigned char *b = (volatile unsigned char *)p;
            uint64_t first = readWord(b), last = readWord(b + len - 8);
            uint64_t a = 0x5a5a000000000000ULL | ((uint64_t)Parts[i].pid << 8) | 0x11, z = ~a;

            mapped = 1;
            writeWord(b, a);
            writeWord(b + len - 8, z);
            readback = readWord(b) == a && readWord(b + len - 8) == z;
            writeWord(b, first);
            writeWord(b + len - 8, last);
            munmap(p, len);
        }
        if (mapped) {
            /* the driver's ring rule: up to twice the partition, the same memory twice in a row */
            void *r = mmap(NULL, 2 * len, PROT_READ, MAP_SHARED, dev.fd(), (off_t)addr);

            if (r != MAP_FAILED) {
                const volatile unsigned char *b = (const volatile unsigned char *)r;

                ringMapped = 1;
                ring = readWord(b) == readWord(b + len) && readWord(b + len - 8) == readWord(b + 2 * len - 8);
                munmap(r, 2 * len);
            }
            r = mmap(NULL, 2 * len + 4096, PROT_READ, MAP_SHARED, dev.fd(), (off_t)addr);
            if (r == MAP_FAILED) {
                overRefused = 1;
            } else {
                munmap(r, 2 * len + 4096);
            }
        }
        Row("map").add("part", Parts[i].name).add("pid", (long long)Parts[i].pid).add("length", (long long)len)
            .add("present", 1LL).add("mapped", mapped).add("map_errno", mapErr).add("readback", readback)
            .add("ring_mapped", ringMapped).add("ring", ring).add("over_refused", overRefused).print();
        /*
         * The ring map is informational: the bare-metal driver refuses it for QPMATRIX (a map there is the user part or
         * the whole buffer, iav_enc_mem.c get_map_info), the virtual driver allows twice every entry (a superset, G8).
         */
        bad |= !(mapped && readback && overRefused);
    }
    return bad;
}

/* ---- canvas measurements ---- */

void statsRow(const char *phase, const RegionStats &s)
{
    Row("region").add("phase", phase).add("mono_pts", (long long)s.pts).add("mean", s.mean).add("var", s.var)
        .add("hash", (long long)s.hash).add("canvas_width", (long long)s.width)
        .add("canvas_height", (long long)s.height).print();
}

/* ---- --mask ---- */

int runMask(Device &dev, int argc, char **argv)
{
    const unsigned int x = (unsigned int)argValue(argc, argv, "--x", 256) & ~7u;
    const unsigned int y = (unsigned int)argValue(argc, argv, "--y", 128) & ~1u;
    const unsigned int w = (unsigned int)argValue(argc, argv, "--w", 256) & ~7u;
    const unsigned int h = (unsigned int)argValue(argc, argv, "--h", 128) & ~1u;
    const unsigned int yv = (unsigned int)argValue(argc, argv, "--y-value", 200);
    const long settle = argValue(argc, argv, "--settle-ms", 1500);
    unsigned long dspAddr = 0, dspLen = 0, maskAddr = 0, maskLen = 0;
    struct iav_mem_layout_info *lay = (struct iav_mem_layout_info *)calloc(1, sizeof(*lay));
    Mapping dsp;
    RegionStats before, during, after;
    int rcCfg, errCfg, rcApply, errApply, rcOff, errOff;

    if (lay == NULL || dev.partition(IAV_PART_DSP, &dspAddr, &dspLen) != 0 || !dsp.map(dev.fd(), dspAddr, dspLen, PROT_READ) ||
        dev.partition(IAV_PART_MASK, &maskAddr, &maskLen) != 0 || maskLen == 0) {
        fprintf(stderr, "no DSP or MASK partition\n");
        return 1;
    }
    if (dev.call(IAV_IOC_QUERY_MEM_LAYOUT_INFO, lay, "IAV_IOC_QUERY_MEM_LAYOUT_INFO") != 0) {
        return 1;
    }
    const struct iav_mem_layout &slot = lay->mask_layout.buf_layout[0];
    if (canvasRegion(dev, dsp, 0, x, y, w, h, settle, &before) != 0) {
        return 1;
    }
    /* the mask of channel 0: one bit per pixel of its main buffer, pitch rounded up to 128 bytes (get_mask_pitch) */
    const unsigned int pitch = ((before.width + 7) / 8 + 127) & ~127u;
    const unsigned long need = (unsigned long)pitch * before.height;
    if (slot.offset + need > maskLen || slot.size < need) {
        fprintf(stderr, "mask slot 0 (offset 0x%x size 0x%x) cannot hold %lu bytes\n", slot.offset, slot.size, need);
        return 1;
    }
    void *p = mmap(NULL, maskLen, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)maskAddr);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap MASK: %s\n", strerror(errno));
        return 1;
    }
    volatile unsigned char *m = (volatile unsigned char *)p + slot.offset;
    unsigned char *saved = (unsigned char *)malloc(need), *pic = (unsigned char *)calloc(1, need);

    windowCopy(saved, m, need);
    for (unsigned int r = y; r < y + h && r < before.height; r++) {
        memset(pic + (unsigned long)r * pitch + x / 8, 0xff, w / 8);
    }
    windowWrite(m, pic, need);

    struct iav_video_proc v;
    struct iav_apply_flag flags[IAV_VIDEO_PROC_NUM];
    int cid = IAV_VIDEO_PROC_MASK;

    memset(&v, 0, sizeof(v));
    memcpy(&v.cid, &cid, sizeof(cid));
    v.arg.mask.channel_id = 0;
    v.arg.mask.enable = 1;
    v.arg.mask.y = (u8)yv;
    v.arg.mask.u = 128;
    v.arg.mask.v = 128;
    v.arg.mask.buf_pitch = (u16)pitch;
    v.arg.mask.buf_height = (u16)before.height;
    rcCfg = dev.call(IAV_IOC_CFG_VIDEO_PROC, &v, "IAV_IOC_CFG_VIDEO_PROC mask", true);
    errCfg = errno;
    memset(flags, 0, sizeof(flags));
    flags[IAV_VIDEO_PROC_MASK].apply = 1;
    flags[IAV_VIDEO_PROC_MASK].param = 0x1;
    rcApply = dev.call(IAV_IOC_APPLY_VIDEO_PROC, flags, "IAV_IOC_APPLY_VIDEO_PROC mask", true);
    errApply = errno;
    if (canvasRegion(dev, dsp, 0, x, y, w, h, settle, &during) != 0) {
        memset(&during, 0, sizeof(during));
    }
    /* off again, the slot as it was */
    v.arg.mask.enable = 0;
    rcOff = dev.call(IAV_IOC_CFG_VIDEO_PROC, &v, "IAV_IOC_CFG_VIDEO_PROC mask off", true);
    errOff = errno;
    dev.call(IAV_IOC_APPLY_VIDEO_PROC, flags, "IAV_IOC_APPLY_VIDEO_PROC mask off", true);
    windowWrite(m, saved, need);
    munmap(p, maskLen);
    if (canvasRegion(dev, dsp, 0, x, y, w, h, settle, &after) != 0) {
        memset(&after, 0, sizeof(after));
    }
    callRow("call", 0, "cfg_video_proc_mask_on", rcCfg, errCfg);
    callRow("call", 1, "apply_video_proc_mask", rcApply, errApply);
    callRow("call", 2, "cfg_video_proc_mask_off", rcOff, errOff);
    statsRow("before", before);
    statsRow("during", during);
    statsRow("after", after);
    Row("mask").add("x", (long long)x).add("y", (long long)y).add("w", (long long)w).add("h", (long long)h)
        .add("y_value", (long long)yv).add("pitch", (long long)pitch).add("slot_offset", (long long)slot.offset).print();
    free(saved);
    free(pic);
    free(lay);
    return rcCfg != 0 || rcApply != 0 || rcOff != 0;
}

/* ---- --canvas-overlay ---- */

int runCanvasOverlay(Device &dev, int argc, char **argv)
{
    const unsigned int x = (unsigned int)argValue(argc, argv, "--x", 256) & ~1u;
    const unsigned int y = (unsigned int)argValue(argc, argv, "--y", 128) & ~3u;
    const unsigned int w = (unsigned int)argValue(argc, argv, "--w", 256) & ~31u;
    const unsigned int h = (unsigned int)argValue(argc, argv, "--h", 128) & ~3u;
    const long settle = argValue(argc, argv, "--settle-ms", 1500);
    const long hold = argValue(argc, argv, "--hold-ms", 0);
    unsigned long dspAddr = 0, dspLen = 0, ovAddr = 0, ovLen = 0;
    const unsigned long clutOff = 0, dataOff = 4096;
    Mapping dsp;
    RegionStats before, during, after;
    int rcOn, errOn, rcOff, errOff, rcGet, errGet;

    if (dev.partition(IAV_PART_DSP, &dspAddr, &dspLen) != 0 || !dsp.map(dev.fd(), dspAddr, dspLen, PROT_READ) ||
        dev.partition(IAV_PART_CANVAS_OVERLAY, &ovAddr, &ovLen) != 0 || ovLen < dataOff + (unsigned long)w * h) {
        fprintf(stderr, "no DSP or CANVAS_OVERLAY partition\n");
        return 1;
    }
    if (canvasRegion(dev, dsp, 0, x, y, w, h, settle, &before) != 0) {
        return 1;
    }
    void *p = mmap(NULL, ovLen, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)ovAddr);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap CANVAS_OVERLAY: %s\n", strerror(errno));
        return 1;
    }
    volatile unsigned char *ov = (volatile unsigned char *)p;
    const unsigned long bytes = dataOff + (unsigned long)w * h;
    unsigned char *saved = (unsigned char *)malloc(bytes), *pic = (unsigned char *)calloc(1, bytes);

    windowCopy(saved, ov, bytes);
    /* colour table entries are V, U, Y, alpha: 0 transparent, 1 opaque white */
    pic[clutOff + 4 + 0] = 128;
    pic[clutOff + 4 + 1] = 128;
    pic[clutOff + 4 + 2] = 235;
    pic[clutOff + 4 + 3] = 255;
    memset(pic + dataOff, 1, (unsigned long)w * h);
    windowWrite(ov, pic, bytes);

    struct iav_overlay_insert o;

    memset(&o, 0, sizeof(o));
    o.id = 0;
    o.id_for_canvas = 1;
    o.enable = 1;
    o.area[0].enable = 1;
    o.area[0].start_x = (u16)x;
    o.area[0].start_y = (u16)y;
    o.area[0].width = (u16)w;
    o.area[0].pitch = (u16)w;
    o.area[0].height = (u16)h;
    o.area[0].total_size = w * h;
    o.area[0].clut_addr_offset = clutOff;
    o.area[0].data_addr_offset = dataOff;
    rcOn = dev.call(IAV_IOC_SET_OVERLAY_INSERT, &o, "IAV_IOC_SET_OVERLAY_INSERT canvas", true);
    errOn = errno;
    if (canvasRegion(dev, dsp, 0, x, y, w, h, settle, &during) != 0) {
        memset(&during, 0, sizeof(during));
    }
    {
        struct iav_overlay_insert g;

        memset(&g, 0, sizeof(g));
        g.id = 0;
        g.id_for_canvas = 1;
        rcGet = dev.call(IAV_IOC_GET_OVERLAY_INSERT, &g, "IAV_IOC_GET_OVERLAY_INSERT canvas", true);
        errGet = errno;
        Row("readback").add("enable", (long long)g.enable).add("area0_enable", (long long)g.area[0].enable)
            .add("area0_width", (long long)g.area[0].width).add("area0_data_offset", (long long)g.area[0].data_addr_offset)
            .print();
    }
    if (hold > 0) {
        usleep((useconds_t)hold * 1000);
    }
    o.enable = 0;
    rcOff = dev.call(IAV_IOC_SET_OVERLAY_INSERT, &o, "IAV_IOC_SET_OVERLAY_INSERT canvas off", true);
    errOff = errno;
    windowWrite(ov, saved, bytes);
    munmap(p, ovLen);
    if (canvasRegion(dev, dsp, 0, x, y, w, h, settle, &after) != 0) {
        memset(&after, 0, sizeof(after));
    }
    callRow("call", 0, "set_overlay_insert_canvas_on", rcOn, errOn);
    callRow("call", 1, "get_overlay_insert_canvas", rcGet, errGet);
    callRow("call", 2, "set_overlay_insert_canvas_off", rcOff, errOff);
    statsRow("before", before);
    statsRow("during", during);
    statsRow("after", after);
    free(saved);
    free(pic);
    return rcOn != 0 || rcOff != 0;
}

/* ---- --misc ---- */

int runMisc(Device &dev)
{
    struct img_misc_info m;
    u8 query[IMG_MAX_VIN_CHANNEL_NUM], valid[IMG_MAX_VIN_CHANNEL_NUM];
    struct img_extra_raw_info extra[IMG_MAX_VIN_CHANNEL_NUM];
    int rc, err;
    long long nvalid = 0;

    memset(&m, 0, sizeof(m));
    rc = dev.call(IAV_IOC_IMG_GET_MISC_INFO, &m, "IAV_IOC_IMG_GET_MISC_INFO", true);
    err = errno;
    callRow("call", 0, "misc_info_no_query", rc, err);
    memset(&m, 0, sizeof(m));
    memset(query, 1, sizeof(query));
    memset(valid, 0xee, sizeof(valid));
    memset(extra, 0xee, sizeof(extra));
    m.query_extra_raw = query;
    m.extra_raw_valid = valid;
    m.extra_raw = extra;
    rc = dev.call(IAV_IOC_IMG_GET_MISC_INFO, &m, "IAV_IOC_IMG_GET_MISC_INFO query", true);
    err = errno;
    callRow("call", 1, "misc_info_query", rc, err);
    for (unsigned int i = 0; i < IMG_MAX_VIN_CHANNEL_NUM; i++) {
        nvalid += valid[i] == 1;
    }
    {
        long long written = 0;

        /* how many valid flags the call wrote (the driver writes one per channel of the pipeline) */
        for (unsigned int i = 0; i < IMG_MAX_VIN_CHANNEL_NUM; i++) {
            written += valid[i] != 0xee;
        }
        Row("misc").add("valid_flags_written", written).add("valid_set", nvalid)
            .add("pointers_kept", (long long)(m.query_extra_raw == query && m.extra_raw_valid == valid &&
                                              m.extra_raw == (void *)extra)).print();
    }
    /* a query without its output buffers: the driver's own EINVAL */
    memset(&m, 0, sizeof(m));
    m.query_extra_raw = query;
    rc = dev.call(IAV_IOC_IMG_GET_MISC_INFO, &m, "IAV_IOC_IMG_GET_MISC_INFO bad", true);
    err = errno;
    callRow("call", 2, "misc_info_query_without_buffers", rc, err);
    return 0;
}

/* ---- --probe consumer / bad ---- */

struct Probe
{
    long long seq;
    int bad;

    Probe() : seq(0), bad(0) {}

    void expect(const char *name, int rc, int err, int want)
    {
        int ok = want == 0 ? rc == 0 : (rc < 0 && err == want);

        Row("probe").add("seq", seq++).add("case", name).add("rc", (long long)rc).add("errno", (long long)(rc < 0 ? err : 0))
            .add("expected_errno", (long long)want).add("as_expected", (long long)ok).print();
        bad |= !ok;
    }

    /* a mapping that must fail, whatever the errno */
    void expectNoMap(const char *name, int fd, unsigned long addr, unsigned long len)
    {
        void *p = len ? mmap(NULL, len, PROT_READ, MAP_SHARED, fd, (off_t)addr) : MAP_FAILED;
        int err = errno;
        int ok = p == MAP_FAILED;

        if (p != MAP_FAILED) {
            munmap(p, len);
        }
        Row("probe").add("seq", seq++).add("case", name).add("rc", (long long)(ok ? -1 : 0))
            .add("errno", (long long)(ok ? err : 0)).add("expected_errno", -1LL).add("as_expected", (long long)ok).print();
        bad |= !ok;
    }
};

int call(Device &dev, unsigned long cmd, void *arg, int *err)
{
    int rc = dev.call(cmd, arg, "probe", true);

    *err = errno;
    return rc;
}

int runConsumer(Device &dev)
{
    Probe pr;
    int err, rc;
    static unsigned char arg[4096];

    struct {
        unsigned long cmd;
        const char *name;
    } calls[] = {
        { IAV_IOC_CFG_VIDEO_PROC, "cfg_video_proc" },        { IAV_IOC_CFG_WARP_CTRL, "cfg_warp_ctrl" },
        { IAV_IOC_SET_STREAM_CONFIG, "set_stream_config" },  { IAV_IOC_CFG_FRAME_SYNC_PROC, "cfg_frame_sync_proc" },
        { IAV_IOC_SET_YUV_REMAP_INSERT, "set_yuv_remap_insert" },
        { IAV_IOC_CE_GO_MAP_REQUEST_BUFFER, "ce_go_map_request" },
        { IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, "ce_go_map_handshake" },
        { IAV_IOC_SET_BLUR_INSERT, "set_blur_insert" },       { IAV_IOC_APPLY_VIDEO_PROC, "apply_video_proc" },
        { IAV_IOC_APPLY_WARP_CTRL, "apply_warp_ctrl" },
    };
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        memset(arg, 0, sizeof(arg));
        rc = call(dev, calls[i].cmd, arg, &err);
        pr.expect(calls[i].name, rc, err, EPERM);
    }
    {
        struct iav_overlay_insert o;

        memset(&o, 0, sizeof(o));
        o.id_for_canvas = 1;
        rc = call(dev, IAV_IOC_SET_OVERLAY_INSERT, &o, &err);
        pr.expect("set_overlay_insert_canvas", rc, err, EPERM);
    }
    {
        struct img_misc_info m;

        memset(&m, 0, sizeof(m));
        rc = call(dev, IAV_IOC_IMG_GET_MISC_INFO, &m, &err);
        pr.expect("img_get_misc_info", rc, err, EPERM);
    }
    for (unsigned int i = 0; i < NParts; i++) {
        unsigned long addr = 0, len = 0;
        char name[64];

        snprintf(name, sizeof(name), "map_%s", Parts[i].name);
        if (dev.partition(Parts[i].pid, &addr, &len) != 0 || len == 0) {
            continue;
        }
        pr.expectNoMap(name, dev.fd(), addr, len);
    }
    return pr.bad;
}

/*
 * Whether this guest's aperture carries the partition: its mapping succeeds. A call naming memory in a partition the
 * aperture does not carry is refused with EPERM before its range is looked at; with the entry, an out-of-range offset
 * is EINVAL.
 */
bool hasEntry(Device &dev, unsigned int pid, unsigned long *len)
{
    unsigned long addr = 0;

    *len = 0;
    if (dev.partition(pid, &addr, len) != 0 || *len == 0) {
        return false;
    }
    void *p = mmap(NULL, *len, PROT_READ, MAP_SHARED, dev.fd(), (off_t)addr);

    if (p == MAP_FAILED) {
        return false;
    }
    munmap(p, *len);
    return true;
}

int runBad(Device &dev)
{
    Probe pr;
    int err, rc;
    unsigned long remapLen = 0, warpLen = 0, arbLen = 0, covlLen = 0;
    const int remapWant = hasEntry(dev, IAV_PART_YUV_REMAP, &remapLen) ? EINVAL : EPERM;
    const int warpWant = hasEntry(dev, IAV_PART_WARP, &warpLen) ? EINVAL : EPERM;
    const int arbWant = hasEntry(dev, IAV_PART_ARB_BLUR, &arbLen) ? EINVAL : EPERM;
    const int covlWant = hasEntry(dev, IAV_PART_CANVAS_OVERLAY, &covlLen) ? EINVAL : EPERM;

    Row("entries").add("yuv_remap", (long long)(remapWant == EINVAL)).add("warp", (long long)(warpWant == EINVAL))
        .add("arb_blur", (long long)(arbWant == EINVAL)).add("canvas_overlay", (long long)(covlWant == EINVAL)).print();
    {
        struct iav_ce_go_map_handshake_buffer h;

        memset(&h, 0, sizeof(h));
        h.buffer_idx = 8;
        rc = call(dev, IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, &h, &err);
        pr.expect("ce_handshake_buffer_idx_8", rc, err, EINVAL);
        h.buffer_idx = 0;
        h.chan_id = 4;
        rc = call(dev, IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, &h, &err);
        pr.expect("ce_handshake_chan_4", rc, err, EINVAL);
    }
    {
        struct iav_yuv_remap r;

        memset(&r, 0, sizeof(r));
        r.id = 1;
        r.enable = 1;
        r.area[0].enable = 1;
        r.area[0].clut_addr_offset = remapLen - 1023;
        rc = call(dev, IAV_IOC_SET_YUV_REMAP_INSERT, &r, &err);
        pr.expect("yuv_remap_clut_past_entry", rc, err, remapWant);
        r.area[0].clut_addr_offset = ~0ul - 100;
        rc = call(dev, IAV_IOC_SET_YUV_REMAP_INSERT, &r, &err);
        pr.expect("yuv_remap_clut_wraps", rc, err, remapWant);
        r.area[0].clut_addr_offset = 0;
        r.id = 32;
        rc = call(dev, IAV_IOC_SET_YUV_REMAP_INSERT, &r, &err);
        pr.expect("yuv_remap_stream_32", rc, err, EINVAL);
    }
    {
        struct iav_blur_insert b;

        memset(&b, 0, sizeof(b));
        b.stream_id = 1;
        b.enable = 1;
        b.area[0].enable = 1;
        b.area[0].use_arb_blur = 1;
        b.area[0].width = 64;
        b.area[0].height = 16;
        b.area[0].arb_blur_pitch = 64;
        b.area[0].arb_blur_offset = (u32)(arbLen - 64 * 15 - 8);
        rc = call(dev, IAV_IOC_SET_BLUR_INSERT, &b, &err);
        pr.expect("arb_blur_past_entry", rc, err, arbWant);
        b.area[0].arb_blur_offset = 0;
        b.area[0].arb_blur_pitch = 64 * 1023;
        b.area[0].height = 1000;
        rc = call(dev, IAV_IOC_SET_BLUR_INSERT, &b, &err);
        pr.expect("arb_blur_pitch_past_entry", rc, err, arbWant);
        b.area[0].arb_blur_pitch = 64;
        b.area[0].height = 16;
        b.type = IAV_BLUR_TYPE_SMOOTH;
        rc = call(dev, IAV_IOC_SET_BLUR_INSERT, &b, &err);
        pr.expect("arb_smooth_blur", rc, err, EPERM);
    }
    {
        struct iav_warp_ctrl *w = (struct iav_warp_ctrl *)calloc(1, sizeof(*w));

        w->arg.main.area[0].enable = 1;
        w->arg.main.area[0].v_map.data_addr_offset = warpLen;
        rc = call(dev, IAV_IOC_CFG_WARP_CTRL, w, &err);
        pr.expect("warp_table_past_entry", rc, err, warpWant);
        w->arg.main.area[0].v_map.data_addr_offset = 0;
        w->channel_id = 4;
        rc = call(dev, IAV_IOC_CFG_WARP_CTRL, w, &err);
        pr.expect("warp_channel_4", rc, err, EINVAL);
        free(w);
    }
    {
        struct iav_stream_cfg *c = (struct iav_stream_cfg *)calloc(1, sizeof(*c));
        int cid = IAV_H264_CFG_QP_ROI;

        c->id = 0;
        memcpy(&c->cid, &cid, sizeof(cid));
        c->arg.h264_roi.qpm_raw = 1;
        c->arg.h264_roi.data_offset = 0x40000000ul;
        rc = call(dev, IAV_IOC_SET_STREAM_CONFIG, c, &err);
        pr.expect("qp_roi_raw", rc, err, EPERM);
        memset(c, 0, sizeof(*c));
        c->id = 32;
        rc = call(dev, IAV_IOC_SET_STREAM_CONFIG, c, &err);
        pr.expect("stream_config_id_32", rc, err, EINVAL);
        memset(c, 0, sizeof(*c));
        cid = IAV_STMCFG_CUSTOM_SEI;
        memcpy(&c->cid, &cid, sizeof(cid));
        rc = call(dev, IAV_IOC_CFG_FRAME_SYNC_PROC, c, &err);
        pr.expect("frame_sync_custom_sei", rc, err, EPERM);
        free(c);
    }
    {
        struct iav_video_proc v;
        int cid = IAV_VIDEO_PROC_MULTI_BLEND;

        memset(&v, 0, sizeof(v));
        memcpy(&v.cid, &cid, sizeof(cid));
        rc = call(dev, IAV_IOC_CFG_VIDEO_PROC, &v, &err);
        pr.expect("video_proc_multi_blend", rc, err, EPERM);
        cid = IAV_VIDEO_PROC_MASK;
        memcpy(&v.cid, &cid, sizeof(cid));
        v.arg.mask.channel_id = 4;
        rc = call(dev, IAV_IOC_CFG_VIDEO_PROC, &v, &err);
        pr.expect("video_proc_mask_channel_4", rc, err, EINVAL);
        memset(&v, 0, sizeof(v));
        cid = IAV_VIDEO_PROC_CANVAS_ORDER;
        memcpy(&v.cid, &cid, sizeof(cid));
        v.arg.canvas_order.chan_order[3] = 40;
        rc = call(dev, IAV_IOC_CFG_VIDEO_PROC, &v, &err);
        pr.expect("video_proc_canvas_order_shift", rc, err, EINVAL);
    }
    {
        struct iav_overlay_insert o;

        memset(&o, 0, sizeof(o));
        o.id_for_canvas = 1;
        o.enable = 1;
        o.area[0].enable = 1;
        o.area[0].width = 64;
        o.area[0].pitch = 64;
        o.area[0].height = 4;
        o.area[0].total_size = 256;
        o.area[0].clut_addr_offset = covlLen - 1023;
        rc = call(dev, IAV_IOC_SET_OVERLAY_INSERT, &o, &err);
        pr.expect("canvas_overlay_clut_past_entry", rc, err, covlWant);
        o.area[0].clut_addr_offset = 0;
        o.id = 20;
        rc = call(dev, IAV_IOC_SET_OVERLAY_INSERT, &o, &err);
        pr.expect("canvas_overlay_canvas_20", rc, err, EINVAL);
        memset(&o, 0, sizeof(o));
        o.id = 32;
        rc = call(dev, IAV_IOC_SET_OVERLAY_INSERT, &o, &err);
        pr.expect("stream_overlay_stream_32", rc, err, EINVAL);
    }
    {
        struct iav_apply_flag f[IAV_VIDEO_PROC_NUM];

        memset(f, 0, sizeof(f));
        f[IAV_VIDEO_PROC_ARB_BLEND].apply = 1;
        rc = call(dev, IAV_IOC_APPLY_VIDEO_PROC, f, &err);
        pr.expect("apply_video_proc_arb_blend", rc, err, EPERM);
    }
    {
        /* a good call afterwards: YUV remap off on stream 1 */
        struct iav_yuv_remap r;

        memset(&r, 0, sizeof(r));
        r.id = 1;
        rc = call(dev, IAV_IOC_SET_YUV_REMAP_INSERT, &r, &err);
        pr.expect("good_yuv_remap_off", rc, err, 0);
    }
    return pr.bad;
}

} // namespace

int main(int argc, char **argv)
{
    Device dev;

    if (!dev.open()) {
        return 1;
    }
    if (argFlag(argc, argv, "--map")) {
        return runMap(dev);
    }
    if (argFlag(argc, argv, "--mask")) {
        return runMask(dev, argc, argv);
    }
    if (argFlag(argc, argv, "--canvas-overlay")) {
        return runCanvasOverlay(dev, argc, argv);
    }
    if (argFlag(argc, argv, "--misc")) {
        return runMisc(dev);
    }
    {
        const char *probe = argString(argc, argv, "--probe", "");

        if (strcmp(probe, "consumer") == 0) {
            return runConsumer(dev);
        }
        if (strcmp(probe, "bad") == 0) {
            if (strcmp(environment(), "guest") != 0) {
                fprintf(stderr, "--probe bad runs only in a guest: natively the calls would reach the driver\n");
                return 2;
            }
            return runBad(dev);
        }
    }
    fprintf(stderr, "usage: %s --map | --mask | --canvas-overlay | --misc | --probe consumer|bad\n", argv[0]);
    return 2;
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
