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
        if (mknod(path.c_str(), S_IFCHR | 0600, makedev(major, minor)) < 0) {
            if (errno != EEXIST)
                return -errno;
        }
        chmod(path.c_str(), 0600);
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
          _shmSize(other._shmSize), _info(other._info),
          _pendingRequest(std::move(other._pendingRequest)) {
        other._fd = -1;
        other._shmMap = MAP_FAILED;
        other._mappedSize = 0;
        other._shmSize = 0;
        other._pendingRequest.clear();
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
            _pendingRequest = std::move(other._pendingRequest);
            other._fd = -1;
            other._shmMap = MAP_FAILED;
            other._mappedSize = 0;
            other._shmSize = 0;
            other._pendingRequest.clear();
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
        _pendingRequest.clear();
        if (_fd >= 0) {
            close(_fd);
            _fd = -1;
        }
    }

    int getFd() const { return _fd; }
    bool isOpen() const { return _fd >= 0; }
    const struct amba_virt_info &getInfo() const { return _info; }
    uint32_t getShmSize() const { return _shmSize; }
    size_t getMappedSize() const { return _mappedSize; }

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
        if (len == 0 || len > AMBA_VIRT_MAX_MSG)
            return -EINVAL;

        if (!data)
            return -EINVAL;
        if (!_pendingRequest.empty())
            return -EBUSY;
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        _pendingRequest.assign(bytes, bytes + len);
        return 0;
    }

    int recv(void *buf, uint32_t maxLen, uint32_t &receivedLen, int32_t timeoutMs = 5000) {
        if (_fd < 0)
            return -EBADF;

        struct amba_virt_xfer rx;
        memset(&rx, 0, sizeof(rx));
        rx.timeout_ms = timeoutMs;

        if (_pendingRequest.empty())
            return -ENODATA;
        rx.len = static_cast<uint32_t>(_pendingRequest.size());
        memcpy(rx.data, _pendingRequest.data(), rx.len);
        _pendingRequest.clear();
        if (ioctl(_fd, AMBA_VIRT_IOC_RPC, &rx) < 0)
            return -errno;

        receivedLen = rx.len;
        if (buf && rx.len > 0) {
            uint32_t toCopy = (rx.len < maxLen) ? rx.len : maxLen;
            memcpy(buf, rx.data, toCopy);
        }
        return 0;
    }

    int flushRx(int maxDrain = 100) {
        (void)maxDrain;
        if (_fd < 0)
            return -EBADF;
        _pendingRequest.clear();
        return 0;
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

#if defined(__QNX__) || defined(__QNXNTO__)
        if (_info.shm_phys != 0) {
            _shmMap = mmap_device_memory(nullptr, size, prot, 0, _info.shm_phys + offset);
            if (_shmMap != MAP_FAILED) {
                _mappedSize = size;
                return _shmMap;
            }
        } else {
            size_t map_sz = size;
            if (map_sz > 64 * 1024 * 1024)
                map_sz = 64 * 1024 * 1024;
            _shmMap = mmap(nullptr, map_sz, prot, MAP_ANON | MAP_PRIVATE, NOFD, 0);
            if (_shmMap != MAP_FAILED) {
                _mappedSize = map_sz;
                return _shmMap;
            }
        }
#endif

        _shmMap = mmap(nullptr, size, prot, flags, _fd, offset);
        if (_shmMap == MAP_FAILED)
            return MAP_FAILED;

        _mappedSize = size;
        return _shmMap;
    }

    void unmapShm() {
        if (_shmMap != MAP_FAILED) {
#if defined(__QNX__) || defined(__QNXNTO__)
            if (_info.shm_phys != 0)
                munmap_device_memory(_shmMap, _mappedSize);
            else
                munmap(_shmMap, _mappedSize);
#else
            munmap(_shmMap, _mappedSize);
#endif
            _shmMap = MAP_FAILED;
            _mappedSize = 0;
        }
    }

    void *getShmPtr() const { return _shmMap; }

    int rpcTransaction(uint32_t msgType, uint32_t seq,
                       const void *reqData, uint32_t reqLen,
                       uint32_t expectedRespType,
                       void *respData, uint32_t maxRespLen,
                       uint32_t *outRespLen = nullptr,
                       int32_t timeoutMs = 5000) {
        if (_fd < 0)
            return -EBADF;

        std::vector<uint8_t> txBuf(sizeof(struct amba_virt_msg) + reqLen);
        struct amba_virt_msg *msg = reinterpret_cast<struct amba_virt_msg *>(txBuf.data());
        msg->type = msgType;
        msg->seq = seq;
        msg->shm_off = 0;
        msg->shm_len = reqLen;
        if (reqData && reqLen > 0)
            memcpy(txBuf.data() + sizeof(struct amba_virt_msg), reqData, reqLen);

        int ret = send(txBuf.data(), static_cast<uint32_t>(txBuf.size()));
        if (ret < 0)
            return ret;

        std::vector<uint8_t> rxBuf(sizeof(struct amba_virt_msg) + maxRespLen);
        uint32_t rxLen = 0;
        ret = recv(rxBuf.data(), static_cast<uint32_t>(rxBuf.size()), rxLen, timeoutMs);
        if (ret < 0)
            return ret;

        if (rxLen < sizeof(struct amba_virt_msg))
            return -EBADMSG;

        const struct amba_virt_msg *respMsg = reinterpret_cast<const struct amba_virt_msg *>(rxBuf.data());
        if (respMsg->type != expectedRespType)
            return -EPROTO;

        uint32_t payloadLen = (rxLen > sizeof(struct amba_virt_msg)) ? (rxLen - sizeof(struct amba_virt_msg)) : 0;
        uint32_t copyLen = (payloadLen < maxRespLen) ? payloadLen : maxRespLen;
        if (respData && copyLen > 0)
            memcpy(respData, rxBuf.data() + sizeof(struct amba_virt_msg), copyLen);
        if (outRespLen)
            *outRespLen = payloadLen;

        return 0;
    }

    int setDeviceBounds(const struct amba_virt_dev_bounds_req &req,
                        struct amba_virt_dev_bounds_resp &resp,
                        uint32_t seq = 1) {
        memset(&resp, 0, sizeof(resp));
        int ret = rpcTransaction(AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ, seq,
                                 &req, sizeof(req),
                                 AMBA_VIRT_MSG_DEV_SET_BOUNDS_RESP,
                                 &resp, sizeof(resp));
        if (ret < 0)
            return ret;
        return resp.status;
    }

    int releaseDeviceBounds(uint32_t devType,
                            struct amba_virt_dev_bounds_resp *outResp = nullptr,
                            uint32_t seq = 1) {
        struct amba_virt_dev_bounds_req req;
        memset(&req, 0, sizeof(req));
        req.dev_type = devType;
        struct amba_virt_dev_bounds_resp resp;
        memset(&resp, 0, sizeof(resp));
        int ret = rpcTransaction(AMBA_VIRT_MSG_DEV_RELEASE_BOUNDS_REQ, seq,
                                 &req, sizeof(req),
                                 AMBA_VIRT_MSG_DEV_RELEASE_BOUNDS_RESP,
                                 &resp, sizeof(resp));
        if (ret < 0)
            return ret;
        if (outResp)
            *outResp = resp;
        return resp.status;
    }

    int allocMemory(uint32_t size, uint32_t align, uint32_t devAffinity,
                    struct amba_virt_mem_resp &resp, uint32_t flags = 0,
                    uint32_t seq = 1) {
        struct amba_virt_mem_req req;
        memset(&req, 0, sizeof(req));
        req.op = AMBA_VIRT_MEM_OP_ALLOC;
        req.size = size;
        req.align = align;
        req.dev_affinity = devAffinity;
        req.flags = flags;
        memset(&resp, 0, sizeof(resp));
        int ret = rpcTransaction(AMBA_VIRT_MSG_MEM_ALLOC_REQ, seq,
                                 &req, sizeof(req),
                                 AMBA_VIRT_MSG_MEM_ALLOC_RESP,
                                 &resp, sizeof(resp));
        if (ret < 0)
            return ret;
        return resp.status;
    }

    int freeMemory(uint32_t barOffset,
                   struct amba_virt_mem_resp *outResp = nullptr,
                   uint32_t seq = 1) {
        struct amba_virt_mem_req req;
        memset(&req, 0, sizeof(req));
        req.op = AMBA_VIRT_MEM_OP_FREE;
        req.bar_offset = barOffset;
        struct amba_virt_mem_resp resp;
        memset(&resp, 0, sizeof(resp));
        int ret = rpcTransaction(AMBA_VIRT_MSG_MEM_ALLOC_REQ, seq,
                                 &req, sizeof(req),
                                 AMBA_VIRT_MSG_MEM_ALLOC_RESP,
                                 &resp, sizeof(resp));
        if (ret < 0)
            return ret;
        if (outResp)
            *outResp = resp;
        return resp.status;
    }

    int querySelf(struct amba_virt_peer_desc &selfDesc,
                  struct amba_virt_query_resp *outResp = nullptr,
                  uint32_t seq = 1) {
        struct amba_virt_query_req req;
        memset(&req, 0, sizeof(req));
        req.query_op = AMBA_VIRT_QUERY_SELF;
        struct amba_virt_query_resp resp;
        memset(&resp, 0, sizeof(resp));
        int ret = rpcTransaction(AMBA_VIRT_MSG_QUERY_REQ, seq,
                                 &req, sizeof(req),
                                 AMBA_VIRT_MSG_QUERY_RESP,
                                 &resp, sizeof(resp));
        if (ret < 0)
            return ret;
        if (outResp)
            *outResp = resp;
        if (resp.status < 0)
            return resp.status;
        if (resp.count > 0)
            memcpy(&selfDesc, resp.payload, sizeof(selfDesc));
        return 0;
    }

    int queryPeers(std::vector<struct amba_virt_peer_desc> &peers,
                   uint32_t targetCid = 0,
                   struct amba_virt_query_resp *outResp = nullptr,
                   uint32_t seq = 1) {
        struct amba_virt_query_req req;
        memset(&req, 0, sizeof(req));
        req.query_op = AMBA_VIRT_QUERY_PEERS;
        req.target_cid = targetCid;
        struct amba_virt_query_resp resp;
        memset(&resp, 0, sizeof(resp));
        int ret = rpcTransaction(AMBA_VIRT_MSG_QUERY_REQ, seq,
                                 &req, sizeof(req),
                                 AMBA_VIRT_MSG_QUERY_RESP,
                                 &resp, sizeof(resp));
        if (ret < 0)
            return ret;
        if (outResp)
            *outResp = resp;
        if (resp.status < 0)
            return resp.status;
        peers.clear();
        peers.resize(resp.count);
        if (resp.count > 0) {
            uint32_t maxBytes = sizeof(resp.payload);
            uint32_t copyBytes = resp.count * sizeof(struct amba_virt_peer_desc);
            if (copyBytes > maxBytes)
                copyBytes = maxBytes;
            memcpy(peers.data(), resp.payload, copyBytes);
        }
        return 0;
    }

    int queryTopology(struct amba_virt_topo_desc &topo,
                      struct amba_virt_query_resp *outResp = nullptr,
                      uint32_t seq = 1) {
        struct amba_virt_query_req req;
        memset(&req, 0, sizeof(req));
        req.query_op = AMBA_VIRT_QUERY_DEV_TOPOLOGY;
        struct amba_virt_query_resp resp;
        memset(&resp, 0, sizeof(resp));
        int ret = rpcTransaction(AMBA_VIRT_MSG_QUERY_REQ, seq,
                                 &req, sizeof(req),
                                 AMBA_VIRT_MSG_QUERY_RESP,
                                 &resp, sizeof(resp));
        if (ret < 0)
            return ret;
        if (outResp)
            *outResp = resp;
        if (resp.status < 0)
            return resp.status;
        if (resp.count > 0)
            memcpy(&topo, resp.payload, sizeof(topo));
        return 0;
    }

    int queryDevMem(std::vector<struct amba_virt_dev_mem_desc> &devs,
                    uint32_t devId = 0,
                    struct amba_virt_query_resp *outResp = nullptr,
                    uint32_t seq = 1) {
        struct amba_virt_query_req req;
        memset(&req, 0, sizeof(req));
        req.query_op = AMBA_VIRT_QUERY_DEV_MEM;
        req.dev_id = devId;
        struct amba_virt_query_resp resp;
        memset(&resp, 0, sizeof(resp));
        int ret = rpcTransaction(AMBA_VIRT_MSG_QUERY_REQ, seq,
                                 &req, sizeof(req),
                                 AMBA_VIRT_MSG_QUERY_RESP,
                                 &resp, sizeof(resp));
        if (ret < 0)
            return ret;
        if (outResp)
            *outResp = resp;
        if (resp.status < 0)
            return resp.status;
        devs.clear();
        devs.resize(resp.count);
        if (resp.count > 0) {
            uint32_t maxBytes = sizeof(resp.payload);
            uint32_t copyBytes = resp.count * sizeof(struct amba_virt_dev_mem_desc);
            if (copyBytes > maxBytes)
                copyBytes = maxBytes;
            memcpy(devs.data(), resp.payload, copyBytes);
        }
        return 0;
    }

private:
    std::string _path;
    int _fd;
    void *_shmMap;
    size_t _mappedSize{0};
    uint32_t _shmSize{0};
    struct amba_virt_info _info{};
    std::vector<uint8_t> _pendingRequest;
};

#endif /* AMBA_VIRT_DEV_HXX */
