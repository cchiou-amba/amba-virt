/*
 * RealBackend.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_REALBACKEND_HXX
#define IAVQUAL_REALBACKEND_HXX

#include "IavBackend.hxx"

/* The system calls, unchanged. */
class RealBackend : public IavBackend
{
public:
    int open(const char *path, int flags);
    int close(int fd);
    int dup(int fd);
    int ioctl(int fd, unsigned long cmd, unsigned long arg);
    void *mmap(size_t length, int prot, int flags, int fd, off_t offset);
    int munmap(void *addr, size_t length);
    int poll(struct pollfd *fds, nfds_t nfds, int timeoutMs);
    const char *name() const;
};

#endif /* IAVQUAL_REALBACKEND_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
