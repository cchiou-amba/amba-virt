/*
 * drivers/amba_virt/tools/test/TestHostIavMultiTenantBroadcast.cxx
 *
 * CppUTest test suite for Dom0 1-to-N Multi-Tenant Decoupled Frame Broadcaster.
 * Verifies non-destructive reads, concurrent tenant isolation, independent pacing,
 * and high-concurrency multi-threaded streaming under ASan/UBSan.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include "CppUTest/TestHarness.h"
#include "CppUTest/CommandLineTestRunner.h"

#include <pb_encode.h>
#include <pb_decode.h>
#include "amba_virt.pb.h"
#include "iav_proxy.h"
#include "cavalry_proxy.h"

/*
 * C-linkage mock buffers for multi-tenant cavalry handles
 */
static uint8_t g_tenant0_in[1228800];
static uint8_t g_tenant0_out[8400 * 85 * sizeof(float)];
static uint8_t g_tenant0_jpeg[512 * 1024];

static uint8_t g_tenant1_in[1228800];
static uint8_t g_tenant1_out[8400 * 85 * sizeof(float)];
static uint8_t g_tenant1_jpeg[512 * 1024];

extern "C" {

int cavalry_proxy_get_handle_buffer(uint32_t handle_id,
                                    uint32_t client_cid,
                                    uint32_t session_id,
                                    void **out_virt,
                                    size_t *out_size)
{
    (void)session_id;

    if (client_cid == 3266) { // Tenant 0 (Ubuntu)
        if (handle_id == 1) {
            *out_virt = g_tenant0_in;
            *out_size = sizeof(g_tenant0_in);
            return 0;
        } else if (handle_id == 2) {
            *out_virt = g_tenant0_out;
            *out_size = sizeof(g_tenant0_out);
            return 0;
        } else if (handle_id == 3) {
            *out_virt = g_tenant0_jpeg;
            *out_size = sizeof(g_tenant0_jpeg);
            return 0;
        }
    } else if (client_cid == 3265) { // Tenant 1 (Alpine)
        if (handle_id == 1) {
            *out_virt = g_tenant1_in;
            *out_size = sizeof(g_tenant1_in);
            return 0;
        } else if (handle_id == 2) {
            *out_virt = g_tenant1_out;
            *out_size = sizeof(g_tenant1_out);
            return 0;
        } else if (handle_id == 3) {
            *out_virt = g_tenant1_jpeg;
            *out_size = sizeof(g_tenant1_jpeg);
            return 0;
        }
    }

    return -EACCES;
}

int cavalry_proxy_run_dag_with_handles(uint32_t dag_id,
                                       uint32_t in_handle_id,
                                       uint32_t out_handle_id,
                                       uint32_t client_cid,
                                       uint32_t session_id,
                                       uint32_t *ticks,
                                       uint32_t *rval,
                                       uint64_t *sub_ns,
                                       uint64_t *start_ns,
                                       uint64_t *fin_ns)
{
    (void)dag_id;
    (void)in_handle_id;
    (void)out_handle_id;
    (void)client_cid;
    (void)session_id;

    if (ticks) *ticks = 1000;
    if (rval) *rval = 0;
    if (sub_ns) *sub_ns = 1000000;
    if (start_ns) *start_ns = 1000500;
    if (fin_ns) *fin_ns = 1001500;

    return 0;
}

} // extern "C"

TEST_GROUP(IavMultiTenantBroadcast)
{
    struct iav_tap_ring *ring = nullptr;

    void setup() override
    {
        ring = (struct iav_tap_ring *)aligned_alloc(4096, sizeof(struct iav_tap_ring));
        memset(ring, 0, sizeof(struct iav_tap_ring));
        memset(g_tenant0_in, 0, sizeof(g_tenant0_in));
        memset(g_tenant0_out, 0, sizeof(g_tenant0_out));
        memset(g_tenant0_jpeg, 0, sizeof(g_tenant0_jpeg));
        memset(g_tenant1_in, 0, sizeof(g_tenant1_in));
        memset(g_tenant1_out, 0, sizeof(g_tenant1_out));
        memset(g_tenant1_jpeg, 0, sizeof(g_tenant1_jpeg));

        iav_proxy_init(ring);
        iav_proxy_attach(3266, 1);
        iav_proxy_attach(3265, 1);
        iav_proxy_set_cohort(1);
    }

    void teardown() override
    {
        iav_proxy_cleanup();
        if (ring) {
            free(ring);
            ring = nullptr;
        }
    }

    void publish_frame(uint64_t seq, uint8_t byte_val, const uint8_t *jpeg_data, uint32_t jpeg_len)
    {
        iav_proxy_ring_lock();
        int slot_idx = (int)(seq % IAV_TAP_RING_SLOTS);
        struct iav_tap_slot *slot = &ring->slots[slot_idx];

        slot->state = IAV_TAP_SLOT_WRITING;
        iav_proxy_ring_unlock();

        memset(slot->payload, byte_val, 1920 * 1080);
        memset(slot->payload + 1920 * 1080, 128, 1920 * 1080 / 2);

        if (jpeg_data && jpeg_len > 0) {
            memcpy(slot->jpeg_payload, jpeg_data, jpeg_len);
            slot->jpeg_size = jpeg_len;
        } else {
            slot->jpeg_size = 0;
        }

        iav_proxy_ring_lock();
        slot->seq = seq;
        slot->width = 1920;
        slot->height = 1080;
        slot->pitch = 1920;
        slot->fourcc = 0x3231564E; // NV12
        slot->nbytes = 1920 * 1080 * 3 / 2;
        slot->refcount = 0;
        slot->state = IAV_TAP_SLOT_PUBLISHED;
        ring->published_count++;
        iav_proxy_note_published_locked(seq);
        iav_proxy_ring_unlock();
    }

    int send_tap_request(uint32_t client_cid, uint64_t expected_seq, ambarella_virt_v1_IavTapRunResponse *out_resp)
    {
        (void)expected_seq;
        ambarella_virt_v1_RpcEnvelope req_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        req_env.api_major = 1;
        req_env.api_minor = 0;
        req_env.request_id = (uint32_t)expected_seq;
        req_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag;

        ambarella_virt_v1_IavTapRunRequest *req = &req_env.body.iav_tap_run_request;
        req->session_id = 1;
        req->dag_id = 10;
        req->in_handle_id = 1;
        req->out_handle_id = 2;
        req->jpeg_handle_id = 3;
        req->jpeg_capacity = sizeof(g_tenant0_jpeg);
        req->want_jpeg = true;
        req->take_latest = true;

        uint8_t req_buf[512];
        pb_ostream_t ostream = pb_ostream_from_buffer(req_buf, sizeof(req_buf));
        bool enc_ok = pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &req_env);
        CHECK_TRUE(enc_ok);

        uint8_t resp_buf[512];
        size_t resp_len = 0;
        int ret = iav_proxy_handle_nanopb_rpc(req_buf, ostream.bytes_written,
                                             resp_buf, sizeof(resp_buf), &resp_len, client_cid);
        if (ret != 0)
            return ret;

        ambarella_virt_v1_RpcEnvelope resp_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(resp_buf, resp_len);
        bool dec_ok = pb_decode(&istream, ambarella_virt_v1_RpcEnvelope_fields, &resp_env);
        CHECK_TRUE(dec_ok);
        CHECK_EQUAL(ambarella_virt_v1_RpcEnvelope_iav_tap_run_response_tag, resp_env.which_body);

        *out_resp = resp_env.body.iav_tap_run_response;
        return 0;
    }
};

/*
 * Test 1: 1-to-N Broadcast Invariant
 * A single published frame N is consumed by both Tenant 0 (CID 3266) and Tenant 1 (CID 3265).
 * Both tenants receive identical sequence numbers, identical JPEG payloads, and bit-exact data.
 */
TEST(IavMultiTenantBroadcast, test_1_to_n_broadcast_identical_delivery)
{
    static const uint8_t test_jpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'T', 'E', 'S', 'T', 'J', 'P', 'E', 'G', 0x00, 0x01,
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
        0xFF, 0xD9
    };

    publish_frame(101, 0x5A, test_jpeg, sizeof(test_jpeg));

    ambarella_virt_v1_IavTapRunResponse resp0 = ambarella_virt_v1_IavTapRunResponse_init_zero;
    ambarella_virt_v1_IavTapRunResponse resp1 = ambarella_virt_v1_IavTapRunResponse_init_zero;

    int ret0 = send_tap_request(3266, 101, &resp0);
    int ret1 = send_tap_request(3265, 101, &resp1);

    LONGS_EQUAL(0, ret0);
    LONGS_EQUAL(0, ret1);

    LONGS_EQUAL(101, resp0.active_seq);
    LONGS_EQUAL(101, resp1.active_seq);

    LONGS_EQUAL(sizeof(test_jpeg), resp0.jpeg_len);
    LONGS_EQUAL(sizeof(test_jpeg), resp1.jpeg_len);

    MEMCMP_EQUAL(test_jpeg, g_tenant0_jpeg, sizeof(test_jpeg));
    MEMCMP_EQUAL(test_jpeg, g_tenant1_jpeg, sizeof(test_jpeg));
    MEMCMP_EQUAL(g_tenant0_in, g_tenant1_in, sizeof(g_tenant0_in));
}

/*
 * Test 2: Non-Destructive Concurrent Read
 * Reading a frame by Tenant 0 leaves the slot published and uncorrupted for Tenant 1.
 */
TEST(IavMultiTenantBroadcast, test_non_destructive_read)
{
    static const uint8_t test_jpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'N', 'O', 'N', 'D', 'E', 'S', 'T', 'R', 0x00, 0x01,
        0xFF, 0xD9
    };

    publish_frame(201, 0x7E, test_jpeg, sizeof(test_jpeg));

    int slot_idx = (int)(201 % IAV_TAP_RING_SLOTS);
    LONGS_EQUAL(IAV_TAP_SLOT_PUBLISHED, ring->slots[slot_idx].state);
    LONGS_EQUAL(0, ring->slots[slot_idx].refcount);

    ambarella_virt_v1_IavTapRunResponse resp0 = ambarella_virt_v1_IavTapRunResponse_init_zero;
    int ret0 = send_tap_request(3266, 201, &resp0);
    LONGS_EQUAL(0, ret0);
    LONGS_EQUAL(201, resp0.active_seq);

    // Slot must still be published with 0 refcount after Tenant 0 finishes reading
    LONGS_EQUAL(IAV_TAP_SLOT_PUBLISHED, ring->slots[slot_idx].state);
    LONGS_EQUAL(0, ring->slots[slot_idx].refcount);

    ambarella_virt_v1_IavTapRunResponse resp1 = ambarella_virt_v1_IavTapRunResponse_init_zero;
    int ret1 = send_tap_request(3265, 201, &resp1);
    LONGS_EQUAL(0, ret1);
    LONGS_EQUAL(201, resp1.active_seq);

    MEMCMP_EQUAL(g_tenant0_jpeg, g_tenant1_jpeg, sizeof(test_jpeg));
}

/*
 * Test 3: Independent Pacing (Slow/Fast Consumer Isolation)
 * Fast tenant queries every frame (30 Hz). Slow tenant queries every 3rd frame (10 Hz).
 * Fast tenant receives all frames without delays or drops.
 * Slow tenant drops intermediate frames without stalling the producer or fast tenant.
 */
TEST(IavMultiTenantBroadcast, test_independent_pacing_slow_fast)
{
    static const uint8_t sample_jpeg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0xAA, 0xBB, 0xFF, 0xD9 };

    // Publish frame 1
    publish_frame(1, 0x11, sample_jpeg, sizeof(sample_jpeg));

    ambarella_virt_v1_IavTapRunResponse resp_fast, resp_slow;
    LONGS_EQUAL(0, send_tap_request(3266, 1, &resp_fast));
    LONGS_EQUAL(1, resp_fast.active_seq);
    LONGS_EQUAL(0, resp_fast.drop_count);

    LONGS_EQUAL(0, send_tap_request(3265, 1, &resp_slow));
    LONGS_EQUAL(1, resp_slow.active_seq);
    LONGS_EQUAL(0, resp_slow.drop_count);

    // Publish frame 2 & 3 (slow tenant is sleeping)
    publish_frame(2, 0x22, sample_jpeg, sizeof(sample_jpeg));
    LONGS_EQUAL(0, send_tap_request(3266, 2, &resp_fast));
    LONGS_EQUAL(2, resp_fast.active_seq);
    LONGS_EQUAL(0, resp_fast.drop_count);

    publish_frame(3, 0x33, sample_jpeg, sizeof(sample_jpeg));
    LONGS_EQUAL(0, send_tap_request(3266, 3, &resp_fast));
    LONGS_EQUAL(3, resp_fast.active_seq);
    LONGS_EQUAL(0, resp_fast.drop_count);

    // Publish frame 4; slow tenant now wakes up and requests latest frame
    publish_frame(4, 0x44, sample_jpeg, sizeof(sample_jpeg));
    LONGS_EQUAL(0, send_tap_request(3266, 4, &resp_fast));
    LONGS_EQUAL(4, resp_fast.active_seq);
    LONGS_EQUAL(0, resp_fast.drop_count);

    LONGS_EQUAL(0, send_tap_request(3265, 4, &resp_slow));
    LONGS_EQUAL(4, resp_slow.active_seq);
    // Slow tenant skipped frames 2 and 3 -> drop_count should be exactly 2
    LONGS_EQUAL(2, resp_slow.drop_count);

    // Producer ring must not have dropped any frames due to slow tenant
    LONGS_EQUAL(0, ring->drop_count);
}

/*
 * Test 4: Subscriber Disconnect and Reconnect
 * Disconnecting Tenant 1 causes zero disruption for Tenant 0.
 */
TEST(IavMultiTenantBroadcast, test_subscriber_disconnect_reconnect)
{
    static const uint8_t sample_jpeg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x55, 0x66, 0xFF, 0xD9 };

    publish_frame(10, 0x10, sample_jpeg, sizeof(sample_jpeg));
    ambarella_virt_v1_IavTapRunResponse resp;
    LONGS_EQUAL(0, send_tap_request(3266, 10, &resp));
    LONGS_EQUAL(0, send_tap_request(3265, 10, &resp));

    // Disconnect Tenant 1
    iav_proxy_client_disconnect(3265);

    // Continue publishing frames 11..15
    for (uint64_t f = 11; f <= 15; f++) {
        publish_frame(f, (uint8_t)f, sample_jpeg, sizeof(sample_jpeg));
        LONGS_EQUAL(0, send_tap_request(3266, f, &resp));
        LONGS_EQUAL(f, resp.active_seq);
        LONGS_EQUAL(0, resp.drop_count);
    }

    // Reconnect Tenant 1 on frame 16
    publish_frame(16, 0x16, sample_jpeg, sizeof(sample_jpeg));
    LONGS_EQUAL(0, send_tap_request(3265, 16, &resp));
    LONGS_EQUAL(16, resp.active_seq);
}

/*
 * Test 5: Concurrent Multi-Threaded Stress Test
 * 2 simulated tenant worker threads hammering iav_proxy_handle_nanopb_rpc
 * concurrently for 100 iterations while frames are continuously published.
 */
struct ThreadArgs {
    uint32_t cid;
    int iterations;
    int success_count;
    struct iav_tap_ring *ring;
};

static void *tenant_worker_thread(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    for (int i = 0; i < args->iterations; i++) {
        ambarella_virt_v1_RpcEnvelope req_env = ambarella_virt_v1_RpcEnvelope_init_zero;
        req_env.api_major = 1;
        req_env.api_minor = 0;
        req_env.request_id = (uint32_t)i;
        req_env.which_body = ambarella_virt_v1_RpcEnvelope_iav_tap_run_request_tag;

        ambarella_virt_v1_IavTapRunRequest *req = &req_env.body.iav_tap_run_request;
        req->session_id = 1;
        req->dag_id = 10;
        req->in_handle_id = 1;
        req->out_handle_id = 2;
        req->jpeg_handle_id = 3;
        req->jpeg_capacity = 512 * 1024;
        req->want_jpeg = true;
        req->take_latest = true;

        uint8_t req_buf[512];
        pb_ostream_t ostream = pb_ostream_from_buffer(req_buf, sizeof(req_buf));
        if (!pb_encode(&ostream, ambarella_virt_v1_RpcEnvelope_fields, &req_env))
            continue;

        uint8_t resp_buf[512];
        size_t resp_len = 0;
        int ret = iav_proxy_handle_nanopb_rpc(req_buf, ostream.bytes_written,
                                             resp_buf, sizeof(resp_buf), &resp_len, args->cid);
        if (ret == 0)
            args->success_count++;

        usleep(500); // 0.5 ms
    }
    return nullptr;
}

TEST(IavMultiTenantBroadcast, test_concurrent_multithreaded_stress)
{
    static const uint8_t sample_jpeg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x12, 0x34, 0xFF, 0xD9 };

    publish_frame(1, 0x01, sample_jpeg, sizeof(sample_jpeg));

    ThreadArgs args0 = { 3266, 100, 0, ring };
    ThreadArgs args1 = { 3265, 100, 0, ring };

    pthread_t th0, th1;
    pthread_create(&th0, nullptr, tenant_worker_thread, &args0);
    pthread_create(&th1, nullptr, tenant_worker_thread, &args1);

    for (uint64_t seq = 2; seq <= 50; seq++) {
        usleep(1500); // simulate 30 Hz host frame ticks
        publish_frame(seq, (uint8_t)seq, sample_jpeg, sizeof(sample_jpeg));
    }

    pthread_join(th0, nullptr);
    pthread_join(th1, nullptr);

    CHECK_TRUE(args0.success_count > 0);
    CHECK_TRUE(args1.success_count > 0);
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
