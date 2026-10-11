/*
 * IavBrokerHarness.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBAVIRT_TEST_IAVBROKERHARNESS_HXX
#define AMBAVIRT_TEST_IAVBROKERHARNESS_HXX

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include <vector>

#include "MockIavDriver.hxx"

#include <iav_ioctl.h>

/* A broker wired to a mock driver, and helpers that speak the wire protocol to it. */
class IavBrokerHarness
{
public:
    struct Result
    {
        int ret;
        struct amba_virt_iav_resp r;
        std::vector<uint8_t> out;

        Result() : ret(-1) { memset(&r, 0, sizeof(r)); }
    };

    explicit IavBrokerHarness(enum iav_profile profile = IAV_PROFILE_QUALIFICATION,
                              unsigned int perCid = 0, unsigned int rate = 0, unsigned int burst = 0,
                              uint32_t seed = 0) :
        _inited(false)
    {
        struct iav_broker_cfg cfg;
        struct iav_broker_window w[2];
        struct iav_broker_ops ops = drv.ops();

        if (seed) {
            drv.setSeed(seed);
        }
        memset(&cfg, 0, sizeof(cfg));
        cfg.profile = profile;
        cfg.max_sessions_per_cid = perCid;
        cfg.block_max_ms = MockIavDriver::BlockMaxMs;
        cfg.rate_per_sec = rate;
        cfg.rate_burst = burst;
        _initRc = iav_broker_init(&b, &ops, &cfg);
        _inited = _initRc == 0;
        memset(w, 0, sizeof(w));
        w[0].id = AMBA_VIRT_IAV_WIN_DSP;
        w[0].base = MockIavDriver::DspBase;
        w[0].part_len = MockIavDriver::DspLen;
        w[0].size = MockIavDriver::DspLen;
        w[1].id = AMBA_VIRT_IAV_WIN_BSB;
        w[1].base = MockIavDriver::BsbBase;
        w[1].part_len = MockIavDriver::BsbLen;
        w[1].size = MockIavDriver::BsbWindow;
        w[1].ring = 1;
        if (_inited) {
            _windowsRc = iav_broker_set_windows(&b, w, 2);
        }
    }

    ~IavBrokerHarness()
    {
        if (_inited) {
            iav_broker_destroy(&b);
        }
    }

    /* Destroy the broker early, as the server does at shutdown. The destructor will not repeat it. */
    void destroyBroker()
    {
        if (_inited) {
            iav_broker_destroy(&b);
            _inited = false;
        }
    }

    int initRc() const { return _initRc; }
    int windowsRc() const { return _windowsRc; }

    Result send(uint32_t cid, const void *req, size_t len)
    {
        Result res;
        uint8_t buf[AMBA_VIRT_MAX_MSG];
        size_t n = 0;

        res.ret = iav_broker_handle(&b, cid, req, len, buf, sizeof(buf), &n);
        if (res.ret == 0 && n >= sizeof(res.r)) {
            memcpy(&res.r, buf, sizeof(res.r));
            if (n > sizeof(res.r)) {
                res.out.assign(buf + sizeof(res.r), buf + n);
            }
        }
        return res;
    }

    /* The parts of a response a threaded test needs. Uses only stack buffers: no allocation. */
    struct Raw
    {
        int ret;
        int32_t status;
        int32_t rc;
        int32_t err;
        uint64_t handle;
    };

    Raw call(uint32_t cid, uint32_t op, uint64_t handle, uint32_t cmd, uint64_t value,
             const void *arg, uint32_t argLen)
    {
        uint8_t req[AMBA_VIRT_MAX_MSG];
        uint8_t resp[AMBA_VIRT_MAX_MSG];
        struct amba_virt_iav_req q;
        struct amba_virt_iav_resp r;
        size_t n = 0;
        Raw out;

        memset(&q, 0, sizeof(q));
        q.proto = AMBA_VIRT_IAV_PROTO;
        q.op = op;
        q.handle = handle;
        q.cmd = cmd;
        q.arg_len = argLen;
        q.arg_value = value;
        memcpy(req, &q, sizeof(q));
        if (argLen && argLen <= sizeof(req) - sizeof(q)) {
            memcpy(req + sizeof(q), arg, argLen);
        }
        out.ret = iav_broker_handle(&b, cid, req, sizeof(q) + (argLen <= sizeof(req) - sizeof(q) ? argLen : 0),
                                    resp, sizeof(resp), &n);
        out.status = out.rc = out.err = 0;
        out.handle = 0;
        if (out.ret == 0 && n >= sizeof(r)) {
            memcpy(&r, resp, sizeof(r));
            out.status = r.status;
            out.rc = r.rc;
            out.err = r.err;
            out.handle = r.handle;
        } else {
            out.status = INT32_MIN;
        }
        return out;
    }

    static std::vector<uint8_t> build(uint32_t op, uint64_t handle, uint32_t cmd, uint64_t value,
                                      uint64_t arg2, const void *arg, uint32_t argLen)
    {
        struct amba_virt_iav_req q;
        std::vector<uint8_t> v(sizeof(q) + argLen);

        memset(&q, 0, sizeof(q));
        q.proto = AMBA_VIRT_IAV_PROTO;
        q.op = op;
        q.handle = handle;
        q.cmd = cmd;
        q.arg_len = argLen;
        q.arg_value = value;
        q.arg2 = arg2;
        memcpy(&v[0], &q, sizeof(q));
        if (argLen) {
            memcpy(&v[sizeof(q)], arg, argLen);
        }
        return v;
    }

    Result open(uint32_t cid)
    {
        std::vector<uint8_t> v = build(AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, NULL, 0);
        return send(cid, &v[0], v.size());
    }

    Result close(uint32_t cid, uint64_t handle)
    {
        std::vector<uint8_t> v = build(AMBA_VIRT_IAV_OP_CLOSE, handle, 0, 0, 0, NULL, 0);
        return send(cid, &v[0], v.size());
    }

    Result ioctl(uint32_t cid, uint64_t handle, unsigned long cmd, const void *arg, uint32_t argLen,
                 uint64_t value = 0)
    {
        std::vector<uint8_t> v = build(AMBA_VIRT_IAV_OP_IOCTL, handle, (uint32_t)cmd, value, 0, arg,
                                       argLen);
        return send(cid, &v[0], v.size());
    }

    Result map(uint32_t cid, uint64_t handle, uint64_t addr, uint64_t len, uint32_t prot)
    {
        std::vector<uint8_t> v = build(AMBA_VIRT_IAV_OP_MAP, handle, prot, addr, len, NULL, 0);
        return send(cid, &v[0], v.size());
    }

    Result poll(uint32_t cid, uint64_t handle, uint32_t events, uint64_t timeoutMs)
    {
        std::vector<uint8_t> v = build(AMBA_VIRT_IAV_OP_POLL, handle, events, timeoutMs, 0, NULL, 0);
        return send(cid, &v[0], v.size());
    }

    Result info(uint32_t cid)
    {
        std::vector<uint8_t> v = build(AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, NULL, 0);
        return send(cid, &v[0], v.size());
    }

    /* QUERY_DESC for canvas 0. */
    Result canvas(uint32_t cid, uint64_t handle, bool block)
    {
        struct iav_querydesc q;

        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_CANVAS;
        q.arg.canvas.canvas_id = 0;
        q.arg.canvas.non_block_flag = block ? 0 : 1;
        return ioctl(cid, handle, IAV_IOC_QUERY_DESC, &q, sizeof(q));
    }

    /* An open that must succeed. */
    uint64_t mustOpen(uint32_t cid)
    {
        Result r = open(cid);

        return (r.ret == 0 && r.r.status == 0 && r.r.rc == 0) ? r.r.handle : 0;
    }

    MockIavDriver drv;
    struct iav_broker b;

private:
    bool _inited;
    int _initRc;
    int _windowsRc;
};

#endif /* AMBAVIRT_TEST_IAVBROKERHARNESS_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
