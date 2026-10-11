/*
 * LoopbackBackend.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "LoopbackBackend.hxx"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <iav_ioctl.h>

namespace {

uint64_t clockMs()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

uint64_t nextPow2(uint64_t v)
{
    uint64_t p = 4096;

    while (p < v && p != 0) {
        p <<= 1;
    }
    return p;
}

} // namespace

int LoopbackBackend::opOpen(void *ctx)
{
    LoopbackBackend *self = static_cast<LoopbackBackend *>(ctx);

    return self->_driver->open(self->_devicePath, O_RDWR);
}

int LoopbackBackend::opClose(void *ctx, int fd)
{
    return static_cast<LoopbackBackend *>(ctx)->_driver->close(fd);
}

int LoopbackBackend::opIoctl(void *ctx, int fd, unsigned long cmd, unsigned long arg)
{
    return static_cast<LoopbackBackend *>(ctx)->_driver->ioctl(fd, cmd, arg);
}

int LoopbackBackend::opPoll(void *ctx, int fd, int events, int timeoutMs, int *revents)
{
    struct pollfd p;
    int n;

    p.fd = fd;
    p.events = (short)events;
    p.revents = 0;
    n = static_cast<LoopbackBackend *>(ctx)->_driver->poll(&p, 1, timeoutMs);
    *revents = p.revents;
    return n;
}

void LoopbackBackend::opSleep(void *, unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

uint32_t LoopbackBackend::opRandom(void *)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint32_t)(ts.tv_nsec * 2654435761u) ^ (uint32_t)ts.tv_sec;
}

uint64_t LoopbackBackend::opNow(void *)
{
    return clockMs();
}

LoopbackBackend::LoopbackBackend(IavBackend *driver, const char *devicePath, uint32_t cid) :
    _driver(driver),
    _devicePath(devicePath),
    _cid(cid),
    _ok(false),
    _brokerUp(false),
    _mapFd(-1)
{
    struct iav_broker_ops ops;
    struct iav_broker_cfg cfg;

    pthread_mutex_init(&_lock, NULL);
    memset(_fds, 0, sizeof(_fds));
    memset(_ofds, 0, sizeof(_ofds));
    memset(&ops, 0, sizeof(ops));
    ops.ctx = this;
    ops.open = opOpen;
    ops.close = opClose;
    ops.ioctl = opIoctl;
    ops.poll = opPoll;
    ops.sleep_ms = opSleep;
    ops.random32 = opRandom;
    ops.now_ms = opNow;
    memset(&cfg, 0, sizeof(cfg));
    cfg.profile = IAV_PROFILE_QUALIFICATION;
    cfg.max_sessions_per_cid = 64;
    if (iav_broker_init(&_broker, &ops, &cfg) != 0) {
        return;
    }
    _brokerUp = true;
    _mapFd = _driver->open(_devicePath, O_RDWR);
    if (_mapFd < 0 || !setWindows()) {
        return;
    }
    _ok = true;
}

LoopbackBackend::~LoopbackBackend()
{
    if (_brokerUp) {
        iav_broker_destroy(&_broker);
    }
    if (_mapFd >= 0) {
        _driver->close(_mapFd);
    }
    pthread_mutex_destroy(&_lock);
    delete _driver;
}

/* The windows are the DSP partition and the bitstream buffer, as the driver reports them. */
bool LoopbackBackend::setWindows()
{
    struct iav_broker_window w[2];
    unsigned int pids[2] = {IAV_PART_DSP, IAV_PART_BSB};
    unsigned int ids[2] = {AMBA_VIRT_IAV_WIN_DSP, AMBA_VIRT_IAV_WIN_BSB};
    unsigned int n = 0;

    memset(w, 0, sizeof(w));
    for (int i = 0; i < 2; i++) {
        struct iav_querymem q;

        memset(&q, 0, sizeof(q));
        q.mid = IAV_MEM_PARTITION;
        q.arg.partition.pid = pids[i];
        if (_driver->ioctl(_mapFd, IAV_IOC_QUERY_MEMBLOCK, (unsigned long)&q) != 0) {
            continue;
        }
        w[n].id = ids[i];
        w[n].base = q.arg.partition.mem.addr;
        w[n].part_len = q.arg.partition.mem.length;
        w[n].size = nextPow2(q.arg.partition.mem.length);
        w[n].ring = ids[i] == AMBA_VIRT_IAV_WIN_BSB;
        n++;
    }
    return n == 2 && iav_broker_set_windows(&_broker, w, n) == 0;
}

const char *LoopbackBackend::name() const
{
    return "loopback";
}

bool LoopbackBackend::call(uint32_t op, uint64_t handle, uint32_t cmd, uint64_t value,
                           uint64_t arg2, const void *arg, uint32_t argLen,
                           struct amba_virt_iav_resp *resp, uint8_t *out, size_t outCap)
{
    uint8_t req[AMBA_VIRT_MAX_MSG];
    uint8_t buf[AMBA_VIRT_MAX_MSG];
    struct amba_virt_iav_req q;
    size_t n = 0;

    memset(&q, 0, sizeof(q));
    q.proto = AMBA_VIRT_IAV_PROTO;
    q.op = op;
    q.handle = handle;
    q.cmd = cmd;
    q.arg_len = argLen;
    q.arg_value = value;
    q.arg2 = arg2;
    if (argLen > sizeof(req) - sizeof(q)) {
        return false;
    }
    memcpy(req, &q, sizeof(q));
    if (argLen) {
        memcpy(req + sizeof(q), arg, argLen);
    }
    if (iav_broker_handle(&_broker, _cid, req, sizeof(q) + argLen, buf, sizeof(buf), &n) != 0 ||
        n < sizeof(*resp)) {
        return false;
    }
    memcpy(resp, buf, sizeof(*resp));
    if (out != NULL && n > sizeof(*resp)) {
        size_t take = n - sizeof(*resp);

        memcpy(out, buf + sizeof(*resp), take < outCap ? take : outCap);
    }
    return true;
}

bool LoopbackBackend::lookup(int fd, uint64_t *handle)
{
    int slot = fd - FdBase;
    bool ok = false;

    pthread_mutex_lock(&_lock);
    if (slot >= 0 && slot < Slots && _fds[slot].used) {
        *handle = _ofds[_fds[slot].ofd].handle;
        ok = true;
    }
    pthread_mutex_unlock(&_lock);
    return ok;
}

int LoopbackBackend::open(const char *path, int flags)
{
    struct amba_virt_iav_resp r;
    int fd = -1;

    (void)flags;
    if (strcmp(path, _devicePath) != 0) {
        errno = ENOENT;
        return -1;
    }
    if (!call(AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, NULL, 0, &r, NULL, 0)) {
        errno = EIO;
        return -1;
    }
    if (r.status != 0) {
        errno = -r.status;
        return -1;
    }
    if (r.rc < 0) {
        errno = r.err;
        return -1;
    }
    pthread_mutex_lock(&_lock);
    for (int o = 0; o < Slots && fd < 0; o++) {
        if (_ofds[o].used) {
            continue;
        }
        for (int i = 0; i < Slots; i++) {
            if (!_fds[i].used) {
                _ofds[o].used = true;
                _ofds[o].handle = r.handle;
                _ofds[o].refs = 1;
                _fds[i].used = true;
                _fds[i].ofd = o;
                fd = FdBase + i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&_lock);
    if (fd < 0) {
        call(AMBA_VIRT_IAV_OP_CLOSE, r.handle, 0, 0, 0, NULL, 0, &r, NULL, 0);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

int LoopbackBackend::dup(int fd)
{
    int slot = fd - FdBase;
    int copy = -1;

    pthread_mutex_lock(&_lock);
    if (slot >= 0 && slot < Slots && _fds[slot].used) {
        for (int i = 0; i < Slots; i++) {
            if (!_fds[i].used) {
                _fds[i].used = true;
                _fds[i].ofd = _fds[slot].ofd;
                _ofds[_fds[slot].ofd].refs++;
                copy = FdBase + i;
                break;
            }
        }
        if (copy < 0) {
            errno = EMFILE;
        }
    } else {
        errno = EBADF;
    }
    pthread_mutex_unlock(&_lock);
    return copy;
}

int LoopbackBackend::close(int fd)
{
    int slot = fd - FdBase;
    uint64_t handle = 0;
    bool last = false;

    pthread_mutex_lock(&_lock);
    if (slot < 0 || slot >= Slots || !_fds[slot].used) {
        pthread_mutex_unlock(&_lock);
        errno = EBADF;
        return -1;
    }
    {
        int o = _fds[slot].ofd;

        _fds[slot].used = false;
        if (--_ofds[o].refs == 0) {
            handle = _ofds[o].handle;
            _ofds[o].used = false;
            last = true;
        }
    }
    pthread_mutex_unlock(&_lock);
    if (last) {
        struct amba_virt_iav_resp r;

        call(AMBA_VIRT_IAV_OP_CLOSE, handle, 0, 0, 0, NULL, 0, &r, NULL, 0);
    }
    return 0;
}

int LoopbackBackend::ioctl(int fd, unsigned long cmd, unsigned long arg)
{
    const struct iav_policy_entry *e;
    struct amba_virt_iav_resp r;
    uint8_t out[AMBA_VIRT_MAX_MSG];
    uint64_t handle = 0;
    uint32_t argLen = 0;
    uint64_t value = 0;
    const void *in = NULL;

    if (!lookup(fd, &handle)) {
        errno = EBADF;
        return -1;
    }
    e = iav_policy_lookup((unsigned int)cmd);
    if (e == NULL) {
        errno = ENOTTY;
        return -1;
    }
    switch (e->kind) {
    case IAV_ARG_VALUE:
        value = arg;
        break;
    case IAV_ARG_PTR:
        if ((e->dir & IAV_DIR_W)) {
            if (arg == 0) {
                errno = EFAULT;
                return -1;
            }
            in = (const void *)arg;
            argLen = (uint32_t)e->size;
        }
        break;
    default:
        break;
    }
    if (!call(AMBA_VIRT_IAV_OP_IOCTL, handle, (uint32_t)cmd, value, 0, in, argLen, &r, out,
              sizeof(out))) {
        errno = EIO;
        return -1;
    }
    if (r.status != 0) {
        errno = -r.status;
        return -1;
    }
    if (r.rc < 0) {
        errno = r.err;
        return -1;
    }
    if (r.out_len) {
        if (arg == 0) {
            errno = EFAULT;
            return -1;
        }
        memcpy((void *)arg, out, r.out_len);
    } else if (e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_R) && arg == 0) {
        errno = EFAULT;
        return -1;
    }
    return r.rc;
}

void *LoopbackBackend::mmap(size_t length, int prot, int flags, int fd, off_t offset)
{
    struct amba_virt_iav_resp r;
    uint64_t handle = 0;

    (void)flags;
    if (!lookup(fd, &handle)) {
        errno = EBADF;
        return MAP_FAILED;
    }
    if (!call(AMBA_VIRT_IAV_OP_MAP, handle, (uint32_t)prot, (uint64_t)offset, (uint64_t)length,
              NULL, 0, &r, NULL, 0)) {
        errno = EIO;
        return MAP_FAILED;
    }
    if (r.status != 0) {
        errno = -r.status;
        return MAP_FAILED;
    }
    /* Authorized: map the memory itself, as a guest maps the window. */
    return _driver->mmap(length, prot, MAP_SHARED, _mapFd, offset);
}

int LoopbackBackend::munmap(void *addr, size_t length)
{
    return _driver->munmap(addr, length);
}

int LoopbackBackend::poll(struct pollfd *fds, nfds_t nfds, int timeoutMs)
{
    struct amba_virt_iav_resp r;
    uint64_t handle = 0;
    uint64_t start = clockMs();

    if (nfds != 1) {
        errno = EINVAL;
        return -1;
    }
    if (!lookup(fds[0].fd, &handle)) {
        fds[0].revents = POLLNVAL;
        return 1;
    }
    for (;;) {
        uint64_t elapsed = clockMs() - start;
        uint64_t slice = AMBA_VIRT_IAV_POLL_SLICE_MS;

        if (timeoutMs >= 0) {
            uint64_t left = (uint64_t)timeoutMs > elapsed ? (uint64_t)timeoutMs - elapsed : 0;

            slice = left < slice ? left : slice;
        }
        if (!call(AMBA_VIRT_IAV_OP_POLL, handle, (uint32_t)fds[0].events, slice, 0, NULL, 0, &r,
                  NULL, 0)) {
            errno = EIO;
            return -1;
        }
        if (r.status != 0) {
            errno = -r.status;
            return -1;
        }
        if (r.rc < 0) {
            errno = r.err;
            return -1;
        }
        if (r.rc > 0) {
            fds[0].revents = (short)r.value;
            return 1;
        }
        if (timeoutMs == 0 || (timeoutMs > 0 && clockMs() - start >= (uint64_t)timeoutMs)) {
            fds[0].revents = 0;
            return 0;
        }
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
