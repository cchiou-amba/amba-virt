/*
 * RealBackend.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "RealBackend.hxx"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int RealBackend::open(const char *path, int flags)
{
    return ::open(path, flags);
}

int RealBackend::close(int fd)
{
    return ::close(fd);
}

int RealBackend::dup(int fd)
{
    return ::dup(fd);
}

int RealBackend::ioctl(int fd, unsigned long cmd, unsigned long arg)
{
    return ::ioctl(fd, cmd, arg);
}

void *RealBackend::mmap(size_t length, int prot, int flags, int fd, off_t offset)
{
    return ::mmap(NULL, length, prot, flags, fd, offset);
}

int RealBackend::munmap(void *addr, size_t length)
{
    return ::munmap(addr, length);
}

int RealBackend::poll(struct pollfd *fds, nfds_t nfds, int timeoutMs)
{
    return ::poll(fds, nfds, timeoutMs);
}

const char *RealBackend::name() const
{
    return "real";
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
