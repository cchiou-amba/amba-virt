/*
 * IavRefPreproc.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, pre-processing class (plan_iav_usb_yolo, Envelope H): what the USB Webcam tab does to a
 * camera frame before the network. One process keeps one JPEG decoder and, for every frame, chains
 *
 *   copy the JPEG into the decode window -> DECODE_JPEG -> SET_IMG_SCALE (non-blocking) -> WAIT_IMG_SCALE
 *   -> read the 640x640 NV12 output buffer out of the window -> NV12 to planar RGB (the network input)
 *
 * with the layout of IavRefScale (JPEG at offset 0, decoder output from 3 MiB, 640x640 NV12 output buffer at 1 MiB,
 * the scaled picture in the window {0, 0, 640, 360} and gray pad around it). The same frames can instead go through the
 * demo's software path (--mode cpu): stb_image decode (the demo's DecodeJpegRgb) and the demo's ResizeLetterbox640,
 * linked from guest-os/apps/amba-virt-demo/YoloGeometry.cxx. --mode capture only dequeues and requeues camera buffers
 * (the camera's own delivered rate). Every stage of every frame is timed with CLOCK_MONOTONIC.
 *
 *   IavRefPreproc --mode hw|cpu|capture --jpeg-dir <dir> --frames <n> [--warmup <n>]
 *   IavRefPreproc --mode hw|cpu|capture --v4l2 <node> --size 1280x720 --frames <n> [--warmup <n>] [--fps 30]
 *   IavRefPreproc --v4l2 <node> --size 1280x720 --save-dir <dir> --save <n>       (store n camera frames as files)
 *   [--expect-chain <hash>]   exit 1 unless the hash chain of the first frame set equals it (file sources)
 *
 * Output: one JSON row per frame ("frame"), then one "summary" row. In file mode the files of the directory are used
 * in name order, cyclically; the summary carries a chain hash over the output hashes of the first cycle, which equals
 * between native and guest runs of the same files. In live mode the V4L2 sequence gaps are the camera frames dropped
 * (the loop processes one frame at a time, as the demo's worker does). Warm-up frames are processed but not counted.
 * Exit 0 when every counted frame was processed without error.
 */

#include "IavRefCommon.hxx"

#include <dirent.h>
#include <linux/videodev2.h>
#include <poll.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb/stb_image.h"
#pragma GCC diagnostic pop

#include "YoloGeometry.hxx"

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
const size_t NV12Bytes = (size_t)OutPitch * OutHeight * 3 / 2;

bool readFile(const std::string &name, std::vector<unsigned char> &out)
{
    FILE *f = fopen(name.c_str(), "rb");
    unsigned char buf[65536];
    size_t n;

    if (f == NULL) {
        fprintf(stderr, "open %s: %s\n", name.c_str(), strerror(errno));
        return false;
    }
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    fclose(f);
    return !out.empty();
}

int jpegSize(const unsigned char *d, size_t len, unsigned int *w, unsigned int *h)
{
    size_t i = 2;

    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return -1;
    }
    while (i + 9 < len) {
        unsigned char m;
        size_t l;

        if (d[i] != 0xFF) {
            return -1;
        }
        m = d[i + 1];
        l = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            *h = ((unsigned int)d[i + 5] << 8) | d[i + 6];
            *w = ((unsigned int)d[i + 7] << 8) | d[i + 8];
            return 0;
        }
        if (m == 0xDA || l < 2) {
            return -1;
        }
        i += 2 + l;
    }
    return -1;
}

double percentile(std::vector<double> v, double p)
{
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[(size_t)((double)(v.size() - 1) * p)];
}

double mean(const std::vector<double> &v)
{
    double s = 0;

    for (size_t i = 0; i < v.size(); i++) {
        s += v[i];
    }
    return v.empty() ? 0 : s / (double)v.size();
}

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

unsigned long long hashBytes(const unsigned char *p, size_t n)
{
    return frameHash((const volatile unsigned char *)p, n);
}

/* NV12 640x640 to planar RGB (BT.601 full range, integer), the network input layout (R plane, G plane, B plane). */
void nv12ToPlanar(const unsigned char *nv12, unsigned char *rgb)
{
    const unsigned char *yp = nv12;
    const unsigned char *uvp = nv12 + (size_t)OutPitch * OutHeight;
    unsigned char *r = rgb, *g = rgb + (size_t)OutPitch * OutHeight, *b = rgb + 2 * (size_t)OutPitch * OutHeight;

    for (unsigned int row = 0; row < OutHeight; row++) {
        const unsigned char *uv = uvp + (size_t)(row / 2) * OutPitch;

        for (unsigned int col = 0; col < OutPitch; col++) {
            int y = yp[(size_t)row * OutPitch + col];
            int u = uv[col & ~1u] - 128;
            int v = uv[(col & ~1u) + 1] - 128;
            int rr = y + ((359 * v) >> 8);
            int gg = y - ((88 * u + 183 * v) >> 8);
            int bb = y + ((454 * u) >> 8);
            size_t o = (size_t)row * OutPitch + col;

            r[o] = (unsigned char)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
            g[o] = (unsigned char)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
            b[o] = (unsigned char)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
        }
    }
}

/* ---- the sources of JPEG frames ---- */

struct Frame
{
    const unsigned char *data;
    size_t len;
    long long seq;      /* V4L2 sequence, -1 for files */
    long long dqNs;     /* when the frame was dequeued */
    bool bad;
};

class FileSource
{
public:
    bool load(const char *dir)
    {
        std::vector<std::string> names;
        DIR *d = opendir(dir);
        struct dirent *e;

        if (d == NULL) {
            fprintf(stderr, "opendir %s: %s\n", dir, strerror(errno));
            return false;
        }
        while ((e = readdir(d)) != NULL) {
            size_t l = strlen(e->d_name);

            if (l > 4 && strcmp(e->d_name + l - 4, ".jpg") == 0) {
                names.push_back(std::string(dir) + "/" + e->d_name);
            }
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (size_t i = 0; i < names.size(); i++) {
            std::vector<unsigned char> b;

            if (!readFile(names[i], b)) {
                return false;
            }
            _files.push_back(b);
        }
        if (_files.empty()) {
            fprintf(stderr, "no .jpg files in %s\n", dir);
            return false;
        }
        return true;
    }

    size_t count() const { return _files.size(); }

    const std::vector<unsigned char> &first() const { return _files[0]; }

    bool next(Frame *f)
    {
        const std::vector<unsigned char> &b = _files[_i % _files.size()];

        f->data = &b[0];
        f->len = b.size();
        f->seq = -1;
        f->dqNs = nowNs();
        f->bad = false;
        _i++;
        return true;
    }

    void done(const Frame &) {}

private:
    std::vector<std::vector<unsigned char> > _files;
    size_t _i = 0;
};

class V4l2Source
{
public:
    V4l2Source() : _fd(-1), _on(false) { memset(_start, 0, sizeof(_start)); memset(_length, 0, sizeof(_length)); }

    ~V4l2Source()
    {
        if (_fd >= 0) {
            enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;

            if (_on) {
                ioctl(_fd, VIDIOC_STREAMOFF, &t);
            }
            for (int i = 0; i < Count; i++) {
                if (_start[i] != NULL) {
                    munmap(_start[i], _length[i]);
                }
            }
            close(_fd);
        }
    }

    bool open(const char *node, unsigned int w, unsigned int h, unsigned int fps)
    {
        struct v4l2_format fmt;
        struct v4l2_streamparm parm;
        struct v4l2_requestbuffers req;
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        _fd = ::open(node, O_RDWR);
        if (_fd < 0) {
            fprintf(stderr, "open %s: %s\n", node, strerror(errno));
            return false;
        }
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = w;
        fmt.fmt.pix.height = h;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(_fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG ||
            fmt.fmt.pix.width != w || fmt.fmt.pix.height != h) {
            fprintf(stderr, "%s does not give MJPEG %ux%u (%s)\n", node, w, h, strerror(errno));
            return false;
        }
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        ioctl(_fd, VIDIOC_S_PARM, &parm);
        _fpsGot = parm.parm.capture.timeperframe.numerator != 0 ?
                  parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator : 0;
        memset(&req, 0, sizeof(req));
        req.count = Count;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(_fd, VIDIOC_REQBUFS, &req) < 0 || req.count != Count) {
            fprintf(stderr, "REQBUFS: %s (count %u)\n", strerror(errno), req.count);
            return false;
        }
        for (int i = 0; i < Count; i++) {
            struct v4l2_buffer b;

            memset(&b, 0, sizeof(b));
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (ioctl(_fd, VIDIOC_QUERYBUF, &b) < 0) {
                fprintf(stderr, "QUERYBUF: %s\n", strerror(errno));
                return false;
            }
            _length[i] = b.length;
            _start[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, b.m.offset);
            if (_start[i] == MAP_FAILED) {
                _start[i] = NULL;
                fprintf(stderr, "mmap buffer: %s\n", strerror(errno));
                return false;
            }
            if (ioctl(_fd, VIDIOC_QBUF, &b) < 0) {
                fprintf(stderr, "QBUF: %s\n", strerror(errno));
                return false;
            }
        }
        if (ioctl(_fd, VIDIOC_STREAMON, &t) < 0) {
            fprintf(stderr, "STREAMON: %s\n", strerror(errno));
            return false;
        }
        _on = true;
        return true;
    }

    long fpsGot() const { return (long)_fpsGot; }

    bool next(Frame *f)
    {
        struct pollfd p = { _fd, POLLIN, 0 };
        struct v4l2_buffer b;

        if (poll(&p, 1, 3000) <= 0) {
            fprintf(stderr, "no camera frame in 3 s\n");
            return false;
        }
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(_fd, VIDIOC_DQBUF, &b) < 0) {
            fprintf(stderr, "DQBUF: %s\n", strerror(errno));
            return false;
        }
        f->dqNs = nowNs();
        f->data = (const unsigned char *)_start[b.index];
        f->len = b.bytesused;
        f->seq = b.sequence;
        f->bad = (b.flags & V4L2_BUF_FLAG_ERROR) != 0 || b.bytesused < 1000;
        _held = b;
        return true;
    }

    void done(const Frame &)
    {
        ioctl(_fd, VIDIOC_QBUF, &_held);
    }

private:
    static const int Count = 4;
    int _fd;
    bool _on;
    void *_start[Count];
    size_t _length[Count];
    struct v4l2_buffer _held;
    unsigned int _fpsGot = 0;
};

/* ---- the hardware path ---- */

class HwPath
{
public:
    HwPath() : _entered(false), _created(false), _win(NULL), _w(0), _h(0) {}

    ~HwPath()
    {
        if (_created) {
            _dev.callValue(IAV_IOC_DESTROY_DECODER, 0, "IAV_IOC_DESTROY_DECODER");
        }
        if (_entered) {
            _dev.callValue(IAV_IOC_LEAVE_DECODE_MODE, 0, "IAV_IOC_LEAVE_DECODE_MODE");
        }
    }

    bool open(unsigned int w, unsigned int h)
    {
        struct iav_decode_mode_config mode;
        struct iav_decoder_info info;

        _w = w;
        _h = h;
        if (!_dev.open() || _dev.partition(IAV_PART_DEC_BSB, &_addr, &_length) != 0) {
            return false;
        }
        if (_length < 0x600000) {
            fprintf(stderr, "decode partition %lu bytes is smaller than the layout\n", _length);
            return false;
        }
        if (!_part.map(_dev.fd(), _addr, _length, PROT_READ | PROT_WRITE)) {
            return false;
        }
        _win = (volatile unsigned char *)_part.bytes();
        windowFill(_win, OutY, (size_t)OutPitch * OutHeight, PadY);
        windowFill(_win, OutUv, (size_t)OutPitch * OutHeight / 2, PadUv);
        memset(&mode, 0, sizeof(mode));
        mode.num_decoder = 1;
        mode.decoder_type[0] = IAV_DEC_TYPE_JPEG;
        mode.max_width = w;
        mode.max_height = h;
        if (_dev.call(IAV_IOC_ENTER_DECODE_MODE, &mode, "IAV_IOC_ENTER_DECODE_MODE") != 0) {
            return false;
        }
        _entered = true;
        memset(&info, 0, sizeof(info));
        info.decoder_id = 0;
        info.decoder_type = IAV_DEC_TYPE_JPEG;
        info.video_width = w;
        info.video_height = h;
        info.bsb_start_offset = 0;
        info.bsb_size = (u32)_length;
        if (_dev.call(IAV_IOC_CREATE_DECODER, &info, "IAV_IOC_CREATE_DECODER") != 0) {
            return false;
        }
        _created = true;
        _out.resize(NV12Bytes);
        return true;
    }

    unsigned long length() const { return _length; }

    struct Times { double copyIn, dec, scale, roWin, roPad, cvt; long polls; unsigned int fmt; int padOk; };

    /* One frame. false on any call failure (a failure is counted by the caller and the loop goes on). */
    bool run(const Frame &f, unsigned char *rgb, Times *t, unsigned long long *hash)
    {
        struct iav_decode_jpeg dj;
        struct iav_decode_status st;
        struct iav_img_scale s;
        long long t0, t1, t2, t3, t4, t5, t6;
        int tries;

        if (f.len > OutY) {
            fprintf(stderr, "JPEG of %zu bytes does not fit before the output buffer\n", f.len);
            return false;
        }
        t0 = nowNs();
        windowWrite(_win, f.data, f.len);
        t1 = nowNs();
        memset(&dj, 0, sizeof(dj));
        dj.decoder_id = 0;
        dj.is_dec_only = 1;
        dj.is_last_frame = 1;
        dj.jpeg_size = (u32)f.len;
        dj.jpeg_start_offset = 0;
        dj.yuv_start_offset = 0x300000;
        dj.yuv_size = (u32)(_length - 0x300000);
        if (_dev.call(IAV_IOC_DECODE_JPEG, &dj, "IAV_IOC_DECODE_JPEG") != 0) {
            return false;
        }
        for (tries = 0; tries < 2000; tries++) {
            memset(&st, 0, sizeof(st));
            st.decoder_id = 0;
            st.non_block_flag = 1;
            if (_dev.call(IAV_IOC_QUERY_DECODE_STATUS, &st, "IAV_IOC_QUERY_DECODE_STATUS") != 0) {
                return false;
            }
            if (st.decoded_still_pic_info.width != 0 && st.decoded_still_pic_info.height != 0) {
                break;
            }
            usleep(500);
        }
        t2 = nowNs();
        const struct iav_yuv_cap &y = st.decoded_still_pic_info;
        if (tries >= 2000 || y.width != _w || y.height != _h) {
            fprintf(stderr, "no decoded picture of %ux%u (got %ux%u after %d polls)\n", _w, _h, y.width, y.height, tries);
            return false;
        }
        memset(&s, 0, sizeof(s));
        s.format = IAV_YUV_FORMAT_YUV420;
        s.input.width = y.width;
        s.input.height = y.height;
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
        s.non_block_flag = 1;
        if (_dev.call(IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE") != 0) {
            return false;
        }
        {
            struct iav_wait_img_scale ws;

            memset(&ws, 0, sizeof(ws));
            ws.task_id = s.task_id;
            if (_dev.call(IAV_IOC_WAIT_IMG_SCALE, &ws, "IAV_IOC_WAIT_IMG_SCALE") != 0) {
                return false;
            }
        }
        t3 = nowNs();
        /* the scaled window: Y rows [0, WinH) and UV rows [0, WinH / 2) of the buffer, then the pad */
        windowCopy(&_out[0], _win + OutY, (size_t)OutPitch * WinH);
        windowCopy(&_out[(size_t)OutPitch * OutHeight], _win + OutUv, (size_t)OutPitch * (WinH / 2));
        t4 = nowNs();
        windowCopy(&_out[(size_t)OutPitch * WinH], _win + OutY + (size_t)OutPitch * WinH, (size_t)OutPitch * (OutHeight - WinH));
        windowCopy(&_out[(size_t)OutPitch * OutHeight + (size_t)OutPitch * (WinH / 2)],
                   _win + OutUv + (size_t)OutPitch * (WinH / 2), (size_t)OutPitch * (OutHeight / 2 - WinH / 2));
        t5 = nowNs();
        nv12ToPlanar(&_out[0], rgb);
        t6 = nowNs();
        int pad = 1;

        for (size_t i = (size_t)OutPitch * WinH; i < (size_t)OutPitch * OutHeight && pad; i++) {
            pad = _out[i] == PadY;
        }
        for (size_t i = (size_t)OutPitch * OutHeight + (size_t)OutPitch * (WinH / 2); i < NV12Bytes && pad; i++) {
            pad = _out[i] == PadUv;
        }
        *hash = hashBytes(&_out[0], NV12Bytes);
        t->copyIn = (double)(t1 - t0) / 1000.0;
        t->dec = (double)(t2 - t1) / 1000.0;
        t->scale = (double)(t3 - t2) / 1000.0;
        t->roWin = (double)(t4 - t3) / 1000.0;
        t->roPad = (double)(t5 - t4) / 1000.0;
        t->cvt = (double)(t6 - t5) / 1000.0;
        t->polls = tries;
        t->fmt = (unsigned int)y.format;
        t->padOk = pad;
        return true;
    }

private:
    Device _dev;
    Mapping _part;
    unsigned long _addr = 0, _length = 0;
    bool _entered, _created;
    volatile unsigned char *_win;
    unsigned int _w, _h;
    std::vector<unsigned char> _out;
};

/* The demo's DecodeJpegRgb (AmbaVirtDemo.cxx), unchanged. */
uint8_t *DecodeJpegRgb(const uint8_t *jpeg, size_t jpeg_len, int *width, int *height)
{
    if (!jpeg || jpeg_len == 0 || !width || !height)
        return nullptr;

    int channels = 0;
    uint8_t *rgb = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(jpeg),
                                         static_cast<int>(jpeg_len),
                                         width, height, &channels, 3);
    if (!rgb || *width <= 0 || *height <= 0) {
        if (rgb)
            stbi_image_free(rgb);
        *width = 0;
        *height = 0;
        return nullptr;
    }
    return rgb;
}

bool parseSize(const char *s, unsigned int *w, unsigned int *h)
{
    return s != NULL && sscanf(s, "%ux%u", w, h) == 2 && *w > 0 && *h > 0;
}

} // namespace

int main(int argc, char **argv)
{
    const char *modeName = argString(argc, argv, "--mode", "hw");
    const char *dir = argString(argc, argv, "--jpeg-dir", NULL);
    const char *node = argString(argc, argv, "--v4l2", NULL);
    const char *saveDir = argString(argc, argv, "--save-dir", NULL);
    const char *expectChain = argString(argc, argv, "--expect-chain", NULL);
    const long frames = argValue(argc, argv, "--frames", 300);
    const long warmup = argValue(argc, argv, "--warmup", 10);
    const long save = argValue(argc, argv, "--save", 0);
    const long fps = argValue(argc, argv, "--fps", 30);
    const bool hw = strcmp(modeName, "hw") == 0;
    const bool cpu = strcmp(modeName, "cpu") == 0;
    const bool capOnly = strcmp(modeName, "capture") == 0;
    unsigned int w = 0, h = 0;
    FileSource files;
    V4l2Source cam;
    HwPath hwp;
    std::vector<unsigned char> rgb((size_t)3 * 640 * 640, 114);
    std::vector<double> vCopy, vDec, vScale, vRoWin, vRoPad, vRo, vCvt, vSum, vCpuDec, vCpuRes, vInterval, vBytes;
    std::vector<unsigned long long> chain;
    long bad = 0, padBroken = 0, badFrames = 0, dropped = 0, done = 0, polls = 0;
    long long lastDq = 0, lastSeq = -1, firstDq = 0, endDq = 0;
    unsigned int fmtSeen = 0;
    bool live = node != NULL;
    int rc = 0;

    if ((!hw && !cpu && !capOnly) || (dir == NULL) == (node == NULL) || frames <= 0) {
        fprintf(stderr, "usage: IavRefPreproc --mode hw|cpu|capture (--jpeg-dir <dir> | --v4l2 <node> --size WxH) "
                "--frames <n> [--warmup <n>] [--fps <n>] [--expect-chain <hash>]\n"
                "       IavRefPreproc --v4l2 <node> --size WxH --save-dir <dir> --save <n>\n");
        return 2;
    }
    if (live && !parseSize(argString(argc, argv, "--size", NULL), &w, &h)) {
        fprintf(stderr, "--size WxH is required with --v4l2\n");
        return 2;
    }
    if (live) {
        if (!cam.open(node, w, h, (unsigned int)fps)) {
            return 1;
        }
    } else if (!files.load(dir)) {
        return 1;
    }
    if (saveDir != NULL) {
        /* store camera frames as files (the sets the native runs replay) */
        for (long i = 0; i < save + warmup; i++) {
            Frame f;
            char name[512];
            FILE *o;

            if (!cam.next(&f)) {
                return 1;
            }
            if (i >= warmup && !f.bad) {
                snprintf(name, sizeof(name), "%s/f%03ld.jpg", saveDir, i - warmup);
                o = fopen(name, "wb");
                if (o == NULL || fwrite(f.data, 1, f.len, o) != f.len) {
                    fprintf(stderr, "write %s: %s\n", name, strerror(errno));
                    return 1;
                }
                fclose(o);
            }
            cam.done(f);
        }
        return 0;
    }
    if (!live) {
        /* the size comes from the first file */
        if (jpegSize(&files.first()[0], files.first().size(), &w, &h) != 0) {
            fprintf(stderr, "the first file is not a baseline JPEG\n");
            return 1;
        }
    }
    if (hw && !hwp.open(w, h)) {
        return 1;
    }

    for (long i = 0; i < frames + warmup; i++) {
        Frame f;
        const bool counted = i >= warmup;
        long long t0, t1;

        if (live ? !cam.next(&f) : !files.next(&f)) {
            rc = 1;
            break;
        }
        if (counted) {
            if (done == 0) {
                firstDq = f.dqNs;
            } else {
                vInterval.push_back((double)(f.dqNs - lastDq) / 1000.0);
            }
            if (lastSeq >= 0 && f.seq > lastSeq + 1) {
                dropped += f.seq - lastSeq - 1;
            }
            lastDq = f.dqNs;
            endDq = f.dqNs;
        }
        lastSeq = f.seq >= 0 ? f.seq : lastSeq;
        if (f.bad) {
            if (counted) {
                badFrames++;
            }
            if (live) {
                cam.done(f);
            }
            continue;
        }
        if (capOnly) {
            if (counted) {
                done++;
                vBytes.push_back((double)f.len);
                Row("frame").add("i", (long long)(i - warmup)).add("seq", f.seq).add("bytes", (long long)f.len)
                    .add("interval_us", vInterval.empty() ? 0.0 : vInterval.back()).print();
            }
            if (live) {
                cam.done(f);
            }
            continue;
        }
        if (hw) {
            HwPath::Times t;
            unsigned long long hsh = 0;
            bool ok = hwp.run(f, &rgb[0], &t, &hsh);

            if (live) {
                cam.done(f);
            }
            if (!counted) {
                continue;
            }
            if (!ok) {
                bad++;
                rc = 1;
                Row("frame").add("i", (long long)(i - warmup)).add("seq", f.seq).add("bytes", (long long)f.len)
                    .add("failed", (long long)1).print();
                continue;
            }
            done++;
            polls += t.polls;
            fmtSeen = t.fmt;
            padBroken += t.padOk ? 0 : 1;
            vCopy.push_back(t.copyIn);
            vDec.push_back(t.dec);
            vScale.push_back(t.scale);
            vRoWin.push_back(t.roWin);
            vRoPad.push_back(t.roPad);
            vRo.push_back(t.roWin + t.roPad);
            vCvt.push_back(t.cvt);
            vSum.push_back(t.copyIn + t.dec + t.scale + t.roWin + t.roPad + t.cvt);
            vBytes.push_back((double)f.len);
            if (!live && chain.size() < files.count()) {
                chain.push_back(hsh);
            }
            Row("frame").add("i", (long long)(i - warmup)).add("seq", f.seq).add("bytes", (long long)f.len)
                .add("copy_in_us", t.copyIn).add("decode_us", t.dec).add("scale_us", t.scale)
                .add("readout_win_us", t.roWin).add("readout_pad_us", t.roPad).add("cvt_us", t.cvt)
                .add("sum_us", vSum.back()).add("polls", (long long)t.polls).add("pad_intact", (long long)t.padOk)
                .add("out_hash", (long long)hsh).print();
        } else {
            int iw = 0, ih = 0;
            uint8_t *img;
            double dUs, rUs;

            t0 = nowNs();
            img = DecodeJpegRgb(f.data, f.len, &iw, &ih);
            t1 = nowNs();
            dUs = (double)(t1 - t0) / 1000.0;
            if (img == NULL || ResizeLetterbox640(img, iw, ih, &rgb[0]) < 0) {
                if (img != NULL) {
                    stbi_image_free(img);
                }
                if (live) {
                    cam.done(f);
                }
                if (counted) {
                    bad++;
                    rc = 1;
                }
                continue;
            }
            rUs = (double)(nowNs() - t1) / 1000.0;
            stbi_image_free(img);
            unsigned long long hsh = hashBytes(&rgb[0], rgb.size());

            if (live) {
                cam.done(f);
            }
            if (!counted) {
                continue;
            }
            done++;
            vCpuDec.push_back(dUs);
            vCpuRes.push_back(rUs);
            vSum.push_back(dUs + rUs);
            vBytes.push_back((double)f.len);
            if (!live && chain.size() < files.count()) {
                chain.push_back(hsh);
            }
            Row("frame").add("i", (long long)(i - warmup)).add("seq", f.seq).add("bytes", (long long)f.len)
                .add("cpu_decode_us", dUs).add("cpu_resize_us", rUs).add("sum_us", dUs + rUs)
                .add("out_hash", (long long)hsh).print();
        }
    }

    unsigned long long chainHash = 1469598103934665603ULL;

    for (size_t i = 0; i < chain.size(); i++) {
        chainHash ^= chain[i];
        chainHash *= 1099511628211ULL;
        chainHash ^= chainHash >> 29;
    }
    {
        char hex[32];
        const double span = (double)(endDq - firstDq) / 1e9; /* seconds between the first and last counted dequeue */
        Row r("summary");

        snprintf(hex, sizeof(hex), "%016llx", chainHash);
        r.add("mode", modeName).add("source", live ? "v4l2" : "files").add("width", (long long)w).add("height", (long long)h)
            .add("frames_requested", (long long)frames).add("frames_done", (long long)done).add("failed", (long long)bad)
            .add("bad_frames", (long long)badFrames).add("dropped_by_camera", (long long)dropped)
            .add("pad_broken", (long long)padBroken).add("bytes_p50", percentile(vBytes, 0.5))
            .add("fps_requested", (long long)(live ? fps : 0)).add("fps_granted", (long long)(live ? cam.fpsGot() : 0));
        if (live && done > 1 && span > 0) {
            r.add("delivered_fps", (double)(done - 1) / span).add("interval_p50_us", percentile(vInterval, 0.5))
                .add("interval_p99_us", percentile(vInterval, 0.99)).add("interval_max_us", percentile(vInterval, 1.0));
        }
        r.add("chain_hash", hex).print();
        Row r2("stages");

        if (hw) {
            r2.add("copy_in_p50_us", percentile(vCopy, 0.5)).add("copy_in_p99_us", percentile(vCopy, 0.99))
                .add("decode_p50_us", percentile(vDec, 0.5)).add("decode_p99_us", percentile(vDec, 0.99))
                .add("scale_p50_us", percentile(vScale, 0.5)).add("scale_p99_us", percentile(vScale, 0.99))
                .add("readout_p50_us", percentile(vRo, 0.5)).add("readout_p99_us", percentile(vRo, 0.99))
                .add("readout_win_p50_us", percentile(vRoWin, 0.5)).add("cvt_p50_us", percentile(vCvt, 0.5))
                .add("cvt_p99_us", percentile(vCvt, 0.99)).add("sum_p50_us", percentile(vSum, 0.5))
                .add("sum_p99_us", percentile(vSum, 0.99)).add("sum_mean_us", mean(vSum))
                .add("polls_mean", done > 0 ? (double)polls / (double)done : 0.0)
                .add("decoded_format", (long long)fmtSeen);
        } else if (cpu) {
            r2.add("cpu_decode_p50_us", percentile(vCpuDec, 0.5)).add("cpu_decode_p99_us", percentile(vCpuDec, 0.99))
                .add("cpu_resize_p50_us", percentile(vCpuRes, 0.5)).add("cpu_resize_p99_us", percentile(vCpuRes, 0.99))
                .add("sum_p50_us", percentile(vSum, 0.5)).add("sum_p99_us", percentile(vSum, 0.99))
                .add("sum_mean_us", mean(vSum));
        }
        r2.print();
        if (expectChain != NULL && strcmp(expectChain, hex) != 0) {
            fprintf(stderr, "chain hash %s differs from the expected %s\n", hex, expectChain);
            rc = 1;
        }
    }
    if (done != frames || padBroken != 0 || bad != 0) {
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
