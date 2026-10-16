/*
 * IavRefCommon.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Shared helpers of the IAV reference applications. They use only the real /dev/iav ABI, so the same
 * binary runs on the bare board (Dom0) and in a KVM guest. Results go to stdout as one JSON object per
 * line (records a gate can read); diagnostics go to stderr. The environment (bare metal or guest) is
 * reported from /sys/class/misc/iav/virtualized and never changes behaviour.
 */

#ifndef IAVREF_IAVREFCOMMON_HXX
#define IAVREF_IAVREFCOMMON_HXX

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <basetypes.h>
#include <iav_ioctl.h>

namespace iavref {

inline long long nowNs()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* "guest" when the virtual driver's attribute is present and reads 1, "bare" otherwise. */
inline const char *environment()
{
    FILE *f = fopen("/sys/class/misc/iav/virtualized", "r");
    int v = 0;

    if (f == NULL) {
        return "bare";
    }
    if (fscanf(f, "%d", &v) != 1) {
        v = 0;
    }
    fclose(f);
    return v == 1 ? "guest" : "bare";
}

/*
 * Copy out of a mapped window. In a guest the window is device memory at stage 2 (the DSP partitions are memory
 * the host kernel does not map as RAM), where an unaligned or wide-but-unaligned access raises an alignment
 * fault that KVM cannot emulate: the guest VM is destroyed. libc's memcpy does such accesses, so a copy out of a
 * window uses only byte loads and aligned 8-byte loads. The same code is correct, and fast enough, on the bare board.
 */
inline void windowCopy(void *dstv, const volatile unsigned char *src, size_t n)
{
    unsigned char *dst = (unsigned char *)dstv;
    size_t i = 0;

    while (i < n && (((uintptr_t)(src + i)) & 7) != 0) {
        dst[i] = src[i];
        i++;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t w = *(const volatile uint64_t *)(src + i);

        memcpy(dst + i, &w, sizeof(w));
    }
    for (; i < n; i++) {
        dst[i] = src[i];
    }
}

/* The same rule for writes into a window: byte stores up to an 8-byte boundary, aligned 8-byte stores, byte tail. */
inline void windowWrite(volatile unsigned char *dst, const void *srcv, size_t n)
{
    const unsigned char *src = (const unsigned char *)srcv;
    size_t i = 0;

    while (i < n && (((uintptr_t)(dst + i)) & 7) != 0) {
        dst[i] = src[i];
        i++;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t w;

        memcpy(&w, src + i, sizeof(w));
        *(volatile uint64_t *)(dst + i) = w;
    }
    for (; i < n; i++) {
        dst[i] = src[i];
    }
}

/* The same 64-bit hash the qualification suite and the Dom0 witness use, so frames compare across them. */
inline unsigned long long frameHash(const volatile unsigned char *p, size_t n)
{
    unsigned char buf[65536];
    unsigned long long h = 1469598103934665603ULL;
    size_t off = 0;

    while (off < n) {
        size_t c = n - off < sizeof(buf) ? n - off : sizeof(buf);
        size_t i = 0;

        windowCopy(buf, p + off, c);
        for (; i + 8 <= c; i += 8) {
            uint64_t w;

            memcpy(&w, buf + i, sizeof(w));
            h ^= w;
            h *= 1099511628211ULL;
            h ^= h >> 29;
        }
        for (; i < c; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
        off += c;
    }
    return h;
}

/* One JSON object per line on stdout. */
class Row
{
public:
    explicit Row(const char *kind) : _n(0)
    {
        _buf[0] = 0;
        add("kind", kind);
        add("env", environment());
    }

    Row &add(const char *key, const char *v)
    {
        append("\"%s\":\"%s\"", key, v);
        return *this;
    }

    Row &add(const char *key, long long v)
    {
        append("\"%s\":%lld", key, v);
        return *this;
    }

    Row &add(const char *key, double v)
    {
        append("\"%s\":%.3f", key, v);
        return *this;
    }

    void print() const
    {
        printf("{%s}\n", _buf);
        fflush(stdout);
    }

private:
    void append(const char *fmt, ...)
    {
        va_list ap;
        int w;

        if (_n > 0 && _n < (int)sizeof(_buf) - 2) {
            _buf[_n++] = ',';
        }
        va_start(ap, fmt);
        w = vsnprintf(_buf + _n, sizeof(_buf) - (size_t)_n, fmt, ap);
        va_end(ap);
        if (w > 0) {
            _n += w;
            if (_n > (int)sizeof(_buf) - 1) {
                _n = (int)sizeof(_buf) - 1;
            }
        }
    }

    char _buf[1024];
    int _n;
};

/* The device: every call is checked, and a failure names the call and the errno. */
class Device
{
public:
    Device() : _fd(-1) {}
    ~Device() { if (_fd >= 0) close(_fd); }

    bool open()
    {
        _fd = ::open("/dev/iav", O_RDWR);
        if (_fd < 0) {
            fprintf(stderr, "open /dev/iav: %s\n", strerror(errno));
            return false;
        }
        return true;
    }

    int fd() const { return _fd; }

    /* A pointer argument. Returns 0, or -1 with errno set after naming the call on stderr. */
    int call(unsigned long cmd, void *arg, const char *name, bool quiet = false)
    {
        int rc = ioctl(_fd, cmd, arg);
        int e = errno;

        if (rc < 0 && !quiet) {
            fprintf(stderr, "%s: %s\n", name, strerror(e));
        }
        errno = e;
        return rc < 0 ? -1 : 0;
    }

    /* A value argument (the ioctl takes no pointer). */
    int callValue(unsigned long cmd, unsigned long value, const char *name, bool quiet = false)
    {
        int rc = ioctl(_fd, cmd, value);
        int e = errno;

        if (rc < 0 && !quiet) {
            fprintf(stderr, "%s: %s\n", name, strerror(e));
        }
        errno = e;
        return rc < 0 ? -1 : 0;
    }

    int partition(unsigned int pid, unsigned long *addr, unsigned long *length)
    {
        struct iav_querymem q;

        memset(&q, 0, sizeof(q));
        q.mid = IAV_MEM_PARTITION;
        q.arg.partition.pid = pid;
        if (call(IAV_IOC_QUERY_MEMBLOCK, &q, "IAV_IOC_QUERY_MEMBLOCK") != 0) {
            return -1;
        }
        *addr = q.arg.partition.mem.addr;
        *length = q.arg.partition.mem.length;
        return 0;
    }

    int state()
    {
        u32 s = 0;

        if (call(IAV_IOC_GET_IAV_STATE, &s, "IAV_IOC_GET_IAV_STATE") != 0) {
            return -1000;
        }
        return (int)(int32_t)s;
    }

private:
    Device(const Device &);
    Device &operator=(const Device &);

    int _fd;
};

/* A read-only (or read-write) mapping of a partition. The offset is the partition address, as /dev/iav expects. */
class Mapping
{
public:
    Mapping() : _p(MAP_FAILED), _len(0) {}
    ~Mapping() { if (_p != MAP_FAILED) munmap(_p, _len); }

    bool map(int fd, unsigned long addr, unsigned long length, int prot)
    {
        _p = mmap(NULL, length, prot, MAP_SHARED, fd, (off_t)addr);
        if (_p == MAP_FAILED) {
            fprintf(stderr, "mmap partition: %s\n", strerror(errno));
            return false;
        }
        _len = length;
        return true;
    }

    const volatile unsigned char *bytes() const { return (const volatile unsigned char *)_p; }
    size_t length() const { return _len; }

private:
    Mapping(const Mapping &);
    Mapping &operator=(const Mapping &);

    void *_p;
    size_t _len;
};

/* Statistics of the luma of a rectangle of one fresh canvas frame (plan_iav_usb_yolo Envelope F, the aperture checks). */
struct RegionStats
{
    unsigned long long pts;
    unsigned int width, height;	/* of the canvas */
    double mean, var;		/* of the rectangle */
    unsigned long long hash;	/* of the rectangle's rows */
};

/*
 * Read canvas frames for settleMs and measure [x, y, w, h) of the last one (the queue holds frames made since the last
 * reader; the last is fresh). The canvas is read from the DSP partition mapping as IavRefCapture does. Returns 0, or -1.
 */
inline int canvasRegion(Device &dev, const Mapping &dsp, unsigned int canvas, unsigned int x, unsigned int y,
                        unsigned int w, unsigned int h, long settleMs, RegionStats *st)
{
    const long long end = nowNs() + (long long)settleMs * 1000000LL;
    struct iav_querydesc q;
    bool have = false;
    long tries = 0;

    do {
        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_CANVAS;
        q.arg.canvas.canvas_id = canvas;
        q.arg.canvas.non_block_flag = 0;
        if (dev.call(IAV_IOC_QUERY_DESC, &q, "IAV_IOC_QUERY_DESC", true) != 0) {
            if (errno == EAGAIN && ++tries < 200) {
                continue;
            }
            fprintf(stderr, "IAV_IOC_QUERY_DESC canvas %u: %s\n", canvas, strerror(errno));
            return -1;
        }
        have = true;
    } while (nowNs() < end);
    if (!have) {
        return -1;
    }
    const struct iav_yuv_cap &c = q.arg.canvas.yuv;
    if (c.pitch == 0 || x + w > c.width || y + h > c.height ||
        c.y_addr_offset + (unsigned long long)c.pitch * c.height > dsp.length()) {
        fprintf(stderr, "region %u,%u %ux%u outside canvas %u (%ux%u)\n", x, y, w, h, canvas, c.width, c.height);
        return -1;
    }
    {
        static unsigned char row[8192];
        double sum = 0, sq = 0;
        unsigned long long hsh = 1469598103934665603ULL;

        if (w > sizeof(row)) {
            return -1;
        }
        for (unsigned int r = 0; r < h; r++) {
            windowCopy(row, dsp.bytes() + c.y_addr_offset + (unsigned long long)(y + r) * c.pitch + x, w);
            for (unsigned int i = 0; i < w; i++) {
                sum += row[i];
                sq += (double)row[i] * row[i];
                hsh ^= row[i];
                hsh *= 1099511628211ULL;
            }
        }
        st->pts = c.mono_pts;
        st->width = c.width;
        st->height = c.height;
        st->mean = sum / ((double)w * h);
        st->var = sq / ((double)w * h) - st->mean * st->mean;
        st->hash = hsh;
    }
    return 0;
}

/* One call's outcome as a row: its return code and errno (the comparison unit of the native and guest runs). */
inline void callRow(const char *kind, long long seq, const char *name, int rc, int err)
{
    Row(kind).add("seq", seq).add("case", name).add("rc", (long long)rc).add("errno", (long long)(rc < 0 ? err : 0)).print();
}

/* A positive integer option value, or the default. */
inline long argValue(int argc, char **argv, const char *name, long def)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return strtol(argv[i + 1], NULL, 0);
        }
    }
    return def;
}

inline bool argFlag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

inline const char *argString(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return def;
}

} // namespace iavref

#endif /* IAVREF_IAVREFCOMMON_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
