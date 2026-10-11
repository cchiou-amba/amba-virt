/*
 * IavTestSupport.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_IAVTESTSUPPORT_HXX
#define IAVQUAL_IAVTESTSUPPORT_HXX

#include "IavBackend.hxx"
#include "IavQualContext.hxx"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>

#include <iav_ioctl.h>

/* Helpers shared by the test groups. Include the C++ library headers before this one. */
namespace iavtest {

inline IavBackend &backend()
{
    return IavQualContext::instance().backend();
}

inline IavQualContext &ctx()
{
    return IavQualContext::instance();
}

inline void note(const char *key, long long value)
{
    IavQualContext::instance().note(key, value);
}

inline long long nowNs()
{
    return IavQualContext::nowNs();
}

/* An ioctl whose argument is a pointer. Returns the system call result, errno preserved. */
inline int ioctlPtr(int fd, unsigned long cmd, void *arg)
{
    return backend().ioctl(fd, cmd, (unsigned long)arg);
}

/* An open device file, closed on destruction. */
class ScopedFd
{
public:
    ScopedFd() : _fd(-1) {}
    ~ScopedFd() { close(); }

    bool open()
    {
        close();
        _fd = backend().open(IavQualContext::instance().config()._device.c_str(), O_RDWR);
        return _fd >= 0;
    }

    void close()
    {
        if (_fd >= 0) {
            backend().close(_fd);
            _fd = -1;
        }
    }

    int fd() const { return _fd; }
    int release()
    {
        int fd = _fd;

        _fd = -1;
        return fd;
    }

private:
    ScopedFd(const ScopedFd &);
    ScopedFd &operator=(const ScopedFd &);

    int _fd;
};

/* A mapping, unmapped on destruction. */
class ScopedMap
{
public:
    ScopedMap() : _addr(NULL), _length(0) {}
    ~ScopedMap() { unmap(); }

    /* Returns false with errno from mmap on failure. */
    bool map(int fd, unsigned long physAddr, size_t length, int prot)
    {
        void *p;

        unmap();
        p = backend().mmap(length, prot, MAP_SHARED, fd, (off_t)physAddr);
        if (p == MAP_FAILED) {
            return false;
        }
        _addr = p;
        _length = length;
        return true;
    }

    void unmap()
    {
        if (_addr != NULL) {
            backend().munmap(_addr, _length);
            _addr = NULL;
            _length = 0;
        }
    }

    const volatile unsigned char *bytes() const { return (const volatile unsigned char *)_addr; }
    size_t length() const { return _length; }

private:
    ScopedMap(const ScopedMap &);
    ScopedMap &operator=(const ScopedMap &);

    void *_addr;
    size_t _length;
};

/* QUERY_MEMBLOCK for a partition. Returns 0 or -1 with errno. */
inline int queryPartition(int fd, unsigned int pid, unsigned long *addr, unsigned long *length)
{
    struct iav_querymem q;
    int rc;

    memset(&q, 0, sizeof(q));
    q.mid = IAV_MEM_PARTITION;
    q.arg.partition.pid = pid;
    rc = ioctlPtr(fd, IAV_IOC_QUERY_MEMBLOCK, &q);
    if (rc == 0) {
        if (addr != NULL) {
            *addr = q.arg.partition.mem.addr;
        }
        if (length != NULL) {
            *length = q.arg.partition.mem.length;
        }
    }
    return rc;
}

/* Canvas information: IAV_IOC_QUERY_INFO with IAV_INFO_CANVAS. Returns 0 or -1 with errno. */
inline int queryCanvasInfo(int fd, unsigned int canvasId, struct iav_canvas_info *out)
{
    struct iav_queryinfo q;
    int rc;

    memset(&q, 0, sizeof(q));
    q.qid = IAV_INFO_CANVAS;
    q.arg.canvas.canvas_id = canvasId;
    rc = ioctlPtr(fd, IAV_IOC_QUERY_INFO, &q);
    if (rc == 0 && out != NULL) {
        *out = q.arg.canvas;
    }
    return rc;
}

/* Query one canvas descriptor of canvas 0. */
inline int queryCanvas(int fd, bool block, struct iav_querydesc *out)
{
    memset(out, 0, sizeof(*out));
    out->qid = IAV_DESC_CANVAS;
    out->arg.canvas.canvas_id = 0;
    out->arg.canvas.non_block_flag = block ? 0 : 1;
    return ioctlPtr(fd, IAV_IOC_QUERY_DESC, out);
}

/* Query the layer 0 pyramid descriptor of channel 0. */
inline int queryPyramid(int fd, bool block, struct iav_querydesc *out)
{
    memset(out, 0, sizeof(*out));
    out->qid = IAV_DESC_PYRAMID;
    out->arg.pyramid.chan_id = 0;
    out->arg.pyramid.layers_map = 1;
    out->arg.pyramid.non_block_flag = block ? 0 : 1;
    return ioctlPtr(fd, IAV_IOC_QUERY_DESC, out);
}

/* A poll configuration with one canvas 0 event. */
inline void canvasPollConfig(struct iav_poll_event_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->valid_num = 1;
    cfg->desc_event[0].qid = IAV_DESC_CANVAS;
    cfg->desc_event[0].arg.canvas.canvas_id = 0;
}

} // namespace iavtest

#endif /* IAVQUAL_IAVTESTSUPPORT_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
