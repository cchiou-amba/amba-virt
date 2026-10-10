/*
 * test_virt_host_ops.c
 *
 * Unit tests for the direct Dom0 host operations (virt_host_ops.c) and for the
 * module loader that sits on them. The kernel's answers are injected through
 * virt_host_set_sys(), so every refusal (EEXIST, ENOEXEC, EKEYREJECTED, EBUSY)
 * is exercised without a kernel. The file locations are temporary directories.
 *
 * Each case prints "[PASS] <name>" or "[FAIL] <name>". The exit status is 1
 * when any case fails.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "virt_driver_matrix.h"
#include "virt_host_ops.h"
#include "virt_module_loader.h"

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("  check failed: %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            return false;                                                         \
        }                                                                         \
    } while (0)

static char g_root[128];
static char g_state[160];
static char g_sys[160];
static char g_dir1[160];
static char g_dir2[160];

/* ---- the fake kernel --------------------------------------------------- */

#define MAX_LOADS 8

static int g_finit_calls;
static int g_finit_errno;        /* applied to every call when g_finit_fail_at == 0 */
static int g_finit_fail_at;      /* 1-based call that fails with g_finit_errno */
static char g_finit_params[300];
static char g_finit_content[64];
static char g_finit_names[MAX_LOADS][64];
static int g_delete_calls;
static int g_delete_errno;
static char g_deleted[64];

static int fake_finit(int fd, const char *params)
{
    char link[64], path[PATH_MAX];
    ssize_t n;
    int idx = g_finit_calls;

    g_finit_calls++;
    snprintf(g_finit_params, sizeof(g_finit_params), "%s", params);
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    n = readlink(link, path, sizeof(path) - 1);
    if (n > 0) {
        const char *base;

        path[n] = '\0';
        base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (idx < MAX_LOADS)
            snprintf(g_finit_names[idx], sizeof(g_finit_names[idx]), "%.63s", base);
    }
    g_finit_content[0] = '\0';
    n = pread(fd, g_finit_content, sizeof(g_finit_content) - 1, 0);
    if (n >= 0)
        g_finit_content[n] = '\0';
    if (g_finit_errno && (g_finit_fail_at == 0 || g_finit_fail_at == g_finit_calls)) {
        errno = g_finit_errno;
        return -1;
    }
    return 0;
}

static int fake_delete(const char *name)
{
    g_delete_calls++;
    snprintf(g_deleted, sizeof(g_deleted), "%s", name);
    if (g_delete_errno) {
        errno = g_delete_errno;
        return -1;
    }
    return 0;
}

static const struct virt_host_sys g_fake = { .finit_module = fake_finit, .delete_module = fake_delete };

static void reset_fake(void)
{
    g_finit_calls = 0;
    g_finit_errno = 0;
    g_finit_fail_at = 0;
    g_finit_params[0] = '\0';
    g_finit_content[0] = '\0';
    memset(g_finit_names, 0, sizeof(g_finit_names));
    g_delete_calls = 0;
    g_delete_errno = 0;
    g_deleted[0] = '\0';
}

/* ---- file helpers ------------------------------------------------------ */

static void write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (!fp) {
        perror(path);
        exit(2);
    }
    fputs(text, fp);
    fclose(fp);
}

static void module_file(const char *dir, const char *name, const char *content)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    write_file(path, content);
}

static void use_dirs(const char *proc, const char *node)
{
    struct virt_host_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.module_dirs[0] = g_dir1;
    cfg.module_dirs[1] = g_dir2;
    cfg.module_dir_count = 2;
    cfg.proc_modules = proc;
    cfg.fw_node = node;
    cfg.state_dir = g_state;
    cfg.sysfs_module_dir = g_sys;
    virt_host_configure(&cfg);
}

static void path_in_root(char *out, size_t cap, const char *leaf)
{
    snprintf(out, cap, "%s/%s", g_root, leaf);
}

static int rm_cb(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    (void)sb;
    (void)type;
    (void)ftw;
    remove(path);
    return 0;
}

/* ---- names and parameters ---------------------------------------------- */

static bool case_module_name_valid(void)
{
    char longest[80];

    CHECK(virt_host_module_name_valid("cavalry.ko"));
    CHECK(virt_host_module_name_valid("a.ko"));
    memset(longest, 'a', 60);
    strcpy(longest + 60, ".ko");
    CHECK(strlen(longest) == VIRT_HOST_MODULE_MAX_NAME);
    CHECK(virt_host_module_name_valid(longest));
    return true;
}

static bool case_module_name_traversal_rejected(void)
{
    CHECK(!virt_host_module_name_valid("../evil.ko"));
    CHECK(!virt_host_module_name_valid("a/b.ko"));
    CHECK(!virt_host_module_name_valid("/abs.ko"));
    CHECK(!virt_host_module_name_valid("a\\b.ko"));
    CHECK(!virt_host_module_name_valid("a..b.ko"));
    CHECK(!virt_host_module_name_valid("..ko"));
    return true;
}

static bool case_module_name_length_rejected(void)
{
    char too_long[80];

    memset(too_long, 'a', 61);
    strcpy(too_long + 61, ".ko");
    CHECK(strlen(too_long) == VIRT_HOST_MODULE_MAX_NAME + 1);
    CHECK(!virt_host_module_name_valid(too_long));
    CHECK(!virt_host_module_name_valid(NULL));
    CHECK(!virt_host_module_name_valid(""));
    CHECK(!virt_host_module_name_valid(".ko"));
    CHECK(!virt_host_module_name_valid("cavalry"));
    CHECK(!virt_host_module_name_valid("cavalry.ko.txt"));
    return true;
}

static bool case_module_params_valid(void)
{
    char longest[VIRT_HOST_MODULE_MAX_PARAM + 2];

    CHECK(virt_host_module_params_valid(NULL));
    CHECK(virt_host_module_params_valid(""));
    CHECK(virt_host_module_params_valid("ama_enable=1 dsp_buf_size=0x40000000"));
    CHECK(virt_host_module_params_valid("a,b.c_d"));
    memset(longest, 'a', VIRT_HOST_MODULE_MAX_PARAM);
    longest[VIRT_HOST_MODULE_MAX_PARAM] = '\0';
    CHECK(virt_host_module_params_valid(longest));
    return true;
}

static bool case_module_params_rejected(void)
{
    char too_long[VIRT_HOST_MODULE_MAX_PARAM + 2];

    CHECK(!virt_host_module_params_valid("a;b"));
    CHECK(!virt_host_module_params_valid("$(id)"));
    CHECK(!virt_host_module_params_valid("a\nb"));
    CHECK(!virt_host_module_params_valid("a|b"));
    CHECK(!virt_host_module_params_valid("a/b"));
    CHECK(!virt_host_module_params_valid("a\"b"));
    memset(too_long, 'a', VIRT_HOST_MODULE_MAX_PARAM + 1);
    too_long[VIRT_HOST_MODULE_MAX_PARAM + 1] = '\0';
    CHECK(!virt_host_module_params_valid(too_long));
    return true;
}

/* ---- load -------------------------------------------------------------- */

static bool case_load_success_opens_first_dir(void)
{
    reset_fake();
    module_file(g_dir1, "cavalry.ko", "first");
    module_file(g_dir2, "cavalry.ko", "second");
    use_dirs(NULL, NULL);
    CHECK(virt_host_module_load("cavalry.ko", NULL) == 0);
    CHECK(g_finit_calls == 1);
    CHECK(strcmp(g_finit_content, "first") == 0);
    CHECK(strcmp(g_finit_names[0], "cavalry.ko") == 0);

    /* A file only the second directory holds is found there. */
    reset_fake();
    module_file(g_dir2, "iav.ko", "only-second");
    CHECK(virt_host_module_load("iav.ko", NULL) == 0);
    CHECK(strcmp(g_finit_content, "only-second") == 0);
    return true;
}

static bool case_load_params_forwarded(void)
{
    reset_fake();
    module_file(g_dir1, "ambcma.ko", "x");
    use_dirs(NULL, NULL);
    CHECK(virt_host_module_load("ambcma.ko", "ama_enable=1 dsp_buf_size=0x40000000") == 0);
    CHECK(strcmp(g_finit_params, "ama_enable=1 dsp_buf_size=0x40000000") == 0);
    CHECK(virt_host_module_load("ambcma.ko", NULL) == 0);
    CHECK(strcmp(g_finit_params, "") == 0);
    return true;
}

static bool case_load_missing_file(void)
{
    reset_fake();
    use_dirs(NULL, NULL);
    CHECK(virt_host_module_load("not_in_image.ko", NULL) == -ENOENT);
    CHECK(g_finit_calls == 0);
    return true;
}

static bool case_load_eexist_is_success(void)
{
    reset_fake();
    module_file(g_dir1, "dsp.ko", "x");
    use_dirs(NULL, NULL);
    g_finit_errno = EEXIST;
    CHECK(virt_host_module_load("dsp.ko", NULL) == 0);
    CHECK(g_finit_calls == 1);
    return true;
}

static bool case_load_wrong_vermagic(void)
{
    reset_fake();
    module_file(g_dir1, "msg.ko", "x");
    use_dirs(NULL, NULL);
    g_finit_errno = ENOEXEC;
    CHECK(virt_host_module_load("msg.ko", NULL) == -ENOEXEC);
    CHECK(g_finit_calls == 1);
    return true;
}

static bool case_load_bad_signature(void)
{
    reset_fake();
    module_file(g_dir1, "imgproc.ko", "x");
    use_dirs(NULL, NULL);
    g_finit_errno = EKEYREJECTED;
    CHECK(virt_host_module_load("imgproc.ko", NULL) == -EKEYREJECTED);
    CHECK(g_finit_calls == 1);
    return true;
}

static bool case_load_invalid_name_no_syscall(void)
{
    reset_fake();
    module_file(g_dir1, "good.ko", "x");
    use_dirs(NULL, NULL);
    CHECK(virt_host_module_load("../good.ko", NULL) == -EINVAL);
    CHECK(virt_host_module_load("good", NULL) == -EINVAL);
    CHECK(virt_host_module_load(NULL, NULL) == -EINVAL);
    CHECK(virt_host_module_load("good.ko", "a;b") == -EINVAL);
    CHECK(g_finit_calls == 0);
    return true;
}

/* ---- unload ------------------------------------------------------------ */

static bool case_unload_success(void)
{
    reset_fake();
    CHECK(virt_host_module_unload("cavalry.ko") == 0);
    CHECK(g_delete_calls == 1);
    CHECK(strcmp(g_deleted, "cavalry") == 0);
    return true;
}

static bool case_unload_absent_is_success(void)
{
    reset_fake();
    g_delete_errno = ENOENT;
    CHECK(virt_host_module_unload("cavalry.ko") == 0);
    CHECK(g_delete_calls == 1);
    return true;
}

static bool case_unload_busy_propagates(void)
{
    reset_fake();
    g_delete_errno = EBUSY;
    CHECK(virt_host_module_unload("cavalry.ko") == -EBUSY);
    CHECK(g_delete_calls == 1);
    return true;
}

static bool case_unload_invalid_name_no_syscall(void)
{
    reset_fake();
    CHECK(virt_host_module_unload("../x.ko") == -EINVAL);
    CHECK(virt_host_module_unload("cavalry") == -EINVAL);
    CHECK(virt_host_module_unload(NULL) == -EINVAL);
    CHECK(g_delete_calls == 0);
    return true;
}

/* ---- module state ------------------------------------------------------ */

static bool case_mod_mask_scan(void)
{
    char proc[PATH_MAX];

    path_in_root(proc, sizeof(proc), "proc_modules");
    write_file(proc,
               "cavalry 393216 0 - Live 0xffff800000000000\n"
               "ambcma 16384 1 cavalry, Live 0xffff800000010000\n"
               "unrelated_mod 4096 0 - Live 0xffff800000020000\n");
    use_dirs(proc, NULL);
    CHECK(virt_host_module_mask() == (HOST_MOD_CAVALRY | HOST_MOD_AMBCMA));

    write_file(proc, "");
    CHECK(virt_host_module_mask() == 0);

    path_in_root(proc, sizeof(proc), "no_such_proc_modules");
    use_dirs(proc, NULL);
    CHECK(virt_host_module_mask() == 0);
    return true;
}

static uint32_t g_seen[16];
static int g_seen_count;
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;

static void watch_cb(uint32_t mask)
{
    pthread_mutex_lock(&g_seen_lock);
    if (g_seen_count < 16)
        g_seen[g_seen_count++] = mask;
    pthread_mutex_unlock(&g_seen_lock);
}

static int seen_count(void)
{
    int n;

    pthread_mutex_lock(&g_seen_lock);
    n = g_seen_count;
    pthread_mutex_unlock(&g_seen_lock);
    return n;
}

static bool case_watcher_initial_and_change(void)
{
    char proc[PATH_MAX];
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000000L };

    path_in_root(proc, sizeof(proc), "watch_modules");
    write_file(proc, "cavalry 1 0 - Live 0x0\n");
    use_dirs(proc, NULL);
    g_seen_count = 0;
    virt_host_set_watch_interval_ms(20);
    CHECK(virt_host_watch_start(watch_cb) == 0);
    CHECK(seen_count() == 1);
    CHECK(g_seen[0] == HOST_MOD_CAVALRY);

    nanosleep(&ts, NULL);
    nanosleep(&ts, NULL);
    nanosleep(&ts, NULL);
    CHECK(seen_count() == 1);   /* nothing changed, no repeat */

    write_file(proc, "cavalry 1 0 - Live 0x0\nambcma 1 0 - Live 0x0\n");
    for (int i = 0; i < 100 && seen_count() < 2; i++)
        nanosleep(&ts, NULL);
    virt_host_watch_stop();
    CHECK(seen_count() == 2);
    CHECK(g_seen[1] == (HOST_MOD_CAVALRY | HOST_MOD_AMBCMA));
    virt_host_set_watch_interval_ms(0);
    return true;
}

static bool case_watcher_double_start_rejected(void)
{
    char proc[PATH_MAX];

    path_in_root(proc, sizeof(proc), "watch_double");
    write_file(proc, "cavalry 1 0 - Live 0x0\n");
    use_dirs(proc, NULL);
    g_seen_count = 0;
    virt_host_set_watch_interval_ms(20);
    CHECK(virt_host_watch_start(watch_cb) == 0);
    CHECK(virt_host_watch_start(watch_cb) == -EBUSY);
    CHECK(seen_count() == 1);   /* the rejected start made no callback */
    virt_host_watch_stop();
    virt_host_set_watch_interval_ms(0);
    return true;
}

static bool case_watcher_stop_before_start_ok(void)
{
    virt_host_watch_stop();
    virt_host_watch_stop();
    return true;
}

static bool case_watcher_restart(void)
{
    char proc[PATH_MAX];
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000000L };

    path_in_root(proc, sizeof(proc), "watch_restart");
    write_file(proc, "cavalry 1 0 - Live 0x0\n");
    use_dirs(proc, NULL);
    g_seen_count = 0;
    virt_host_set_watch_interval_ms(20);
    CHECK(virt_host_watch_start(watch_cb) == 0);
    virt_host_watch_stop();
    virt_host_watch_stop();   /* a second stop is harmless */
    CHECK(seen_count() == 1);

    write_file(proc, "cavalry 1 0 - Live 0x0\nambcma 1 0 - Live 0x0\n");
    CHECK(virt_host_watch_start(watch_cb) == 0);   /* it starts again after a stop */
    CHECK(seen_count() == 2);
    CHECK(g_seen[1] == (HOST_MOD_CAVALRY | HOST_MOD_AMBCMA));
    write_file(proc, "cavalry 1 0 - Live 0x0\n");
    for (int i = 0; i < 100 && seen_count() < 3; i++)
        nanosleep(&ts, NULL);
    virt_host_watch_stop();
    CHECK(seen_count() == 3);
    CHECK(g_seen[2] == HOST_MOD_CAVALRY);
    virt_host_set_watch_interval_ms(0);
    return true;
}

/* ---- the module loader ------------------------------------------------- */

static bool case_loader_runs_conf_in_order(void)
{
    char conf[PATH_MAX];

    reset_fake();
    use_dirs(NULL, NULL);
    module_file(g_dir1, "ambcma.ko", "x");
    module_file(g_dir1, "cavalry.ko", "x");
    module_file(g_dir1, "iav.ko", "x");
    path_in_root(conf, sizeof(conf), "ok.conf");
    write_file(conf, "# early modules\nambcma.ko ama_enable=1\n\ncavalry.ko   # no params\niav.ko\n");
    CHECK(virt_module_loader_run(conf) == 0);
    CHECK(g_finit_calls == 3);
    CHECK(strcmp(g_finit_names[0], "ambcma.ko") == 0);
    CHECK(strcmp(g_finit_names[1], "cavalry.ko") == 0);
    CHECK(strcmp(g_finit_names[2], "iav.ko") == 0);
    CHECK(strcmp(g_finit_params, "") == 0);
    return true;
}

static bool case_loader_stops_on_failure(void)
{
    char conf[PATH_MAX];

    reset_fake();
    use_dirs(NULL, NULL);
    module_file(g_dir1, "ambcma.ko", "x");
    module_file(g_dir1, "cavalry.ko", "x");
    module_file(g_dir1, "iav.ko", "x");
    path_in_root(conf, sizeof(conf), "fail.conf");
    write_file(conf, "ambcma.ko\ncavalry.ko\niav.ko\n");
    g_finit_errno = ENOEXEC;
    g_finit_fail_at = 2;
    CHECK(virt_module_loader_run(conf) == -ENOEXEC);
    CHECK(g_finit_calls == 2);   /* the third module is not attempted */
    return true;
}

static bool case_loader_rejects_bad_line(void)
{
    char conf[PATH_MAX];

    reset_fake();
    use_dirs(NULL, NULL);
    module_file(g_dir1, "good.ko", "x");
    path_in_root(conf, sizeof(conf), "bad.conf");
    write_file(conf, "../evil.ko\ngood.ko\n");
    CHECK(virt_module_loader_run(conf) < 0);
    CHECK(g_finit_calls == 0);
    path_in_root(conf, sizeof(conf), "bad2.conf");
    write_file(conf, "good.ko a;b\n");
    CHECK(virt_module_loader_run(conf) < 0);
    CHECK(g_finit_calls == 0);
    return true;
}

/* ---- firmware ---------------------------------------------------------- */

static bool case_fw_path_valid(void)
{
    char at_limit[VIRT_HOST_FW_MAX_RELPATH + 2];
    char over[VIRT_HOST_FW_MAX_RELPATH + 2];

    CHECK(virt_host_firmware_path_valid("cavalry.bin"));
    CHECK(virt_host_firmware_path_valid("ambarella/n1_655/dsp/260920_075008/default_binary.bin"));
    CHECK(strlen("ambarella/n1_655/dsp/260920_075008/default_binary.bin") == 53);
    memset(at_limit, 'a', VIRT_HOST_FW_MAX_RELPATH);
    at_limit[VIRT_HOST_FW_MAX_RELPATH] = '\0';
    CHECK(virt_host_firmware_path_valid(at_limit));
    memset(over, 'a', VIRT_HOST_FW_MAX_RELPATH + 1);
    over[VIRT_HOST_FW_MAX_RELPATH + 1] = '\0';
    CHECK(!virt_host_firmware_path_valid(over));

    CHECK(!virt_host_firmware_path_valid(NULL));
    CHECK(!virt_host_firmware_path_valid(""));
    CHECK(!virt_host_firmware_path_valid("/abs/cavalry.bin"));
    CHECK(!virt_host_firmware_path_valid("../cavalry.bin"));
    CHECK(!virt_host_firmware_path_valid("a/../b"));
    CHECK(!virt_host_firmware_path_valid("a/./b"));
    CHECK(!virt_host_firmware_path_valid("a//b"));
    CHECK(!virt_host_firmware_path_valid("a/"));
    CHECK(!virt_host_firmware_path_valid("a\\b"));
    CHECK(!virt_host_firmware_path_valid("a\nb"));
    CHECK(!virt_host_firmware_path_valid("a\x7f" "b"));
    return true;
}

static void read_file(const char *path, char *out, size_t cap)
{
    FILE *fp = fopen(path, "r");
    size_t n = 0;

    out[0] = '\0';
    if (!fp)
        return;
    n = fread(out, 1, cap - 1, fp);
    out[n] = '\0';
    fclose(fp);
}

static bool case_fw_select_writes_node(void)
{
    char node[PATH_MAX], got[256];

    path_in_root(node, sizeof(node), "fw_node");
    write_file(node, "/lib/firmware-with-a-longer-name\n");
    use_dirs(NULL, node);
    CHECK(virt_host_firmware_select("/usr/lib/amba-virt/firmware") == 0);
    read_file(node, got, sizeof(got));
    CHECK(strcmp(got, "/usr/lib/amba-virt/firmware") == 0);
    return true;
}

static bool case_fw_select_skips_same(void)
{
    char node[PATH_MAX];
    struct stat st;
    struct timespec times[2] = { { .tv_sec = 1000000000 }, { .tv_sec = 1000000000 } };

    path_in_root(node, sizeof(node), "fw_node_same");
    write_file(node, "/usr/lib/amba-virt/firmware\n");
    CHECK(utimensat(AT_FDCWD, node, times, 0) == 0);
    use_dirs(NULL, node);
    CHECK(virt_host_firmware_select("/usr/lib/amba-virt/firmware") == 0);
    CHECK(stat(node, &st) == 0);
    CHECK(st.st_mtim.tv_sec == 1000000000);   /* untouched: no rewrite */
    return true;
}

static bool case_fw_select_failure_propagates(void)
{
    char node[PATH_MAX], longdir[VIRT_HOST_FW_MAX_DIR + 8];

    path_in_root(node, sizeof(node), "no_such_node");
    use_dirs(NULL, node);
    CHECK(virt_host_firmware_select("/usr/lib/amba-virt/firmware") == -ENOENT);

    /* A directory cannot be opened for writing. */
    use_dirs(NULL, g_root);
    CHECK(virt_host_firmware_select("/usr/lib/amba-virt/firmware") == -EISDIR);

    path_in_root(node, sizeof(node), "fw_node_bad");
    write_file(node, "/x\n");
    use_dirs(NULL, node);
    CHECK(virt_host_firmware_select(NULL) == -EINVAL);
    CHECK(virt_host_firmware_select("usr/lib/firmware") == -EINVAL);
    CHECK(virt_host_firmware_select("/") == -EINVAL);
    CHECK(virt_host_firmware_select("/usr/../etc") == -EINVAL);
    CHECK(virt_host_firmware_select("/usr//lib") == -EINVAL);
    CHECK(virt_host_firmware_select("/usr/lib/") == -EINVAL);
    memset(longdir, 'a', sizeof(longdir) - 1);
    longdir[0] = '/';
    longdir[sizeof(longdir) - 1] = '\0';
    CHECK(virt_host_firmware_select(longdir) == -EINVAL);
    return true;
}

static bool case_hardware_reset_unsupported(void)
{
    CHECK(virt_host_hardware_reset(AMBA_VIRT_DEV_TYPE_CAVALRY) == -EOPNOTSUPP);
    CHECK(virt_host_hardware_reset(0) == -EOPNOTSUPP);
    return true;
}

static bool case_unload_refused_after_drain_timeout(void)
{
    reset_fake();
    CHECK(virt_host_module_unload_drained("cavalry.ko", -1) == -EOPNOTSUPP);
    CHECK(g_delete_calls == 0);   /* the module stays loaded */
    return true;
}

static bool case_unload_proceeds_after_drain_ok(void)
{
    reset_fake();
    CHECK(virt_host_module_unload_drained("cavalry.ko", 0) == 0);
    CHECK(g_delete_calls == 1);
    CHECK(strcmp(g_deleted, "cavalry") == 0);
    return true;
}

static bool case_unload_non_cavalry_ignores_drain(void)
{
    reset_fake();
    CHECK(virt_host_module_unload_drained("iav.ko", -1) == 0);
    CHECK(g_delete_calls == 1);
    CHECK(strcmp(g_deleted, "iav") == 0);
    CHECK(virt_host_module_unload_drained("../x.ko", -1) == -EINVAL);
    CHECK(g_delete_calls == 1);
    return true;
}

static bool case_firmware_inventory_empty(void)
{
    struct virt_host_firmware_resp resp;

    memset(&resp, 0xff, sizeof(resp));
    CHECK(virt_host_get_firmware(&resp) == 0);
    CHECK(resp.count == 0);
    CHECK(virt_host_get_firmware(NULL) == -EINVAL);
    return true;
}

/* ---- parameters of a module that is already loaded --------------------- */

#define AMBCMA_PARAMS "ama_enable=1 dsp_buf_size=0x40000000"

static void sysfs_param(const char *mod, const char *param, const char *value)
{
    char dir[PATH_MAX], path[PATH_MAX];

    snprintf(dir, sizeof(dir), "%s/%s", g_sys, mod);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/%s/parameters", g_sys, mod);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%.4000s/%.60s", dir, param);
    write_file(path, value);
}

static void clear_state(void)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/ambcma.params", g_state);
    unlink(path);
}

static bool case_load_records_params_state(void)
{
    char path[PATH_MAX], got[256];

    reset_fake();
    clear_state();
    module_file(g_dir1, "ambcma.ko", "x");
    use_dirs(NULL, NULL);
    CHECK(virt_host_module_load("ambcma.ko", AMBCMA_PARAMS) == 0);
    snprintf(path, sizeof(path), "%s/ambcma.params", g_state);
    read_file(path, got, sizeof(got));
    CHECK(strcmp(got, AMBCMA_PARAMS) == 0);
    return true;
}

static bool case_eexist_same_params_ok(void)
{
    reset_fake();
    clear_state();
    module_file(g_dir1, "ambcma.ko", "x");
    use_dirs(NULL, NULL);
    CHECK(virt_host_module_load("ambcma.ko", AMBCMA_PARAMS) == 0);   /* records */
    g_finit_errno = EEXIST;                                          /* a restarted worker */
    CHECK(virt_host_module_load("ambcma.ko", AMBCMA_PARAMS) == 0);
    return true;
}

static bool case_eexist_different_params_rejected(void)
{
    reset_fake();
    clear_state();
    module_file(g_dir1, "ambcma.ko", "x");
    use_dirs(NULL, NULL);
    CHECK(virt_host_module_load("ambcma.ko", "ama_enable=0") == 0);
    g_finit_errno = EEXIST;
    CHECK(virt_host_module_load("ambcma.ko", AMBCMA_PARAMS) == -EEXIST);
    return true;
}

static bool case_eexist_unrecorded_sysfs_match_ok(void)
{
    reset_fake();
    clear_state();
    module_file(g_dir1, "ambcma.ko", "x");
    use_dirs(NULL, NULL);
    sysfs_param("ambcma", "ama_enable", "1\n");
    sysfs_param("ambcma", "dsp_buf_size", "1073741824\n");   /* 0x40000000, shown in decimal */
    g_finit_errno = EEXIST;
    CHECK(virt_host_module_load("ambcma.ko", AMBCMA_PARAMS) == 0);
    sysfs_param("ambcma", "ama_enable", "Y\n");               /* a bool parameter */
    CHECK(virt_host_module_load("ambcma.ko", AMBCMA_PARAMS) == 0);
    return true;
}

static bool case_eexist_unrecorded_sysfs_mismatch_rejected(void)
{
    reset_fake();
    clear_state();
    module_file(g_dir1, "ambcma.ko", "x");
    use_dirs(NULL, NULL);
    sysfs_param("ambcma", "ama_enable", "1\n");
    sysfs_param("ambcma", "dsp_buf_size", "536870912\n");    /* 0x20000000, not 0x40000000 */
    g_finit_errno = EEXIST;
    CHECK(virt_host_module_load("ambcma.ko", AMBCMA_PARAMS) == -EEXIST);
    return true;
}

static bool case_eexist_unrecorded_unreadable_rejected(void)
{
    char path[PATH_MAX];

    reset_fake();
    clear_state();
    module_file(g_dir1, "ambcma.ko", "x");
    use_dirs(NULL, NULL);
    snprintf(path, sizeof(path), "%s/ambcma/parameters/ama_enable", g_sys);
    unlink(path);
    g_finit_errno = EEXIST;
    CHECK(virt_host_module_load("ambcma.ko", AMBCMA_PARAMS) == -ENODATA);
    return true;
}

static bool case_eexist_without_params_ok(void)
{
    reset_fake();
    clear_state();
    module_file(g_dir1, "iav.ko", "x");
    use_dirs(NULL, NULL);
    g_finit_errno = EEXIST;
    CHECK(virt_host_module_load("iav.ko", NULL) == 0);
    CHECK(virt_host_module_load("iav.ko", "") == 0);
    return true;
}

/* ---- runner ------------------------------------------------------------ */

struct test_case {
    const char *name;
    bool (*fn)(void);
};

#define CASE(n) { #n, case_##n }

int main(void)
{
    static const struct test_case cases[] = {
        CASE(module_name_valid), CASE(module_name_traversal_rejected), CASE(module_name_length_rejected),
        CASE(module_params_valid), CASE(module_params_rejected), CASE(load_success_opens_first_dir),
        CASE(load_params_forwarded), CASE(load_missing_file), CASE(load_eexist_is_success),
        CASE(load_wrong_vermagic), CASE(load_bad_signature), CASE(load_invalid_name_no_syscall),
        CASE(unload_success), CASE(unload_absent_is_success), CASE(unload_busy_propagates),
        CASE(unload_invalid_name_no_syscall), CASE(mod_mask_scan), CASE(watcher_initial_and_change),
        CASE(loader_runs_conf_in_order), CASE(loader_stops_on_failure), CASE(loader_rejects_bad_line),
        CASE(fw_path_valid), CASE(fw_select_writes_node), CASE(fw_select_skips_same),
        CASE(fw_select_failure_propagates), CASE(firmware_inventory_empty), CASE(hardware_reset_unsupported),
        CASE(unload_refused_after_drain_timeout), CASE(unload_proceeds_after_drain_ok),
        CASE(unload_non_cavalry_ignores_drain), CASE(watcher_double_start_rejected),
        CASE(watcher_stop_before_start_ok), CASE(watcher_restart),
        CASE(load_records_params_state), CASE(eexist_same_params_ok),
        CASE(eexist_different_params_rejected), CASE(eexist_unrecorded_sysfs_match_ok),
        CASE(eexist_unrecorded_sysfs_mismatch_rejected), CASE(eexist_unrecorded_unreadable_rejected),
        CASE(eexist_without_params_ok),
    };
    int failed = 0;
    size_t total = sizeof(cases) / sizeof(cases[0]);

    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(g_root, sizeof(g_root), "/tmp/amba_host_ops_XXXXXX");
    if (!mkdtemp(g_root)) {
        perror("mkdtemp");
        return 2;
    }
    path_in_root(g_dir1, sizeof(g_dir1), "mods1");
    path_in_root(g_dir2, sizeof(g_dir2), "mods2");
    path_in_root(g_state, sizeof(g_state), "state");
    path_in_root(g_sys, sizeof(g_sys), "sys");
    if (mkdir(g_dir1, 0755) != 0 || mkdir(g_dir2, 0755) != 0 || mkdir(g_sys, 0755) != 0) {
        perror("mkdir");
        return 2;
    }
    virt_host_set_sys(&g_fake);

    for (size_t i = 0; i < total; i++) {
        bool ok = cases[i].fn();

        printf("[%s] %s\n", ok ? "PASS" : "FAIL", cases[i].name);
        if (!ok)
            failed++;
    }

    virt_host_set_sys(NULL);
    virt_host_configure(NULL);
    nftw(g_root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    printf("cases=%zu failed=%d\n", total, failed);
    return failed ? 1 : 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
