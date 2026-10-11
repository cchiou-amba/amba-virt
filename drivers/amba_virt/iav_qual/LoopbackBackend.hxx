/*
 * LoopbackBackend.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_LOOPBACKBACKEND_HXX
#define IAVQUAL_LOOPBACKBACKEND_HXX

#include "IavBackend.hxx"

#include <pthread.h>
#include <stdint.h>

extern "C" {
#include "iav_broker.h"
}

/*
 * The guest frontend's marshaling and the broker, in one process. Calls the suite makes (open,
 * ioctl, mmap, poll, close) become protocol messages handed to an IAV broker, which runs them
 * against the driver the backend is given: the real one on a board, or the mock on a build host.
 * The suite sees what a guest application would see through a virtual /dev/iav, so the same tests
 * run native and through the broker, and the benchmarks show the broker's own cost without the
 * transport. Memory mappings are made on the driver directly, as a guest maps a window of shared
 * memory, but only after the broker authorizes them.
 */
class LoopbackBackend : public IavBackend
{
public:
    /* Takes ownership of driver. cid is the guest id the broker is told the requests come from. */
    LoopbackBackend(IavBackend *driver, const char *devicePath, uint32_t cid);
    ~LoopbackBackend();

    /* False if the broker could not be set up. */
    bool ok() const { return _ok; }

    int open(const char *path, int flags);
    int close(int fd);
    int dup(int fd);
    int ioctl(int fd, unsigned long cmd, unsigned long arg);
    void *mmap(size_t length, int prot, int flags, int fd, off_t offset);
    int munmap(void *addr, size_t length);
    int poll(struct pollfd *fds, nfds_t nfds, int timeoutMs);
    const char *name() const;

private:
    /* A descriptor number points at an open file; duplicates share one open file. */
    struct Fd
    {
        bool used;
        int ofd;
    };

    struct Ofd
    {
        bool used;
        uint64_t handle;
        int refs;
    };

    static const int Slots = 256;
    static const int FdBase = 1000;

    static int opOpen(void *ctx);
    static int opClose(void *ctx, int fd);
    static int opIoctl(void *ctx, int fd, unsigned long cmd, unsigned long arg);
    static int opPoll(void *ctx, int fd, int events, int timeoutMs, int *revents);
    static void opSleep(void *ctx, unsigned int ms);
    static uint32_t opRandom(void *ctx);
    static uint64_t opNow(void *ctx);

    /* Send one request and parse the response head. Returns false if the broker gave none. */
    bool call(uint32_t op, uint64_t handle, uint32_t cmd, uint64_t value, uint64_t arg2,
              const void *arg, uint32_t argLen, struct amba_virt_iav_resp *resp, uint8_t *out,
              size_t outCap);
    bool lookup(int fd, uint64_t *handle);
    bool setWindows();

    IavBackend *_driver;
    const char *_devicePath;
    uint32_t _cid;
    struct iav_broker _broker;
    bool _ok;
    bool _brokerUp;
    int _mapFd;
    pthread_mutex_t _lock;
    Fd _fds[Slots];
    Ofd _ofds[Slots];
};

#endif /* IAVQUAL_LOOPBACKBACKEND_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
