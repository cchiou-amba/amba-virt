/*
 * MockBackend.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "MockBackend.hxx"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <iav_ioctl.h>

namespace {

const long long FramePeriodNs = 33000000LL;
const unsigned long PageSize = 4096UL;

long long nowNs()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void sleepNs(long long ns)
{
    struct timespec ts;

    if (ns <= 0) {
        return;
    }
    ts.tv_sec = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    nanosleep(&ts, NULL);
}

} // namespace

bool MockBackend::variantFromName(const char *name, Variant *out)
{
    if (strcmp(name, "mock") == 0) {
        *out = VariantNone;
    } else if (strcmp(name, "mock-bad-errno") == 0) {
        *out = VariantBadErrno;
    } else if (strcmp(name, "mock-bad-alignment") == 0) {
        *out = VariantBadAlignment;
    } else if (strcmp(name, "mock-overlap") == 0) {
        *out = VariantOverlap;
    } else if (strcmp(name, "mock-nonmonotonic") == 0) {
        *out = VariantNonMonotonic;
    } else if (strcmp(name, "mock-leak-on-close") == 0) {
        *out = VariantLeakOnClose;
    } else {
        return false;
    }
    return true;
}

MockBackend::MockBackend(Variant variant) :
    _variant(variant),
    _startNs(nowNs()),
    _leakedValidNum(0),
    _queries(0)
{
    pthread_mutex_init(&_lock, NULL);
    static const unsigned int pids[NumPartitions] = {IAV_PART_DSP, IAV_PART_BSB, IAV_PART_IMG};
    static const unsigned long addrs[NumPartitions] = {0x40000000UL, 0x84300000UL, 0x81421000UL};
    static const unsigned long lens[NumPartitions] = {4UL << 20, 1UL << 20, 1UL << 20};

    memset(_maps, 0, sizeof(_maps));
    memset(_files, 0, sizeof(_files));
    for (int i = 0; i < MaxFds; i++) {
        _fdFile[i] = -1;
    }
    for (int i = 0; i < NumPartitions; i++) {
        size_t cap;

        _parts[i].pid = pids[i];
        _parts[i].addr = addrs[i];
        _parts[i].length = lens[i];
        cap = 2 * lens[i];
        _parts[i].mem = (unsigned char *)::mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (_parts[i].mem == (unsigned char *)MAP_FAILED) {
            _parts[i].mem = NULL;
            continue;
        }
        /* The second half repeats the first, as the ring double mapping does. */
        for (size_t b = 0; b < lens[i]; b++) {
            unsigned char v = (unsigned char)((b * 31 + pids[i]) & 0xff);

            _parts[i].mem[b] = v;
            _parts[i].mem[lens[i] + b] = v;
        }
    }
    if (_variant == VariantBadAlignment) {
        _parts[0].addr += 0x123;
    }
    if (_variant == VariantOverlap) {
        /* The BSB partition straddles the end of the DSP partition: a partial overlap. */
        _parts[1].addr = _parts[0].addr + _parts[0].length - 0x1000;
    }
}

MockBackend::~MockBackend()
{
    pthread_mutex_destroy(&_lock);
    for (int i = 0; i < NumPartitions; i++) {
        if (_parts[i].mem != NULL) {
            ::munmap(_parts[i].mem, 2 * _parts[i].length);
        }
    }
}

const char *MockBackend::name() const
{
    return "mock";
}

void MockBackend::unlockKeepErrno()
{
    int saved = errno;

    pthread_mutex_unlock(&_lock);
    errno = saved;
}

int MockBackend::fail(int err)
{
    errno = err;
    return -1;
}

long long MockBackend::frameIndex() const
{
    return (nowNs() - _startNs) / FramePeriodNs;
}

void MockBackend::waitForFrame(long long after, int timeoutMs)
{
    long long deadline = nowNs() + (long long)timeoutMs * 1000000LL;

    while (frameIndex() <= after) {
        if (timeoutMs >= 0 && nowNs() >= deadline) {
            return;
        }
        pthread_mutex_unlock(&_lock);
        sleepNs(1000000LL);
        pthread_mutex_lock(&_lock);
    }
}

MockBackend::File *MockBackend::fileOf(int fd)
{
    int slot = fd - FdBase;

    if (slot < 0 || slot >= MaxFds || _fdFile[slot] < 0) {
        return NULL;
    }
    return &_files[_fdFile[slot]];
}

const MockBackend::Partition *MockBackend::findPartition(unsigned int pid) const
{
    for (int i = 0; i < NumPartitions; i++) {
        if (_parts[i].pid == pid) {
            return &_parts[i];
        }
    }
    return NULL;
}

int MockBackend::open(const char *path, int flags)
{
    int result;

    (void)flags;
    pthread_mutex_lock(&_lock);
    result = openLocked(path);
    unlockKeepErrno();
    return result;
}

int MockBackend::openLocked(const char *path)
{
    if (strcmp(path, "/dev/iav") != 0) {
        return fail(ENOENT);
    }
    for (int i = 0; i < MaxFds; i++) {
        if (_files[i].refs == 0) {
            for (int s = 0; s < MaxFds; s++) {
                if (_fdFile[s] < 0) {
                    memset(&_files[i], 0, sizeof(_files[i]));
                    _files[i].refs = 1;
                    _files[i].cursor = frameIndex() - 1;
                    _files[i].consumed = frameIndex();
                    if (_variant == VariantLeakOnClose) {
                        _files[i].validNum = _leakedValidNum;
                    }
                    _fdFile[s] = i;
                    return FdBase + s;
                }
            }
        }
    }
    return fail(EMFILE);
}

int MockBackend::close(int fd)
{
    int result;

    pthread_mutex_lock(&_lock);
    result = closeLocked(fd);
    unlockKeepErrno();
    return result;
}

int MockBackend::closeLocked(int fd)
{
    int slot = fd - FdBase;
    File *f = fileOf(fd);

    if (f == NULL) {
        return fail(EBADF);
    }
    f->refs--;
    _fdFile[slot] = -1;
    return 0;
}

int MockBackend::dup(int fd)
{
    int result;

    pthread_mutex_lock(&_lock);
    result = dupLocked(fd);
    unlockKeepErrno();
    return result;
}

int MockBackend::dupLocked(int fd)
{
    File *f = fileOf(fd);

    if (f == NULL) {
        return fail(EBADF);
    }
    for (int s = 0; s < MaxFds; s++) {
        if (_fdFile[s] < 0) {
            _fdFile[s] = _fdFile[fd - FdBase];
            f->refs++;
            return FdBase + s;
        }
    }
    return fail(EMFILE);
}

int MockBackend::doQueryDesc(File *f, void *argp)
{
    struct iav_querydesc *q = (struct iav_querydesc *)argp;
    const Partition *dsp = findPartition(IAV_PART_DSP);
    const Partition *bsb = findPartition(IAV_PART_BSB);
    long long seq;
    int timeoutMs = -1;
    bool block = false;

    (void)dsp;
    switch (q->qid) {
    case IAV_DESC_CANVAS:
        if (q->arg.canvas.canvas_id >= 4) {
            return fail(EINVAL);
        }
        block = !q->arg.canvas.non_block_flag;
        timeoutMs = block ? -1 : 0;
        break;
    case IAV_DESC_PYRAMID:
        block = !q->arg.pyramid.non_block_flag;
        timeoutMs = block ? -1 : 0;
        break;
    case IAV_DESC_FRAME:
        if (q->arg.frame.time_ms == 0xffffffffU) {
            timeoutMs = 0;
        } else if (q->arg.frame.time_ms == 0) {
            timeoutMs = -1;
        } else {
            timeoutMs = (int)q->arg.frame.time_ms;
        }
        break;
    default:
        return fail(EINVAL);
    }

    if (frameIndex() <= f->cursor) {
        waitForFrame(f->cursor, timeoutMs);
        if (frameIndex() <= f->cursor) {
            return fail(EAGAIN);
        }
    }
    f->cursor = frameIndex();
    seq = f->cursor;
    f->consumed = f->cursor;
    _queries++;
    if (_variant == VariantNonMonotonic && _queries % 4 == 0) {
        seq = 7;
    }

    if (q->qid == IAV_DESC_CANVAS) {
        q->arg.canvas.yuv.width = 1920;
        q->arg.canvas.yuv.height = 1080;
        q->arg.canvas.yuv.pitch = 1920;
        q->arg.canvas.yuv.seq_num = (u32)seq;
        q->arg.canvas.yuv.dsp_pts = (u32)(seq * 3000);
        q->arg.canvas.yuv.mono_pts = (u64)seq * 33333ULL;
        q->arg.canvas.yuv.y_addr_offset = 0;
        q->arg.canvas.yuv.uv_addr_offset = 1920UL * 1080UL;
        /* a new frame has new content, so a hash of its Y plane tells frames apart */
        memcpy(_parts[0].mem, &seq, sizeof(unsigned int) < sizeof(seq) ? sizeof(unsigned int) : sizeof(seq));
    } else if (q->qid == IAV_DESC_PYRAMID) {
        q->arg.pyramid.layers[0].width = 1280;
        q->arg.pyramid.layers[0].height = 720;
        q->arg.pyramid.layers[0].pitch = 1280;
        q->arg.pyramid.layers[0].seq_num = (u32)seq;
        q->arg.pyramid.layers[0].dsp_pts = (u32)(seq * 3000);
        q->arg.pyramid.layers[0].y_addr_offset = 0;
        q->arg.pyramid.layers[0].uv_addr_offset = 1280UL * 720UL;
        q->arg.pyramid.seq_num = (u64)seq;
    } else {
        q->arg.frame.size = 1000;
        q->arg.frame.data_addr_offset = 0;
        q->arg.frame.cap_seq_no = (u32)seq;
        q->arg.frame.dsp_pts = (u64)seq * 3000;
        (void)bsb;
    }
    return 0;
}

int MockBackend::doIoctl(File *f, unsigned long cmd, unsigned long arg)
{
    void *p = (void *)arg;

    switch (cmd) {
    case IAV_IOC_GET_IAV_STATE:
        if (p == NULL) {
            return fail(EFAULT);
        }
        *(u32 *)p = 2;
        return 0;
    case IAV_IOC_GET_CHIP_ID:
        if (p == NULL) {
            return fail(EFAULT);
        }
        *(u32 *)p = 0x655;
        return 0;
    case IAV_IOC_QUERY_ENC_MODE_CAP: {
        struct iav_enc_mode_cap *c = (struct iav_enc_mode_cap *)p;

        if (c == NULL) {
            return fail(EFAULT);
        }
        c->max_streams_num = 3;
        c->max_encode_MB = 3732480;
        return 0;
    }
    case IAV_IOC_QUERY_ENC_BUF_CAP: {
        struct iav_enc_buf_cap *c = (struct iav_enc_buf_cap *)p;

        if (c == NULL) {
            return fail(EFAULT);
        }
        c->max.width = 3840;
        c->max.height = 2160;
        return 0;
    }
    case IAV_IOC_QUERY_IAV_CAP:
        if (p == NULL) {
            return fail(EFAULT);
        }
        return 0;
    case IAV_IOC_GET_SYSTEM_RESOURCE: {
        struct iav_system_resource *r = (struct iav_system_resource *)p;

        if (r == NULL) {
            return fail(EFAULT);
        }
        r->encode_mode = 0;
        r->max_stream_num = 3;
        r->canvas_num = 4;
        r->chan_num = 1;
        return 0;
    }
    case IAV_IOC_GET_CHAN_CONFIG: {
        struct iav_chan_cfg *c = (struct iav_chan_cfg *)p;

        if (c == NULL) {
            return fail(EFAULT);
        }
        if (c->chan_id != 0) {
            return fail(EINVAL);
        }
        c->vsrc_id = 0;
        return 0;
    }
    case IAV_IOC_GET_CANVAS_CONFIG: {
        struct iav_canvas_cfg *c = (struct iav_canvas_cfg *)p;

        if (c == NULL) {
            return fail(EFAULT);
        }
        if (c->canvas_id >= 4) {
            return fail(EINVAL);
        }
        return 0;
    }
    case IAV_IOC_DRV_DSP_INFO: {
        struct iav_driver_dsp_info *d = (struct iav_driver_dsp_info *)p;

        if (d == NULL) {
            return fail(EFAULT);
        }
        d->dspid = 0x655;
        return 0;
    }
    case IAV_IOC_QUERY_INFO: {
        struct iav_queryinfo *q = (struct iav_queryinfo *)p;

        if (q == NULL) {
            return fail(EFAULT);
        }
        if (q->qid == IAV_INFO_CANVAS) {
            const Partition *dsp = findPartition(IAV_PART_DSP);

            if (q->arg.canvas.canvas_id >= 4) {
                return fail(EINVAL);
            }
            q->arg.canvas.width = 1920;
            q->arg.canvas.height = 1080;
            q->arg.canvas.yuv_pitch = 1920;
            q->arg.canvas.mem_phys_addr = dsp->addr;
            q->arg.canvas.mem_size = dsp->length;
            return 0;
        }
        if (q->qid != IAV_INFO_DRIVER && q->qid != IAV_INFO_SYSTEM) {
            return fail(EINVAL);
        }
        return 0;
    }
    case IAV_IOC_QUERY_MEMBLOCK: {
        struct iav_querymem *m = (struct iav_querymem *)p;
        const Partition *part;

        if (m == NULL) {
            return fail(EFAULT);
        }
        if (m->mid != IAV_MEM_PARTITION) {
            return fail(EINVAL);
        }
        part = findPartition(m->arg.partition.pid);
        if (part == NULL) {
            return fail(EINVAL);
        }
        m->arg.partition.mem.addr = part->addr;
        m->arg.partition.mem.length = part->length;
        m->arg.partition.mem.offset = 0;
        return 0;
    }
    case IAV_IOC_QUERY_DESC:
        if (p == NULL) {
            return fail(EFAULT);
        }
        return doQueryDesc(f, p);
    case IAV_IOC_SET_POLL_EVENT_CFG: {
        struct iav_poll_event_config *c = (struct iav_poll_event_config *)p;

        if (c == NULL) {
            return fail(EFAULT);
        }
        if (c->valid_num > MAX_POLL_EVENT) {
            return fail(EINVAL);
        }
        f->validNum = c->valid_num;
        if (_variant == VariantLeakOnClose) {
            _leakedValidNum = c->valid_num;
        }
        return 0;
    }
    case IAV_IOC_CLEAR_POLL_EVENT_CFG:
        f->validNum = 0;
        if (_variant == VariantLeakOnClose) {
            _leakedValidNum = 0;
        }
        return 0;
    case IAV_IOC_WAIT_NEXT_IDSP_FRAME: {
        long long waitFrom = frameIndex();

        waitForFrame(waitFrom, 2000);
        return (int)frameIndex();
    }
    default:
        return fail(_variant == VariantBadErrno ? EINVAL : ENOTTY);
    }
}

int MockBackend::ioctl(int fd, unsigned long cmd, unsigned long arg)
{
    File *f;
    int result;

    pthread_mutex_lock(&_lock);
    f = fileOf(fd);
    if (f == NULL) {
        result = fail(EBADF);
    } else {
        result = doIoctl(f, cmd, arg);
    }
    unlockKeepErrno();
    return result;
}

void *MockBackend::mmap(size_t length, int prot, int flags, int fd, off_t offset)
{
    void *result;

    pthread_mutex_lock(&_lock);
    result = mmapLocked(length, fd, offset);
    unlockKeepErrno();
    (void)prot;
    (void)flags;
    return result;
}

void *MockBackend::mmapLocked(size_t length, int fd, off_t offset)
{
    File *f = fileOf(fd);

    if (f == NULL) {
        errno = EBADF;
        return MAP_FAILED;
    }
    if (length == 0 || ((unsigned long)offset % PageSize) != 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    for (int i = 0; i < NumPartitions; i++) {
        if ((unsigned long)offset != _parts[i].addr) {
            continue;
        }
        if (length > 2 * _parts[i].length) {
            errno = EINVAL;
            return MAP_FAILED;
        }
        for (int m = 0; m < MaxMaps; m++) {
            if (_maps[m].addr == NULL) {
                _maps[m].addr = _parts[i].mem;
                _maps[m].length = length;
                return _parts[i].mem;
            }
        }
        errno = ENOMEM;
        return MAP_FAILED;
    }
    errno = EFAULT;
    return MAP_FAILED;
}

int MockBackend::munmap(void *addr, size_t length)
{
    int result = -1;
    int err = EINVAL;

    pthread_mutex_lock(&_lock);
    for (int m = 0; m < MaxMaps; m++) {
        if (_maps[m].addr == addr && _maps[m].length == length) {
            _maps[m].addr = NULL;
            _maps[m].length = 0;
            result = 0;
            break;
        }
    }
    pthread_mutex_unlock(&_lock);
    if (result != 0) {
        errno = err;
    }
    return result;
}

int MockBackend::poll(struct pollfd *fds, nfds_t nfds, int timeoutMs)
{
    long long deadline = nowNs() + (long long)(timeoutMs < 0 ? 0 : timeoutMs) * 1000000LL;
    int ready;

    for (;;) {
        pthread_mutex_lock(&_lock);
        ready = 0;
        for (nfds_t i = 0; i < nfds; i++) {
            File *f = fileOf(fds[i].fd);

            fds[i].revents = 0;
            if (f == NULL) {
                fds[i].revents = POLLNVAL;
                ready++;
            } else if (f->validNum > 0 && frameIndex() > f->consumed) {
                fds[i].revents = (short)(fds[i].events & POLLIN);
                if (fds[i].revents != 0) {
                    ready++;
                }
            }
        }
        pthread_mutex_unlock(&_lock);
        if (ready > 0 || timeoutMs == 0) {
            return ready;
        }
        if (timeoutMs > 0 && nowNs() >= deadline) {
            return 0;
        }
        sleepNs(1000000LL);
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
