/*
 * UsbIavDecoder.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "UsbIavDecoder.hxx"
#include "UsbJpeg.hxx"

#include "IavRefCommon.hxx"

#include <string.h>
#include <unistd.h>

namespace {

const unsigned long kOutY = 0x100000;
const unsigned int kOutPitch = 640;
const unsigned int kOutHeight = 640;
const unsigned long kOutUv = kOutY + static_cast<unsigned long>(kOutPitch) * kOutHeight;
const unsigned int kWinW = 640;
const unsigned int kWinH = 360;
const unsigned long kYuvStart = 0x200000;
const unsigned long kMinPartition = 0x600000;
const unsigned char kPadY = 114;
const unsigned char kPadUv = 128;
const size_t kNv12Bytes = static_cast<size_t>(kOutPitch) * kOutHeight * 3 / 2;
const int kMaxPolls = 2000;

double ms(long long a, long long b)
{
    return static_cast<double>(b - a) / 1e6;
}

void windowFill(volatile unsigned char *win, unsigned long off, size_t n, unsigned char v)
{
    unsigned char buf[4096];
    size_t done = 0;

    memset(buf, v, sizeof(buf));
    while (done < n) {
        size_t c = n - done < sizeof(buf) ? n - done : sizeof(buf);

        iavref::windowWrite(win + off + done, buf, c);
        done += c;
    }
}

} // namespace

struct UsbIavDecoder::Impl {
    Impl() : entered(false), created(false), win(nullptr), addr(0), length(0) {}

    iavref::Device dev;
    iavref::Mapping part;
    bool entered;
    bool created;
    volatile unsigned char *win;
    unsigned long addr;
    unsigned long length;
    std::vector<unsigned char> nv12;
};

UsbIavDecoder::UsbIavDecoder() : _impl(new Impl), _created(false), _w(0), _h(0)
{
}

UsbIavDecoder::~UsbIavDecoder()
{
    close();
    delete _impl;
}

int UsbIavDecoder::open(uint32_t width, uint32_t height)
{
    struct iav_decode_mode_config mode;
    struct iav_decoder_info info;
    Impl &m = *_impl;

    close();
    if (!UsbJpeg::aspectIs16x9(width, height))
        return -EINVAL;
    _w = width;
    _h = height;
    if (!m.dev.open())
        return -ENODEV;
    if (m.dev.partition(IAV_PART_DEC_BSB, &m.addr, &m.length) != 0 || m.length < kMinPartition) {
        close();
        return -ENODEV;
    }
    if (!m.part.map(m.dev.fd(), m.addr, m.length, PROT_READ | PROT_WRITE)) {
        close();
        return -ENOMEM;
    }
    m.win = const_cast<volatile unsigned char *>(m.part.bytes());
    windowFill(m.win, kOutY, static_cast<size_t>(kOutPitch) * kOutHeight, kPadY);
    windowFill(m.win, kOutUv, static_cast<size_t>(kOutPitch) * kOutHeight / 2, kPadUv);
    memset(&mode, 0, sizeof(mode));
    mode.num_decoder = 1;
    mode.decoder_type[0] = IAV_DEC_TYPE_JPEG;
    mode.max_width = width;
    mode.max_height = height;
    if (m.dev.call(IAV_IOC_ENTER_DECODE_MODE, &mode, "IAV_IOC_ENTER_DECODE_MODE") != 0) {
        int e = errno;

        close();
        return -(e ? e : EIO);
    }
    m.entered = true;
    memset(&info, 0, sizeof(info));
    info.decoder_id = 0;
    info.decoder_type = IAV_DEC_TYPE_JPEG;
    info.video_width = width;
    info.video_height = height;
    info.bsb_start_offset = 0;
    info.bsb_size = static_cast<u32>(m.length);
    if (m.dev.call(IAV_IOC_CREATE_DECODER, &info, "IAV_IOC_CREATE_DECODER") != 0) {
        int e = errno;

        close();
        return -(e ? e : EIO);
    }
    m.created = true;
    m.nv12.assign(kNv12Bytes, 0);
    memset(&m.nv12[0], kPadY, static_cast<size_t>(kOutPitch) * kOutHeight);
    memset(&m.nv12[static_cast<size_t>(kOutPitch) * kOutHeight], kPadUv, static_cast<size_t>(kOutPitch) * kOutHeight / 2);
    _created = true;
    return 0;
}

void UsbIavDecoder::close()
{
    Impl &m = *_impl;

    if (m.created)
        m.dev.callValue(IAV_IOC_DESTROY_DECODER, 0, "IAV_IOC_DESTROY_DECODER");
    if (m.entered)
        m.dev.callValue(IAV_IOC_LEAVE_DECODE_MODE, 0, "IAV_IOC_LEAVE_DECODE_MODE");
    m.created = false;
    m.entered = false;
    /* the mapping and the descriptor go with the Impl members; recreate them so open() starts clean */
    delete _impl;
    _impl = new Impl;
    _created = false;
}

void UsbIavDecoder::padFill(uint8_t *planarRgb)
{
    memset(planarRgb, kPadY, static_cast<size_t>(3) * kOutPitch * kOutHeight);
}

int UsbIavDecoder::run(const uint8_t *jpeg, size_t len, uint8_t *planarRgb, Times *times)
{
    Impl &m = *_impl;
    struct iav_decode_jpeg dj;
    struct iav_decode_status st;
    struct iav_img_scale s;
    struct iav_wait_img_scale ws;
    long long t0, t1, t2, t3, t4, t5;
    int tries;

    if (!_created || !jpeg || len == 0 || !planarRgb)
        return -EINVAL;
    if (len > kOutY)
        return -EMSGSIZE;
    t0 = iavref::nowNs();
    iavref::windowWrite(m.win, jpeg, len);
    t1 = iavref::nowNs();
    memset(&dj, 0, sizeof(dj));
    dj.decoder_id = 0;
    dj.is_dec_only = 1;
    dj.is_last_frame = 1;
    dj.jpeg_size = static_cast<u32>(len);
    dj.jpeg_start_offset = 0;
    dj.yuv_start_offset = kYuvStart;
    dj.yuv_size = static_cast<u32>(m.length - kYuvStart);
    if (m.dev.call(IAV_IOC_DECODE_JPEG, &dj, "IAV_IOC_DECODE_JPEG") != 0)
        return -(errno ? errno : EIO);
    for (tries = 0; tries < kMaxPolls; tries++) {
        memset(&st, 0, sizeof(st));
        st.decoder_id = 0;
        st.non_block_flag = 1;
        if (m.dev.call(IAV_IOC_QUERY_DECODE_STATUS, &st, "IAV_IOC_QUERY_DECODE_STATUS") != 0)
            return -(errno ? errno : EIO);
        if (st.decoded_still_pic_info.width != 0 && st.decoded_still_pic_info.height != 0)
            break;
        usleep(500);
    }
    t2 = iavref::nowNs();
    const struct iav_yuv_cap &y = st.decoded_still_pic_info;
    if (tries >= kMaxPolls || y.width != _w || y.height != _h)
        return -EIO;
    memset(&s, 0, sizeof(s));
    /* the decoder reports the picture in the format of the JPEG's chroma sampling (4:2:0, or 4:2:2 for 2x1); the scaler
     * reads it as it is and writes the 4:2:0 NV12 canvas the rest of the path expects */
    if (y.format != IAV_YUV_FORMAT_YUV420 && y.format != IAV_YUV_FORMAT_YUV422)
        return -EIO;
    s.format = y.format == IAV_YUV_FORMAT_YUV422 ? IAV_YUV_FORMAT_YUV422 : IAV_YUV_FORMAT_YUV420;
    if (y.format == IAV_YUV_FORMAT_YUV422) {
        s.output_format_enable = 1;
        s.output_format = IAV_YUV_FORMAT_YUV420;
    }
    s.input.width = y.width;
    s.input.height = y.height;
    s.output.width = kWinW;
    s.output.height = kWinH;
    s.input_buf.pid = IAV_PART_DEC_BSB;
    s.input_buf.y_offset = y.y_addr_offset;
    s.input_buf.uv_offset = y.uv_addr_offset;
    s.input_buf.pitch = static_cast<u16>(y.pitch);
    s.input_buf.height = static_cast<u16>(y.height);
    s.output_buf.pid = IAV_PART_DEC_BSB;
    s.output_buf.y_offset = kOutY;
    s.output_buf.uv_offset = kOutUv;
    s.output_buf.pitch = kOutPitch;
    s.output_buf.height = kOutHeight;
    s.input_skip_cache_sync = 1;
    s.output_skip_cache_sync = 1;
    s.non_block_flag = 1;
    if (m.dev.call(IAV_IOC_SET_IMG_SCALE, &s, "IAV_IOC_SET_IMG_SCALE") != 0)
        return -(errno ? errno : EIO);
    memset(&ws, 0, sizeof(ws));
    ws.task_id = s.task_id;
    if (m.dev.call(IAV_IOC_WAIT_IMG_SCALE, &ws, "IAV_IOC_WAIT_IMG_SCALE") != 0)
        return -(errno ? errno : EIO);
    t3 = iavref::nowNs();
    /* the scaled window: Y rows [0, kWinH) and UV rows [0, kWinH / 2); the pad below is constant */
    iavref::windowCopy(&m.nv12[0], m.win + kOutY, static_cast<size_t>(kOutPitch) * kWinH);
    iavref::windowCopy(&m.nv12[static_cast<size_t>(kOutPitch) * kOutHeight], m.win + kOutUv,
                       static_cast<size_t>(kOutPitch) * (kWinH / 2));
    t4 = iavref::nowNs();
    UsbJpeg::nv12ToPlanar(&m.nv12[0], planarRgb, kWinH);
    t5 = iavref::nowNs();
    if (times) {
        times->copyMs = ms(t0, t1) + ms(t3, t4);
        times->decodeMs = ms(t1, t2);
        times->scaleMs = ms(t2, t3);
        times->convertMs = ms(t4, t5);
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
