/*
 * IavBackend.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_IAVBACKEND_HXX
#define IAVQUAL_IAVBACKEND_HXX

#include <poll.h>
#include <sys/types.h>

#include <cstddef>

/*
 * The only way the qualification suite reaches an IAV device. RealBackend uses the system
 * calls, so the same tests run against the real driver in EVE and against a virtual /dev/iav in
 * a guest. MockBackend simulates the driver on a build host, which proves the tests sound and
 * able to fail. The calls follow the system call conventions: -1 with errno set on failure.
 */
class IavBackend
{
public:
    virtual ~IavBackend() {}

    virtual int open(const char *path, int flags) = 0;
    virtual int close(int fd) = 0;
    virtual int dup(int fd) = 0;
    virtual int ioctl(int fd, unsigned long cmd, unsigned long arg) = 0;
    virtual void *mmap(size_t length, int prot, int flags, int fd, off_t offset) = 0;
    virtual int munmap(void *addr, size_t length) = 0;
    virtual int poll(struct pollfd *fds, nfds_t nfds, int timeoutMs) = 0;
    virtual const char *name() const = 0;
};

#endif /* IAVQUAL_IAVBACKEND_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
