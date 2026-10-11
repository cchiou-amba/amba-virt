/*
 * TestHostIavBrokerFuzz.cxx
 *
 * Seeded, deterministic fuzzing of the broker.
 *
 * Request fuzzing mutates valid requests and random bytes, and checks after every call that the
 * driver was never given anything it should not be, that the response is well formed, and that
 * the broker's limits hold. Sequence fuzzing runs random operation sequences from several guests
 * against an independent model of which handles are valid, and compares the broker with the model
 * after every step. Both finish by releasing every guest and checking that nothing was leaked.
 *
 * IAV_FUZZ_ITERS and IAV_FUZZ_SEED override the defaults. A failing seed reproduces the failure.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <limits.h>
#include <stdlib.h>

#include <map>

#include "IavBrokerHarness.hxx"

#include "CppUTest/TestHarness.h"

namespace {

class Rng
{
public:
    explicit Rng(uint64_t seed) : _s(seed ? seed : 0x9e3779b97f4a7c15ull) {}

    uint64_t next()
    {
        _s ^= _s >> 12;
        _s ^= _s << 25;
        _s ^= _s >> 27;
        return _s * 0x2545f4914f6cdd1dull;
    }

    uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }

    uint64_t extreme()
    {
        static const uint64_t e[] = {0, 1, 2, 0xff, 0x100, 0xffff, 0x10000, 0x7fffffffull,
                                     0x80000000ull, 0xffffffffull, 0x100000000ull,
                                     0x7fffffffffffffffull, 0x8000000000000000ull,
                                     0xffffffffffffffffull, 4095, 4096, 4097};
        return e[below(sizeof(e) / sizeof(e[0]))];
    }

private:
    uint64_t _s;
};

unsigned long envNumber(const char *name, unsigned long fallback)
{
    const char *v = getenv(name);

    return v && *v ? strtoul(v, NULL, 0) : fallback;
}

const unsigned long Cmds[] = {
    IAV_IOC_GET_IAV_STATE,        IAV_IOC_GET_CHIP_ID,          IAV_IOC_QUERY_ENC_MODE_CAP,
    IAV_IOC_QUERY_ENC_BUF_CAP,    IAV_IOC_QUERY_IAV_CAP,        IAV_IOC_GET_SYSTEM_RESOURCE,
    IAV_IOC_GET_CHAN_CONFIG,      IAV_IOC_GET_CANVAS_CONFIG,    IAV_IOC_DRV_DSP_INFO,
    IAV_IOC_QUERY_INFO,           IAV_IOC_QUERY_MEMBLOCK,       IAV_IOC_QUERY_DESC,
    IAV_IOC_SET_POLL_EVENT_CFG,   IAV_IOC_CLEAR_POLL_EVENT_CFG, IAV_IOC_WAIT_NEXT_IDSP_FRAME,
    IAV_IOC_ENTER_IDLE,           IAV_IOC_GDMA_COPY,            IAV_IOC_SET_IMG_SCALE,
};

const uint32_t Cids[] = {1, 2, 3, 4, 0, 0xffffffffu};

/* A request that is valid, or nearly: the starting point of the mutations. */
std::vector<uint8_t> baseRequest(Rng &rng, const std::vector<uint64_t> &handles)
{
    uint64_t handle = handles.empty() ? rng.extreme() : handles[rng.below((uint32_t)handles.size())];
    uint8_t arg[AMBA_VIRT_MAX_MSG];

    memset(arg, 0, sizeof(arg));
    switch (rng.below(7)) {
    case 0:
        return IavBrokerHarness::build(AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, NULL, 0);
    case 1:
        return IavBrokerHarness::build(AMBA_VIRT_IAV_OP_CLOSE, handle, 0, 0, 0, NULL, 0);
    case 2:
        return IavBrokerHarness::build(AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, NULL, 0);
    case 3:
        return IavBrokerHarness::build(AMBA_VIRT_IAV_OP_MAP, handle, PROT_READ,
                                       rng.below(2) ? MockIavDriver::DspBase : MockIavDriver::BsbBase,
                                       rng.below(2) ? 4096 : 0x100000, NULL, 0);
    case 4:
        return IavBrokerHarness::build(AMBA_VIRT_IAV_OP_POLL, handle, POLLIN, rng.below(200), 0,
                                       NULL, 0);
    default: {
        unsigned long cmd = Cmds[rng.below(sizeof(Cmds) / sizeof(Cmds[0]))];
        const struct iav_policy_entry *e = iav_policy_lookup((unsigned int)cmd);
        uint32_t len = (e && e->kind == IAV_ARG_PTR && (e->dir & IAV_DIR_W)) ? (uint32_t)e->size : 0;

        for (uint32_t i = 0; i < len; i++) {
            arg[i] = (uint8_t)rng.below(rng.below(4) == 0 ? 256 : 2);
        }
        return IavBrokerHarness::build(AMBA_VIRT_IAV_OP_IOCTL, handle, (uint32_t)cmd,
                                       rng.next(), 0, arg, len);
    }
    }
}

void mutate(Rng &rng, std::vector<uint8_t> &v)
{
    unsigned int n = 1 + rng.below(4);

    for (unsigned int i = 0; i < n && !v.empty(); i++) {
        switch (rng.below(6)) {
        case 0:
            v[rng.below((uint32_t)v.size())] ^= (uint8_t)(1u << rng.below(8));
            break;
        case 1:
            v[rng.below((uint32_t)v.size())] = (uint8_t)rng.next();
            break;
        case 2: { /* an extreme value over a 4 or 8 byte field */
            size_t at = rng.below((uint32_t)v.size());
            uint64_t x = rng.extreme();

            for (size_t k = 0; k < (rng.below(2) ? 8u : 4u) && at + k < v.size(); k++) {
                v[at + k] = (uint8_t)(x >> (8 * k));
            }
            break;
        }
        case 3:
            v.resize(rng.below((uint32_t)v.size() + 1));
            break;
        case 4:
            v.resize(v.size() + 1 + rng.below(64), (uint8_t)rng.next());
            break;
        default: { /* an extreme in one of the header fields */
            size_t slot = rng.below(sizeof(struct amba_virt_iav_req) / 4);
            uint64_t x = rng.extreme();

            for (size_t k = 0; k < 4 && slot * 4 + k < v.size(); k++) {
                v[slot * 4 + k] = (uint8_t)(x >> (8 * k));
            }
            break;
        }
        }
    }
    if (v.size() > AMBA_VIRT_MAX_MSG) {
        v.resize(AMBA_VIRT_MAX_MSG);
    }
}

} // namespace

TEST_GROUP(IavBrokerFuzz)
{
    /* The checks that hold after every call. */
    void checkResponse(const IavBrokerHarness::Result &r)
    {
        LONGS_EQUAL(0, r.ret);
        LONGS_EQUAL(AMBA_VIRT_IAV_PROTO, r.r.proto);
        LONGS_EQUAL(0, r.r.reserved);
        LONGS_EQUAL(r.r.out_len, r.out.size());
        CHECK(r.r.out_len <= AMBA_VIRT_IAV_MAX_OUT);
        CHECK(r.r.status <= 0 && r.r.status >= -4095);
        if (r.r.status != 0) {
            LONGS_EQUAL(0, r.r.out_len);
        }
        if (r.r.status == 0 && r.r.rc < 0) {
            CHECK(r.r.err > 0 && r.r.err < 4096);
            LONGS_EQUAL(0, r.r.out_len);
        }
    }

    void finish(IavBrokerHarness &h)
    {
        for (size_t i = 0; i < sizeof(Cids) / sizeof(Cids[0]); i++) {
            iav_broker_client_gone(&h.b, Cids[i]);
        }
        LONGS_EQUAL(0, iav_broker_session_count(&h.b));
        LONGS_EQUAL(0, h.drv.openFds());
        LONGS_EQUAL(h.drv.totalOpens(), h.drv.totalCloses());
        CHECK(h.drv.violations().empty());
    }
};

TEST(IavBrokerFuzz, MutatedRequests)
{
    unsigned long iters = envNumber("IAV_FUZZ_ITERS", 20000);
    uint64_t seed = envNumber("IAV_FUZZ_SEED", 1);
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, 8);
    Rng rng(seed);
    std::vector<uint64_t> handles;

    for (unsigned long i = 0; i < iters; i++) {
        std::vector<uint8_t> req;
        uint32_t cid = Cids[rng.below(sizeof(Cids) / sizeof(Cids[0]))];
        IavBrokerHarness::Result r;

        switch (rng.below(4)) {
        case 0: { /* random bytes */
            req.resize(rng.below(AMBA_VIRT_MAX_MSG + 1));
            for (size_t k = 0; k < req.size(); k++) {
                req[k] = (uint8_t)rng.next();
            }
            break;
        }
        case 1:
            req = baseRequest(rng, handles);
            break;
        default:
            req = baseRequest(rng, handles);
            mutate(rng, req);
            break;
        }
        r = h.send(cid, req.empty() ? NULL : &req[0], req.size());
        checkResponse(r);
        if (r.r.status == 0 && r.r.rc == 0 && r.r.handle != 0 && handles.size() < 64) {
            handles.push_back(r.r.handle);
        }
        for (size_t c = 0; c < sizeof(Cids) / sizeof(Cids[0]); c++) {
            CHECK(iav_broker_cid_sessions(&h.b, Cids[c]) <= 8);
        }
        CHECK(h.drv.violations().empty());
        if (i % 997 == 996) {
            iav_broker_client_gone(&h.b, Cids[rng.below(sizeof(Cids) / sizeof(Cids[0]))]);
            h.drv.advance(rng.below(10000));
            iav_broker_reap_idle(&h.b, rng.below(5000));
        }
    }
    finish(h);
}

TEST(IavBrokerFuzz, OperationSequencesAgainstAModel)
{
    unsigned long iters = envNumber("IAV_FUZZ_ITERS", 20000);
    uint64_t seed = envNumber("IAV_FUZZ_SEED", 1) + 1000;
    const unsigned int limit = 6;
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, limit);
    Rng rng(seed);

    struct Live
    {
        uint32_t cid;
        uint64_t handle;
    };
    std::vector<Live> live;
    std::vector<Live> dead;
    std::map<uint32_t, unsigned int> count;
    static const uint32_t cids[] = {1, 2, 3, 4};

    for (unsigned long i = 0; i < iters; i++) {
        uint32_t cid = cids[rng.below(4)];
        IavBrokerHarness::Result r;

        switch (rng.below(10)) {
        case 0:
        case 1: { /* open */
            r = h.open(cid);
            if (count[cid] >= limit) {
                LONGS_EQUAL(-EMFILE, r.r.status);
            } else {
                LONGS_EQUAL(0, r.r.status);
                LONGS_EQUAL(0, r.r.rc);
                CHECK(r.r.handle != 0);
                for (size_t k = 0; k < live.size(); k++) {
                    CHECK(live[k].handle != r.r.handle);
                }
                Live l = {cid, r.r.handle};
                live.push_back(l);
                count[cid]++;
            }
            break;
        }
        case 2: { /* close an own live handle */
            if (live.empty()) {
                break;
            }
            size_t k = rng.below((uint32_t)live.size());
            Live l = live[k];

            r = h.close(l.cid, l.handle);
            LONGS_EQUAL(0, r.r.status);
            live.erase(live.begin() + k);
            dead.push_back(l);
            count[l.cid]--;
            break;
        }
        case 3: { /* use a dead handle: always stale */
            if (dead.empty()) {
                break;
            }
            Live l = dead[rng.below((uint32_t)dead.size())];

            r = h.ioctl(l.cid, l.handle, IAV_IOC_GET_IAV_STATE, NULL, 0);
            LONGS_EQUAL(-ESTALE, r.r.status);
            LONGS_EQUAL(-ESTALE, h.close(l.cid, l.handle).r.status);
            break;
        }
        case 4: { /* use a live handle as another guest: always foreign */
            if (live.empty()) {
                break;
            }
            Live l = live[rng.below((uint32_t)live.size())];
            uint32_t other = cids[rng.below(4)];

            if (other == l.cid) {
                break;
            }
            r = h.ioctl(other, l.handle, IAV_IOC_GET_IAV_STATE, NULL, 0);
            LONGS_EQUAL(-EACCES, r.r.status);
            break;
        }
        case 5: { /* use an own live handle */
            if (live.empty()) {
                break;
            }
            Live l = live[rng.below((uint32_t)live.size())];

            r = h.ioctl(l.cid, l.handle, IAV_IOC_GET_IAV_STATE, NULL, 0);
            LONGS_EQUAL(0, r.r.status);
            LONGS_EQUAL(0, r.r.rc);
            LONGS_EQUAL(sizeof(u32), r.out.size());
            break;
        }
        case 6: { /* a made-up handle */
            uint64_t forged = rng.next();
            bool real = false;

            for (size_t k = 0; k < live.size(); k++) {
                real = real || live[k].handle == forged;
            }
            if (!real) {
                CHECK(h.ioctl(cid, forged, IAV_IOC_GET_IAV_STATE, NULL, 0).r.status != 0);
            }
            break;
        }
        case 7: { /* poll and map on a live handle */
            if (live.empty()) {
                break;
            }
            Live l = live[rng.below((uint32_t)live.size())];

            LONGS_EQUAL(0, h.poll(l.cid, l.handle, POLLIN, rng.below(300)).r.status);
            LONGS_EQUAL(0, h.map(l.cid, l.handle, MockIavDriver::DspBase, 4096, PROT_READ).r.status);
            break;
        }
        case 8: { /* the guest disappears */
            iav_broker_client_gone(&h.b, cid);
            for (size_t k = live.size(); k > 0; k--) {
                if (live[k - 1].cid == cid) {
                    dead.push_back(live[k - 1]);
                    live.erase(live.begin() + (k - 1));
                }
            }
            count[cid] = 0;
            break;
        }
        default:
            LONGS_EQUAL(0, h.info(cid).r.status);
            break;
        }
        /* The broker and the model agree after every step. */
        for (unsigned int c = 0; c < 4; c++) {
            LONGS_EQUAL(count[cids[c]], iav_broker_cid_sessions(&h.b, cids[c]));
        }
        LONGS_EQUAL((int)live.size(), h.drv.openFds());
        if (dead.size() > 256) {
            dead.erase(dead.begin(), dead.begin() + 128);
        }
    }
    for (unsigned int c = 0; c < 4; c++) {
        iav_broker_client_gone(&h.b, cids[c]);
    }
    LONGS_EQUAL(0, h.drv.openFds());
    CHECK(h.drv.violations().empty());
}

TEST(IavBrokerFuzz, FaultsFromTheDriverNeverLeakOrCrash)
{
    unsigned long iters = envNumber("IAV_FUZZ_ITERS", 20000) / 4;
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, 4);
    Rng rng(envNumber("IAV_FUZZ_SEED", 1) + 77);
    std::vector<uint64_t> handles;

    for (unsigned long i = 0; i < iters; i++) {
        uint32_t cid = Cids[rng.below(4)];
        IavBrokerHarness::Result r;
        int errs[] = {0, EIO, EINTR, EAGAIN, ENOMEM, EBUSY, EFAULT, ENODEV, EPERM};

        h.drv.failOpensWith(errs[rng.below(9)]);
        h.drv.failIoctl(Cmds[rng.below(15)], errs[rng.below(9)]);
        h.drv.setEagainCount((int)rng.below(8));
        h.drv.setPollReady(rng.below(2));
        r = h.send(cid, &baseRequest(rng, handles)[0], sizeof(struct amba_virt_iav_req));
        checkResponse(r);
        if (r.r.status == 0 && r.r.rc == 0 && r.r.handle != 0 && handles.size() < 32) {
            handles.push_back(r.r.handle);
        }
        h.drv.failOpensWith(0);
        r = h.open(cid);
        checkResponse(r);
        if (r.r.status == 0 && r.r.rc == 0 && handles.size() < 32) {
            handles.push_back(r.r.handle);
        }
    }
    h.drv.failIoctl(0, 0);
    finish(h);
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
