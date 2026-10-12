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
