/*
 * IavRefArena.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, anonymous partitions (plan_iav_usb_yolo, Envelope E): the allocation pattern of the Cooper IAV
 * tests (test_img_scale.c alloc_input_buf / alloc_output_buf, test_yuvcap.c alloc_gdma_dst_buf_for_gdma), written
 * against the bare-metal /dev/iav ABI only, so the same static binary runs on the bare board and in a guest:
 *
 *   IAV_IOC_ALLOC_ANON_MEM_PART -> mmap(fd_iav, offset = the returned address) -> IAV_IOC_GDMA_COPY and
 *   IAV_IOC_SET_IMG_SCALE naming the returned pid -> read the result through the mapping -> munmap ->
 *   IAV_IOC_FREE_MEM_PART
 *
 * A JPEG is decoded into the decode partition; GDMA_COPY copies the decoded Y and UV planes into an input block; the
 * DSP image scaler scales the input block into a 640x640 NV12 output block (window 0,0,640,360, the rest a gray pad the
 * program writes first and checks afterwards). Block access uses aligned accesses of at most 8 bytes
 * (IavRefCommon.hxx), the same on both sides.
 *
 *   IavRefArena --jpeg <file> [--repeat n] [--mode block|wait]   the pattern; one "arena" row per pass
 *   IavRefArena --probe life                                      the lifecycle rules (both sides; errnos compared)
 *   IavRefArena --probe bad [--foreign <pid>]                     hostile calls (a guest only)
 *   IavRefArena --fill <n>                                        fill the largest block with a pattern and leave n
 *                                                                 non-blocking scales queued into it (a guest only)
 *   IavRefArena --zero                                            read the largest block: count the bytes not zero
 *   IavRefArena --stress <seconds>                                threads allocating, mapping, checking and freeing
 */

#include "IavRefCommon.hxx"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using namespace iavref;

namespace {

const unsigned int OutPitch = 640;
const unsigned int OutHeight = 640;
const unsigned long OutUv = (unsigned long)OutPitch * OutHeight;
const unsigned int WinW = 640;
const unsigned int WinH = 360;
const unsigned char PadY = 114;
const unsigned char PadUv = 128;
const unsigned long LargestBlock = 0x3ff0000ul;	/* a 64 MiB slice less its claim page, in 64 KiB blocks */

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

/* An anonymous partition as the Cooper tests allocate it: the pid, the address to mmap and the length. */
struct Block
{
    unsigned int pid;
    unsigned long offset;
    unsigned long length;
    void *map;

    Block() : pid(0), offset(0), length(0), map(MAP_FAILED) {}
};

int allocBlock(Device &dev, unsigned long size, Block &b, bool quiet = false)
{
    struct iav_alloc_mem_part a;

    memset(&a, 0, sizeof(a));
    a.length = size;
    a.enable_cache = 0;
    a.from_private = 1;
    a.accessibility = IAV_MEM_PART_ACS_ARM | IAV_MEM_PART_ACS_DSP;
    if (dev.call(IAV_IOC_ALLOC_ANON_MEM_PART, &a, "IAV_IOC_ALLOC_ANON_MEM_PART", quiet) != 0) {
        return -1;
    }
    b.pid = a.pid;
    b.offset = a.offset;
    b.length = a.length;
    b.map = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)b.offset);
    if (b.map == MAP_FAILED) {
        fprintf(stderr, "mmap block: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int freeBlock(Device &dev, Block &b)
{
    struct iav_free_mem_part f;

    if (b.map != MAP_FAILED) {
        munmap(b.map, b.length);
        b.map = MAP_FAILED;
    }
    memset(&f, 0, sizeof(f));
    f.pid = b.pid;
    return dev.call(IAV_IOC_FREE_MEM_PART, &f, "IAV_IOC_FREE_MEM_PART");
}

volatile unsigned char *bytes(const Block &b)
{
    return (volatile unsigned char *)b.map;
}

void fill(volatile unsigned char *p, unsigned long off, size_t n, unsigned char v)
{
    unsigned char buf[4096];
    size_t done = 0;

    memset(buf, v, sizeof(buf));
    while (done < n) {
        size_t c = std::min(sizeof(buf), n - done);

        windowWrite(p + off + done, buf, c);
        done += c;
    }
}

bool all(const volatile unsigned char *p, unsigned long off, size_t n, unsigned char v)
{
    unsigned char buf[4096];
    size_t done = 0;

    while (done < n) {
        size_t c = std::min(sizeof(buf), n - done);

        windowCopy(buf, p + off + done, c);
        for (size_t i = 0; i < c; i++) {
            if (buf[i] != v) {
                return false;
            }
        }
        done += c;
    }
    return true;
}

bool padIntact(const volatile unsigned char *p)
{
    for (unsigned int r = 0; r < WinH; r++) {
        if (!all(p, (unsigned long)r * OutPitch + WinW, OutPitch - WinW, PadY)) {
            return false;
        }
    }
    if (!all(p, (unsigned long)WinH * OutPitch, (size_t)OutPitch * (OutHeight - WinH), PadY)) {
        return false;
    }
    return all(p, OutUv + (unsigned long)(WinH / 2) * OutPitch, (size_t)OutPitch * (OutHeight / 2 - WinH / 2), PadUv);
}

int gdma(Device &dev, unsigned int srcPid, unsigned long srcOff, unsigned int dstPid, unsigned long dstOff,
         unsigned int pitch, unsigned int width, unsigned int height, bool quiet = false)
{
    struct iav_gdma_copy g;

    memset(&g, 0, sizeof(g));
    g.src_mmap_type = srcPid;
    g.src_offset = srcOff;
    g.dst_mmap_type = dstPid;
    g.dst_offset = dstOff;
    g.src_pitch = (u16)pitch;
    g.dst_pitch = (u16)pitch;
    g.width = (u16)width;
    g.height = height;
    return dev.call(IAV_IOC_GDMA_COPY, &g, "IAV_IOC_GDMA_COPY", quiet);
}

int syncBlock(Device &dev, unsigned int pid, unsigned long off, unsigned long len, int opt, bool quiet = false)
{
    struct iav_sync_cache_mem_part c;

    memset(&c, 0, sizeof(c));
    c.pid = pid;
    c.offset = off;
    c.length = len;
    memcpy(&c.opt, &opt, sizeof(opt));
    return dev.call(IAV_IOC_SYNC_CACHE_MEM_PART, &c, "IAV_IOC_SYNC_CACHE_MEM_PART", quiet);
}

struct iav_img_scale scaleArg(unsigned int inPid, unsigned int w, unsigned int h, unsigned int pitch,
                              unsigned int outPid)
{
    struct iav_img_scale s;

    memset(&s, 0, sizeof(s));
    s.format = IAV_YUV_FORMAT_YUV420;
    s.input.width = w;
    s.input.height = h;
    s.output.width = WinW;
    s.output.height = WinH;
    s.input_buf.pid = inPid;
    s.input_buf.y_offset = 0;
    s.input_buf.uv_offset = (unsigned long)pitch * h;
    s.input_buf.pitch = (u16)pitch;
    s.input_buf.height = (u16)h;
    s.output_buf.pid = outPid;
    s.output_buf.y_offset = 0;
    s.output_buf.uv_offset = OutUv;
    s.output_buf.pitch = OutPitch;
    s.output_buf.height = OutHeight;
    s.input_skip_cache_sync = 1;
    s.output_skip_cache_sync = 1;
    return s;
}

/* ---- the pattern ---- */

int runPattern(Device &dev, const std::vector<unsigned char> &jpeg, unsigned int w, unsigned int h, long repeat,
               bool waitMode)
{
    Mapping dec;
    unsigned long decAddr = 0, decLen = 0;
    struct iav_decode_mode_config mode;
    struct iav_decoder_info info;
    bool entered = false, created = false;
    long bad = 0, done = 0;
    int rc = 1;

    if (dev.partition(IAV_PART_DEC_BSB, &decAddr, &decLen) != 0 || decLen < 0x600000 || jpeg.size() > 0x300000) {
        fprintf(stderr, "the decode partition (%lu bytes) or the JPEG (%zu bytes) does not fit the layout\n", decLen,
                jpeg.size());
        return 1;
    }
    if (!dec.map(dev.fd(), decAddr, decLen, PROT_READ | PROT_WRITE)) {
        return 1;
    }
    volatile unsigned char *win = (volatile unsigned char *)dec.bytes();

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
    info.decoder_type = IAV_DEC_TYPE_JPEG;
    info.video_width = w;
    info.video_height = h;
    info.bsb_size = (u32)decLen;
    if (dev.call(IAV_IOC_CREATE_DECODER, &info, "IAV_IOC_CREATE_DECODER") != 0) {
        goto done;
    }
    created = true;

    for (long pass = 0; pass < repeat; pass++) {
        struct iav_decode_jpeg dj;
        struct iav_decode_status st;
        Block in, out;
        int tries;

        windowWrite(win, &jpeg[0], jpeg.size());
        memset(&dj, 0, sizeof(dj));
        dj.is_dec_only = 1;
        dj.is_last_frame = 1;
        dj.jpeg_size = (u32)jpeg.size();
        dj.yuv_start_offset = 0x300000;
        dj.yuv_size = (u32)(decLen - 0x300000);
        if (dev.call(IAV_IOC_DECODE_JPEG, &dj, "IAV_IOC_DECODE_JPEG") != 0) {
            bad++;
            break;
        }
        for (tries = 0; tries < 2000; tries++) {
            memset(&st, 0, sizeof(st));
            st.non_block_flag = 1;
            if (dev.call(IAV_IOC_QUERY_DECODE_STATUS, &st, "IAV_IOC_QUERY_DECODE_STATUS") != 0) {
                break;
            }
            if (st.decoded_still_pic_info.width != 0 && st.decoded_still_pic_info.height != 0) {
                break;
            }
            usleep(500);
        }
        const struct iav_yuv_cap &y = st.decoded_still_pic_info;
        if (tries >= 2000 || y.width != w || y.height != h) {
            fprintf(stderr, "no decoded picture of %ux%u\n", w, h);
            bad++;
            break;
        }
        /* Cooper alloc_input_buf / alloc_output_buf, then GDMA the decoded planes into the input block */
        const unsigned long ySize = (unsigned long)y.pitch * h, uvSize = (unsigned long)y.pitch * (h / 2);
        if (allocBlock(dev, ySize + uvSize, in) != 0 || allocBlock(dev, OutUv + OutUv / 2, out) != 0) {
            bad++;
            break;
        }
        fill(bytes(out), 0, OutUv, PadY);
        fill(bytes(out), OutUv, OutUv / 2, PadUv);
        long long t0 = nowNs();
        if (gdma(dev, IAV_PART_DEC_BSB, y.y_addr_offset, in.pid, 0, y.pitch, w, h) != 0 ||
            gdma(dev, IAV_PART_DEC_BSB, y.uv_addr_offset, in.pid, ySize, y.pitch, w, h / 2) != 0 ||
            syncBlock(dev, in.pid, 0, ySize + uvSize, IAV_CACHE_CLEAN) != 0) {
            bad++;
            freeBlock(dev, in);
            freeBlock(dev, out);
            break;
        }
        long long t1 = nowNs();
        struct iav_img_scale s = scaleArg(in.pid, w, h, y.pitch, out.pid);

        s.non_block_flag = waitMode ? 1 : 0;
        if (dev.call(IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE") != 0) {
            bad++;
            freeBlock(dev, in);
            freeBlock(dev, out);
            break;
        }
        if (waitMode) {
            struct iav_wait_img_scale ws;

            memset(&ws, 0, sizeof(ws));
            ws.task_id = s.task_id;
            if (dev.call(IAV_IOC_WAIT_IMG_SCALE, &ws, "IAV_IOC_WAIT_IMG_SCALE") != 0) {
                bad++;
            }
        }
        if (syncBlock(dev, out.pid, 0, OutUv + OutUv / 2, IAV_CACHE_INVALIDATE) != 0) {
            bad++;
        }
        long long t2 = nowNs();
        {
            std::vector<unsigned char> o((size_t)WinW * WinH * 3 / 2);
            std::vector<unsigned char> plane((size_t)w);
            unsigned long long inY = 1469598103934665603ULL, decY = 1469598103934665603ULL;
            bool pad = padIntact(bytes(out));
            bool copied = true;

            for (unsigned int r = 0; r < WinH; r++) {
                windowCopy(&o[(size_t)r * WinW], bytes(out) + (unsigned long)r * OutPitch, WinW);
            }
            for (unsigned int r = 0; r < WinH / 2; r++) {
                windowCopy(&o[(size_t)WinW * WinH + (size_t)r * WinW], bytes(out) + OutUv + (unsigned long)r * OutPitch, WinW);
            }
            /* the GDMA copy: every row of both planes in the block equals the decoder's */
            for (unsigned int r = 0; r < h + h / 2 && copied; r++) {
                std::vector<unsigned char> a2((size_t)w);
                unsigned long src = r < h ? y.y_addr_offset + (unsigned long)r * y.pitch :
                                            y.uv_addr_offset + (unsigned long)(r - h) * y.pitch;

                windowCopy(&plane[0], win + src, w);
                windowCopy(&a2[0], bytes(in) + (unsigned long)r * y.pitch, w);
                copied = plane == a2;
                if (r < h) {
                    decY = decY * 1099511628211ULL ^ frameHash(&plane[0], w);
                    inY = inY * 1099511628211ULL ^ frameHash(&a2[0], w);
                }
            }
            Row("arena").add("pass", (long long)pass).add("mode", waitMode ? "wait" : "block")
                .add("in_width", (long long)w).add("in_height", (long long)h).add("in_pitch", (long long)y.pitch)
                .add("in_pid_is_anon", (long long)((in.pid & 0xffu) == 0xffu)).add("in_length", (long long)in.length)
                .add("out_length", (long long)out.length)
                .add("in_offset_aligned", (long long)((in.offset & 0xfffful) == 0))
                .add("gdma_exact", (long long)(copied ? 1 : 0)).add("in_y_hash", (long long)inY)
                .add("dec_y_hash", (long long)decY)
                .add("y_hash", (long long)frameHash(&o[0], (size_t)WinW * WinH))
                .add("uv_hash", (long long)frameHash(&o[(size_t)WinW * WinH], (size_t)WinW * WinH / 2))
                .add("pad_intact", (long long)(pad ? 1 : 0)).add("gdma_us", (double)(t1 - t0) / 1000.0)
                .add("scale_us", (double)(t2 - t1) / 1000.0).print();
            if (!pad || !copied) {
                bad++;
            }
        }
        if (freeBlock(dev, in) != 0 || freeBlock(dev, out) != 0) {
            bad++;
        }
        done++;
    }
    rc = bad == 0 && done == repeat ? 0 : 1;
done:
    if (created && dev.callValue(IAV_IOC_DESTROY_DECODER, 0, "IAV_IOC_DESTROY_DECODER") != 0) {
        rc = 1;
    }
    if (entered && dev.callValue(IAV_IOC_LEAVE_DECODE_MODE, 0, "IAV_IOC_LEAVE_DECODE_MODE") != 0) {
        rc = 1;
    }
    Row("summary").add("passes", (long long)done).add("failed", (long long)bad).print();
    return rc;
}

/* ---- probes ---- */

long g_probes = 0, g_ok = 0;

/* A call that must give rc (0) or fail with errno want. */
void expect(const char *set, const char *what, int r, int want)
{
    int e = r != 0 ? errno : 0;
    bool ok = want == 0 ? r == 0 : (r != 0 && e == want);

    g_probes++;
    g_ok += ok ? 1 : 0;
    Row("probe").add("set", set).add("case", what).add("rc", (long long)r).add("errno", (long long)e)
        .add("expected_errno", (long long)want).add("as_expected", (long long)(ok ? 1 : 0)).print();
}

int freePid(Device &dev, unsigned int pid)
{
    struct iav_free_mem_part f;

    memset(&f, 0, sizeof(f));
    f.pid = pid;
    return dev.call(IAV_IOC_FREE_MEM_PART, &f, "IAV_IOC_FREE_MEM_PART", true);
}

int anonCall(Device &dev, unsigned long cmd, unsigned int pid, const char *name)
{
    struct iav_anon_mem_part p;

    memset(&p, 0, sizeof(p));
    p.pid = pid;
    return dev.call(cmd, &p, name, true);
}

int queryPid(Device &dev, unsigned int pid, unsigned long *addr, unsigned long *len)
{
    struct iav_querymem q;

    memset(&q, 0, sizeof(q));
    q.mid = IAV_MEM_PARTITION;
    q.arg.partition.pid = pid;
    if (dev.call(IAV_IOC_QUERY_MEMBLOCK, &q, "IAV_IOC_QUERY_MEMBLOCK", true) != 0) {
        return -1;
    }
    *addr = q.arg.partition.mem.addr;
    *len = q.arg.partition.mem.length;
    return 0;
}

/* The rules of the bare-metal driver for anonymous partitions; the errnos are compared with a native run. */
int probeLife(Device &dev)
{
    struct iav_alloc_mem_part a;
    Block b;
    unsigned long addr = 0, len = 0;
    void *m;
    int r;

    memset(&a, 0, sizeof(a));
    a.length = 0;
    a.from_private = 1;
    a.accessibility = IAV_MEM_PART_ACS_ARM | IAV_MEM_PART_ACS_DSP;
    expect("life", "alloc_zero_length", dev.call(IAV_IOC_ALLOC_ANON_MEM_PART, &a, "alloc", true), EINVAL);
    expect("life", "alloc_4k", allocBlock(dev, 4096, b, true), 0);
    r = queryPid(dev, b.pid, &addr, &len);
    expect("life", "query_own_block", r, 0);
    expect("life", "query_matches_alloc", (r == 0 && addr == b.offset && len == b.length) ? 0 : -1, 0);
    expect("life", "free_while_mapped", freePid(dev, b.pid), EFAULT);
    expect("life", "unregister_while_mapped", anonCall(dev, IAV_IOC_UNREGISTER_ANON_MEM_PART, b.pid, "unregister"), EINVAL);
    m = mmap(NULL, b.length * 2, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)b.offset);
    expect("life", "map_twice_the_length_ring", m == MAP_FAILED ? -1 : 0, 0);
    if (m != MAP_FAILED) {
        munmap(m, b.length * 2);
    }
    errno = 0;
    m = mmap(NULL, b.length * 3, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)b.offset);
    expect("life", "map_three_times_the_length", m == MAP_FAILED ? -1 : 0, EINVAL);
    if (m != MAP_FAILED) {
        munmap(m, b.length * 3);
    }
    munmap(b.map, b.length);
    b.map = MAP_FAILED;
    expect("life", "register_own_block", anonCall(dev, IAV_IOC_REGISTER_ANON_MEM_PART, b.pid, "register"), 0);
    expect("life", "unregister_unmapped", anonCall(dev, IAV_IOC_UNREGISTER_ANON_MEM_PART, b.pid, "unregister"), 0);
    expect("life", "free_unmapped", freePid(dev, b.pid), 0);
    expect("life", "free_twice", freePid(dev, b.pid), EINVAL);
    expect("life", "query_freed", queryPid(dev, b.pid, &addr, &len), EINVAL);
    expect("life", "register_freed", anonCall(dev, IAV_IOC_REGISTER_ANON_MEM_PART, b.pid, "register"), EINVAL);
    expect("life", "unregister_freed", anonCall(dev, IAV_IOC_UNREGISTER_ANON_MEM_PART, b.pid, "unregister"), EINVAL);
    expect("life", "register_named_partition", anonCall(dev, IAV_IOC_REGISTER_ANON_MEM_PART, IAV_PART_DEC_BSB, "register"),
           EINVAL);
    {
        /* a block another descriptor registered outlives the descriptor that allocated it */
        int fd2 = ::open("/dev/iav", O_RDWR);
        struct iav_alloc_mem_part c;

        memset(&c, 0, sizeof(c));
        c.length = 4096;
        c.from_private = 1;
        c.accessibility = IAV_MEM_PART_ACS_ARM | IAV_MEM_PART_ACS_DSP;
        if (fd2 >= 0 && ioctl(fd2, IAV_IOC_ALLOC_ANON_MEM_PART, &c) == 0) {
            expect("life", "register_from_second_descriptor",
                   anonCall(dev, IAV_IOC_REGISTER_ANON_MEM_PART, c.pid, "register"), 0);
            ::close(fd2);
            fd2 = -1;
            expect("life", "query_after_allocator_closed", queryPid(dev, c.pid, &addr, &len), 0);
            expect("life", "free_registered_block", freePid(dev, c.pid), 0);
        } else {
            expect("life", "register_from_second_descriptor", -1, 0);
        }
        if (fd2 >= 0) {
            ::close(fd2);
        }
    }
    Row("summary").add("probe", "life").add("cases", (long long)g_probes).add("as_expected", (long long)g_ok).print();
    return g_ok == g_probes ? 0 : 1;
}

/* Hostile calls a guest's broker must refuse; run only in a guest. */
int probeBad(Device &dev, unsigned int foreign)
{
    Block b, big;
    struct iav_img_scale s;
    unsigned long addr = 0, len = 0;
    unsigned int never = (0x7777u << 8) | 0xffu;
    void *m;

    expect("bad", "alloc_more_than_the_arena", allocBlock(dev, 0x4000000ul, big, true), EFAULT);
    expect("bad", "alloc_largest_block", allocBlock(dev, LargestBlock, big, true), 0);
    expect("bad", "largest_block_length", big.length == LargestBlock ? 0 : -1, 0);
    if (big.map != MAP_FAILED) {
        freeBlock(dev, big);
    }
    expect("bad", "alloc_4k", allocBlock(dev, 0x10000, b, true), 0);
    /* a map that does not start at the block, or longer than twice the block */
    errno = 0;
    m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd(), (off_t)(b.offset + 4096));
    expect("bad", "map_inside_the_block", m == MAP_FAILED ? -1 : 0, EINVAL);
    if (m != MAP_FAILED) {
        munmap(m, 4096);
    }
    /* a scale and a GDMA past the block */
    s = scaleArg(b.pid, 64, 64, 128, b.pid);
    s.output_buf.y_offset = 0x10000;
    s.output_buf.uv_offset = 0x10000 + OutUv;
    expect("bad", "scale_output_past_block", dev.call(IAV_IOC_SET_IMG_SCALE, &s, "scale", true), EINVAL);
    expect("bad", "gdma_past_block", gdma(dev, b.pid, 0, b.pid, 0x10000 - 128, 128, 128, 2, true), EINVAL);
    expect("bad", "sync_past_block", syncBlock(dev, b.pid, 0x8000, 0x9000, IAV_CACHE_CLEAN, true), EINVAL);
    /* an id this guest never had, another guest's id, the pool partition named directly */
    expect("bad", "gdma_never_issued_id", gdma(dev, IAV_PART_DEC_BSB, 0, never, 0, 128, 128, 2, true), EINVAL);
    if (foreign) {
        expect("bad", "gdma_foreign_id", gdma(dev, IAV_PART_DEC_BSB, 0, foreign, 0, 128, 128, 2, true), EINVAL);
    }
    expect("bad", "gdma_pool_partition", gdma(dev, IAV_PART_DEC_BSB, 0, 193, 0, 128, 128, 2, true), EPERM);
    expect("bad", "sync_pool_partition", syncBlock(dev, 193, 0, 4096, IAV_CACHE_CLEAN, true), EPERM);
    /* a physical address into the block */
    s = scaleArg(IAV_PART_DEC_BSB, 64, 64, 128, b.pid);
    s.output_buf.use_phys = 1;
    s.output_buf.y_addr = b.offset;
    s.output_buf.uv_addr = b.offset + OutUv;
    expect("bad", "scale_physical_address", dev.call(IAV_IOC_SET_IMG_SCALE, &s, "scale", true), EPERM);
    /* a freed id */
    freeBlock(dev, b);
    expect("bad", "gdma_freed_id", gdma(dev, IAV_PART_DEC_BSB, 0, b.pid, 0, 128, 128, 2, true), EINVAL);
    expect("bad", "query_freed_id", queryPid(dev, b.pid, &addr, &len), EINVAL);
    expect("bad", "alloc_after_probes", allocBlock(dev, 0x10000, b, true), 0);
    expect("bad", "gdma_good_after_probes", gdma(dev, IAV_PART_DEC_BSB, 0, b.pid, 0, 128, 128, 2, true), 0);
    freeBlock(dev, b);
    Row("summary").add("probe", "bad").add("cases", (long long)g_probes).add("as_expected", (long long)g_ok).print();
    return g_ok == g_probes ? 0 : 1;
}

/* A consumer guest: every arena call is refused, and so is a block of the primary guest named by its id. */
int probeConsumer(Device &dev, unsigned int foreign)
{
    Block b;

    expect("consumer", "alloc", allocBlock(dev, 0x10000, b, true), EPERM);
    expect("consumer", "gdma_into_primary_block", gdma(dev, IAV_PART_DEC_BSB, 0, foreign, 0, 128, 128, 2, true), EPERM);
    expect("consumer", "sync_primary_block", syncBlock(dev, foreign, 0, 4096, IAV_CACHE_CLEAN, true), EPERM);
    expect("consumer", "free_primary_block", freePid(dev, foreign), EPERM);
    Row("summary").add("probe", "consumer").add("cases", (long long)g_probes).add("as_expected", (long long)g_ok).print();
    return g_ok == g_probes ? 0 : 1;
}

const char *arenaInfo()
{
    static char line[256];
    FILE *f = fopen("/sys/class/misc/iav/arena", "r");

    strcpy(line, "none");
    if (f != NULL) {
        if (fgets(line, sizeof(line), f) == NULL) {
            strcpy(line, "none");
        }
        fclose(f);
        line[strcspn(line, "\n")] = 0;
    }
    return line;
}

/* Fill the largest block with a pattern and leave non-blocking scales queued into it; the process then exits. */
int runFill(Device &dev, long jobs)
{
    Block b;
    unsigned char pat[4096];
    long queued = 0;

    if (allocBlock(dev, LargestBlock, b) != 0) {
        return 1;
    }
    for (size_t i = 0; i < sizeof(pat); i++) {
        pat[i] = (unsigned char)(0x5a ^ i);
    }
    for (unsigned long off = 0; off < b.length; off += sizeof(pat)) {
        windowWrite(bytes(b) + off, pat, sizeof(pat));
    }
    for (long k = 0; k < jobs; k++) {
        struct iav_img_scale s = scaleArg(b.pid, 1920, 1080, 1920, b.pid);

        s.output_buf.y_offset = 0x1000000;
        s.output_buf.uv_offset = 0x1000000 + OutUv;
        s.non_block_flag = 1;
        if (dev.call(IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE") == 0) {
            queued++;
        }
    }
    /* the pattern is in the slice: the first and the last page read back as written */
    bool written = true;
    for (unsigned long off : {0ul, b.length - sizeof(pat)}) {
        unsigned char back[sizeof(pat)];

        windowCopy(back, bytes(b) + off, sizeof(back));
        written = written && memcmp(back, pat, sizeof(pat)) == 0;
    }
    Row("fill").add("arena", arenaInfo()).add("length", (long long)b.length)
        .add("pattern_written", (long long)(written ? 1 : 0)).add("jobs_queued", (long long)queued).print();
    /* no WAIT, no unmap, no free: the guest is stopped next, with the jobs in the DSP queue */
    return queued == jobs ? 0 : 1;
}

int runZero(Device &dev)
{
    Block b;
    unsigned char buf[65536];
    long long nonzero = 0;

    if (allocBlock(dev, LargestBlock, b) != 0) {
        return 1;
    }
    for (unsigned long off = 0; off < b.length; off += sizeof(buf)) {
        size_t c = std::min((unsigned long)sizeof(buf), b.length - off);

        windowCopy(buf, bytes(b) + off, c);
        for (size_t i = 0; i < c; i++) {
            nonzero += buf[i] != 0;
        }
    }
    Row("zero").add("arena", arenaInfo()).add("length", (long long)b.length).add("nonzero_bytes", nonzero).print();
    freeBlock(dev, b);
    return 0;
}

/* Threads allocate, map, write their own pattern, check it, register on a second descriptor and free, at once. */
int runStress(Device &dev, long seconds)
{
    std::atomic<long> ops(0), errors(0), corrupt(0);
    std::vector<std::thread> t;
    long long until = nowNs() + seconds * 1000000000LL;

    for (int i = 0; i < 6; i++) {
        t.push_back(std::thread([&, i]() {
            unsigned int seed = 77u + (unsigned int)i;
            Device own;

            if (!own.open()) {
                errors++;
                return;
            }
            while (nowNs() < until) {
                Block b;
                unsigned long size = 4096ul * (1 + rand_r(&seed) % 300);
                unsigned char tag = (unsigned char)(i * 37 + 1);
                std::vector<unsigned char> buf(4096);

                if (allocBlock(i & 1 ? own : dev, size, b, true) != 0) {
                    errors++;
                    continue;
                }
                for (unsigned long off = 0; off < b.length; off += 4096) {
                    memset(&buf[0], tag ^ (unsigned char)(off >> 12), buf.size());
                    windowWrite(bytes(b) + off, &buf[0], buf.size());
                }
                std::this_thread::yield();
                for (unsigned long off = 0; off < b.length; off += 4096) {
                    std::vector<unsigned char> want(4096, tag ^ (unsigned char)(off >> 12));

                    windowCopy(&buf[0], bytes(b) + off, buf.size());
                    if (buf != want) {
                        corrupt++;
                        break;
                    }
                }
                if (anonCall(i & 1 ? dev : own, IAV_IOC_REGISTER_ANON_MEM_PART, b.pid, "register") != 0 ||
                    anonCall(i & 1 ? dev : own, IAV_IOC_UNREGISTER_ANON_MEM_PART, b.pid, "unregister") != 0) {
                    errors++;
                }
                if (freeBlock(i & 1 ? own : dev, b) != 0) {
                    errors++;
                }
                ops++;
            }
        }));
    }
    for (auto &x : t) {
        x.join();
    }
    Row("stress").add("seconds", (long long)seconds).add("cycles", (long long)ops.load())
        .add("errors", (long long)errors.load()).add("corrupt", (long long)corrupt.load()).add("arena", arenaInfo()).print();
    return errors == 0 && corrupt == 0 && ops > 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    const char *jpegName = argString(argc, argv, "--jpeg", NULL);
    const char *probe = argString(argc, argv, "--probe", "");
    const unsigned int foreign = (unsigned int)argValue(argc, argv, "--foreign", 0);
    const long fillJobs = argValue(argc, argv, "--fill", -1);
    const long stress = argValue(argc, argv, "--stress", 0);
    bool zero = false;

    for (int i = 1; i < argc; i++) {
        zero = zero || strcmp(argv[i], "--zero") == 0;
    }
    Device dev;

    if (!dev.open()) {
        return 1;
    }
    if (strcmp(probe, "life") == 0) {
        return probeLife(dev);
    }
    if (strcmp(probe, "bad") == 0 || strcmp(probe, "consumer") == 0 || fillJobs >= 0 || zero || stress > 0) {
        if (strcmp(environment(), "guest") != 0) {
            fprintf(stderr, "this mode runs only in a guest\n");
            return 2;
        }
        if (strcmp(probe, "bad") == 0) {
            return probeBad(dev, foreign);
        }
        if (strcmp(probe, "consumer") == 0) {
            return probeConsumer(dev, foreign ? foreign : ((1u << 8) | 0xffu));
        }
        if (fillJobs >= 0) {
            return runFill(dev, fillJobs);
        }
        if (zero) {
            return runZero(dev);
        }
        return runStress(dev, stress);
    }
    {
        std::vector<unsigned char> jpeg;
        unsigned int w = 0, h = 0;

        if (jpegName == NULL || !readFile(jpegName, jpeg) || jpegSize(jpeg, &w, &h) != 0) {
            fprintf(stderr, "usage: IavRefArena --jpeg <file> [--repeat n] [--mode block|wait] | --probe life|bad|consumer "
                            "| --fill n | --zero | --stress s\n");
            return 2;
        }
        return runPattern(dev, jpeg, w, h, argValue(argc, argv, "--repeat", 1),
                          strcmp(argString(argc, argv, "--mode", "block"), "wait") == 0);
    }
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
