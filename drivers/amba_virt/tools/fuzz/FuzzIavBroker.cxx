/*
 * FuzzIavBroker.cxx
 *
 * Coverage-guided fuzz target for the IAV broker (libFuzzer). One input is a sequence of
 * messages from guests, each: one byte selecting the guest, a 16-bit length, then that many bytes
 * of request. A request whose handle field has 0xee in its top byte gets a real handle from an
 * earlier OPEN substituted (selected by the low byte), so the fuzzer can reach states that need a
 * valid handle without having to guess one. After every message the response is checked, the mock
 * driver is checked for anything it should not have been given, and the broker's limits are checked.
 * A failed check aborts, which libFuzzer reports as a crash.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdio.h>
#include <stdlib.h>

#include "IavBrokerHarness.hxx"

#define FUZZ_CHECK(cond)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "iav broker fuzz check failed: %s (line %d)\n", #cond,   \
                    __LINE__);                                                        \
            abort();                                                                  \
        }                                                                             \
    } while (0)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint32_t cids[] = {1, 2, 3, 0, 0xffffffffu};
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, 6);
    std::vector<uint64_t> handles;
    size_t pos = 0;
    int messages = 0;

    while (pos + 3 <= size && messages < 64) {
        uint32_t cid = cids[data[pos] % (sizeof(cids) / sizeof(cids[0]))];
        size_t len = (size_t)data[pos + 1] | ((size_t)data[pos + 2] << 8);
        std::vector<uint8_t> req;
        IavBrokerHarness::Result r;

        pos += 3;
        if (len > size - pos) {
            len = size - pos;
        }
        if (len > AMBA_VIRT_MAX_MSG) {
            len = AMBA_VIRT_MAX_MSG;
        }
        req.assign(data + pos, data + pos + len);
        pos += len;
        messages++;

        if (req.size() >= sizeof(struct amba_virt_iav_req) && !handles.empty()) {
            struct amba_virt_iav_req q;

            memcpy(&q, &req[0], sizeof(q));
            if ((q.handle >> 56) == 0xee) {
                q.handle = handles[(size_t)(q.handle & 0xff) % handles.size()];
                memcpy(&req[0], &q, sizeof(q));
            }
        }
        r = h.send(cid, req.empty() ? NULL : &req[0], req.size());

        FUZZ_CHECK(r.ret == 0);
        FUZZ_CHECK(r.r.proto == AMBA_VIRT_IAV_PROTO);
        FUZZ_CHECK(r.r.reserved == 0);
        FUZZ_CHECK(r.r.out_len == r.out.size());
        FUZZ_CHECK(r.r.out_len <= AMBA_VIRT_IAV_MAX_OUT);
        FUZZ_CHECK(r.r.status <= 0 && r.r.status >= -4095);
        if (r.r.status != 0) {
            FUZZ_CHECK(r.r.out_len == 0);
        }
        if (r.r.status == 0 && r.r.rc < 0) {
            FUZZ_CHECK(r.r.err > 0 && r.r.err < 4096);
        }
        if (r.r.status == 0 && r.r.rc == 0 && r.r.handle != 0 && handles.size() < 32) {
            handles.push_back(r.r.handle);
        }
        FUZZ_CHECK(h.drv.violations().empty());
        for (size_t c = 0; c < sizeof(cids) / sizeof(cids[0]); c++) {
            FUZZ_CHECK(iav_broker_cid_sessions(&h.b, cids[c]) <= 6);
        }
        if (data[pos > 0 ? pos - 1 : 0] % 31 == 0) {
            iav_broker_client_gone(&h.b, cid);
        }
    }
    for (size_t c = 0; c < sizeof(cids) / sizeof(cids[0]); c++) {
        iav_broker_client_gone(&h.b, cids[c]);
    }
    FUZZ_CHECK(iav_broker_session_count(&h.b) == 0);
    FUZZ_CHECK(h.drv.openFds() == 0);
    return 0;
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
