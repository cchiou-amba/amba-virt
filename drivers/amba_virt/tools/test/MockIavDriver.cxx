/*
 * MockIavDriver.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "MockIavDriver.hxx"

#include "iav_policy.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

#include <algorithm>

#include <iav_ioctl.h>
#include <ambas_imgproc_ioctl_arch.h>

namespace {

const unsigned long Allowed[] = {
    IAV_IOC_GET_IAV_STATE,        IAV_IOC_GET_CHIP_ID,          IAV_IOC_QUERY_ENC_MODE_CAP,
    IAV_IOC_QUERY_ENC_BUF_CAP,    IAV_IOC_QUERY_IAV_CAP,        IAV_IOC_GET_SYSTEM_RESOURCE,
    IAV_IOC_GET_CHAN_CONFIG,      IAV_IOC_GET_CANVAS_CONFIG,    IAV_IOC_DRV_DSP_INFO,
    IAV_IOC_QUERY_INFO,           IAV_IOC_QUERY_MEMBLOCK,       IAV_IOC_QUERY_DESC,
    IAV_IOC_SET_POLL_EVENT_CFG,   IAV_IOC_CLEAR_POLL_EVENT_CFG, IAV_IOC_WAIT_NEXT_IDSP_FRAME,
};

bool isAllowed(unsigned long cmd)
{
    for (size_t i = 0; i < sizeof(Allowed) / sizeof(Allowed[0]); i++) {
        if (Allowed[i] == cmd) {
            return true;
        }
    }
    return false;
}

const unsigned long PrimaryListed[] = {
#define PRIMARY_ALLOW(name) name,
#include "iav_allow_primary.inc"
#undef PRIMARY_ALLOW
};

bool isPrimaryListed(unsigned long cmd)
{
    for (size_t i = 0; i < sizeof(PrimaryListed) / sizeof(PrimaryListed[0]); i++) {
        if (PrimaryListed[i] == cmd) {
            return true;
        }
    }
    return false;
}

/* The decode calls the primary guest may make with a checked argument (iav_policy.c, version 3). Written out here,
 * apart from the policy, so the mock is an independent judge of what may reach the driver. */
const unsigned long DecodeListed[] = {
    IAV_IOC_CREATE_DECODER,   IAV_IOC_DECODE_JPEG,        IAV_IOC_DECODE_VIDEO,   IAV_IOC_QUERY_DECODE_BSB,
    IAV_IOC_WAIT_DECODE_BSB,  IAV_IOC_QUERY_DECODE_STATUS, IAV_IOC_WAIT_PLAYBACK_EOS,
    IAV_IOC_SET_OVERLAY_INSERT, IAV_IOC_GET_OVERLAY_INSERT,
};

bool isDecodeListed(unsigned long cmd)
{
    for (size_t i = 0; i < sizeof(DecodeListed) / sizeof(DecodeListed[0]); i++) {
        if (DecodeListed[i] == cmd) {
            return true;
        }
    }
    return false;
}

/* The image calls of inventory class W the primary guest may make (iav_policy.c, version 6), written out here apart
 * from the policy; imageViolation() below judges each argument that reaches the driver. */
const unsigned long ImageListed[] = {
    IAV_IOC_SET_IMG_SCALE,       IAV_IOC_WAIT_IMG_SCALE,          IAV_IOC_GET_VIDEO_PROC,
    IAV_IOC_GET_VIN_IDSP_CFG,    IAV_IOC_WAIT_SKIP_IDSP_CFG,      IAV_IOC_GET_WARP_CTRL,
    IAV_IOC_SET_BLUR_INSERT,     IAV_IOC_GET_BLUR_INSERT,         IAV_IOC_GET_YUV_REMAP_INSERT,
    IAV_IOC_GET_FRAME_SYNC_PROC, IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC, IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC,
    IAV_IOC_GET_STREAM_CONFIG,   IAV_IOC_QUERY_MEM_LAYOUT_INFO,   IAV_IOC_GDMA_COPY,
    IAV_IOC_SYNC_CACHE_MEM_PART, IAV_IOC_GET_ISP_NN_CFG,          IAV_IOC_EFM_GET_POOL_INFO,
    IAV_IOC_EFM_RELEASE_ALL_FRAMES, IAV_IOC_FEED_CANVAS_BUF,      IAV_IOC_RELEASE_CANVAS_BUF,
    IAV_IOC_FEED_PYRAMID_BUF,    IAV_IOC_RELEASE_PYRAMID_BUF,     IAV_IOC_FEED_RAW_BUF,
    IAV_IOC_RELEASE_RAW_BUF,
};

bool isImageListed(unsigned long cmd)
{
    for (size_t i = 0; i < sizeof(ImageListed) / sizeof(ImageListed[0]); i++) {
        if (ImageListed[i] == cmd) {
            return true;
        }
    }
    return false;
}

/* The calls the aux aperture unlocks (iav_policy.c, version 7), written out here apart from the policy; apertureViolation()
 * judges each argument that reaches the driver, and the overlay calls too. */
const unsigned long ApertureListed[] = {
    IAV_IOC_CFG_VIDEO_PROC,      IAV_IOC_CFG_WARP_CTRL,           IAV_IOC_SET_STREAM_CONFIG,
    IAV_IOC_CFG_FRAME_SYNC_PROC, IAV_IOC_SET_YUV_REMAP_INSERT,    IAV_IOC_CE_GO_MAP_REQUEST_BUFFER,
    IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER, IAV_IOC_IMG_GET_MISC_INFO,
};

bool isApertureListed(unsigned long cmd)
{
    for (size_t i = 0; i < sizeof(ApertureListed) / sizeof(ApertureListed[0]); i++) {
        if (ApertureListed[i] == cmd) {
            return true;
        }
    }
    return false;
}

/* [off, off + len) inside [0, limit), with no wrap: the oracle's own arithmetic, in 128 bits. */
bool inside(unsigned __int128 off, unsigned __int128 len, uint64_t limit)
{
    return off + len <= (unsigned __int128)limit;
}

const size_t AllowedCount = sizeof(Allowed) / sizeof(Allowed[0]);

} // namespace

int MockIavDriver::slotOf(unsigned long cmd)
{
    for (size_t i = 0; i < AllowedCount; i++) {
        if (Allowed[i] == cmd) {
            return (int)i;
        }
    }
    return ReachedSlots - 1;
}

MockIavDriver::MockIavDriver() :
    _nextFd(100),
    _openErr(0),
    _eagain(0),
    _failCmd(0),
    _capCmd(0),
    _capLen(0),
    _primarySet(false),
    _primaryCid(0),
    _failErr(0),
    _pollReady(true),
    _now(1000),
    _opens(0),
    _closes(0),
    _ioctls(0),
    _waitCounter(500),
    _rnd(0x12345678u),
    _blockNext(false),
    _blocked(false),
    _released(false)
{
    memset(_reachedCount, 0, sizeof(_reachedCount));
    for (unsigned int i = 0; i < MaxChans; i++) {
        _chan[i].preDec = false;
        _chan[i].vsrc = i;
        _chan[i].swCe = false;
    }
    _chanMap = 0x3;
    _decBase = DecBase;
    _decLen = DecLen;
    _decNextBase = _decNextLen = 0;
    _blurLen = 0;
    _smoothBlurLen = 0;
    _poolBase = PoolBase;
    _poolLen = PoolLen;
    _narena = 0;
    memset(_arenaStart, 0, sizeof(_arenaStart));
    memset(_arenaLen, 0, sizeof(_arenaLen));
    _maxStreams = 3;
    _streamMap = 0x7;
    _apertureCid = 0;
    _decFail = false;
    memset(&_aperture, 0, sizeof(_aperture));
    _nauxParts = 0;
    memset(_auxPid, 0, sizeof(_auxPid));
    memset(_auxBase, 0, sizeof(_auxBase));
    memset(_auxLen, 0, sizeof(_auxLen));
    /* Threads call in concurrently, and CppUTest's leak detector is not thread safe, so the calls
     * must not allocate: reserve once here. */
    _fds.reserve(1024);
    _pollConfigured.reserve(1024);
}

int MockIavDriver::cIsPrimary(void *ctx, uint32_t cid)
{
    MockIavDriver *d = static_cast<MockIavDriver *>(ctx);

    return d->_primaryCid != 0 && cid == d->_primaryCid;
}

struct iav_broker_ops MockIavDriver::ops()
{
    struct iav_broker_ops o;

    memset(&o, 0, sizeof(o));
    o.ctx = this;
    o.open = cOpen;
    o.close = cClose;
    o.ioctl = cIoctl;
    o.poll = cPoll;
    o.sleep_ms = cSleep;
    o.random32 = cRandom;
    o.now_ms = cNow;
    o.is_primary = cIsPrimary;
    o.aperture_of = cApertureOf;
    o.read_dec = cReadDec;
    return o;
}

int MockIavDriver::cReadDec(void *ctx, uint64_t off, void *buf, size_t len)
{
    MockIavDriver *d = static_cast<MockIavDriver *>(ctx);
    std::lock_guard<std::mutex> g(d->_lock);
    std::vector<uint8_t> c = d->_decContent.empty() ? jpegHeader() : d->_decContent;

    (void)off;
    if (d->_decFail) {
        return -EIO;
    }
    memset(buf, 0, len);
    memcpy(buf, &c[0], c.size() < len ? c.size() : len);
    return 0;
}

void MockIavDriver::setDecodeContent(const std::vector<uint8_t> &bytes)
{
    std::lock_guard<std::mutex> g(_lock);

    _decContent = bytes;
}

void MockIavDriver::failDecodeRead(bool fail)
{
    std::lock_guard<std::mutex> g(_lock);

    _decFail = fail;
}

/* SOI, JFIF APP0, DQT, SOF (64x48, three components with the given sampling), the four standard DHT, optional DRI, SOS. */
std::vector<uint8_t> MockIavDriver::jpegHeader(unsigned int hv0, unsigned int hv1, unsigned int hv2,
                                               unsigned int sofMarker, unsigned int restart, bool dqt, bool dht)
{
    std::vector<uint8_t> v;
    const uint8_t soi[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00 };
    const uint8_t sof[] = { 0xFF, (uint8_t)sofMarker, 0x00, 0x11, 0x08, 0x00, 0x30, 0x00, 0x40, 0x03,
                            0x01, (uint8_t)hv0, 0x00, 0x02, (uint8_t)hv1, 0x01, 0x03, (uint8_t)hv2, 0x01 };

    v.insert(v.end(), soi, soi + sizeof(soi));
    if (dqt) {
        const uint8_t q[] = { 0xFF, 0xDB, 0x00, 0x43, 0x00 };

        v.insert(v.end(), q, q + sizeof(q));
        v.insert(v.end(), 64, 8);
    }
    v.insert(v.end(), sof, sof + sizeof(sof));
    if (dht) {
        for (unsigned int t = 0; t < IAV_JPEG_DHT_N; t++) {
            const size_t n = iav_jpeg_std_dht[t].n;
            const uint8_t h[] = { 0xFF, 0xC4, (uint8_t)((n + 2) >> 8), (uint8_t)(n + 2) };

            v.insert(v.end(), h, h + sizeof(h));
            v.insert(v.end(), iav_jpeg_std_dht[t].t, iav_jpeg_std_dht[t].t + n);
        }
    }
    if (restart) {
        const uint8_t r[] = { 0xFF, 0xDD, 0x00, 0x04, (uint8_t)(restart >> 8), (uint8_t)restart };

        v.insert(v.end(), r, r + sizeof(r));
    }
    {
        const uint8_t sos[] = { 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00 };

        v.insert(v.end(), sos, sos + sizeof(sos));
    }
    return v;
}

int MockIavDriver::cApertureOf(void *ctx, uint32_t cid, struct iav_broker_aperture *out)
{
    MockIavDriver *d = static_cast<MockIavDriver *>(ctx);
    std::lock_guard<std::mutex> g(d->_lock);

    memset(out, 0, sizeof(*out));
    if (d->_apertureCid != 0 && cid == d->_apertureCid) {
        *out = d->_aperture;
    }
    return 0;
}

const struct iav_broker_aux_entry MockIavDriver::StandardAux[MockIavDriver::AuxCount] = {
    { IAV_PART_MASK, 0x87900000ull, 0x2000000ull, 0x0ull },
    { IAV_PART_WARP, 0x86100000ull, 0x360000ull, 0x2000000ull },
    { IAV_PART_QPMATRIX, 0x84d00000ull, 0x400000ull, 0x2360000ull },
    { IAV_PART_CE_GO_MAP, 0x84259000ull, 0x1000ull, 0x2760000ull },
    { IAV_PART_CANVAS_OVERLAY, 0x28900000ull, 0x200000ull, 0x2770000ull },
    { IAV_PART_ARB_BLUR, 0xb4800000ull, 0x100000ull, 0x2970000ull },
    { IAV_PART_YUV_REMAP, 0xb4a00000ull, 0x10000ull, 0x2a70000ull },
};

void MockIavDriver::setAperture(uint32_t cid, uint64_t size, uint64_t epoch, const struct iav_broker_aux_entry *e,
                                unsigned int n)
{
    std::lock_guard<std::mutex> g(_lock);

    memset(&_aperture, 0, sizeof(_aperture));
    _apertureCid = cid;
    _aperture.size = size;
    _aperture.epoch = epoch;
    _aperture.count = n < IAV_BROKER_AUX_MAX ? n : IAV_BROKER_AUX_MAX;
    for (unsigned int i = 0; i < _aperture.count; i++) {
        _aperture.e[i] = e[i];
    }
    for (unsigned int i = 0; i < _aperture.count; i++) {
        int k = auxPartIndex(e[i].pid);

        if (k < 0 && _nauxParts < MaxAuxParts) {
            k = (int)_nauxParts++;
        }
        if (k >= 0) {
            _auxPid[k] = e[i].pid;
            _auxBase[k] = e[i].base;
            _auxLen[k] = e[i].len;
        }
    }
}

void MockIavDriver::setStandardAperture(uint32_t cid)
{
    setAperture(cid, StandardAuxSize, StandardAuxEpoch, StandardAux, AuxCount);
}

void MockIavDriver::setAuxPartition(uint32_t pid, uint64_t base, uint64_t len)
{
    std::lock_guard<std::mutex> g(_lock);
    int k = auxPartIndex(pid);

    if (k < 0 && _nauxParts < MaxAuxParts) {
        k = (int)_nauxParts++;
    }
    if (k >= 0) {
        _auxPid[k] = pid;
        _auxBase[k] = base;
        _auxLen[k] = len;
    }
}

/* The slot of an aperture partition the mock answers, or -1. Reads only fields set before the threads start. */
int MockIavDriver::auxPartIndex(uint32_t pid) const
{
    for (unsigned int i = 0; i < _nauxParts; i++) {
        if (_auxPid[i] == pid) {
            return (int)i;
        }
    }
    return -1;
}

int MockIavDriver::cOpen(void *ctx) { return static_cast<MockIavDriver *>(ctx)->doOpen(); }
int MockIavDriver::cClose(void *ctx, int fd) { return static_cast<MockIavDriver *>(ctx)->doClose(fd); }
int MockIavDriver::cIoctl(void *ctx, int fd, unsigned long cmd, unsigned long arg)
{
    return static_cast<MockIavDriver *>(ctx)->doIoctl(fd, cmd, arg);
}
int MockIavDriver::cPoll(void *ctx, int fd, int events, int timeoutMs, int *revents)
{
    return static_cast<MockIavDriver *>(ctx)->doPoll(fd, events, timeoutMs, revents);
}
void MockIavDriver::cSleep(void *ctx, unsigned int ms) { static_cast<MockIavDriver *>(ctx)->advance(ms); }
uint32_t MockIavDriver::cRandom(void *ctx)
{
    MockIavDriver *m = static_cast<MockIavDriver *>(ctx);
    std::lock_guard<std::mutex> g(m->_lock);

    m->_rnd = m->_rnd * 1664525u + 1013904223u;
    return m->_rnd;
}
uint64_t MockIavDriver::cNow(void *ctx) { return static_cast<MockIavDriver *>(ctx)->now(); }

void MockIavDriver::violation(const std::string &what)
{
    _violations.push_back(what);
}

bool MockIavDriver::fdOpen(int fd)
{
    return std::find(_fds.begin(), _fds.end(), fd) != _fds.end();
}

int MockIavDriver::doOpen()
{
    std::function<void()> hook;
    int fd;

    {
        std::lock_guard<std::mutex> g(_lock);

        if (_openErr) {
            errno = _openErr;
            return -1;
        }
        _opens++;
        _fds.push_back(_nextFd);
        _pollConfigured.push_back(0);
        fd = _nextFd++;
        hook.swap(_openHook);
    }
    if (hook) {
        hook();
    }
    return fd;
}

int MockIavDriver::doClose(int fd)
{
    std::lock_guard<std::mutex> g(_lock);
    std::vector<int>::iterator it = std::find(_fds.begin(), _fds.end(), fd);

    if (it == _fds.end()) {
        violation("close of a descriptor that is not open");
        errno = EBADF;
        return -1;
    }
    _pollConfigured.erase(_pollConfigured.begin() + (it - _fds.begin()));
    _fds.erase(it);
    _closes++;
    return 0;
}

int MockIavDriver::doIoctl(int fd, unsigned long cmd, unsigned long arg)
{
    bool wait = false;

    {
        std::lock_guard<std::mutex> g(_lock);

        _ioctls++;
        _reachedCount[slotOf(cmd)]++;
        if (!fdOpen(fd)) {
            violation("ioctl on a descriptor that is not open");
            errno = EBADF;
            return -1;
        }
        if (!isAllowed(cmd) &&
            !(_primarySet && (isPrimaryListed(cmd) || isDecodeListed(cmd) || isImageListed(cmd) || isApertureListed(cmd)))) {
            violation("an ioctl that is not on the allow-list reached the driver");
            errno = ENOTTY;
            return -1;
        }
        if (isImageListed(cmd)) {
            std::string bad = imageViolation(cmd, arg);

            if (!bad.empty()) {
                violation(bad);
            }
        }
        if (isApertureListed(cmd) || cmd == IAV_IOC_SET_OVERLAY_INSERT || cmd == IAV_IOC_GET_OVERLAY_INSERT ||
            cmd == IAV_IOC_SET_BLUR_INSERT || cmd == IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC ||
            cmd == IAV_IOC_APPLY_VIDEO_PROC || cmd == IAV_IOC_APPLY_WARP_CTRL ||
            cmd == IAV_IOC_APPLY_CANVAS_FRAME_SYNC_PROC) {
            std::string bad = apertureViolation(cmd, arg);

            if (!bad.empty()) {
                violation(bad);
            }
        }
        if (_failCmd == cmd && _failErr) {
            errno = _failErr;
            return -1;
        }
        if (_capCmd == cmd && _capLen != 0 && arg != 0) {
            _capBuf.assign(reinterpret_cast<uint8_t *>(arg), reinterpret_cast<uint8_t *>(arg) + _capLen);
            _capCmd = 0;
        }
    }

    switch (cmd) {
    case IAV_IOC_GET_IAV_STATE:
    case IAV_IOC_GET_CHIP_ID: {
        u32 *p = reinterpret_cast<u32 *>(arg);

        if (p == NULL) {
            errno = EFAULT;
            return -1;
        }
        *p = cmd == IAV_IOC_GET_IAV_STATE ? 2 : 0;
        return 0;
    }
    case IAV_IOC_QUERY_MEMBLOCK: {
        struct iav_querymem *m = reinterpret_cast<struct iav_querymem *>(arg);
        int mid;

        /* The value comes from a guest. Read it as an int: an enum outside its range is undefined. */
        memcpy(&mid, &m->mid, sizeof(mid));
        if (mid != (int)IAV_MEM_PARTITION) {
            errno = EINVAL;
            return -1;
        }
        if (m->arg.partition.pid == IAV_PART_DSP) {
            m->arg.partition.mem.addr = DspBase;
            m->arg.partition.mem.length = DspLen;
        } else if (m->arg.partition.pid == IAV_PART_BSB) {
            m->arg.partition.mem.addr = BsbBase;
            m->arg.partition.mem.length = BsbLen;
        } else if (m->arg.partition.pid == IAV_PART_DEC_BSB) {
            std::lock_guard<std::mutex> g(_lock);
            m->arg.partition.mem.addr = _decBase;
            m->arg.partition.mem.length = _decLen;
        } else if (m->arg.partition.pid == IAV_PART_OVERLAY) {
            m->arg.partition.mem.addr = OvlBase;
            m->arg.partition.mem.length = OvlLen;
        } else if (m->arg.partition.pid == PoolPid) {
            std::lock_guard<std::mutex> g(_lock);

            if (_poolLen == 0) {
                errno = EINVAL;
                return -1;
            }
            m->arg.partition.mem.addr = _poolBase;
            m->arg.partition.mem.length = _poolLen;
        } else if (auxPartIndex(m->arg.partition.pid) >= 0) {
            std::lock_guard<std::mutex> g(_lock);
            int k = auxPartIndex(m->arg.partition.pid);

            m->arg.partition.mem.addr = _auxBase[k];
            m->arg.partition.mem.length = _auxLen[k];
        } else if (m->arg.partition.pid == IAV_PART_BLUR || m->arg.partition.pid == IAV_PART_SMOOTH_BLUR) {
            std::lock_guard<std::mutex> g(_lock);
            uint64_t len = m->arg.partition.pid == IAV_PART_BLUR ? _blurLen : _smoothBlurLen;

            m->arg.partition.mem.addr = len ? 0x87000000ull : 0;
            m->arg.partition.mem.length = len;
        } else {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    case IAV_IOC_QUERY_DESC: {
        struct iav_querydesc *q = reinterpret_cast<struct iav_querydesc *>(arg);
        bool nonBlock = false;
        int qid;

        memcpy(&qid, &q->qid, sizeof(qid));
        if (qid == (int)IAV_DESC_CANVAS) {
            nonBlock = q->arg.canvas.non_block_flag != 0;
            if (q->arg.canvas.yuv_use_dma_buf_fd || q->arg.canvas.me_use_dma_buf_fd ||
                q->arg.canvas.mo_use_dma_buf_fd) {
                std::lock_guard<std::mutex> g(_lock);
                violation("a canvas query with a dma-buf fd flag reached the driver");
            }
        } else if (qid == (int)IAV_DESC_PYRAMID) {
            nonBlock = q->arg.pyramid.non_block_flag != 0;
            if (q->arg.pyramid.use_dma_buf_fd) {
                std::lock_guard<std::mutex> g(_lock);
                violation("a pyramid query with a dma-buf fd flag reached the driver");
            }
        } else if (qid == (int)IAV_DESC_FRAME) {
            nonBlock = true;
            if (q->arg.frame.time_ms != 0xffffffffU && (q->arg.frame.time_ms == 0 ||
                q->arg.frame.time_ms > BlockMaxMs)) {
                std::lock_guard<std::mutex> g(_lock);
                violation("a frame query with no bound on its wait reached the driver");
            }
        } else {
            std::lock_guard<std::mutex> g(_lock);
            violation("a descriptor query of an unsupported type reached the driver");
            errno = EINVAL;
            return -1;
        }
        {
            std::lock_guard<std::mutex> g(_lock);

            if (_eagain > 0) {
                _eagain--;
                /* the real driver waits TWO_SECONDS on the blocking path before giving up */
                if (!nonBlock)
                    _now += BlockMaxMs;
                errno = EAGAIN;
                return -1;
            }
        }
        return 0;
    }
    case IAV_IOC_SET_POLL_EVENT_CFG: {
        struct iav_poll_event_config *c = reinterpret_cast<struct iav_poll_event_config *>(arg);
        std::lock_guard<std::mutex> g(_lock);
        std::vector<int>::iterator it = std::find(_fds.begin(), _fds.end(), fd);

        if (c->valid_num > MAX_POLL_EVENT) {
            violation("a poll configuration with too many events reached the driver");
        }
        _pollConfigured[it - _fds.begin()] = c->valid_num;
        return 0;
    }
    case IAV_IOC_CLEAR_POLL_EVENT_CFG: {
        std::lock_guard<std::mutex> g(_lock);
        std::vector<int>::iterator it = std::find(_fds.begin(), _fds.end(), fd);

        _pollConfigured[it - _fds.begin()] = 0;
        return 0;
    }
    case IAV_IOC_WAIT_NEXT_IDSP_FRAME: {
        {
            std::unique_lock<std::mutex> g(_blockMutex);

            if (_blockNext) {
                _blockNext = false;
                _blocked = true;
                wait = true;
                _blockCv.notify_all();
                while (!_released) {
                    _blockCv.wait(g);
                }
                _released = false;
                _blocked = false;
            }
        }
        (void)wait;
        std::lock_guard<std::mutex> g(_lock);
        return (int)_waitCounter++;
    }
    case IAV_IOC_QUERY_DECODE_STATUS: {
        struct iav_decode_status *st = reinterpret_cast<struct iav_decode_status *>(arg);

        if (st == NULL) {
            errno = EFAULT;
            return -1;
        }
        /* The real driver reports the physical addresses of the display frame; the broker must clear them. */
        st->yuv422_y_addr = 0x91a00000ul;
        st->yuv422_uv_addr = 0x91b00000ul;
        st->decoded_still_pic_info.width = 64;
        st->decoded_still_pic_info.height = 48;
        return 0;
    }
    case IAV_IOC_GET_CHAN_CONFIG: {
        struct iav_chan_cfg *c = reinterpret_cast<struct iav_chan_cfg *>(arg);
        std::lock_guard<std::mutex> g(_lock);
        unsigned int id = c->chan_id;

        if (id >= MaxChans) {
            errno = EINVAL;
            return -1;
        }
        c->pre_dec_enable = _chan[id].preDec ? 1 : 0;
        c->vsrc_id = _chan[id].vsrc;
        c->sw_ce_enable = _chan[id].swCe ? 1 : 0;
        return 0;
    }
    case IAV_IOC_GET_SYSTEM_RESOURCE: {
        struct iav_system_resource *res = reinterpret_cast<struct iav_system_resource *>(arg);
        std::lock_guard<std::mutex> g(_lock);

        res->chan_map = _chanMap;
        res->max_stream_num = (u8)_maxStreams;
        res->stream_map = _streamMap;
        return 0;
    }
    case IAV_IOC_SET_CHAN_CONFIG: {
        const struct iav_chan_cfg *c = reinterpret_cast<const struct iav_chan_cfg *>(arg);
        std::lock_guard<std::mutex> g(_lock);
        unsigned int id = c->chan_id;

        if (id >= MaxChans) {
            errno = EINVAL;
            return -1;
        }
        if ((c->pre_dec_enable != 0) != _chan[id].preDec || (c->sw_ce_enable != 0) != _chan[id].swCe ||
            ((c->pre_dec_enable || _chan[id].preDec) && c->vsrc_id != _chan[id].vsrc)) {
            violation("a channel configuration that moves a windowed partition reached the driver");
        }
        return 0;
    }
    case IAV_IOC_SET_SYSTEM_RESOURCE: {
        const struct iav_system_resource *res = reinterpret_cast<const struct iav_system_resource *>(arg);
        std::lock_guard<std::mutex> g(_lock);

        if (res->chan_map != _chanMap) {
            violation("a resource configuration that changes the channel map reached the driver");
        }
        return 0;
    }
    case IAV_IOC_GET_ISP_NN_CFG: {
        struct iav_isp_nn_params *p = reinterpret_cast<struct iav_isp_nn_params *>(arg);

        /* The real driver returns the physical addresses of the NN graphs and the FIFO; the broker must clear them. */
        p->arg.nn_cfg.fifo_addr = 0x7f000000u;
        for (unsigned int i = 0; i < IAV_ISP_NN_TOTAL_MAX; i++) {
            p->arg.nn_cfg.nn_info[i].dag_addr_phys_addr = 0x7f100000ul + i;
            p->arg.nn_cfg.nn_info[i].port_map_phys_addr = 0x7f200000ul + i;
            p->arg.nn_cfg.nn_info[i].port_pos_map_phys_addr = 0x7f300000ul + i;
        }
        return 0;
    }
    case IAV_IOC_SET_IMG_SCALE: {
        struct iav_img_scale *s = reinterpret_cast<struct iav_img_scale *>(arg);

        s->task_id = 77;
        return 0;
    }
    case IAV_IOC_IMG_GET_MISC_INFO: {
        /* As imgproc_drv.c and imgproc_statis.c do for two channels: read a query flag, write a valid flag and an entry. */
        struct img_misc_info *m = reinterpret_cast<struct img_misc_info *>(arg);

        if (m->query_extra_raw && (!m->extra_raw_valid || !m->extra_raw)) {
            errno = EINVAL;
            return -1;
        }
        if (m->query_extra_raw) {
            for (unsigned int i = 0; i < 2; i++) {
                m->extra_raw_valid[i] = m->query_extra_raw[i] ? 1 : 0;
                if (m->query_extra_raw[i]) {
                    memset(static_cast<uint8_t *>(m->extra_raw) + i * 32, (int)(0xa0 + i), 32);
                }
            }
        }
        m->ik_id_in_use[0] = 11;
        m->ik_id_in_use[1] = 12;
        return 0;
    }
    case IAV_IOC_ENABLE_PREVIEW: {
        std::lock_guard<std::mutex> g(_lock);

        if (_decNextLen != 0) {
            _decBase = _decNextBase;
            _decLen = _decNextLen;
            _decNextBase = _decNextLen = 0;
        }
        return 0;
    }
    default:
        return 0;
    }
}

void MockIavDriver::setChannel(unsigned int chanId, bool preDec, unsigned int vsrcId, bool swCe)
{
    std::lock_guard<std::mutex> g(_lock);

    if (chanId < MaxChans) {
        _chan[chanId].preDec = preDec;
        _chan[chanId].vsrc = vsrcId;
        _chan[chanId].swCe = swCe;
    }
}

void MockIavDriver::setChanMap(uint32_t chanMap)
{
    std::lock_guard<std::mutex> g(_lock);
    _chanMap = chanMap;
}

void MockIavDriver::setBlurPartitions(uint64_t blurLen, uint64_t smoothBlurLen)
{
    std::lock_guard<std::mutex> g(_lock);
    _blurLen = blurLen;
    _smoothBlurLen = smoothBlurLen;
}

void MockIavDriver::setPool(uint64_t base, uint64_t len)
{
    std::lock_guard<std::mutex> g(_lock);
    _poolBase = base;
    _poolLen = len;
}

void MockIavDriver::allowArena(uint64_t start, uint64_t len)
{
    std::lock_guard<std::mutex> g(_lock);

    if (_narena < MaxArena) {
        _arenaStart[_narena] = start;
        _arenaLen[_narena] = len;
        _narena++;
    }
}

void MockIavDriver::clearArena()
{
    std::lock_guard<std::mutex> g(_lock);
    _narena = 0;
}

/* [off, off + len) of the pool lies inside one range the guest under test owns. _lock is held. */
bool MockIavDriver::arenaInside(unsigned __int128 off, unsigned __int128 len) const
{
    for (unsigned int i = 0; i < _narena; i++) {
        if (off >= _arenaStart[i] && off + len <= (unsigned __int128)_arenaStart[i] + _arenaLen[i]) {
            return true;
        }
    }
    return false;
}

/*
 * A buffer the guest may name: inside a window (a destination only in its own content, decode or overlay), or inside
 * an arena range it owns. _lock is held.
 */
bool MockIavDriver::bufInside(uint32_t pid, unsigned __int128 off, unsigned __int128 len, bool dst) const
{
    if (pid == PoolPid) {
        return arenaInside(off, len);
    }
    if (auxLen(pid) != 0) {
        return inside(off, len, auxLen(pid));	/* an entry of the guest's own aperture: its own content */
    }
    if (dst && pid != IAV_PART_DEC_BSB && pid != IAV_PART_OVERLAY) {
        return false;
    }
    return windowedLen(pid) != 0 && inside(off, len, windowedLen(pid));
}

void MockIavDriver::setStreams(unsigned int maxStreams, uint32_t streamMap)
{
    std::lock_guard<std::mutex> g(_lock);
    _maxStreams = maxStreams;
    _streamMap = streamMap;
}

/* The length of a partition the guests hold a window over, or 0. Called with _lock held. */
uint64_t MockIavDriver::windowedLen(uint32_t pid) const
{
    switch (pid) {
    case IAV_PART_DSP:
        return DspLen;
    case IAV_PART_BSB:
        return BsbLen;
    case IAV_PART_DEC_BSB:
        return _decLen;
    case IAV_PART_OVERLAY:
        return OvlLen;
    default:
        return 0;
    }
}

namespace {

/* The plane bytes of a scale buffer and whether the buffer is safe: an offset in the decode partition only. */
std::string scaleBufferProblem(const struct iav_scale_buf_info *b, int fmt, uint64_t decLen,
                               const std::function<bool(unsigned __int128, unsigned __int128)> &arenaOk)
{
    unsigned __int128 y = (unsigned __int128)b->pitch * b->height, uv;

    if (b->use_phys || b->use_dma_buf_fd) {
        return "a scale buffer named by a physical address or a dma-buf fd reached the driver";
    }
    uv = fmt == IAV_YUV_FORMAT_YUV422 ? y : fmt == IAV_YUV_FORMAT_YUV420 ? (unsigned __int128)b->pitch * (b->height / 2) : 0;
    if (b->pid == MockIavDriver::PoolPid) {
        if (!arenaOk(b->y_offset, y) || (uv && !arenaOk(b->uv_offset, uv))) {
            return "a scale plane outside the guest's own arena reached the driver";
        }
        return "";
    }
    if (b->pid != IAV_PART_DEC_BSB) {
        return "a scale buffer outside the decode partition reached the driver";
    }
    if (!inside(b->y_offset, y, decLen) || !inside(b->uv_offset, uv, decLen)) {
        return "a scale plane outside the decode window reached the driver";
    }
    return "";
}

} // namespace

/* What may never reach the driver for the image calls; the empty string when the argument is safe. _lock is held. */
std::string MockIavDriver::imageViolation(unsigned long cmd, unsigned long arg) const
{
    switch (cmd) {
    case IAV_IOC_SET_IMG_SCALE: {
        const struct iav_img_scale *s = reinterpret_cast<const struct iav_img_scale *>(arg);
        int fmt, ofmt;
        std::string bad;
        unsigned __int128 xEnd, yEnd;
        bool hp = s->input_win_format != IAV_INPUT_WIN_FORMAT_U32;

        memcpy(&fmt, &s->format, sizeof(fmt));
        ofmt = fmt;
        if (s->output_format_enable) {
            memcpy(&ofmt, &s->output_format, sizeof(ofmt));
        }
        {
            auto arenaOk = [this](unsigned __int128 o, unsigned __int128 l) { return arenaInside(o, l); };

            bad = scaleBufferProblem(&s->input_buf, fmt, _decLen, arenaOk);
            if (bad.empty()) {
                bad = scaleBufferProblem(&s->output_buf, ofmt, _decLen, arenaOk);
            }
        }
        if (!bad.empty()) {
            return bad;
        }
        /* The pixels the DSP reads and writes, in whole pixels, before any alignment: inside their buffers. */
        xEnd = hp ? ((unsigned __int128)s->input.x + s->input.width + 255) / 256 : (unsigned __int128)s->input.x + s->input.width;
        yEnd = hp ? ((unsigned __int128)s->input.y + s->input.height + 255) / 256 : (unsigned __int128)s->input.y + s->input.height;
        if (xEnd > s->input_buf.pitch || yEnd > s->input_buf.height) {
            return "a scale input window outside its buffer reached the driver";
        }
        if ((unsigned __int128)s->output.x + s->output.width > s->output_buf.pitch ||
            (unsigned __int128)s->output.y + s->output.height > s->output_buf.height) {
            return "a scale output window outside its buffer reached the driver";
        }
        return "";
    }
    case IAV_IOC_GDMA_COPY: {
        const struct iav_gdma_copy *g = reinterpret_cast<const struct iav_gdma_copy *>(arg);

        if (g->src_use_phys || g->dst_use_phys || g->src_use_dma_buf_fd || g->dst_use_dma_buf_fd) {
            return "a GDMA copy with a physical address or a dma-buf fd reached the driver";
        }
        if (g->dst_mmap_type != IAV_PART_DEC_BSB && g->dst_mmap_type != IAV_PART_OVERLAY &&
            g->dst_mmap_type != PoolPid && auxLen(g->dst_mmap_type) == 0) {
            return "a GDMA copy into a partition that is not the guest's own content reached the driver";
        }
        if (g->width == 0 || g->height == 0 || g->width > g->src_pitch || g->width > g->dst_pitch ||
            !bufInside(g->src_mmap_type, g->src_offset, (unsigned __int128)g->src_pitch * g->height, false) ||
            !bufInside(g->dst_mmap_type, g->dst_offset, (unsigned __int128)g->dst_pitch * g->height, true)) {
            return "a GDMA copy outside its windows or the guest's own arena reached the driver";
        }
        return "";
    }
    case IAV_IOC_SYNC_CACHE_MEM_PART: {
        const struct iav_sync_cache_mem_part *c = reinterpret_cast<const struct iav_sync_cache_mem_part *>(arg);
        int opt;

        memcpy(&opt, &c->opt, sizeof(opt));
        if (c->length == 0 || !bufInside(c->pid, c->offset, c->length, false)) {
            return "a cache operation outside a window or the guest's own arena reached the driver";
        }
        if (opt == IAV_CACHE_INVALIDATE && c->pid != IAV_PART_DEC_BSB && c->pid != IAV_PART_OVERLAY &&
            c->pid != PoolPid && auxLen(c->pid) == 0) {
            return "a cache invalidate of memory the guest does not own reached the driver";
        }
        if (opt != IAV_CACHE_CLEAN && opt != IAV_CACHE_INVALIDATE) {
            return "an unknown cache operation reached the driver";
        }
        return "";
    }
    case IAV_IOC_SET_BLUR_INSERT:
    case IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC: {
        const struct iav_blur_insert *b;

        if (cmd == IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC) {
            const struct iav_canvas_sync_cfg *c = reinterpret_cast<const struct iav_canvas_sync_cfg *>(arg);
            int cid;

            memcpy(&cid, &c->cid, sizeof(cid));
            if (cid == IAV_CANVAS_SYNC_CFG_POSTP_SCALE) {
                return "";
            }
            if (cid == IAV_CANVAS_SYNC_CFG_OVERLAY) {
                return overlayInside(&c->arg.overlay, true, c->id) ? "" :
                    "a canvas overlay outside the guest's CANVAS_OVERLAY entry reached the driver";
            }
            if (cid != IAV_CANVAS_SYNC_CFG_BLUR) {
                return "a canvas sync configuration other than blur, overlay or post-processing scale reached the driver";
            }
            b = &c->arg.blur;
        } else {
            b = reinterpret_cast<const struct iav_blur_insert *>(arg);
        }
        if (!b->enable) {
            return "";
        }
        if (!blurInside(b)) {
            return "an arbitrary blur bitmap outside the guest's ARB_BLUR entry reached the driver";
        }
        if ((b->type == IAV_BLUR_TYPE_SMOOTH ? _smoothBlurLen : b->type == IAV_BLUR_TYPE_PM ? _blurLen : 0) == 0) {
            return "an enabled blur without its blur partition reached the driver (NULL dereference)";
        }
        return "";
    }
    case IAV_IOC_GET_VIDEO_PROC: {
        const struct iav_video_proc *v = reinterpret_cast<const struct iav_video_proc *>(arg);
        int cid;

        memcpy(&cid, &v->cid, sizeof(cid));
        if (cid == IAV_VIDEO_PROC_MULTI_BLEND || cid == IAV_VIDEO_PROC_BLEND_WARP) {
            return "a video-processing get that copies inside a partition without a window reached the driver";
        }
        return "";
    }
    case IAV_IOC_GET_VIN_IDSP_CFG: {
        const struct iav_vin_idsp_config *c = reinterpret_cast<const struct iav_vin_idsp_config *>(arg);

        if (c->idsp_cfg.chan_id >= 32 || c->vin_cfg.vsrc_id >= 32) {
            return "a VIN/IDSP configuration id the driver shifts by 32 or more reached the driver";
        }
        return "";
    }
    case IAV_IOC_EFM_GET_POOL_INFO:
    case IAV_IOC_EFM_RELEASE_ALL_FRAMES: {
        unsigned int id = cmd == IAV_IOC_EFM_GET_POOL_INFO ?
            reinterpret_cast<const struct iav_efm_get_pool_info *>(arg)->stream_id :
            reinterpret_cast<const struct iav_efm_release_all_frame *>(arg)->stream_id;

        if (id >= _maxStreams || id >= 32 || !((_streamMap >> id) & 1u)) {
            return "an EFM call for a stream without a context reached the driver";
        }
        return "";
    }
    case IAV_IOC_RELEASE_PYRAMID_BUF:
        return reinterpret_cast<const struct iav_pyramiddesc *>(arg)->chan_id < 4 ? "" :
            "a pyramid release past the driver's feed table reached the driver";
    case IAV_IOC_RELEASE_CANVAS_BUF:
        return reinterpret_cast<const struct iav_canvasdesc *>(arg)->canvas_id < 20 ? "" :
            "a canvas release past the driver's feed table reached the driver";
    case IAV_IOC_RELEASE_RAW_BUF:
        return reinterpret_cast<const struct iav_rawbufdesc *>(arg)->vin_id < 30 ? "" :
            "a raw release past the driver's feed table reached the driver";
    default:
        return "";
    }
}

void MockIavDriver::setDecodePartition(uint64_t base, uint64_t len)
{
    std::lock_guard<std::mutex> g(_lock);
    _decBase = base;
    _decLen = len;
}

void MockIavDriver::moveDecodeOnPreview(uint64_t base, uint64_t len)
{
    std::lock_guard<std::mutex> g(_lock);
    _decNextBase = base;
    _decNextLen = len;
}

int MockIavDriver::doPoll(int fd, int events, int timeoutMs, int *revents)
{
    std::lock_guard<std::mutex> g(_lock);
    std::vector<int>::iterator it = std::find(_fds.begin(), _fds.end(), fd);

    if (it == _fds.end()) {
        violation("poll on a descriptor that is not open");
        errno = EBADF;
        return -1;
    }
    if (events & ~(POLLIN | POLLRDNORM | POLLPRI | POLLOUT | POLLWRNORM)) {
        violation("a poll with events outside the allowed set reached the driver");
    }
    if (timeoutMs < 0 || timeoutMs > (int)AMBA_VIRT_IAV_POLL_SLICE_MS) {
        violation("a poll with no bound on its wait reached the driver");
    }
    if (_pollConfigured[it - _fds.begin()] && _pollReady && (events & POLLIN)) {
        *revents = POLLIN;
        return 1;
    }
    _now += (timeoutMs > 0 ? (uint64_t)timeoutMs : 0);
    *revents = 0;
    return 0;
}

void MockIavDriver::failOpensWith(int err)
{
    std::lock_guard<std::mutex> g(_lock);
    _openErr = err;
}

void MockIavDriver::setEagainCount(int n)
{
    std::lock_guard<std::mutex> g(_lock);
    _eagain = n;
}

void MockIavDriver::captureArgOf(unsigned long cmd, size_t len)
{
    std::lock_guard<std::mutex> g(_lock);
    _capCmd = cmd;
    _capLen = len;
    _capBuf.clear();
}

std::vector<uint8_t> MockIavDriver::capturedArg()
{
    std::lock_guard<std::mutex> g(_lock);
    return _capBuf;
}

void MockIavDriver::failIoctl(unsigned long cmd, int err)
{
    std::lock_guard<std::mutex> g(_lock);
    _failCmd = cmd;
    _failErr = err;
}

void MockIavDriver::setPollReady(bool ready)
{
    std::lock_guard<std::mutex> g(_lock);
    _pollReady = ready;
}

void MockIavDriver::advance(uint64_t ms)
{
    std::lock_guard<std::mutex> g(_lock);
    _now += ms;
}

void MockIavDriver::setOpenHook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> g(_lock);
    _openHook = hook;
}

void MockIavDriver::setSeed(uint32_t seed)
{
    std::lock_guard<std::mutex> g(_lock);
    _rnd = seed;
}

void MockIavDriver::blockNextWait()
{
    std::lock_guard<std::mutex> g(_blockMutex);
    _blockNext = true;
    _blocked = false;
    _released = false;
}

void MockIavDriver::waitUntilBlocked()
{
    std::unique_lock<std::mutex> g(_blockMutex);

    while (!_blocked) {
        _blockCv.wait(g);
    }
}

void MockIavDriver::release()
{
    std::lock_guard<std::mutex> g(_blockMutex);
    _released = true;
    _blockCv.notify_all();
}

int MockIavDriver::openFds()
{
    std::lock_guard<std::mutex> g(_lock);
    return (int)_fds.size();
}

unsigned long MockIavDriver::totalOpens()
{
    std::lock_guard<std::mutex> g(_lock);
    return _opens;
}

unsigned long MockIavDriver::totalCloses()
{
    std::lock_guard<std::mutex> g(_lock);
    return _closes;
}

unsigned long MockIavDriver::ioctlCalls()
{
    std::lock_guard<std::mutex> g(_lock);
    return _ioctls;
}

unsigned long MockIavDriver::reached(unsigned long cmd)
{
    std::lock_guard<std::mutex> g(_lock);

    return _reachedCount[slotOf(cmd)];
}

uint64_t MockIavDriver::now()
{
    std::lock_guard<std::mutex> g(_lock);
    return _now;
}

std::vector<std::string> MockIavDriver::violations()
{
    std::lock_guard<std::mutex> g(_lock);
    return _violations;
}

void MockIavDriver::clearViolations()
{
    std::lock_guard<std::mutex> g(_lock);
    _violations.clear();
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

/* The bytes of partition pid the guest under test maps through its aperture, or 0. _lock is held. */
uint64_t MockIavDriver::auxLen(uint32_t pid) const
{
    if (_apertureCid == 0) {
        return 0;
    }
    for (unsigned int i = 0; i < _aperture.count; i++) {
        if (_aperture.e[i].pid == pid) {
            return _aperture.e[i].len;
        }
    }
    return 0;
}

/* Every enabled area's colour table and pixels inside the window (stream) or the CANVAS_OVERLAY entry (canvas). */
bool MockIavDriver::overlayInside(const struct iav_overlay_insert *o, bool canvas, uint32_t id) const
{
    uint64_t limit = canvas ? auxLen(IAV_PART_CANVAS_OVERLAY) : OvlLen;

    if (canvas ? id >= 20 : id >= 32) {
        return false;
    }
    if (!o->enable) {
        return true;
    }
    for (unsigned int i = 0; i < MAX_NUM_OVERLAY_AREA; i++) {
        const struct iav_overlay_area *a = &o->area[i];
        unsigned __int128 need = (unsigned __int128)a->pitch * a->height;

        if (!a->enable) {
            continue;
        }
        if (a->total_size > need) {
            need = a->total_size;
        }
        if (limit == 0 || !inside(a->data_addr_offset, need, limit) || !inside(a->clut_addr_offset, 1024, limit)) {
            return false;
        }
    }
    return true;
}

/*
 * Every arbitrary-blur area of a normal blur: the rows the DSP reads (height rows, pitch apart, each at most width / 8
 * rounded up plus one bytes from its start) inside ARB_BLUR; smooth arbitrary blur has no entry at all.
 */
bool MockIavDriver::blurInside(const struct iav_blur_insert *b) const
{
    if (b->id_for_canvas ? b->canvas_id >= 20 : b->stream_id >= 32) {
        return false;
    }
    if (!b->enable) {
        return true;
    }
    for (unsigned int i = 0; i < MAX_NUM_BLUR_AREA; i++) {
        const struct iav_blur_area *a = &b->area[i];
        unsigned __int128 end;

        if (!a->enable || !a->use_arb_blur) {
            continue;
        }
        if (b->type != IAV_BLUR_TYPE_PM || a->height == 0) {
            return false;
        }
        end = (unsigned __int128)a->arb_blur_offset + (unsigned __int128)(a->height - 1) * a->arb_blur_pitch +
              (a->width + 7u) / 8u + 1u;
        if (end > auxLen(IAV_PART_ARB_BLUR)) {
            return false;
        }
    }
    return true;
}

/* What may never reach the driver for the calls the aperture unlocks; the empty string when it is safe. _lock is held. */
std::string MockIavDriver::apertureViolation(unsigned long cmd, unsigned long arg) const
{
    switch (cmd) {
    case IAV_IOC_SET_OVERLAY_INSERT: {
        const struct iav_overlay_insert *o = reinterpret_cast<const struct iav_overlay_insert *>(arg);

        return overlayInside(o, o->id_for_canvas, o->id) ? "" :
            "an overlay outside its window or entry, or with an id the driver shifts by, reached the driver";
    }
    case IAV_IOC_GET_OVERLAY_INSERT: {
        const struct iav_overlay_insert *o = reinterpret_cast<const struct iav_overlay_insert *>(arg);

        return !o->id_for_canvas || (o->id < 20 && auxLen(IAV_PART_CANVAS_OVERLAY)) ? "" :
            "a canvas overlay read without the CANVAS_OVERLAY entry reached the driver";
    }
    case IAV_IOC_CFG_VIDEO_PROC: {
        const struct iav_video_proc *v = reinterpret_cast<const struct iav_video_proc *>(arg);
        int cid;

        memcpy(&cid, &v->cid, sizeof(cid));
        switch (cid) {
        case IAV_VIDEO_PROC_MASK:
            return v->arg.mask.channel_id < 4 && auxLen(IAV_PART_MASK) ? "" :
                "a privacy mask without the MASK entry or with an id the driver shifts by reached the driver";
        case IAV_VIDEO_PROC_DPTZ:
            return v->arg.dptz.channel_id < 4 && v->arg.dptz.pass_id < 2 && v->arg.dptz.buf_id < 8 &&
                (v->arg.dptz.buf_cfg.canvas_id < 20 || v->arg.dptz.buf_cfg.canvas_id == 0xff) ? "" :
                "a DPTZ with an unbounded id reached the driver";
        case IAV_VIDEO_PROC_CANVAS_FPS:
        case IAV_VIDEO_PROC_CANVAS_VV_SYNC_CFG:
        case IAV_VIDEO_PROC_CANVAS_DUMMY_LATENCY: {
            uint32_t id = cid == IAV_VIDEO_PROC_CANVAS_FPS ? v->arg.canvas_fps.id :
                cid == IAV_VIDEO_PROC_CANVAS_VV_SYNC_CFG ? v->arg.canvas_vv_sync.id : v->arg.canvas_dummy_latency.id;

            return id < 20 ? "" : "a canvas configuration past the canvas table reached the driver";
        }
        case IAV_VIDEO_PROC_VSRC_UPDATE:
            if (v->arg.vsrc_update.chan_id >= 4 || v->arg.vsrc_update.vsrc_id >= 32) {
                return "a vsrc update with an id the driver shifts by reached the driver";
            }
            return _chan[v->arg.vsrc_update.chan_id].preDec ?
                "a vsrc update of a decoder channel (the decode window would move) reached the driver" : "";
        case IAV_VIDEO_PROC_CANVAS_OVERLAY:
            return overlayInside(&v->arg.overlay, true, v->arg.overlay.id) ? "" :
                "a canvas overlay outside the CANVAS_OVERLAY entry reached the driver";
        case IAV_VIDEO_PROC_PYRAMID_ROI:
            return v->arg.pyramid_roi.chan_id < 4 ? "" : "a pyramid ROI past the channel table reached the driver";
        case IAV_VIDEO_PROC_PYRAMID_FPS:
            return v->arg.pyramid_fps.chan_id < 4 ? "" : "a pyramid fps past the channel table reached the driver";
        case IAV_VIDEO_PROC_CANVAS_ORDER:
            if (v->arg.canvas_order.id >= 20 || v->arg.canvas_order.chan_num > 16) {
                return "a canvas order past its tables reached the driver";
            }
            for (unsigned int i = 0; i < 16; i++) {
                if (v->arg.canvas_order.chan_order[i] >= 32) {
                    return "a canvas order the driver shifts by 32 or more reached the driver";
                }
            }
            return "";
        case IAV_VIDEO_PROC_VCAP_FLUSH:
            return v->arg.vcap_flush.channel_id < 4 ? "" : "a VCAP flush past the channel table reached the driver";
        case IAV_VIDEO_PROC_POSTP_SCALE:
            if (v->arg.postp_scale.canvas_id >= 20) {
                return "a post-processing scale past the canvas table reached the driver";
            }
            for (unsigned int i = 0; i < IAV_MAX_POSTP_SCALE_AREA_NUM; i++) {
                if (v->arg.postp_scale.area[i].enable && v->arg.postp_scale.area[i].src_canvas_id >= 20) {
                    return "a post-processing scale from a canvas past the table reached the driver";
                }
            }
            return "";
        case IAV_VIDEO_PROC_CAWARP:
            return v->arg.cawarp.chan_id < 4 ? "" : "a CA warp past the channel table reached the driver";
        default:
            return "a video-processing configuration naming a partition with no entry reached the driver";
        }
    }
    case IAV_IOC_CFG_WARP_CTRL: {
        const struct iav_warp_ctrl *w = reinterpret_cast<const struct iav_warp_ctrl *>(arg);
        int cid;

        memcpy(&cid, &w->cid, sizeof(cid));
        if (w->channel_id >= 4) {
            return "a warp configuration past the channel table reached the driver";
        }
        if (cid == IAV_WARP_CTRL_MAIN) {
            uint64_t len = auxLen(IAV_PART_WARP);

            if (len == 0) {
                return "a warp configuration without the WARP entry reached the driver";
            }
            for (unsigned int i = 0; i < MAX_NUM_WARP_AREAS; i++) {
                const struct iav_warp_area *a = &w->arg.main.area[i];

                if (a->enable && (a->h_map.data_addr_offset >= len || a->v_map.data_addr_offset >= len ||
                                  a->me1_v_map.data_addr_offset >= len)) {
                    return "a warp table offset outside the WARP entry reached the driver";
                }
            }
            return "";
        }
        if (cid == IAV_WARP_CTRL_DPTZ) {
            return w->arg.dptz.buf_id < 8 ? "" : "a warp DPTZ past the buffer table reached the driver";
        }
        return cid == IAV_WARP_CTRL_MAX_CFG ? "" : "an unknown warp configuration reached the driver";
    }
    case IAV_IOC_SET_STREAM_CONFIG:
    case IAV_IOC_CFG_FRAME_SYNC_PROC: {
        const struct iav_stream_cfg *c = reinterpret_cast<const struct iav_stream_cfg *>(arg);
        int cid;

        memcpy(&cid, &c->cid, sizeof(cid));
        if (c->id >= 32) {
            return "a stream configuration with an id the driver shifts by reached the driver";
        }
        switch (cid) {
        case IAV_STMCFG_CUSTOM_SEI:
            return "a custom SEI configuration reached the driver";
        case IAV_H264_CFG_QP_ROI:
        case IAV_H265_CFG_QP_ROI: {
            const struct iav_qpmatrix *q = cid == IAV_H264_CFG_QP_ROI ? &c->arg.h264_roi : &c->arg.h265_roi;

            if (q->qpm_raw) {
                return "a raw QP matrix offset (unchecked by the driver) reached the driver";
            }
            return auxLen(IAV_PART_QPMATRIX) ? "" : "a QP ROI without the QPMATRIX entry reached the driver";
        }
        case IAV_STMCFG_OVERLAY:
            return overlayInside(&c->arg.overlay, false, c->id) ? "" :
                "a stream overlay outside the overlay window reached the driver";
        case IAV_STMCFG_BLUR:
            return blurInside(&c->arg.blur) && (!c->arg.blur.enable || _blurLen) ? "" :
                "a stream blur outside its partitions reached the driver";
        case IAV_STMCFG_YUV_REMAP: {
            const struct iav_yuv_remap *r = &c->arg.yuv_remap;

            if (!r->enable) {
                return "";
            }
            for (unsigned int i = 0; i < MAX_NUM_YUV_REMAP_AREA; i++) {
                if (r->area[i].enable && !inside(r->area[i].clut_addr_offset, 1024, auxLen(IAV_PART_YUV_REMAP))) {
                    return "a YUV remap table outside the YUV_REMAP entry reached the driver";
                }
            }
            return "";
        }
        default:
            return "";
        }
    }
    case IAV_IOC_SET_YUV_REMAP_INSERT: {
        const struct iav_yuv_remap *r = reinterpret_cast<const struct iav_yuv_remap *>(arg);

        if (r->id >= 32) {
            return "a YUV remap with a stream id the driver shifts by reached the driver";
        }
        if (!r->enable) {
            return "";
        }
        for (unsigned int i = 0; i < MAX_NUM_YUV_REMAP_AREA; i++) {
            if (r->area[i].enable && !inside(r->area[i].clut_addr_offset, 1024, auxLen(IAV_PART_YUV_REMAP))) {
                return "a YUV remap table outside the YUV_REMAP entry reached the driver";
            }
        }
        return "";
    }
    case IAV_IOC_CE_GO_MAP_REQUEST_BUFFER:
        return reinterpret_cast<const struct iav_ce_go_map_request_buffer *>(arg)->chan_id < 4 &&
            auxLen(IAV_PART_CE_GO_MAP) ? "" : "a CE GO map request without the entry reached the driver";
    case IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER: {
        const struct iav_ce_go_map_handshake_buffer *h =
            reinterpret_cast<const struct iav_ce_go_map_handshake_buffer *>(arg);

        return h->chan_id < 4 && h->buffer_idx < 8 && auxLen(IAV_PART_CE_GO_MAP) ? "" :
            "a CE GO map handshake past the buffer table or without the entry reached the driver";
    }
    case IAV_IOC_APPLY_VIDEO_PROC: {
        const struct iav_apply_flag *f = reinterpret_cast<const struct iav_apply_flag *>(arg);
        const int refused[] = { IAV_VIDEO_PROC_MULTI_BLEND, IAV_VIDEO_PROC_BLEND_WARP, IAV_VIDEO_PROC_ARB_BLEND,
                                IAV_VIDEO_PROC_TML_FUSION, IAV_VIDEO_PROC_FUSION, IAV_VIDEO_PROC_OF_NN };

        for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
            if (f[refused[k]].apply) {
                return "an apply of a configuration a guest cannot set (no partition) reached the driver";
            }
        }
        if ((f[IAV_VIDEO_PROC_MASK].apply && f[IAV_VIDEO_PROC_MASK].param >= 16) ||
            (f[IAV_VIDEO_PROC_CANVAS_FPS].apply && f[IAV_VIDEO_PROC_CANVAS_FPS].param >= (1u << 20))) {
            return "an apply map past the driver's channels or canvases reached the driver";
        }
        return "";
    }
    case IAV_IOC_APPLY_WARP_CTRL: {
        const struct iav_apply_flag *f = reinterpret_cast<const struct iav_apply_flag *>(arg);

        return (f->apply == IAV_WARP_CTRL_MAIN || f->apply == IAV_WARP_CTRL_DPTZ) && f->param < 16 ? "" :
            "a warp apply of an unknown cid or channel reached the driver";
    }
    case IAV_IOC_APPLY_CANVAS_FRAME_SYNC_PROC:
        return reinterpret_cast<const struct iav_apply_canvas_frame_sync *>(arg)->canvas_updated_map < (1u << 20) ? "" :
            "a canvas sync apply past the canvas table reached the driver";
    case IAV_IOC_IMG_GET_MISC_INFO: {
        const struct img_misc_info *m = reinterpret_cast<const struct img_misc_info *>(arg);
        const uint8_t *base = reinterpret_cast<const uint8_t *>(arg) + sizeof(*m);
        const uint8_t *q = reinterpret_cast<const uint8_t *>(m->query_extra_raw);
        const uint8_t *v = reinterpret_cast<const uint8_t *>(m->extra_raw_valid);
        const uint8_t *e = reinterpret_cast<const uint8_t *>(m->extra_raw);

        /* each pointer is NULL or one of the broker's own buffers, right after the argument: never a guest's */
        if ((q && q != base) || (v && v != base + 16) || (e && e != base + 32)) {
            return "a misc-info pointer that is not the broker's own buffer reached the driver";
        }
        return "";
    }
    default:
        return "";
    }
}
