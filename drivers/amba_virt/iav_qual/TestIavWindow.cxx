/*
 * TestIavWindow.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The host IAV window nodes (amba_virt_iav_dsp, amba_virt_iav_bsb): configuration limits, map
 * limits, and that the memory they expose is the memory the real driver maps, including a live
 * frame. Runs in Dom0 only, in --mode windows, with the amba_virt_iav_win module loaded.
 */

#include "IavTestSupport.hxx"

#include <stdint.h>
#include <sys/ioctl.h>
#include <utility>
#include <vector>
#include <stdio.h>
#include <unistd.h>

#include <CppUTest/TestHarness.h>

#include <amba_virt_iav_win.h>

using namespace iavtest;

namespace {

const unsigned long long PageBytes = 4096ULL;
const unsigned long long FourGiB = 0x100000000ULL;

unsigned long long pow2AtLeast(unsigned long long n)
{
    unsigned long long p = PageBytes;

    while (p < n) {
        p <<= 1;
    }
    return p;
}

int cfgIoctl(int fd, const struct amba_virt_iav_win_cfg &c)
{
    return ioctl(fd, AMBA_VIRT_IAV_WIN_IOC_CONFIG, &c);
}

struct amba_virt_iav_win_cfg getCfg(int fd)
{
    struct amba_virt_iav_win_cfg c;

    memset(&c, 0, sizeof(c));
    if (ioctl(fd, AMBA_VIRT_IAV_WIN_IOC_GET, &c) != 0) {
        memset(&c, 0xff, sizeof(c));
    }
    return c;
}

bool sameCfg(const struct amba_virt_iav_win_cfg &a, const struct amba_virt_iav_win_cfg &b)
{
    return a.phys == b.phys && a.len == b.len && a.size == b.size;
}

unsigned long long fnv(const volatile unsigned char *p, size_t n)
{
    unsigned long long h = 1469598103934665603ULL;

    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace

TEST_GROUP(IavWindow)
{
    ScopedFd _iav;
    int _dsp;
    int _bsb;
    unsigned long _dspAddr, _dspLen, _bsbAddr, _bsbLen;
    struct amba_virt_iav_win_cfg _dspCfg, _bsbCfg;
    std::vector<std::pair<void *, size_t> > _maps;

    void setup()
    {
        _dsp = _bsb = -1;
        CHECK_TRUE(_iav.open());
        LONGS_EQUAL(0, queryPartition(_iav.fd(), IAV_PART_DSP, &_dspAddr, &_dspLen));
        LONGS_EQUAL(0, queryPartition(_iav.fd(), IAV_PART_BSB, &_bsbAddr, &_bsbLen));
        _dsp = ::open(AMBA_VIRT_IAV_WIN_DSP_PATH, O_RDWR);
        _bsb = ::open(AMBA_VIRT_IAV_WIN_BSB_PATH, O_RDWR);
        CHECK_TEXT(_dsp >= 0 && _bsb >= 0, "window nodes missing: is amba_virt_iav_win loaded?");
        memset(&_dspCfg, 0, sizeof(_dspCfg));
        _dspCfg.phys = _dspAddr + ctx().config()._windowSkew;
        _dspCfg.len = _dspLen;
        _dspCfg.size = pow2AtLeast(_dspLen);
        _bsbCfg = _dspCfg;
        _bsbCfg.phys = _bsbAddr;
        _bsbCfg.len = _bsbLen;
        _bsbCfg.size = pow2AtLeast(_bsbLen);
        configure(_dsp, _dspCfg, "dsp");
        configure(_bsb, _bsbCfg, "bsb");
    }

    void configure(int fd, const struct amba_virt_iav_win_cfg &c, const char *what)
    {
        char msg[160];

        if (cfgIoctl(fd, c) != 0) {
            snprintf(msg, sizeof(msg), "configuring %s failed, errno %d (phys 0x%llx len 0x%llx size 0x%llx)",
                     what, errno, (unsigned long long)c.phys, (unsigned long long)c.len,
                     (unsigned long long)c.size);
            FAIL(msg);
        }
    }

    void teardown()
    {
        for (size_t i = 0; i < _maps.size(); i++) {
            ::munmap(_maps[i].first, _maps[i].second);
        }
        _maps.clear();
        if (_dsp >= 0) {
            ::close(_dsp);
        }
        if (_bsb >= 0) {
            ::close(_bsb);
        }
    }

    /* Every mapping is released in teardown, so a failed check cannot leave one behind. */
    void *mapNode(int fd, size_t length, off_t off, int flags = MAP_SHARED)
    {
        void *p = ::mmap(NULL, length, PROT_READ | PROT_WRITE, flags, fd, off);

        if (p != MAP_FAILED) {
            _maps.push_back(std::make_pair(p, length));
        }
        return p;
    }

    void unmapNode(void *p, size_t length)
    {
        ::munmap(p, length);
        for (size_t i = 0; i < _maps.size(); i++) {
            if (_maps[i].first == p) {
                _maps.erase(_maps.begin() + (long)i);
                return;
            }
        }
    }
};

TEST(IavWindow, ConfigurationReadsBackWhatWasSet)
{
    struct amba_virt_iav_win_cfg c = getCfg(_dsp);

    CHECK_TRUE(sameCfg(c, _dspCfg));
    c = getCfg(_bsb);
    CHECK_TRUE(sameCfg(c, _bsbCfg));
    note("dsp_phys", (long long)_dspCfg.phys);
    note("dsp_len", (long long)_dspCfg.len);
    note("dsp_size", (long long)_dspCfg.size);
    note("bsb_phys", (long long)_bsbCfg.phys);
    note("bsb_len", (long long)_bsbCfg.len);
    note("bsb_size", (long long)_bsbCfg.size);
}

TEST(IavWindow, BadConfigurationIsRefusedAndChangesNothing)
{
    struct Case
    {
        const char *name;
        struct amba_virt_iav_win_cfg cfg;
        int err;
    };
    struct amba_virt_iav_win_cfg good = _bsbCfg;
    std::vector<Case> cases;
    Case c;

    c.name = "misaligned phys"; c.cfg = good; c.cfg.phys += 1; c.err = EINVAL; cases.push_back(c);
    c.name = "misaligned len"; c.cfg = good; c.cfg.len += 1; c.err = EINVAL; cases.push_back(c);
    c.name = "zero len"; c.cfg = good; c.cfg.len = 0; c.err = EINVAL; cases.push_back(c);
    c.name = "zero size"; c.cfg = good; c.cfg.size = 0; c.err = EINVAL; cases.push_back(c);
    c.name = "size not a power of two"; c.cfg = good; c.cfg.size = good.size + PageBytes;
    c.err = EINVAL; cases.push_back(c);
    c.name = "size below len"; c.cfg = good; c.cfg.size = good.size / 2; c.err = EINVAL;
    cases.push_back(c);
    c.name = "flags set"; c.cfg = good; c.cfg.flags = 1; c.err = EINVAL; cases.push_back(c);
    c.name = "reserved set"; c.cfg = good; c.cfg.reserved = 1; c.err = EINVAL; cases.push_back(c);
    c.name = "below the allowed range"; c.cfg = good; c.cfg.phys = 0x1000; c.err = ERANGE;
    cases.push_back(c);
    c.name = "above 4 GiB"; c.cfg = good; c.cfg.phys = FourGiB; c.err = ERANGE;
    cases.push_back(c);
    c.name = "ends above 4 GiB"; c.cfg = good; c.cfg.phys = FourGiB - PageBytes;
    c.cfg.len = 2 * PageBytes; c.cfg.size = 2 * PageBytes; c.err = ERANGE; cases.push_back(c);
    c.name = "phys plus len overflows"; c.cfg = good; c.cfg.phys = ~0ULL & ~(PageBytes - 1);
    c.err = EINVAL; cases.push_back(c);
    c.name = "padding beyond the limit"; c.cfg = good; c.cfg.len = PageBytes;
    c.cfg.size = 1ULL << 30; c.err = EINVAL; cases.push_back(c);
    c.name = "size beyond the limit"; c.cfg = good; c.cfg.size = 1ULL << 40; c.err = EINVAL;
    cases.push_back(c);

    for (size_t i = 0; i < cases.size(); i++) {
        char msg[96];

        errno = 0;
        snprintf(msg, sizeof(msg), "%s was accepted", cases[i].name);
        CHECK_TEXT(cfgIoctl(_bsb, cases[i].cfg) == -1, msg);
        snprintf(msg, sizeof(msg), "%s: wrong errno %d", cases[i].name, errno);
        CHECK_TEXT(errno == cases[i].err, msg);
        CHECK_TEXT(sameCfg(getCfg(_bsb), _bsbCfg), "a refused configuration changed the window");
    }
    note("cases", (long long)cases.size());
}

TEST(IavWindow, UnknownIoctlIsNotTty)
{
    errno = 0;
    LONGS_EQUAL(-1, ioctl(_dsp, _IO('W', 99), 0));
    LONGS_EQUAL(ENOTTY, errno);
}

TEST(IavWindow, MapLimits)
{
    void *p;

    p = mapNode(_bsb, _bsbCfg.size, 0);
    CHECK_TRUE(p != MAP_FAILED);
    unmapNode(p, _bsbCfg.size);

    errno = 0;
    p = mapNode(_bsb, _bsbCfg.size + PageBytes, 0);
    CHECK_TRUE(p == MAP_FAILED);
    LONGS_EQUAL(EINVAL, errno);

    errno = 0;
    p = mapNode(_bsb, PageBytes, (off_t)_bsbCfg.size);
    CHECK_TRUE(p == MAP_FAILED);
    LONGS_EQUAL(EINVAL, errno);

    errno = 0;
    p = mapNode(_bsb, PageBytes, (off_t)(_bsbCfg.size - PageBytes));
    CHECK_TRUE(p != MAP_FAILED);
    unmapNode(p, PageBytes);

    errno = 0;
    p = mapNode(_bsb, PageBytes, 0, MAP_PRIVATE);
    CHECK_TRUE(p == MAP_FAILED);
    LONGS_EQUAL(EINVAL, errno);
}

TEST(IavWindow, UnconfiguredNodeCannotBeMapped)
{
    void *p;

    LONGS_EQUAL(0, ioctl(_bsb, AMBA_VIRT_IAV_WIN_IOC_CLEAR));
    errno = 0;
    p = mapNode(_bsb, PageBytes, 0);
    CHECK_TRUE(p == MAP_FAILED);
    LONGS_EQUAL(ENODEV, errno);
    LONGS_EQUAL(0, cfgIoctl(_bsb, _bsbCfg));
    p = mapNode(_bsb, PageBytes, 0);
    CHECK_TRUE(p != MAP_FAILED);
    unmapNode(p, PageBytes);
}

TEST(IavWindow, ReconfigurationWhileMappedIsBusy)
{
    void *p = mapNode(_bsb, _bsbCfg.size, 0);

    CHECK_TRUE(p != MAP_FAILED);
    errno = 0;
    LONGS_EQUAL(-1, cfgIoctl(_bsb, _bsbCfg));
    LONGS_EQUAL(EBUSY, errno);
    errno = 0;
    LONGS_EQUAL(-1, ioctl(_bsb, AMBA_VIRT_IAV_WIN_IOC_CLEAR));
    LONGS_EQUAL(EBUSY, errno);
    unmapNode(p, _bsbCfg.size);
    LONGS_EQUAL(0, cfgIoctl(_bsb, _bsbCfg));
}

/* Splitting a mapping and unmapping every piece must return the mapping count to zero. */
TEST(IavWindow, MapAccountingSurvivesSplitsAndRepeats)
{
    for (int i = 0; i < 100; i++) {
        unsigned char *p = (unsigned char *)mapNode(_bsb, _bsbCfg.size, 0);

        CHECK_TRUE((void *)p != MAP_FAILED);
        LONGS_EQUAL(0, ::munmap(p + PageBytes, PageBytes));
        LONGS_EQUAL(0, ::munmap(p + 5 * PageBytes, 2 * PageBytes));
        LONGS_EQUAL(0, ::munmap(p, PageBytes));
        LONGS_EQUAL(0, ::munmap(p + 2 * PageBytes, 3 * PageBytes));
        LONGS_EQUAL(0, ::munmap(p + 7 * PageBytes, _bsbCfg.size - 7 * PageBytes));
    }
    LONGS_EQUAL(0, cfgIoctl(_bsb, _bsbCfg));
}

/* The padding behind the real range is private zeroed memory and never reaches the real range. */
TEST(IavWindow, PaddingIsZeroWritableAndDoesNotReachTheRealRange)
{
    unsigned char *w;
    unsigned char *real;
    size_t pad = (size_t)(_bsbCfg.size - _bsbCfg.len);
    unsigned long long before, after;
    ScopedMap iavMap;

    if (pad == 0) {
        note("padding_bytes", 0);
        return;
    }
    CHECK_TRUE(iavMap.map(_iav.fd(), _bsbAddr, _bsbLen, PROT_READ));
    real = (unsigned char *)iavMap.bytes();
    w = (unsigned char *)mapNode(_bsb, _bsbCfg.size, 0);
    CHECK_TRUE((void *)w != MAP_FAILED);

    /* the identity header sits in the last bytes of the padding */
    for (size_t i = 0; i + 512 < pad; i += 512) {
        CHECK_TEXT(w[_bsbCfg.len + i] == 0, "padding is not zero");
    }
    memset(w + _bsbCfg.len, 0xA5, pad);
    for (size_t i = 0; i < pad; i += 512) {
        CHECK_TEXT(w[_bsbCfg.len + i] == 0xA5, "padding does not hold what was written");
    }
    /* the pattern must not appear in the last real page */
    before = fnv(real + _bsbLen - PageBytes, PageBytes);
    {
        unsigned long long pattern = fnv(w + _bsbCfg.len, PageBytes);

        CHECK_TEXT(before != pattern, "the padding pattern appeared at the end of the real range");
    }
    after = fnv(real + _bsbLen - PageBytes, PageBytes);
    (void)after;
    memset(w + _bsbCfg.len, 0, pad);
    unmapNode(w, _bsbCfg.size);
    note("padding_bytes", (long long)pad);
}

/* A window with padding says what it is in the last bytes of the padding. */
TEST(IavWindow, PaddingCarriesTheIdentityHeader)
{
    struct amba_virt_iav_win_hdr hdr;
    unsigned char *w = (unsigned char *)mapNode(_bsb, _bsbCfg.size, 0);

    CHECK_TRUE((void *)w != MAP_FAILED);
    memcpy(&hdr, w + _bsbCfg.size - AMBA_VIRT_IAV_WIN_HDR_BYTES, sizeof(hdr));
    LONGS_EQUAL(AMBA_VIRT_IAV_WIN_HDR_MAGIC, hdr.magic);
    LONGS_EQUAL(AMBA_VIRT_IAV_WIN_HDR_VERSION, hdr.version);
    LONGS_EQUAL(1, hdr.window_id);
    CHECK_TRUE(hdr.part_len == _bsbCfg.len);
    CHECK_TRUE(hdr.size == _bsbCfg.size);
}

/*
 * The tail of the DSP partition, which the running pipeline does not use, reads the same through
 * the window and through the real driver's mapping. Equal zeros prove little, so the frame test
 * below carries the weight; this one proves the address arithmetic at a far offset.
 */
TEST(IavWindow, TailOfTheDspPartitionMatchesTheDriversMapping)
{
    const size_t n = 4ULL << 20;
    ScopedMap iavMap;
    unsigned char *w;
    unsigned char *d;
    off_t off = (off_t)(_dspLen - n);

    CHECK_TRUE(iavMap.map(_iav.fd(), _dspAddr, _dspLen, PROT_READ));
    w = (unsigned char *)mapNode(_dsp, n, off);
    CHECK_TRUE((void *)w != MAP_FAILED);
    d = (unsigned char *)iavMap.bytes() + off;
    CHECK_TEXT(memcmp(w, d, n) == 0, "window and driver mapping differ at the partition tail");
    unmapNode(w, n);
}

/*
 * A live canvas frame, found through the real driver's descriptor, is read through the window
 * and through the driver's own mapping, and the two reads must agree. Two frames with different
 * sequence numbers must have different content, so a window that shows stale or constant memory
 * fails.
 */
TEST(IavWindow, LiveFramesAreVisibleThroughTheWindow)
{
    ScopedMap iavMap;
    unsigned char *w;
    unsigned long long hash[2] = {0, 0};
    unsigned int seq[2] = {0, 0};
    int matched[2] = {0, 0};
    int attempts[2] = {0, 0};

    CHECK_TRUE(iavMap.map(_iav.fd(), _dspAddr, _dspLen, PROT_READ));
    w = (unsigned char *)mapNode(_dsp, _dspCfg.size, 0);
    CHECK_TRUE((void *)w != MAP_FAILED);

    for (int f = 0; f < 2; f++) {
        for (int tries = 0; tries < 20 && !matched[f]; tries++) {
            struct iav_querydesc q;
            size_t yBytes;
            unsigned long long hw, hd;

            if (queryCanvas(_iav.fd(), true, &q) != 0) {
                continue;
            }
            if (f == 1 && q.arg.canvas.yuv.seq_num == seq[0]) {
                continue;
            }
            yBytes = (size_t)q.arg.canvas.yuv.pitch * q.arg.canvas.yuv.height;
            CHECK_TRUE(yBytes > 0 && q.arg.canvas.yuv.y_addr_offset + yBytes <= _dspLen);
            attempts[f]++;
            hw = fnv(w + q.arg.canvas.yuv.y_addr_offset, yBytes);
            hd = fnv(iavMap.bytes() + q.arg.canvas.yuv.y_addr_offset, yBytes);
            if (hw == hd) {
                matched[f] = 1;
                hash[f] = hw;
                seq[f] = q.arg.canvas.yuv.seq_num;
            }
        }
        CHECK_TEXT(matched[f], "the window and the driver mapping never agreed on a frame");
    }
    CHECK_TEXT(seq[0] != seq[1], "the two frames have the same sequence number");
    CHECK_TEXT(hash[0] != hash[1], "two different frames have identical content");
    note("seq_a", seq[0]);
    note("seq_b", seq[1]);
    note("hash_a", (long long)hash[0]);
    note("hash_b", (long long)hash[1]);
    note("attempts_a", attempts[0]);
    note("attempts_b", attempts[1]);
    unmapNode(w, _dspCfg.size);
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
