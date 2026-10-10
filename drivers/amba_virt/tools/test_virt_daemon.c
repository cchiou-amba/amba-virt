/*
 * test_virt_daemon.c
 *
 * Tests for the daemon-mode entry of amba-virt-server (virt_daemon.c) and its
 * readiness helpers (virt_ready.c). The worker is a function in this file that
 * runs in a real forked child, so every case exercises real fork, signal,
 * pipe, and reaping behavior. No hardware is involved.
 *
 * Each case prints "[PASS] <name>" or "[FAIL] <name>". The exit status is 1 when
 * any case fails.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "virt_daemon.h"
#include "virt_ready.h"

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("  check failed: %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            return false;                                                         \
        }                                                                         \
    } while (0)

static char g_root[128];

enum worker_mode {
    W_READY_THEN_WAIT,      /* ready after ms, then wait for a signal */
    W_NEVER_READY,          /* never ready */
    W_READY_THEN_CRASH,     /* ready, then exit 3 after 50 ms */
    W_CRASH_IMMEDIATELY,    /* exit 7 at once, never ready */
    W_READY_ORPHAN,         /* ready, leave a short-lived grandchild, and exit */
    W_RECORD_FOREGROUND,    /* record the pid and the ready fd, exit 9 */
};

struct wctx {
    enum worker_mode mode;
    unsigned int ms;
    char file[256];
};

static void msleep(unsigned int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static unsigned long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static void append_line(const char *path, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    int fd, n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    line[n++] = '\n';
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        if (write(fd, line, (size_t)n) < 0) {
            /* A failed append shows up as a missing line. */
        }
        close(fd);
    }
}

static int count_lines_with(const char *path, const char *prefix)
{
    FILE *fp = fopen(path, "r");
    char line[256];
    int n = 0;

    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, prefix, strlen(prefix)) == 0)
            n++;
    }
    fclose(fp);
    return n;
}

static bool wait_lines(const char *path, const char *prefix, int want, unsigned int timeout_ms)
{
    unsigned long long end = now_ms() + timeout_ms;

    while (now_ms() < end) {
        if (count_lines_with(path, prefix) >= want)
            return true;
        msleep(10);
    }
    return count_lines_with(path, prefix) >= want;
}

static int worker(int ready_fd, void *ctx_)
{
    struct wctx *ctx = ctx_;

    append_line(ctx->file, "start %llu", now_ms());
    append_line(ctx->file, "pid %d", (int)getpid());
    switch (ctx->mode) {
    case W_READY_THEN_WAIT:
        msleep(ctx->ms);
        virt_ready_notify(ready_fd);
        for (;;)
            pause();
    case W_NEVER_READY:
        for (;;)
            pause();
    case W_READY_THEN_CRASH:
        msleep(ctx->ms);
        virt_ready_notify(ready_fd);
        msleep(50);
        return 3;
    case W_CRASH_IMMEDIATELY:
        return 7;
    case W_READY_ORPHAN:
        virt_ready_notify(ready_fd);
        if (fork() == 0) {
            msleep(100);
            _exit(0);
        }
        return 0;
    case W_RECORD_FOREGROUND:
        append_line(ctx->file, "fg_pid %d", (int)getpid());
        append_line(ctx->file, "fg_ready_fd %d", ready_fd);
        return 9;
    }
    return 1;
}

static void base_cfg(struct virt_daemon_cfg *cfg, struct wctx *ctx, const char *name)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->worker = worker;
    cfg->ctx = ctx;
    cfg->ready_timeout_ms = 3000;
    cfg->restart_min_ms = 20;
    cfg->restart_max_ms = 80;
    cfg->restart = true;
    snprintf(ctx->file, sizeof(ctx->file), "%s/%s.log", g_root, name);
    unlink(ctx->file);
}

static bool alive(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

/* SIGTERM the daemon and wait for it. Returns its exit status, or -1. */
static int stop_daemon(pid_t d)
{
    int status = 0;
    unsigned long long end = now_ms() + 6000;

    if (d <= 0)
        return -1;
    kill(d, SIGTERM);
    while (now_ms() < end) {
        pid_t r = waitpid(d, &status, WNOHANG);

        if (r == d)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        msleep(10);
    }
    kill(d, SIGKILL);
    waitpid(d, &status, 0);
    return -1;
}

static int file_pid(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[256];
    int pid = -1;

    if (!fp)
        return -1;
    while (fgets(line, sizeof(line), fp)) {
        int v;

        if (sscanf(line, "pid %d", &v) == 1)
            pid = v;
    }
    fclose(fp);
    return pid;
}

/* ---- the daemon --------------------------------------------------------- */

static bool case_daemon_ready_returns_zero(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_READY_THEN_WAIT, .ms = 100 };
    pid_t d = 0;
    unsigned long long t0;

    base_cfg(&cfg, &ctx, "ready");
    t0 = now_ms();
    CHECK(virt_daemon_start(&cfg, &d) == 0);
    CHECK(now_ms() - t0 < 2500);
    CHECK(alive(d));
    CHECK(alive(file_pid(ctx.file)));
    CHECK(stop_daemon(d) == 0);
    return true;
}

static bool case_daemon_ready_timeout(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_NEVER_READY };
    pid_t d = 0;
    unsigned long long t0;

    base_cfg(&cfg, &ctx, "timeout");
    cfg.ready_timeout_ms = 300;
    t0 = now_ms();
    CHECK(virt_daemon_start(&cfg, &d) == -ETIMEDOUT);
    CHECK(now_ms() - t0 >= 250);
    CHECK(alive(d));                       /* boot goes on; the daemon keeps supervising */
    CHECK(alive(file_pid(ctx.file)));
    CHECK(stop_daemon(d) == 0);
    return true;
}

static bool case_daemon_worker_crash_restart(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_READY_THEN_CRASH, .ms = 20 };
    pid_t d = 0;

    base_cfg(&cfg, &ctx, "crash");
    CHECK(virt_daemon_start(&cfg, &d) == 0);
    CHECK(wait_lines(ctx.file, "start", 3, 3000));   /* it died twice and was started again */
    CHECK(alive(d));
    CHECK(stop_daemon(d) == 0);
    return true;
}

static bool case_daemon_restart_backoff_bounded(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_CRASH_IMMEDIATELY };
    pid_t d = 0;
    unsigned long long t[16];
    int n = 0;
    FILE *fp;
    char line[256];

    base_cfg(&cfg, &ctx, "backoff");
    CHECK(virt_daemon_start(&cfg, &d) == -EIO);      /* it died before it was ready */
    CHECK(wait_lines(ctx.file, "start", 6, 4000));
    CHECK(stop_daemon(d) == 0);

    fp = fopen(ctx.file, "r");
    CHECK(fp != NULL);
    while (fgets(line, sizeof(line), fp) && n < 16) {
        unsigned long long v;

        if (sscanf(line, "start %llu", &v) == 1)
            t[n++] = v;
    }
    fclose(fp);
    CHECK(n >= 6);
    for (int i = 1; i < n; i++) {
        unsigned long long gap = t[i] - t[i - 1];

        CHECK(gap >= 15);                  /* never faster than the first delay */
        CHECK(gap <= 80 + 400);            /* never slower than the cap, plus scheduling slack */
    }
    CHECK(t[5] - t[4] >= 60);              /* the delay has grown to the cap */
    return true;
}

static bool case_daemon_no_restart_when_disabled(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_READY_THEN_CRASH, .ms = 20 };
    pid_t d = 0;
    int status = 0;
    unsigned long long end;

    base_cfg(&cfg, &ctx, "norestart");
    cfg.restart = false;
    CHECK(virt_daemon_start(&cfg, &d) == 0);
    end = now_ms() + 3000;
    while (now_ms() < end && waitpid(d, &status, WNOHANG) != d)
        msleep(10);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);   /* the daemon ended with the worker */
    CHECK(count_lines_with(ctx.file, "start") == 1);
    return true;
}

static bool case_daemon_worker_start_failure(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_CRASH_IMMEDIATELY };
    pid_t d = 0;
    int status = 0;

    base_cfg(&cfg, &ctx, "startfail");
    cfg.restart = false;
    CHECK(virt_daemon_start(&cfg, &d) == -EIO);
    CHECK(waitpid(d, &status, 0) == d);
    CHECK(count_lines_with(ctx.file, "start") == 1);
    return true;
}

/* True when some process still lists parent as its parent and is a zombie. */
static bool has_zombie_child(pid_t parent)
{
    DIR *dir = opendir("/proc");
    struct dirent *e;
    bool found = false;

    if (!dir)
        return false;
    while ((e = readdir(dir)) != NULL) {
        char path[64], buf[512];
        FILE *fp;
        char *rp;
        int ppid;
        char state;

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%.40s/stat", e->d_name);
        fp = fopen(path, "r");
        if (!fp)
            continue;
        if (!fgets(buf, sizeof(buf), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);
        rp = strrchr(buf, ')');
        if (rp && sscanf(rp + 1, " %c %d", &state, &ppid) == 2 && ppid == parent && state == 'Z')
            found = true;
    }
    closedir(dir);
    return found;
}

static bool case_daemon_reaps_children(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_READY_ORPHAN };
    pid_t d = 0;

    base_cfg(&cfg, &ctx, "reap");
    cfg.restart_min_ms = 5000;             /* keep the daemon from starting another worker */
    cfg.restart_max_ms = 5000;
    CHECK(virt_daemon_start(&cfg, &d) == 0);
    msleep(500);                           /* the worker exited and its grandchild ended */
    CHECK(alive(d));
    CHECK(!has_zombie_child(d));           /* the worker and the orphan were both reaped */
    CHECK(stop_daemon(d) == 0);
    return true;
}

static bool case_daemon_hold_and_resume(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_READY_THEN_WAIT, .ms = 20 };
    pid_t d = 0, w;

    base_cfg(&cfg, &ctx, "hold");
    CHECK(virt_daemon_start(&cfg, &d) == 0);
    w = file_pid(ctx.file);
    CHECK(alive(w));
    kill(d, SIGUSR1);
    msleep(500);
    CHECK(kill(w, 0) != 0);                /* the worker is gone */
    CHECK(count_lines_with(ctx.file, "start") == 1);   /* and it was not restarted */
    kill(d, SIGUSR2);
    CHECK(wait_lines(ctx.file, "start", 2, 3000));
    CHECK(stop_daemon(d) == 0);
    return true;
}

static bool case_daemon_term_stops_worker(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_READY_THEN_WAIT, .ms = 20 };
    pid_t d = 0, w;

    base_cfg(&cfg, &ctx, "term");
    CHECK(virt_daemon_start(&cfg, &d) == 0);
    w = file_pid(ctx.file);
    CHECK(alive(w));
    CHECK(stop_daemon(d) == 0);
    CHECK(kill(w, 0) != 0);
    CHECK(kill(d, 0) != 0);
    return true;
}

static bool case_daemon_diag_written(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_READY_THEN_CRASH, .ms = 20 };
    char diag[256];
    pid_t d = 0;

    base_cfg(&cfg, &ctx, "diag");
    snprintf(diag, sizeof(diag), "%s/diag.txt", g_root);
    unlink(diag);
    cfg.diag_path = diag;
    CHECK(virt_daemon_start(&cfg, &d) == 0);
    CHECK(wait_lines(diag, "[", 4, 3000));
    CHECK(stop_daemon(d) == 0);
    {
        FILE *fp = fopen(diag, "r");
        char buf[4096] = "";
        size_t n;

        CHECK(fp != NULL);
        n = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[n] = '\0';
        fclose(fp);
        CHECK(strstr(buf, "worker started pid=") != NULL);
        CHECK(strstr(buf, "is ready") != NULL);
        CHECK(strstr(buf, "exited status=3") != NULL);
    }
    return true;
}

static bool case_daemon_foreground_runs_in_process(void)
{
    struct virt_daemon_cfg cfg;
    struct wctx ctx = { .mode = W_RECORD_FOREGROUND };
    FILE *fp;
    char line[256];
    int fg_pid = -1, fg_fd = 0;

    base_cfg(&cfg, &ctx, "foreground");
    CHECK(virt_daemon_run_foreground(&cfg) == 9);    /* the worker's status comes back */
    fp = fopen(ctx.file, "r");
    CHECK(fp != NULL);
    while (fgets(line, sizeof(line), fp)) {
        sscanf(line, "fg_pid %d", &fg_pid);
        sscanf(line, "fg_ready_fd %d", &fg_fd);
    }
    fclose(fp);
    CHECK(fg_pid == (int)getpid());                  /* no fork */
    CHECK(fg_fd == -1);                              /* and no readiness channel */
    return true;
}

/* ---- readiness ---------------------------------------------------------- */

static bool case_ready_shm_absent_waits(void)
{
    char dir[256], decoy[300];
    unsigned long long t0;

    snprintf(dir, sizeof(dir), "%s/dev_absent", g_root);
    mkdir(dir, 0755);
    snprintf(decoy, sizeof(decoy), "%.200s/amba_virt", dir);   /* not an amba_virt_shm node */
    close(open(decoy, O_CREAT | O_WRONLY, 0644));
    CHECK(!virt_ready_shm_present(dir));
    t0 = now_ms();
    CHECK(virt_ready_wait_shm(dir, 200) == -ETIMEDOUT);
    CHECK(now_ms() - t0 >= 150);
    return true;
}

static bool case_ready_shm_present_returns(void)
{
    char dir[256], node[300];
    unsigned long long t0;

    snprintf(dir, sizeof(dir), "%s/dev_present", g_root);
    mkdir(dir, 0755);
    snprintf(node, sizeof(node), "%.200s/amba_virt_shm1", dir);
    close(open(node, O_CREAT | O_WRONLY, 0644));
    CHECK(virt_ready_shm_present(dir));
    t0 = now_ms();
    CHECK(virt_ready_wait_shm(dir, 2000) == 0);
    CHECK(now_ms() - t0 < 500);
    CHECK(!virt_ready_shm_present("/nonexistent-dir-for-test"));
    return true;
}

static bool case_ready_notify_writes_byte(void)
{
    int p[2];
    char b = 0;

    CHECK(pipe(p) == 0);
    virt_ready_notify(p[1]);
    CHECK(read(p[0], &b, 1) == 1);
    CHECK(b == VIRT_READY_BYTE);
    close(p[0]);
    close(p[1]);
    return true;
}

static bool case_ready_notify_no_fd_noop(void)
{
    virt_ready_notify(-1);
    return true;
}

/* ---- runner ------------------------------------------------------------- */

static int rm_cb(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    (void)sb;
    (void)type;
    (void)ftw;
    remove(path);
    return 0;
}

struct test_case {
    const char *name;
    bool (*fn)(void);
};

#define CASE(n) { #n, case_##n }

int main(void)
{
    static const struct test_case cases[] = {
        CASE(daemon_ready_returns_zero), CASE(daemon_ready_timeout), CASE(daemon_worker_crash_restart),
        CASE(daemon_restart_backoff_bounded), CASE(daemon_no_restart_when_disabled),
        CASE(daemon_worker_start_failure), CASE(daemon_reaps_children), CASE(daemon_hold_and_resume),
        CASE(daemon_term_stops_worker), CASE(daemon_diag_written), CASE(daemon_foreground_runs_in_process),
        CASE(ready_shm_absent_waits), CASE(ready_shm_present_returns), CASE(ready_notify_writes_byte),
        CASE(ready_notify_no_fd_noop),
    };
    int failed = 0;
    size_t total = sizeof(cases) / sizeof(cases[0]);

    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(g_root, sizeof(g_root), "/tmp/amba_daemon_XXXXXX");
    if (!mkdtemp(g_root)) {
        perror("mkdtemp");
        return 2;
    }
    for (size_t i = 0; i < total; i++) {
        bool ok = cases[i].fn();

        printf("[%s] %s\n", ok ? "PASS" : "FAIL", cases[i].name);
        if (!ok)
            failed++;
    }
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
