/*
 * MockBackend.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_MOCKBACKEND_HXX
#define IAVQUAL_MOCKBACKEND_HXX

#include "IavBackend.hxx"

#include <pthread.h>
#include <stddef.h>

/*
 * A simulated IAV driver for build hosts. It models just enough of the driver for the
 * suite: partitions that can be mapped by physical address, canvas and frame descriptors that
 * advance with time, a per-file poll configuration, and the error returns of the real driver.
 * The Variant values are deliberately broken drivers, used as negative controls: each one must
 * make the suite fail.
 */
class MockBackend : public IavBackend
{
public:
    enum Variant
    {
        VariantNone,
        VariantBadErrno,
        VariantBadAlignment,
        VariantOverlap,
        VariantNonMonotonic,
        VariantLeakOnClose
    };

    /* Returns false for an unknown variant name. Accepts "mock" and "mock-<variant>". */
    static bool variantFromName(const char *name, Variant *out);

    explicit MockBackend(Variant variant);
    ~MockBackend();

    int open(const char *path, int flags);
    int close(int fd);
    int dup(int fd);
    int ioctl(int fd, unsigned long cmd, unsigned long arg);
    void *mmap(size_t length, int prot, int flags, int fd, off_t offset);
    int munmap(void *addr, size_t length);
    int poll(struct pollfd *fds, nfds_t nfds, int timeoutMs);
    const char *name() const;

private:
    struct Partition
    {
        unsigned int pid;
        unsigned long addr;
        unsigned long length;
        unsigned char *mem;
    };

    struct File
    {
        int refs;
        unsigned int validNum;
        unsigned int qid;
        unsigned int canvasId;
        long long consumed;
        long long cursor;
    };

    struct Mapping
    {
        void *addr;
        size_t length;
    };

    static const int MaxFds = 128;
    static const int FdBase = 100;
    static const int MaxMaps = 128;
    static const int NumPartitions = 3;

    int openLocked(const char *path);
    int closeLocked(int fd);
    int dupLocked(int fd);
    void *mmapLocked(size_t length, int fd, off_t offset);
    long long frameIndex() const;
    /* Called with _lock held; the lock is released while it sleeps. */
    void waitForFrame(long long after, int timeoutMs);
    int fail(int err);
    void unlockKeepErrno();
    File *fileOf(int fd);
    const Partition *findPartition(unsigned int pid) const;
    int doIoctl(File *f, unsigned long cmd, unsigned long arg);
    int doQueryDesc(File *f, void *arg);

    Variant _variant;
    long long _startNs;
    Partition _parts[NumPartitions];
    int _fdFile[MaxFds];
    File _files[MaxFds];
    Mapping _maps[MaxMaps];
    unsigned int _leakedValidNum;
    long long _queries;
    pthread_mutex_t _lock;
};

#endif /* IAVQUAL_MOCKBACKEND_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
