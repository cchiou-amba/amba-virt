/*
 * amba_virt_dev.hxx
 *
 * Portable C++ device abstraction for Ambarella Virtualization (/dev/amba_virt).
 * Supports both Linux (Ubuntu HVM) and QNX Neutrino RTOS (QNX 8.0 HVM).
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_DEV_HXX
#define AMBA_VIRT_DEV_HXX

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sysmacros.h>
#endif

#include "amba_virt.h"
#include "amba_virt_test.h"

class AmbaVirtDevice {
public:
    static int ensureDevNode(const std::string &path = AMBA_VIRT_DEV_PATH) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            if (S_ISCHR(st.st_mode))
                return 0;
            return -1;
        }

#if defined(__QNX__) || defined(__QNXNTO__)
        /* On QNX Neutrino, device nodes are registered directly in pathname space
         * by the Resource Manager (resmgr_attach), not created via mknod.
         * If the node is missing, the resource manager is not running. */
        return -ENODEV;
#elif defined(__linux__)
        int major = -1, minor = 0;
        FILE *fp = nullptr;
        char line[256];

        /* 1. Try reading major:minor from sysfs */
        fp = fopen("/sys/class/amba_virt/amba_virt/dev", "r");
        if (fp) {
            if (fscanf(fp, "%d:%d", &major, &minor) == 2) {
                fclose(fp);
                goto create_node;
            }
            fclose(fp);
        }

        /* 2. Fallback: Parse /proc/devices */
        fp = fopen("/proc/devices", "r");
        if (fp) {
            int inChar = 0;
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "Character devices:")) {
                    inChar = 1;
                    continue;
                }
                if (strstr(line, "Block devices:")) {
                    inChar = 0;
                    break;
                }
                if (inChar) {
                    int num;
                    char name[64];
                    if (sscanf(line, "%d %63s", &num, name) == 2) {
                        if (strcmp(name, AMBA_VIRT_DEV_NAME) == 0) {
                            major = num;
                            minor = 0;
                            break;
                        }
                    }
                }
            }
            fclose(fp);
        }

        if (major < 0)
            return -ENODEV;

create_node:
        if (mknod(path.c_str(), S_IFCHR | 0666, makedev(major, minor)) < 0) {
            if (errno != EEXIST)
                return -errno;
        }
        chmod(path.c_str(), 0666);
        return 0;
#else
        return -ENOSYS;
#endif
    }

    explicit AmbaVirtDevice(const std::string &path = AMBA_VIRT_DEV_PATH)
        : _path(path), _fd(-1), _shmMap(MAP_FAILED), _mappedSize(0),
          _shmSize(0) {
        memset(&_info, 0, sizeof(_info));
    }

    ~AmbaVirtDevice() {
        closeDevice();
    }

    // Disable copy, enable move
    AmbaVirtDevice(const AmbaVirtDevice &) = delete;
    AmbaVirtDevice &operator=(const AmbaVirtDevice &) = delete;

    AmbaVirtDevice(AmbaVirtDevice &&other) noexcept
        : _path(std::move(other._path)), _fd(other._fd),
          _shmMap(other._shmMap), _mappedSize(other._mappedSize),
          _shmSize(other._shmSize), _info(other._info) {
        other._fd = -1;
        other._shmMap = MAP_FAILED;
        other._mappedSize = 0;
        other._shmSize = 0;
        memset(&other._info, 0, sizeof(other._info));
    }

    AmbaVirtDevice &operator=(AmbaVirtDevice &&other) noexcept {
        if (this != &other) {
            closeDevice();
            _path = std::move(other._path);
            _fd = other._fd;
            _shmMap = other._shmMap;
            _mappedSize = other._mappedSize;
            _shmSize = other._shmSize;
            _info = other._info;
            other._fd = -1;
            other._shmMap = MAP_FAILED;
            other._mappedSize = 0;
            other._shmSize = 0;
            memset(&other._info, 0, sizeof(other._info));
        }
        return *this;
    }

    int openDevice(int flags = O_RDWR) {
        if (_fd >= 0)
            return 0;

        ensureDevNode(_path);
        _fd = open(_path.c_str(), flags);
        if (_fd < 0)
            return -errno;

        memset(&_info, 0, sizeof(_info));
        if (ioctl(_fd, AMBA_VIRT_IOC_GET_INFO, &_info) == 0) {
            _shmSize = _info.shm_size;
        }
        return 0;
    }

    void closeDevice() {
        unmapShm();
        if (_fd >= 0) {
            close(_fd);
            _fd = -1;
        }
    }

    int getFd() const { return _fd; }
    bool isOpen() const { return _fd >= 0; }
    const struct amba_virt_info &getInfo() const { return _info; }
    uint32_t getShmSize() const { return _shmSize; }

    int refreshInfo() {
        if (_fd < 0)
            return -EBADF;
        if (ioctl(_fd, AMBA_VIRT_IOC_GET_INFO, &_info) < 0)
            return -errno;
        _shmSize = _info.shm_size;
        return 0;
    }

    int connect() {
        if (_fd < 0)
            return -EBADF;
        if (ioctl(_fd, AMBA_VIRT_IOC_CONNECT, nullptr) < 0)
            return -errno;
        return 0;
    }

    int send(const void *data, uint32_t len) {
        if (_fd < 0)
            return -EBADF;
        if (len > AMBA_VIRT_MAX_MSG)
            return -EINVAL;

        struct amba_virt_xfer xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.len = len;
        if (data && len > 0)
            memcpy(xfer.data, data, len);

        if (ioctl(_fd, AMBA_VIRT_IOC_SEND, &xfer) < 0)
            return -errno;
        return 0;
    }

    int recv(void *buf, uint32_t maxLen, uint32_t &receivedLen, int32_t timeoutMs = 5000) {
        if (_fd < 0)
            return -EBADF;

        struct amba_virt_xfer rx;
        memset(&rx, 0, sizeof(rx));
        rx.timeout_ms = timeoutMs;

        if (ioctl(_fd, AMBA_VIRT_IOC_RECV, &rx) < 0)
            return -errno;

        receivedLen = rx.len;
        if (buf && rx.len > 0) {
            uint32_t toCopy = (rx.len < maxLen) ? rx.len : maxLen;
            memcpy(buf, rx.data, toCopy);
        }
        return 0;
    }

    int flushRx(int maxDrain = 100) {
        if (_fd < 0)
            return -EBADF;
        struct amba_virt_xfer rx;
        int drained = 0;
        while (drained < maxDrain) {
            memset(&rx, 0, sizeof(rx));
            rx.timeout_ms = 10;
            if (ioctl(_fd, AMBA_VIRT_IOC_RECV, &rx) < 0) {
                break;
            }
            drained++;
        }
        return drained;
    }

    void *mapShm(size_t size = 0, int prot = PROT_READ | PROT_WRITE, int flags = MAP_SHARED, off_t offset = 0) {
        if (_fd < 0)
            return MAP_FAILED;
        if (size == 0)
            size = _shmSize;
        if (size == 0)
            return MAP_FAILED;

        if (_shmMap != MAP_FAILED)
            unmapShm();

        _shmMap = mmap(nullptr, size, prot, flags, _fd, offset);
        if (_shmMap == MAP_FAILED)
            return MAP_FAILED;

        _mappedSize = size;
        return _shmMap;
    }

    void unmapShm() {
        if (_shmMap != MAP_FAILED) {
            munmap(_shmMap, _mappedSize);
            _shmMap = MAP_FAILED;
            _mappedSize = 0;
        }
    }

    void *getShmPtr() const { return _shmMap; }

private:
    std::string _path;
    int _fd;
    void *_shmMap;
    size_t _mappedSize{0};
    uint32_t _shmSize{0};
    struct amba_virt_info _info{};
};

#endif /* AMBA_VIRT_DEV_HXX */
