/*
 * MockIavDriver.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "MockIavDriver.hxx"

#include <errno.h>
#include <poll.h>
#include <string.h>

#include <algorithm>

#include <iav_ioctl.h>

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
    /* Threads call in concurrently, and CppUTest's leak detector is not thread safe, so the calls
     * must not allocate: reserve once here. */
    _fds.reserve(1024);
    _pollConfigured.reserve(1024);
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
    return o;
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
        if (!isAllowed(cmd)) {
            violation("an ioctl that is not on the allow-list reached the driver");
            errno = ENOTTY;
            return -1;
        }
        if (_failCmd == cmd && _failErr) {
            errno = _failErr;
            return -1;
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
    default:
        return 0;
    }
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
