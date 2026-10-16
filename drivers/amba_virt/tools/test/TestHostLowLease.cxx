/*
 * drivers/amba_virt/tools/test/TestHostLowLease.cxx
 *
 * Paired low-slice leases: geometry, cavalry pairing, persistence,
 * stale epochs, and eight concurrent reserves.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "CppUTest/TestHarness.h"

extern "C" {
#include "iav_slice_rpc.h"
#include "virt_low_lease.h"
#include "virt_slice_lease.h"
}

static const uint64_t kCavalryBytes = 1ULL << 30;
static const uint64_t kParent = 0x94000000ull;
static const char *kUuidA = "11111111-1111-4111-8111-111111111111";

static void exchange(const char *req, char *resp, size_t resp_len)
{
    memset(resp, 0, resp_len);
    LONGS_EQUAL(0, virt_low_lease_admin(req, resp, resp_len));
}

static int starts_with(const char *resp, const char *prefix)
{
    return strncmp(resp, prefix, strlen(prefix)) == 0;
}

static void hold_cavalry(const char *uuid)
{
    char req[160];
    char resp[160];

    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", uuid,
             (unsigned long long)kCavalryBytes);
    memset(resp, 0, sizeof(resp));
    LONGS_EQUAL(0, virt_slice_lease_admin(req, resp, sizeof(resp)));
    CHECK_TRUE(starts_with(resp, "OK "));
}

TEST_GROUP(LowLease)
{
    char dir[64];
    char low_store[96];
    char high_store[96];

    void setup() override
    {
        snprintf(dir, sizeof(dir), "/tmp/amba-low-XXXXXX");
        CHECK(mkdtemp(dir) != nullptr);
        snprintf(low_store, sizeof(low_store), "%s/low", dir);
        snprintf(high_store, sizeof(high_store), "%s/high", dir);
        virt_slice_lease_reset();
        virt_low_lease_reset();
        virt_slice_lease_set_store(high_store);
        virt_slice_lease_set_geometry(1, 8, kCavalryBytes);
        virt_low_lease_set_store(low_store);
        CHECK_EQUAL(0, virt_low_lease_set_parent(kParent));
    }

    void teardown() override
    {
        unlink(low_store);
        unlink(high_store);
        rmdir(dir);
        virt_low_lease_reset();
        virt_slice_lease_reset();
    }
};

TEST(LowLease, refuses_without_cavalry_lease)
{
    char resp[160];

    exchange("RESERVE_LOW 11111111-1111-4111-8111-111111111111 101",
             resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR NOT_PAIRED"));
}

TEST(LowLease, refuses_until_the_parent_is_known)
{
    char resp[160];

    virt_low_lease_reset();
    virt_low_lease_set_store(low_store);
    hold_cavalry(kUuidA);
    exchange("RESERVE_LOW 11111111-1111-4111-8111-111111111111 101",
             resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR GEOMETRY"));
}

TEST(LowLease, grants_binds_and_keeps_the_lease_on_guest_release)
{
    char resp[192];
    uint32_t window = 0;
    uint32_t usable = 0;

    hold_cavalry(kUuidA);
    exchange("RESERVE_LOW 11111111-1111-4111-8111-111111111111 101",
             resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0 path=/dev/amba_virt_iav_slice0 "));
    CHECK_EQUAL(0, iav_slice_rpc_held(101, &window, &usable));
    CHECK_EQUAL(0, window);
    CHECK_EQUAL(64u * 1024u * 1024u - 4096u, usable);

    exchange("QUERY_LOW 11111111-1111-4111-8111-111111111111", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0 "));
}

TEST(LowLease, reload_keeps_the_binding)
{
    char resp[192];
    uint32_t window = 99;

    hold_cavalry(kUuidA);
    exchange("RESERVE_LOW 11111111-1111-4111-8111-111111111111 101",
             resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK "));

    virt_low_lease_reset();
    virt_slice_lease_reset();
    virt_slice_lease_set_store(high_store);
    virt_slice_lease_set_geometry(1, 8, kCavalryBytes);
    virt_slice_lease_load();
    virt_low_lease_set_store(low_store);
    CHECK_EQUAL(0, virt_low_lease_set_parent(kParent));
    virt_low_lease_load();

    exchange("QUERY_LOW 11111111-1111-4111-8111-111111111111", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0 "));
    CHECK_EQUAL(0, iav_slice_rpc_held(101, &window, NULL));
    CHECK_EQUAL(0, window);
}

TEST(LowLease, stale_and_duplicate_release)
{
    char resp[192];
    char req[160];

    hold_cavalry(kUuidA);
    exchange("RESERVE_LOW 11111111-1111-4111-8111-111111111111 101",
             resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK "));

    snprintf(req, sizeof(req), "RELEASE_LOW %s 99", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR STALE"));

    exchange("QUERY_LOW 11111111-1111-4111-8111-111111111111", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK "));

    snprintf(req, sizeof(req), "RELEASE_LOW %s 1", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK"));

    snprintf(req, sizeof(req), "RELEASE_LOW %s 1", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR NOT_FOUND"));
    CHECK_EQUAL(-ENOENT, iav_slice_rpc_held(101, NULL, NULL));
}

TEST(LowLease, persist_failure_rolls_back)
{
    char blocked[80];
    char store[96];
    char resp[160];
    FILE *fp;

    hold_cavalry(kUuidA);
    snprintf(blocked, sizeof(blocked), "%s/blocked", dir);
    fp = fopen(blocked, "w");
    CHECK(fp != nullptr);
    fclose(fp);
    snprintf(store, sizeof(store), "%s/leases", blocked);
    virt_low_lease_set_store(store);
    exchange("RESERVE_LOW 11111111-1111-4111-8111-111111111111 101",
             resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR PERSIST"));
    virt_low_lease_set_store(low_store);
    exchange("QUERY_LOW 11111111-1111-4111-8111-111111111111", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR NOT_FOUND"));
    unlink(blocked);
}

struct race_arg {
    unsigned int n;
    int rc;
    char resp[192];
};

static void *race_reserve(void *arg)
{
    struct race_arg *a = (struct race_arg *)arg;
    char req[160];

    snprintf(req, sizeof(req),
             "RESERVE_LOW 00000000-0000-4000-8000-00000000000%u %u",
             a->n + 1, 200 + a->n);
    memset(a->resp, 0, sizeof(a->resp));
    a->rc = virt_low_lease_admin(req, a->resp, sizeof(a->resp));
    return NULL;
}

TEST(LowLease, eight_concurrent_reserves_then_exhaustion)
{
    pthread_t threads[8];
    struct race_arg args[8];
    char req[160];
    char resp[160];
    unsigned int i;
    unsigned int seen = 0;

    for (i = 0; i < 8; i++) {
        snprintf(req, sizeof(req),
                 "RESERVE_SLICE 00000000-0000-4000-8000-00000000000%u %llu",
                 i + 1, (unsigned long long)kCavalryBytes);
        memset(resp, 0, sizeof(resp));
        LONGS_EQUAL(0, virt_slice_lease_admin(req, resp, sizeof(resp)));
        CHECK_TRUE(starts_with(resp, "OK "));
        args[i].n = i;
        args[i].rc = -1;
        CHECK_EQUAL(0, pthread_create(&threads[i], NULL, race_reserve, &args[i]));
    }
    for (i = 0; i < 8; i++)
        CHECK_EQUAL(0, pthread_join(threads[i], NULL));
    for (i = 0; i < 8; i++) {
        unsigned int index = 8;

        CHECK_EQUAL(0, args[i].rc);
        CHECK_TRUE(starts_with(args[i].resp, "OK index="));
        CHECK_EQUAL(1, sscanf(args[i].resp, "OK index=%u", &index));
        CHECK_TRUE(index < 8);
        CHECK_EQUAL(0u, seen & (1u << index));
        seen |= 1u << index;
    }
    CHECK_EQUAL(0xffu, seen);
}

TEST(LowLease, full_table_returns_exhausted)
{
    char resp[160];
    FILE *fp;
    unsigned int i;

    hold_cavalry(kUuidA);
    fp = fopen(low_store, "w");
    CHECK(fp != nullptr);
    for (i = 0; i < 8; i++)
        fprintf(fp, "00000000-0000-4000-8000-00000000000%u %u %u %u\n",
                i + 1, i, i + 1, 300 + i);
    fclose(fp);
    virt_low_lease_load();
    exchange("RESERVE_LOW 11111111-1111-4111-8111-111111111111 101",
             resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR EXHAUSTED"));
}

/*
 * ---- the scrub of released slices (plan_iav_usb_yolo, Envelope E) ----
 *
 * The slices are host buffers here (allocated when first mapped) and the clock is a fake one, so the quarantine is
 * exact. A released slice is quarantined: it is not leased again and refuses new arena jobs until nothing is in flight
 * and the quarantine has passed since the release and the last job; then it is zeroed, read back and freed.
 */

extern "C" {
#include "amba_virt_iav_win.h"
#include "virt_slice_arena.h"
}

#include <stdlib.h>

namespace {

uint8_t *g_mem[8];
uint64_t g_clock = 100000;
int g_mapFails = 0;
unsigned int g_maps = 0;

void *fakeMap(void *ctx, uint32_t index, size_t bytes)
{
    (void)ctx;
    if (index >= 8 || bytes != VIRT_SLICE_ARENA_BYTES || g_mapFails) {
        errno = ENODEV;
        return NULL;
    }
    if (g_mem[index] == NULL) {
        g_mem[index] = (uint8_t *)malloc(bytes);
        if (g_mem[index] == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        memset(g_mem[index], 0xa5, bytes);
    }
    g_maps++;
    return g_mem[index];
}

void fakeUnmap(void *ctx, void *p, size_t bytes)
{
    (void)ctx;
    (void)p;
    (void)bytes;
}

uint64_t fakeNow(void *ctx)
{
    (void)ctx;
    return g_clock;
}

bool allZero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

void reserve(const char *uuid, unsigned int cid, unsigned int *index)
{
    char req[160], resp[192];

    snprintf(req, sizeof(req), "RESERVE_LOW %s %u", uuid, cid);
    memset(resp, 0, sizeof(resp));
    LONGS_EQUAL(0, virt_low_lease_admin(req, resp, sizeof(resp)));
    CHECK_TRUE(starts_with(resp, "OK index="));
    CHECK_EQUAL(1, sscanf(resp, "OK index=%u", index));
}

void release(const char *uuid, unsigned long long epoch, const char *want)
{
    char req[160], resp[192];

    snprintf(req, sizeof(req), "RELEASE_LOW %s %llu", uuid, epoch);
    memset(resp, 0, sizeof(resp));
    LONGS_EQUAL(0, virt_low_lease_admin(req, resp, sizeof(resp)));
    CHECK_TRUE(starts_with(resp, want));
}

const char *kUuidB = "22222222-2222-4222-8222-222222222222";
const char *kUuidC = "33333333-3333-4333-8333-333333333333";

} // namespace

TEST_GROUP(LowLeaseScrub)
{
    char dir[64];
    char low_store[96];
    char high_store[96];

    void setup() override
    {
        struct virt_slice_io io;

        snprintf(dir, sizeof(dir), "/tmp/amba-scrub-XXXXXX");
        CHECK(mkdtemp(dir) != nullptr);
        snprintf(low_store, sizeof(low_store), "%s/low", dir);
        snprintf(high_store, sizeof(high_store), "%s/high", dir);
        g_clock = 100000;
        g_mapFails = 0;
        g_maps = 0;
        memset(&io, 0, sizeof(io));
        io.map = fakeMap;
        io.unmap = fakeUnmap;
        io.now_ms = fakeNow;
        virt_slice_lease_reset();
        virt_low_lease_reset();
        virt_slice_arena_set_io(&io);
        virt_slice_arena_reset();
        virt_low_lease_set_reserve_wait_ms(0);
        virt_slice_lease_set_store(high_store);
        virt_slice_lease_set_geometry(1, 8, kCavalryBytes);
        virt_low_lease_set_store(low_store);
        CHECK_EQUAL(0, virt_low_lease_set_parent(kParent));
        hold_cavalry(kUuidA);
        hold_cavalry(kUuidB);
        hold_cavalry(kUuidC);
    }

    void teardown() override
    {
        unlink(low_store);
        unlink(high_store);
        rmdir(dir);
        virt_low_lease_reset();
        virt_slice_lease_reset();
        virt_slice_arena_set_io(NULL);
        virt_slice_arena_reset();
        for (unsigned int i = 0; i < 8; i++) {
            free(g_mem[i]);
            g_mem[i] = NULL;
        }
    }
};

TEST(LowLeaseScrub, the_lease_writes_the_slice_header_in_the_claim_page)
{
    unsigned int index = 9;
    struct amba_virt_iav_win_hdr hdr;

    reserve(kUuidA, 101, &index);
    CHECK_EQUAL(0u, index);
    CHECK(g_mem[0] != NULL);
    memcpy(&hdr, g_mem[0] + VIRT_SLICE_ARENA_BYTES - AMBA_VIRT_IAV_WIN_HDR_BYTES, sizeof(hdr));
    UNSIGNED_LONGS_EQUAL(AMBA_VIRT_IAV_WIN_HDR_MAGIC, hdr.magic);
    UNSIGNED_LONGS_EQUAL(AMBA_VIRT_IAV_WIN_HDR_VERSION, hdr.version);
    UNSIGNED_LONGS_EQUAL(AMBA_VIRT_IAV_WIN_ID_SLICE + 0, hdr.window_id);
    UNSIGNED_LONGS_EQUAL(VIRT_SLICE_ARENA_BYTES - 4096, hdr.part_len);
    UNSIGNED_LONGS_EQUAL(VIRT_SLICE_ARENA_BYTES, hdr.size);
    /* the arena itself is not touched by the header */
    UNSIGNED_LONGS_EQUAL(0xa5, g_mem[0][VIRT_SLICE_ARENA_BYTES - 4096 - 1]);
}

TEST(LowLeaseScrub, a_released_slice_is_quarantined_scrubbed_and_only_then_leased_again)
{
    unsigned int a = 9, b = 9, c = 9;

    reserve(kUuidA, 101, &a);
    CHECK_EQUAL(0u, a);
    memset(g_mem[0], 0x5a, VIRT_SLICE_ARENA_BYTES);
    release(kUuidA, 1, "OK");
    CHECK_EQUAL(1, virt_low_lease_scrub_pending(0));
    CHECK_EQUAL(1, virt_slice_arena_quarantined(0));
    LONGS_EQUAL(-EBUSY, virt_slice_arena_begin(0));
    /* the next holder does not get it while it is quarantined */
    reserve(kUuidB, 102, &b);
    CHECK_EQUAL(1u, b);
    /* the quarantine has not passed */
    g_clock += VIRT_SLICE_QUARANTINE_MS - 1;
    CHECK_EQUAL(0, virt_low_lease_scrub_pass());
    CHECK_FALSE(allZero(g_mem[0], VIRT_SLICE_ARENA_BYTES));
    g_clock += 1;
    CHECK_EQUAL(1, virt_low_lease_scrub_pass());
    CHECK_TRUE(allZero(g_mem[0], VIRT_SLICE_ARENA_BYTES));
    CHECK_EQUAL(0, virt_low_lease_scrub_pending(0));
    CHECK_EQUAL(0, virt_slice_arena_begin(0));
    virt_slice_arena_end(0);
    /* the next holder reads zero, apart from the header the lease writes into the claim page */
    reserve(kUuidC, 103, &c);
    CHECK_EQUAL(0u, c);
    CHECK_TRUE(allZero(g_mem[0], VIRT_SLICE_ARENA_BYTES - AMBA_VIRT_IAV_WIN_HDR_BYTES));
}

TEST(LowLeaseScrub, a_job_in_flight_and_the_last_job_extend_the_quarantine)
{
    unsigned int a = 9;

    reserve(kUuidA, 101, &a);
    memset(g_mem[0], 0x5a, VIRT_SLICE_ARENA_BYTES);
    CHECK_EQUAL(0, virt_slice_arena_begin(0));
    release(kUuidA, 1, "OK");
    /* a job still running: never scrubbed under it, however long it takes */
    g_clock += 60000;
    CHECK_EQUAL(0, virt_low_lease_scrub_pass());
    /* it returned now: the quarantine counts from here (a job it queued may still run) */
    virt_slice_arena_end(0);
    g_clock += VIRT_SLICE_QUARANTINE_MS - 1;
    CHECK_EQUAL(0, virt_low_lease_scrub_pass());
    g_clock += 1;
    CHECK_EQUAL(1, virt_low_lease_scrub_pass());
    CHECK_TRUE(allZero(g_mem[0], VIRT_SLICE_ARENA_BYTES));
}

TEST(LowLeaseScrub, a_failed_scrub_keeps_the_slice_quarantined)
{
    unsigned int a = 9;

    reserve(kUuidA, 101, &a);
    release(kUuidA, 1, "OK");
    g_clock += VIRT_SLICE_QUARANTINE_MS;
    g_mapFails = 1;
    CHECK_EQUAL(0, virt_low_lease_scrub_pass());
    CHECK_EQUAL(1, virt_low_lease_scrub_pending(0));
    g_mapFails = 0;
    CHECK_EQUAL(1, virt_low_lease_scrub_pass());
    CHECK_EQUAL(0, virt_low_lease_scrub_pending(0));
}

TEST(LowLeaseScrub, the_quarantine_survives_a_server_restart)
{
    unsigned int a = 9, b = 9;

    reserve(kUuidA, 101, &a);
    reserve(kUuidB, 102, &b);
    memset(g_mem[0], 0x5a, VIRT_SLICE_ARENA_BYTES);
    release(kUuidA, 1, "OK");
    /* the server restarts before the scrub */
    virt_low_lease_reset();
    virt_slice_arena_reset();
    virt_low_lease_set_reserve_wait_ms(0);
    virt_low_lease_set_store(low_store);
    CHECK_EQUAL(0, virt_low_lease_set_parent(kParent));
    virt_low_lease_load();
    CHECK_EQUAL(1, virt_low_lease_scrub_pending(0));
    /* quarantined again from the restart, and the unleased slices are scrubbed too; B keeps slice 1 */
    CHECK_EQUAL(0, virt_low_lease_scrub_pending(1));
    CHECK_EQUAL(1, virt_low_lease_scrub_pending(2));
    CHECK_EQUAL(6, virt_low_lease_scrub_pass());
    CHECK_EQUAL(1, virt_low_lease_scrub_pending(0));
    g_clock += VIRT_SLICE_QUARANTINE_MS;
    CHECK_EQUAL(1, virt_low_lease_scrub_pass());
    CHECK_TRUE(allZero(g_mem[0], VIRT_SLICE_ARENA_BYTES));
}

TEST(LowLeaseScrub, a_reserve_waits_for_the_scrub_when_every_free_slice_is_quarantined)
{
    char req[160];
    unsigned int index = 9, i;
    pthread_t t;
    struct race_arg arg;

    for (i = 0; i < 8; i++) {
        char uuid[64];

        snprintf(uuid, sizeof(uuid), "00000000-0000-4000-8000-00000000001%u", i);
        if (i >= 3) {
            snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", uuid, (unsigned long long)kCavalryBytes);
            char resp[160];

            memset(resp, 0, sizeof(resp));
            virt_slice_lease_admin(req, resp, sizeof(resp));
        }
    }
    reserve(kUuidA, 101, &index);
    reserve(kUuidB, 102, &index);
    reserve(kUuidC, 103, &index);
    for (i = 3; i < 8; i++) {
        char uuid[64];

        snprintf(uuid, sizeof(uuid), "00000000-0000-4000-8000-00000000001%u", i);
        reserve(uuid, 110 + i, &index);
    }
    release(kUuidA, 1, "OK");
    /* every slice is leased or quarantined: without a wait the reserve is refused */
    {
        char resp[192];

        exchange("RESERVE_LOW 11111111-1111-4111-8111-111111111111 120", resp, sizeof(resp));
        CHECK_TRUE(starts_with(resp, "ERR EXHAUSTED"));
    }
    /* with a wait it gets the slice once the scrub freed it */
    virt_low_lease_set_reserve_wait_ms(5000);
    arg.n = 0;
    arg.rc = -1;
    CHECK_EQUAL(0, pthread_create(&t, NULL, [](void *p) -> void * {
        struct race_arg *a = (struct race_arg *)p;

        memset(a->resp, 0, sizeof(a->resp));
        a->rc = virt_low_lease_admin("RESERVE_LOW 11111111-1111-4111-8111-111111111111 120", a->resp,
                                     sizeof(a->resp));
        return NULL;
    }, &arg));
    usleep(100000);
    g_clock += VIRT_SLICE_QUARANTINE_MS;
    CHECK_EQUAL(1, virt_low_lease_scrub_pass());
    CHECK_EQUAL(0, pthread_join(t, NULL));
    CHECK_EQUAL(0, arg.rc);
    CHECK_TRUE(starts_with(arg.resp, "OK index=0 "));
}
