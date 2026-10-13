/*
 * drivers/amba_virt/tools/test/TestHostSliceLease.cxx
 *
 * Reservation lifecycle for one high-memory slice per domain UUID.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "CppUTest/TestHarness.h"

extern "C" {
#include "virt_slice_lease.h"
}

static const uint64_t kSliceBytes = 1ULL << 30;
static const char *kUuidA = "11111111-1111-4111-8111-111111111111";
static const char *kUuidB = "22222222-2222-4222-8222-222222222222";

static void exchange(const char *req, char *resp, size_t resp_len)
{
    memset(resp, 0, resp_len);
    LONGS_EQUAL(0, virt_slice_lease_admin(req, resp, resp_len));
}

static int starts_with(const char *resp, const char *prefix)
{
    return strncmp(resp, prefix, strlen(prefix)) == 0;
}

TEST_GROUP(SliceLease)
{
    char dir[64];
    char store[96];

    void setup() override
    {
        snprintf(dir, sizeof(dir), "/tmp/amba-slice-XXXXXX");
        CHECK(mkdtemp(dir) != nullptr);
        snprintf(store, sizeof(store), "%s/leases", dir);
        virt_slice_lease_reset();
        virt_slice_lease_set_store(store);
        virt_slice_lease_set_geometry(1, 8, kSliceBytes);
    }

    void teardown() override
    {
        unlink(store);
        rmdir(dir);
        virt_slice_lease_reset();
    }
};

TEST(SliceLease, rejects_until_geometry_matches)
{
    char resp[128];

    virt_slice_lease_set_geometry(0, 0, 0);
    char req[128];
    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", kUuidA,
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR GEOMETRY"));
}

TEST(SliceLease, rejects_zero_misaligned_and_wrong_size)
{
    char resp[128];
    char req[160];

    snprintf(req, sizeof(req), "RESERVE_SLICE %s 0", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR ZERO"));

    snprintf(req, sizeof(req), "RESERVE_SLICE %s 1000", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR ALIGN"));

    snprintf(req, sizeof(req), "RESERVE_SLICE %s 8192", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR SIZE"));

    snprintf(req, sizeof(req), "RESERVE_SLICE %s 18446744073709551616", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR INVAL"));
}

TEST(SliceLease, grants_eight_then_exhausts_and_is_idempotent)
{
    char resp[160];
    char req[160];
    char first[160];
    unsigned int i;

    for (i = 0; i < 8; i++) {
        snprintf(req, sizeof(req),
                 "RESERVE_SLICE 00000000-0000-4000-8000-00000000000%u %llu",
                 i + 1, (unsigned long long)kSliceBytes);
        exchange(req, resp, sizeof(resp));
        CHECK_TRUE(starts_with(resp, "OK index="));
        if (i == 0)
            snprintf(first, sizeof(first), "%s", resp);
    }
    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", kUuidB,
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR EXHAUSTED"));

    snprintf(req, sizeof(req),
             "RESERVE_SLICE 00000000-0000-4000-8000-000000000001 %llu",
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    STRCMP_EQUAL(first, resp);
}

TEST(SliceLease, stale_foreign_and_duplicate_release)
{
    char resp[160];
    char req[160];

    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", kUuidA,
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0"));

    snprintf(req, sizeof(req), "RELEASE_SLICE %s 99", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR STALE"));

    snprintf(req, sizeof(req), "QUERY_SLICE %s", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0"));

    snprintf(req, sizeof(req), "RELEASE_SLICE %s 1", kUuidB);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR NOT_FOUND"));

    snprintf(req, sizeof(req), "RELEASE_SLICE %s 1", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK"));
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR NOT_FOUND"));

    snprintf(req, sizeof(req), "QUERY_SLICE %s", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR NOT_FOUND"));
}

TEST(SliceLease, reload_after_restart_keeps_the_grant)
{
    char resp[160];
    char req[160];
    char saved[160];

    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", kUuidA,
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    snprintf(saved, sizeof(saved), "%s", resp);

    virt_slice_lease_reset();
    virt_slice_lease_set_store(store);
    virt_slice_lease_set_geometry(1, 8, kSliceBytes);
    virt_slice_lease_load();

    snprintf(req, sizeof(req), "QUERY_SLICE %s", kUuidA);
    exchange(req, resp, sizeof(resp));
    /* The reserve reply is the query reply plus the camera verdict. */
    CHECK_TRUE(strncmp(saved, resp, strlen(resp) - 1) == 0);
    CHECK_TRUE(strstr(saved, " camera=0\n") != nullptr);
}

TEST(SliceLease, geometry_loss_blocks_new_grants_and_keeps_old_ones)
{
    char resp[160];
    char req[160];

    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", kUuidA,
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    virt_slice_lease_set_geometry(0, 0, 0);

    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", kUuidB,
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR GEOMETRY"));

    snprintf(req, sizeof(req), "QUERY_SLICE %s", kUuidA);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0"));
}

struct lease_race {
    char uuid[64];
    char resp[160];
};

static void *reserve_one(void *arg)
{
    struct lease_race *race = (struct lease_race *)arg;
    char req[160];

    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", race->uuid,
             (unsigned long long)kSliceBytes);
    virt_slice_lease_admin(req, race->resp, sizeof(race->resp));
    return nullptr;
}

TEST(SliceLease, concurrent_reserves_get_distinct_slices)
{
    struct lease_race races[2];
    pthread_t threads[2];

    snprintf(races[0].uuid, sizeof(races[0].uuid), "%s", kUuidA);
    snprintf(races[1].uuid, sizeof(races[1].uuid), "%s", kUuidB);
    CHECK(pthread_create(&threads[0], nullptr, reserve_one, &races[0]) == 0);
    CHECK(pthread_create(&threads[1], nullptr, reserve_one, &races[1]) == 0);
    CHECK(pthread_join(threads[0], nullptr) == 0);
    CHECK(pthread_join(threads[1], nullptr) == 0);
    CHECK_TRUE(starts_with(races[0].resp, "OK index="));
    CHECK_TRUE(starts_with(races[1].resp, "OK index="));
    CHECK(strcmp(races[0].resp, races[1].resp) != 0);
}

TEST(SliceLease, primary_keeps_index_zero)
{
    char resp[160];
    char req[160];

    snprintf(req, sizeof(req), "RESERVE_SLICE_MIN %s %llu 1 amba_shm1 absent", kUuidB,
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=1"));

    snprintf(req, sizeof(req), "RESERVE_SLICE_AT %s %llu 0 amba_shm absent", kUuidA,
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0"));

    snprintf(req, sizeof(req), "RESERVE_SLICE_AT %s %llu 0 amba_shm absent",
             "33333333-3333-4333-8333-333333333333",
             (unsigned long long)kSliceBytes);
    exchange(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR EXHAUSTED"));
}
