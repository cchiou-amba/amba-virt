/*
 * drivers/amba_virt/tools/test/TestHostCameraPolicy.cxx
 *
 * The camera window policy: the verdict matrix, the reserve grammar that carries the ticket and the
 * fleet intent, the persisted per-ticket override, the admin commands, and concurrent use.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "CppUTest/TestHarness.h"

extern "C" {
#include "amba_virt_iav.h"
#include "virt_camera_policy.h"
#include "virt_slice_lease.h"
}

static const uint64_t kSliceBytes = 1ULL << 30;
static const uint64_t kDsp = 1ULL << 30;
static const uint64_t kBsb = 16ULL << 20;
static const uint64_t kDec = 8ULL << 20;
static const uint64_t kOvl = 8ULL << 20;
static const char *kUuidA = "11111111-1111-4111-8111-111111111111";

static void admin(const char *req, char *resp, size_t resp_len)
{
    memset(resp, 0, resp_len);
    LONGS_EQUAL(0, virt_camera_policy_admin(req, resp, resp_len));
}

static void lease(const char *req, char *resp, size_t resp_len)
{
    memset(resp, 0, resp_len);
    LONGS_EQUAL(0, virt_slice_lease_admin(req, resp, resp_len));
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool has(const char *s, const char *needle)
{
    return strstr(s, needle) != nullptr;
}

static void windows_all(uint64_t ready_ms)
{
    struct virt_camera_window w[4] = {
        { AMBA_VIRT_IAV_WIN_DSP, kDsp }, { AMBA_VIRT_IAV_WIN_BSB, kBsb },
        { AMBA_VIRT_IAV_WIN_DEC, kDec }, { AMBA_VIRT_IAV_WIN_OVL, kOvl },
    };

    virt_camera_policy_set_windows(w, 4, ready_ms);
}

static void write_file(const char *path, const void *data, size_t len)
{
    FILE *fp = fopen(path, "wb");

    CHECK(fp != nullptr);
    CHECK_EQUAL(len, fwrite(data, 1, len, fp));
    fclose(fp);
}

static void read_file(const char *path, char *buf, size_t buf_len, long *len)
{
    FILE *fp = fopen(path, "rb");
    size_t n;

    CHECK(fp != nullptr);
    n = fread(buf, 1, buf_len - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    *len = (long)n;
}

TEST_GROUP(CameraPolicy)
{
    char dir[64];
    char store[96];
    char tmp[112];
    char leases[96];

    void fresh()
    {
        virt_camera_policy_reset();
        virt_camera_policy_set_store(store);
        virt_slice_lease_reset();
        virt_slice_lease_set_store(leases);
        virt_slice_lease_set_geometry(1, 8, kSliceBytes);
    }

    void setup() override
    {
        snprintf(dir, sizeof(dir), "/tmp/amba-camera-XXXXXX");
        CHECK(mkdtemp(dir) != nullptr);
        snprintf(store, sizeof(store), "%s/camera-tickets", dir);
        snprintf(tmp, sizeof(tmp), "%s.tmp", store);
        snprintf(leases, sizeof(leases), "%s/leases", dir);
        fresh();
    }

    void teardown() override
    {
        rmdir(tmp);
        unlink(tmp);
        unlink(store);
        unlink(leases);
        rmdir(dir);
        virt_camera_policy_reset();
        virt_slice_lease_reset();
    }

    void reserve(const char *cmd, const char *uuid, unsigned int index, const char *ticket,
                 const char *intent, char *resp, size_t resp_len)
    {
        char req[256];

        snprintf(req, sizeof(req), "%s %s %llu %u %s %s", cmd, uuid, (unsigned long long)kSliceBytes,
                 index, ticket, intent);
        lease(req, resp, resp_len);
    }
};

TEST(CameraPolicy, ticket_and_intent_parsing)
{
    enum virt_camera_intent in = VIRT_CAMERA_INTENT_ON;
    char longest[VIRT_CAMERA_TICKET_MAX + 2];

    CHECK_TRUE(virt_camera_ticket_ok("amba_shm0"));
    CHECK_TRUE(virt_camera_ticket_ok("a"));
    CHECK_TRUE(virt_camera_ticket_ok("A-b.c_9"));
    CHECK_FALSE(virt_camera_ticket_ok(nullptr));
    CHECK_FALSE(virt_camera_ticket_ok(""));
    CHECK_FALSE(virt_camera_ticket_ok("amba shm"));
    CHECK_FALSE(virt_camera_ticket_ok("amba/shm"));
    CHECK_FALSE(virt_camera_ticket_ok("amba\nshm"));
    CHECK_FALSE(virt_camera_ticket_ok("amba_sh\xc3\xa9"));
    memset(longest, 'a', VIRT_CAMERA_TICKET_MAX);
    longest[VIRT_CAMERA_TICKET_MAX] = '\0';
    CHECK_TRUE(virt_camera_ticket_ok(longest));
    longest[VIRT_CAMERA_TICKET_MAX] = 'a';
    longest[VIRT_CAMERA_TICKET_MAX + 1] = '\0';
    CHECK_FALSE(virt_camera_ticket_ok(longest));

    LONGS_EQUAL(0, virt_camera_intent_parse("on", &in));
    LONGS_EQUAL(VIRT_CAMERA_INTENT_ON, in);
    LONGS_EQUAL(0, virt_camera_intent_parse("off", &in));
    LONGS_EQUAL(VIRT_CAMERA_INTENT_OFF, in);
    LONGS_EQUAL(0, virt_camera_intent_parse("absent", &in));
    LONGS_EQUAL(VIRT_CAMERA_INTENT_ABSENT, in);
    LONGS_EQUAL(-1, virt_camera_intent_parse("maybe", &in));
    LONGS_EQUAL(-1, virt_camera_intent_parse("", &in));
    LONGS_EQUAL(-1, virt_camera_intent_parse("ON", &in));
    LONGS_EQUAL(-1, virt_camera_intent_parse(nullptr, &in));
}

TEST(CameraPolicy, verdict_matrix_is_ready_and_override_or_intent)
{
    static const char *const kIntent[3] = { "absent", "off", "on" };
    static const char *const kOverride[3] = { "inherit", "on", "off" };
    int ready, ov, in, cases = 0;

    for (ready = 0; ready < 2; ready++) {
        for (ov = 0; ov < 3; ov++) {
            for (in = 0; in < 3; in++) {
                char resp[256], req[96];
                int on = (ov == 0) ? (in == 2) : (ov == 1);
                int expect = ready && on;

                fresh();
                if (ready)
                    windows_all(1000);
                if (ov != 0) {
                    snprintf(req, sizeof(req), "SET_CAMERA amba_shm3 %s", kOverride[ov]);
                    admin(req, resp, sizeof(resp));
                    STRCMP_EQUAL("OK\n", resp);
                }
                reserve("RESERVE_SLICE_MIN", kUuidA, 1, "amba_shm3", kIntent[in], resp, sizeof(resp));
                CHECK_TRUE(starts_with(resp, "OK index=1 "));
                if (expect) {
                    CHECK_TRUE(has(resp, " camera=1 dsp=1073741824 bsb=16777216\n"));
                } else {
                    CHECK_TRUE(has(resp, " camera=0\n"));
                }
                cases++;
            }
        }
    }
    LONGS_EQUAL(18, cases);
}

TEST(CameraPolicy, decode_and_overlay_only_for_index_zero)
{
    char resp[256];
    unsigned int i;
    char uuid[64];

    windows_all(1000);
    reserve("RESERVE_SLICE_AT", kUuidA, 0, "amba_shm0", "on", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0 "));
    CHECK_TRUE(has(resp, " camera=1 dsp=1073741824 bsb=16777216 dec=8388608 ovl=8388608\n"));

    for (i = 1; i <= 7; i++) {
        snprintf(uuid, sizeof(uuid), "00000000-0000-4000-8000-00000000000%u", i);
        reserve("RESERVE_SLICE_MIN", uuid, 1, "amba_shm1", "on", resp, sizeof(resp));
        CHECK_TRUE(starts_with(resp, "OK index="));
        CHECK_TRUE(has(resp, " camera=1 dsp=1073741824 bsb=16777216\n"));
        CHECK_FALSE(has(resp, " dec="));
        CHECK_FALSE(has(resp, " ovl="));
    }
}

TEST(CameraPolicy, index_zero_without_a_decode_window_gets_overlay_only)
{
    struct virt_camera_window w[3] = {
        { AMBA_VIRT_IAV_WIN_DSP, kDsp }, { AMBA_VIRT_IAV_WIN_BSB, kBsb }, { AMBA_VIRT_IAV_WIN_OVL, kOvl },
    };
    char resp[256];

    virt_camera_policy_set_windows(w, 3, 1000);
    reserve("RESERVE_SLICE_AT", kUuidA, 0, "amba_shm0", "on", resp, sizeof(resp));
    CHECK_TRUE(has(resp, " camera=1 dsp=1073741824 bsb=16777216 ovl=8388608\n"));
    CHECK_FALSE(has(resp, " dec="));
}

TEST(CameraPolicy, windows_are_ready_only_with_dsp_and_bsb)
{
    struct virt_camera_window dsp_only[1] = { { AMBA_VIRT_IAV_WIN_DSP, kDsp } };
    struct virt_camera_window bsb_only[1] = { { AMBA_VIRT_IAV_WIN_BSB, kBsb } };
    struct virt_camera_window bad[3] = {
        { 9, kDsp }, { AMBA_VIRT_IAV_WIN_DSP, 0 }, { AMBA_VIRT_IAV_WIN_BSB, kBsb },
    };
    char resp[256];

    virt_camera_policy_set_windows(dsp_only, 1, 777);
    admin("CAMERA_STATUS", resp, sizeof(resp));
    STRCMP_EQUAL("OK windows=none dsp=1073741824 bsb=0 dec=0 ovl=0 ready_ms=0\n", resp);
    reserve("RESERVE_SLICE_MIN", kUuidA, 1, "amba_shm1", "on", resp, sizeof(resp));
    CHECK_TRUE(has(resp, " camera=0\n"));

    virt_camera_policy_set_windows(bsb_only, 1, 777);
    admin("CAMERA_STATUS", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK windows=none "));

    virt_camera_policy_set_windows(bad, 3, 777);
    admin("CAMERA_STATUS", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK windows=none "));

    virt_camera_policy_set_windows(nullptr, 0, 777);
    admin("CAMERA_STATUS", resp, sizeof(resp));
    STRCMP_EQUAL("OK windows=none dsp=0 bsb=0 dec=0 ovl=0 ready_ms=0\n", resp);
}

TEST(CameraPolicy, status_reports_windows_and_the_ready_time)
{
    char resp[256];
    uint64_t a, b;

    windows_all(12345);
    admin("CAMERA_STATUS", resp, sizeof(resp));
    STRCMP_EQUAL("OK windows=ready dsp=1073741824 bsb=16777216 dec=8388608 ovl=8388608 ready_ms=12345\n",
                 resp);
    admin("CAMERA_STATUS extra", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);

    a = virt_camera_policy_now_ms();
    b = virt_camera_policy_now_ms();
    CHECK_TRUE(a > 0);
    CHECK_TRUE(b >= a);
}

TEST(CameraPolicy, reserve_grammar_is_strict_and_takes_no_lease_on_error)
{
    static const char *const kBad[] = {
        "RESERVE_SLICE_MIN %s 1073741824 1 amba_shm1",
        "RESERVE_SLICE_MIN %s 1073741824 1 amba_shm1 absent extra",
        "RESERVE_SLICE_MIN %s 1073741824 1 amba_shm1 maybe",
        "RESERVE_SLICE_MIN %s 1073741824 1 amba_shm1 ON",
        "RESERVE_SLICE_MIN %s 1073741824 1 amba/shm1 absent",
        "RESERVE_SLICE_MIN %s 1073741824 1 amba_sh\xc3\xa9 absent",
        "RESERVE_SLICE_MIN %s 1073741824 1 amba\nshm1 absent",
        "RESERVE_SLICE_MIN %s 1073741824 1 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa absent",
        "RESERVE_SLICE_MIN %s 1073741824 4294967296 amba_shm1 absent",
        "RESERVE_SLICE_AT %s 1073741824 -1 amba_shm1 absent",
        "RESERVE_SLICE_AT %s 1073741824 0",
        "RESERVE_SLICE_AT %s",
    };
    char resp[256], req[320];
    unsigned int i;

    for (i = 0; i < sizeof(kBad) / sizeof(kBad[0]); i++) {
        snprintf(req, sizeof(req), kBad[i], kUuidA);
        lease(req, resp, sizeof(resp));
        STRCMP_EQUAL("ERR INVAL\n", resp);
        snprintf(req, sizeof(req), "QUERY_SLICE %s", kUuidA);
        lease(req, resp, sizeof(resp));
        CHECK_TRUE(starts_with(resp, "ERR NOT_FOUND"));
    }
}

TEST(CameraPolicy, reserve_keeps_the_existing_size_and_index_errors)
{
    char resp[256], req[256];

    snprintf(req, sizeof(req), "RESERVE_SLICE_MIN %s 0 1 amba_shm1 absent", kUuidA);
    lease(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR ZERO"));
    snprintf(req, sizeof(req), "RESERVE_SLICE_MIN %s 1000 1 amba_shm1 absent", kUuidA);
    lease(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR ALIGN"));
    snprintf(req, sizeof(req), "RESERVE_SLICE_MIN %s 8192 1 amba_shm1 absent", kUuidA);
    lease(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR SIZE"));

    reserve("RESERVE_SLICE_AT", kUuidA, 8, "amba_shm0", "absent", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR EXHAUSTED"));
    reserve("RESERVE_SLICE_AT", kUuidA, 0, "amba_shm0", "absent", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0 "));
    reserve("RESERVE_SLICE_AT", "22222222-2222-4222-8222-222222222222", 0, "amba_shm0", "absent", resp,
            sizeof(resp));
    CHECK_TRUE(starts_with(resp, "ERR EXHAUSTED"));
}

TEST(CameraPolicy, legacy_reserve_never_attaches_the_camera)
{
    char resp[256], req[256];

    windows_all(1000);
    admin("SET_CAMERA amba_shm0 on", resp, sizeof(resp));
    snprintf(req, sizeof(req), "RESERVE_SLICE %s %llu", kUuidA, (unsigned long long)kSliceBytes);
    lease(req, resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK index=0 "));
    CHECK_TRUE(has(resp, " camera=0\n"));
}

TEST(CameraPolicy, a_repeat_reserve_recomputes_the_verdict_and_keeps_index_and_epoch)
{
    char first[256], second[256], third[256], req[96];
    const char *cut;

    windows_all(1000);
    reserve("RESERVE_SLICE_MIN", kUuidA, 1, "amba_shm1", "on", first, sizeof(first));
    CHECK_TRUE(has(first, " camera=1 dsp="));

    admin("SET_CAMERA amba_shm1 off", req, sizeof(req));
    reserve("RESERVE_SLICE_MIN", kUuidA, 1, "amba_shm1", "on", second, sizeof(second));
    CHECK_TRUE(has(second, " camera=0\n"));

    admin("SET_CAMERA amba_shm1 inherit", req, sizeof(req));
    reserve("RESERVE_SLICE_MIN", kUuidA, 1, "amba_shm1", "on", third, sizeof(third));
    CHECK_TRUE(has(third, " camera=1 dsp="));

    cut = strstr(first, " camera=");
    CHECK(cut != nullptr);
    LONGS_EQUAL(0, strncmp(first, second, (size_t)(cut - first)));
    LONGS_EQUAL(0, strncmp(first, third, (size_t)(cut - first)));
}

TEST(CameraPolicy, set_camera_arguments_are_validated)
{
    char resp[128];

    admin("SET_CAMERA", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);
    admin("SET_CAMERA amba_shm0", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);
    admin("SET_CAMERA amba_shm0 maybe", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);
    admin("SET_CAMERA amba_shm0 on extra", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);
    admin("SET_CAMERA amba/shm on", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);
    admin("SET_CAMERA aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa on", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);
    admin("NOPE", resp, sizeof(resp));
    STRCMP_EQUAL("ERR UNKNOWN_CMD\n", resp);

    admin("SET_CAMERA amba_shm9 inherit", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);
    admin("GET_CAMERA", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 0\n", resp);
    LONGS_EQUAL(-1, access(store, F_OK));
}

TEST(CameraPolicy, get_camera_lists_override_intent_and_effective)
{
    char resp[512];

    windows_all(1000);
    admin("SET_CAMERA amba_shm4 on", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);
    reserve("RESERVE_SLICE_MIN", kUuidA, 1, "amba_shm1", "on", resp, sizeof(resp));

    admin("GET_CAMERA amba_shm4", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 1\nTICKET amba_shm4 override=on intent=none effective=1\n", resp);
    admin("GET_CAMERA amba_shm1", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 1\nTICKET amba_shm1 override=inherit intent=on effective=1\n", resp);
    admin("GET_CAMERA", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 2\nTICKET amba_shm4 override=on intent=none effective=1\n"
                 "TICKET amba_shm1 override=inherit intent=on effective=1\n", resp);
    admin("GET_CAMERA unknown_ticket", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 0\n", resp);
    admin("GET_CAMERA bad/ticket", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);
    admin("GET_CAMERA a b", resp, sizeof(resp));
    STRCMP_EQUAL("ERR INVAL\n", resp);

    admin("SET_CAMERA amba_shm1 off", resp, sizeof(resp));
    admin("GET_CAMERA amba_shm1", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 1\nTICKET amba_shm1 override=off intent=on effective=0\n", resp);
}

TEST(CameraPolicy, get_camera_truncates_a_long_list)
{
    char resp[4096], req[96];
    unsigned int i;
    size_t n;

    for (i = 0; i < 40; i++) {
        snprintf(req, sizeof(req), "SET_CAMERA t%02u on", i);
        admin(req, resp, sizeof(resp));
        STRCMP_EQUAL("OK\n", resp);
    }
    admin("GET_CAMERA", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK COUNT 32\n"));
    n = strlen(resp);
    CHECK_TRUE(n > 10 && strcmp(resp + n - 10, "TRUNCATED\n") == 0);
}

TEST(CameraPolicy, the_override_table_is_bounded)
{
    char resp[128], req[96];
    unsigned int i;

    for (i = 0; i < VIRT_CAMERA_MAX_ENTRIES; i++) {
        snprintf(req, sizeof(req), "SET_CAMERA t%02u on", i);
        admin(req, resp, sizeof(resp));
        STRCMP_EQUAL("OK\n", resp);
    }
    admin("SET_CAMERA one_more on", resp, sizeof(resp));
    STRCMP_EQUAL("ERR PERSIST\n", resp);
    admin("SET_CAMERA one_more inherit", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);
    admin("SET_CAMERA t00 inherit", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);
    admin("SET_CAMERA one_more on", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);
}

TEST(CameraPolicy, the_store_is_written_atomically_with_mode_0600)
{
    char buf[256];
    long len = 0;
    struct stat st;
    char resp[64];

    admin("SET_CAMERA amba_shm1 on", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);
    admin("SET_CAMERA amba_shm5 off", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);

    read_file(store, buf, sizeof(buf), &len);
    STRCMP_EQUAL("amba_shm1 on\namba_shm5 off\n", buf);
    LONGS_EQUAL(0, stat(store, &st));
    LONGS_EQUAL(0600, st.st_mode & 0777);
    LONGS_EQUAL(-1, access(tmp, F_OK));
}

TEST(CameraPolicy, a_failed_write_changes_nothing_and_leaves_the_old_file)
{
    char buf[256], resp[64];
    long len = 0;

    admin("SET_CAMERA amba_shm0 on", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);

    /* A directory where the temporary file goes: the write cannot complete. */
    LONGS_EQUAL(0, mkdir(tmp, 0700));
    admin("SET_CAMERA amba_shm1 on", resp, sizeof(resp));
    STRCMP_EQUAL("ERR PERSIST\n", resp);
    read_file(store, buf, sizeof(buf), &len);
    STRCMP_EQUAL("amba_shm0 on\n", buf);
    admin("GET_CAMERA amba_shm1", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 0\n", resp);

    /* A change to an entry that exists is undone as well. */
    admin("SET_CAMERA amba_shm0 off", resp, sizeof(resp));
    STRCMP_EQUAL("ERR PERSIST\n", resp);
    admin("GET_CAMERA amba_shm0", resp, sizeof(resp));
    CHECK_TRUE(has(resp, "override=on"));
    read_file(store, buf, sizeof(buf), &len);
    STRCMP_EQUAL("amba_shm0 on\n", buf);
}

TEST(CameraPolicy, a_store_in_a_missing_directory_is_a_persist_error)
{
    char resp[64];

    virt_camera_policy_set_store("/nonexistent-camera-test/sub/camera-tickets");
    admin("SET_CAMERA amba_shm0 on", resp, sizeof(resp));
    STRCMP_EQUAL("ERR PERSIST\n", resp);
    admin("GET_CAMERA", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 0\n", resp);
}

TEST(CameraPolicy, a_leftover_temporary_file_is_ignored_and_replaced)
{
    char buf[256], resp[64];
    long len = 0;

    write_file(tmp, "junk that is not a store\n", 25);
    virt_camera_policy_load();
    admin("GET_CAMERA", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 0\n", resp);

    admin("SET_CAMERA amba_shm0 on", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);
    read_file(store, buf, sizeof(buf), &len);
    STRCMP_EQUAL("amba_shm0 on\n", buf);
    LONGS_EQUAL(-1, access(tmp, F_OK));
}

TEST(CameraPolicy, inherit_of_the_last_entry_leaves_an_empty_valid_file)
{
    struct stat st;
    char resp[64];

    admin("SET_CAMERA amba_shm0 on", resp, sizeof(resp));
    admin("SET_CAMERA amba_shm0 inherit", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);
    LONGS_EQUAL(0, stat(store, &st));
    LONGS_EQUAL(0, st.st_size);

    virt_camera_policy_reset();
    virt_camera_policy_set_store(store);
    virt_camera_policy_load();
    admin("GET_CAMERA", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 0\n", resp);
}

TEST(CameraPolicy, a_stored_override_survives_a_restart_and_is_applied)
{
    char resp[256];

    windows_all(1000);
    admin("SET_CAMERA amba_shm3 on", resp, sizeof(resp));
    STRCMP_EQUAL("OK\n", resp);

    virt_camera_policy_reset();
    virt_camera_policy_set_store(store);
    virt_camera_policy_load();
    windows_all(2000);
    reserve("RESERVE_SLICE_MIN", kUuidA, 1, "amba_shm3", "off", resp, sizeof(resp));
    CHECK_TRUE(has(resp, " camera=1 dsp="));
}

struct store_case {
    const char *name;
    const char *data;
    size_t len;
    int valid;
};

TEST(CameraPolicy, store_parsing_loads_all_of_a_valid_file_and_none_of_a_corrupt_one)
{
    static const char kValid[] = "amba_shm0 on\namba_shm1 off\n";
    static const char kNoNewline[] = "amba_shm0 on\namba_shm1 off";
    static const struct store_case kCases[] = {
        { "valid", kValid, sizeof(kValid) - 1, 1 },
        { "no trailing newline", kNoNewline, sizeof(kNoNewline) - 1, 1 },
        { "empty", "", 0, 1 },
        { "crlf", "amba_shm0 on\r\n", 14, 0 },
        { "duplicate", "amba_shm0 on\namba_shm0 off\n", 27, 0 },
        { "bad mode", "amba_shm0 maybe\n", 16, 0 },
        { "inherit is not stored", "amba_shm0 inherit\n", 18, 0 },
        { "bad charset", "amba/shm on\n", 12, 0 },
        { "two spaces", "amba_shm0  on\n", 14, 0 },
        { "three fields", "amba_shm0 on x\n", 15, 0 },
        { "no mode", "amba_shm0\n", 10, 0 },
        { "blank line", "amba_shm0 on\n\namba_shm1 off\n", 27, 0 },
        { "nul byte", "amba_shm0 on\n\0amba_shm1 off\n", 27, 0 },
    };
    unsigned int i;

    for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char resp[512];

        fresh();
        write_file(store, kCases[i].data, kCases[i].len);
        virt_camera_policy_load();
        admin("GET_CAMERA", resp, sizeof(resp));
        if (kCases[i].valid && kCases[i].len > 0) {
            if (!starts_with(resp, "OK COUNT 2\n"))
                FAIL(kCases[i].name);
            CHECK_TRUE(has(resp, "TICKET amba_shm0 override=on "));
        } else {
            if (strcmp(resp, "OK COUNT 0\n") != 0)
                FAIL(kCases[i].name);
        }
    }
}

TEST(CameraPolicy, store_limits_on_size_and_line_count)
{
    char resp[4096], big[VIRT_CAMERA_STORE_MAX_BYTES + 8], line[64];
    size_t n = 0;
    unsigned int i;

    /* 4097 bytes is over the size limit whatever they say. */
    memset(big, 'x', VIRT_CAMERA_STORE_MAX_BYTES + 1);
    write_file(store, big, VIRT_CAMERA_STORE_MAX_BYTES + 1);
    virt_camera_policy_load();
    admin("GET_CAMERA", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 0\n", resp);

    /* 64 valid lines load. */
    for (i = 0; i < VIRT_CAMERA_MAX_ENTRIES; i++) {
        int w = snprintf(line, sizeof(line), "t%02u on\n", i);

        memcpy(big + n, line, (size_t)w);
        n += (size_t)w;
    }
    fresh();
    write_file(store, big, n);
    virt_camera_policy_load();
    admin("GET_CAMERA t63", resp, sizeof(resp));
    CHECK_TRUE(starts_with(resp, "OK COUNT 1\n"));

    /* 65 do not. */
    memcpy(big + n, "t64 on\n", 7);
    fresh();
    write_file(store, big, n + 7);
    virt_camera_policy_load();
    admin("GET_CAMERA t00", resp, sizeof(resp));
    STRCMP_EQUAL("OK COUNT 0\n", resp);
}

struct race {
    int failures;
    int rounds;
};

static void *race_setter(void *arg)
{
    struct race *r = (struct race *)arg;
    char resp[128];
    int i;

    for (i = 0; i < r->rounds; i++) {
        virt_camera_policy_admin(i % 2 ? "SET_CAMERA amba_shm1 on" : "SET_CAMERA amba_shm1 off", resp,
                                 sizeof(resp));
        if (strcmp(resp, "OK\n") != 0)
            r->failures++;
    }
    return nullptr;
}

static void *race_reserver(void *arg)
{
    struct race *r = (struct race *)arg;
    char resp[256], req[256];
    int i;

    snprintf(req, sizeof(req), "RESERVE_SLICE_MIN %s %llu 1 amba_shm1 on", kUuidA,
             (unsigned long long)kSliceBytes);
    for (i = 0; i < r->rounds; i++) {
        virt_slice_lease_admin(req, resp, sizeof(resp));
        if (strncmp(resp, "OK index=1 ", 11) != 0 || !strstr(resp, " camera="))
            r->failures++;
    }
    return nullptr;
}

static void *race_reader(void *arg)
{
    struct race *r = (struct race *)arg;
    char resp[1024];
    int i;

    for (i = 0; i < r->rounds; i++) {
        virt_camera_policy_admin("GET_CAMERA", resp, sizeof(resp));
        if (strncmp(resp, "OK COUNT ", 9) != 0)
            r->failures++;
        virt_camera_policy_admin("CAMERA_STATUS", resp, sizeof(resp));
        if (strncmp(resp, "OK windows=", 11) != 0)
            r->failures++;
    }
    return nullptr;
}

TEST(CameraPolicy, set_camera_reserve_and_get_run_together)
{
    pthread_t t[3];
    struct race r[3] = { { 0, 200 }, { 0, 400 }, { 0, 200 } };
    unsigned int i;

    windows_all(1000);
    LONGS_EQUAL(0, pthread_create(&t[0], nullptr, race_setter, &r[0]));
    LONGS_EQUAL(0, pthread_create(&t[1], nullptr, race_reserver, &r[1]));
    LONGS_EQUAL(0, pthread_create(&t[2], nullptr, race_reader, &r[2]));
    for (i = 0; i < 3; i++)
        pthread_join(t[i], nullptr);
    for (i = 0; i < 3; i++)
        LONGS_EQUAL(0, r[i].failures);
}

TEST(CameraPolicy, the_verdict_text_has_its_exact_form_and_a_small_buffer_fails)
{
    struct virt_camera_verdict v;
    char buf[160], small[8];

    memset(&v, 0, sizeof(v));
    LONGS_EQUAL(9, virt_camera_verdict_format(&v, buf, sizeof(buf)));
    STRCMP_EQUAL(" camera=0", buf);

    v.camera = 1;
    v.dsp = kDsp;
    v.bsb = kBsb;
    virt_camera_verdict_format(&v, buf, sizeof(buf));
    STRCMP_EQUAL(" camera=1 dsp=1073741824 bsb=16777216", buf);
    v.dec = kDec;
    v.ovl = kOvl;
    virt_camera_verdict_format(&v, buf, sizeof(buf));
    STRCMP_EQUAL(" camera=1 dsp=1073741824 bsb=16777216 dec=8388608 ovl=8388608", buf);
    LONGS_EQUAL(-1, virt_camera_verdict_format(&v, small, sizeof(small)));
    LONGS_EQUAL(-1, virt_camera_verdict_format(nullptr, buf, sizeof(buf)));
}

TEST(CameraPolicy, decide_refuses_a_bad_ticket)
{
    struct virt_camera_verdict v;
    char conf_buf[512];
    long conf_len = 0;

    windows_all(1000);
    LONGS_EQUAL(-1, virt_camera_policy_decide("bad/ticket", VIRT_CAMERA_INTENT_ON, 1, &v));
    LONGS_EQUAL(0, v.camera);
    LONGS_EQUAL(-1, virt_camera_policy_decide(nullptr, VIRT_CAMERA_INTENT_ON, 1, &v));
    LONGS_EQUAL(-1, virt_camera_policy_decide("amba_shm0", VIRT_CAMERA_INTENT_ON, 1, nullptr));
    LONGS_EQUAL(0, virt_camera_policy_decide("amba_shm0", VIRT_CAMERA_INTENT_ON, 1, &v));
    LONGS_EQUAL(1, v.camera);

    read_file("iav.conf.example", conf_buf, sizeof(conf_buf), &conf_len);
    CHECK_TRUE(has(conf_buf, "amba_shm0 adapter"));
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
