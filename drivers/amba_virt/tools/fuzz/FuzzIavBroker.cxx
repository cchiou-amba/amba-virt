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

#include <errno.h>
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

/*
 * The arena (plan_iav_usb_yolo, Envelope E): guest 1 holds slice 4 of the pinned pool, no other guest holds a slice.
 * The mock driver records a violation for any pool buffer outside guest 1's arena, and the arena accounting must be
 * balanced at the end. An input whose first byte is 0xa5 starts with guest 1 negotiating the arena, so the fuzzer
 * reaches OP_ARENA without having to build a HELLO first.
 */
static int g_begins, g_ends;

static int fuzzSliceOf(void *ctx, uint32_t cid, uint32_t *window)
{
    (void)ctx;
    if (cid != 1 || window == NULL) {
        return -ENOENT;
    }
    *window = 4;
    return 0;
}

static int fuzzArenaBegin(void *ctx, uint32_t index)
{
    (void)ctx;
    (void)index;
    g_begins++;
    return 0;
}

static void fuzzArenaEnd(void *ctx, uint32_t index)
{
    (void)ctx;
    (void)index;
    g_ends++;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint32_t cids[] = {1, 2, 3, 0, 0xffffffffu};
    IavBrokerHarness h(IAV_PROFILE_QUALIFICATION, 6);
    std::vector<uint64_t> handles;
    size_t pos = 0;
    int messages = 0;
    struct virt_iav_guest_pool pool;

    h.addDecodeWindow();
    memset(&pool, 0, sizeof(pool));
    pool.partition_id = MockIavDriver::PoolPid;
    pool.parent_base = MockIavDriver::PoolBase;
    pool.parent_bytes = MockIavDriver::PoolLen;
    pool.slice_count = 8;
    pool.slice_bytes = MockIavDriver::SliceBytes;
    iav_broker_set_guest_pool(&h.b, &pool);
    h.b.ops.slice_of = fuzzSliceOf;
    h.b.ops.arena_begin = fuzzArenaBegin;
    h.b.ops.arena_end = fuzzArenaEnd;
    h.drv.allowArena(4 * MockIavDriver::SliceBytes, MockIavDriver::SliceBytes - 4096);
    /* the aux aperture (Envelope F): guest 1 has the standard one; the mock judges every table against it */
    h.drv.setStandardAperture(1);
    h.drv.setBlurPartitions(0x100000, 0);
    iav_broker_set_aux_sources(&h.b, MockIavDriver::StandardAux, MockIavDriver::AuxCount);
    g_begins = g_ends = 0;
    if (size > 0 && data[0] == 0xa5) {
        struct amba_virt_iav_hello hl;
        std::vector<uint8_t> v;

        memset(&hl, 0, sizeof(hl));
        hl.revision = AMBA_VIRT_IAV_REVISION;
        hl.page_size = 4096;
        hl.features = AMBA_VIRT_IAV_F_EXT_INFO | AMBA_VIRT_IAV_F_SIZE_OVERRIDE | AMBA_VIRT_IAV_F_ARENA |
                      AMBA_VIRT_IAV_F_APERTURE | AMBA_VIRT_IAV_F_INLINE;
        hl.max_window_id = 3;
        v = IavBrokerHarness::build(AMBA_VIRT_IAV_OP_HELLO, 0, 0, 0, 0, &hl, sizeof(hl));
        {
            uint64_t g = h.send(1, &v[0], v.size()).r.value;

            FUZZ_CHECK(g & AMBA_VIRT_IAV_F_ARENA);
            FUZZ_CHECK(g & AMBA_VIRT_IAV_F_APERTURE);
            FUZZ_CHECK(g & AMBA_VIRT_IAV_F_INLINE);
        }
        pos = 1;
    }

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
        /* an authorized map of an aperture entry stays inside guest 1's aperture, within twice its entry */
        if (req.size() >= sizeof(struct amba_virt_iav_req) && r.r.status == 0) {
            struct amba_virt_iav_req q;

            memcpy(&q, &req[0], sizeof(q));
            if (q.op == AMBA_VIRT_IAV_OP_MAP && r.out.size() == sizeof(struct amba_virt_iav_map)) {
                struct amba_virt_iav_map m;

                memcpy(&m, &r.out[0], sizeof(m));
                if (m.window >= AMBA_VIRT_IAV_ENT_ID_AUX) {
                    bool known = false;

                    FUZZ_CHECK(cid == 1);
                    for (unsigned int k = 0; k < MockIavDriver::AuxCount; k++) {
                        const struct iav_broker_aux_entry &e = MockIavDriver::StandardAux[k];

                        if (m.window == AMBA_VIRT_IAV_ENT_ID_AUX + e.pid) {
                            known = true;
                            FUZZ_CHECK(m.window_offset == e.offset && m.length <= 2 * e.len && q.arg_value == e.base);
                        }
                    }
                    FUZZ_CHECK(known);
                }
            }
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
    FUZZ_CHECK(g_begins == g_ends);
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
