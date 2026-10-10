/*
 * virt_host_ops.c
 *
 * Direct Dom0 host operations. See virt_host_ops.h.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "virt_driver_matrix.h"
#include "virt_host_ops.h"

#define HLOG(fmt, ...) printf("amba-virt-server: host: " fmt "\n", ##__VA_ARGS__)

static int real_finit_module(int fd, const char *params)
{
    return (int)syscall(SYS_finit_module, fd, params, 0);
}

static int real_delete_module(const char *name)
{
    return (int)syscall(SYS_delete_module, name, O_NONBLOCK);
}

static const struct virt_host_sys g_real_sys = {
    .finit_module = real_finit_module,
    .delete_module = real_delete_module,
};

static const struct virt_host_sys *g_sys = &g_real_sys;
static struct virt_host_sys g_sys_override;

static char g_module_dirs[VIRT_HOST_MAX_MODULE_DIRS][256];
static size_t g_module_dir_count;
static char g_proc_modules[256] = VIRT_HOST_PROC_MODULES;
static char g_state_dir[256] = VIRT_HOST_STATE_DIR;
static char g_sysfs_dir[256] = VIRT_HOST_SYSFS_MODULE_DIR;
static char g_fw_node[256] = VIRT_HOST_FW_NODE_DEFAULT;
static bool g_configured;

void virt_host_set_sys(const struct virt_host_sys *sys)
{
    if (!sys) {
        g_sys = &g_real_sys;
        return;
    }
    g_sys_override = *sys;
    g_sys = &g_sys_override;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

void virt_host_configure(const struct virt_host_config *cfg)
{
    g_module_dir_count = 0;
    copy_str(g_proc_modules, sizeof(g_proc_modules), VIRT_HOST_PROC_MODULES);
    copy_str(g_fw_node, sizeof(g_fw_node), VIRT_HOST_FW_NODE_DEFAULT);
    copy_str(g_state_dir, sizeof(g_state_dir), VIRT_HOST_STATE_DIR);
    copy_str(g_sysfs_dir, sizeof(g_sysfs_dir), VIRT_HOST_SYSFS_MODULE_DIR);
    g_configured = false;
    if (!cfg)
        return;
    for (size_t i = 0; i < cfg->module_dir_count && i < VIRT_HOST_MAX_MODULE_DIRS; i++) {
        copy_str(g_module_dirs[i], sizeof(g_module_dirs[i]), cfg->module_dirs[i]);
        g_module_dir_count++;
    }
    if (cfg->proc_modules)
        copy_str(g_proc_modules, sizeof(g_proc_modules), cfg->proc_modules);
    if (cfg->fw_node)
        copy_str(g_fw_node, sizeof(g_fw_node), cfg->fw_node);
    if (cfg->state_dir)
        copy_str(g_state_dir, sizeof(g_state_dir), cfg->state_dir);
    if (cfg->sysfs_module_dir)
        copy_str(g_sysfs_dir, sizeof(g_sysfs_dir), cfg->sysfs_module_dir);
    g_configured = true;
}

/* ---- module names and parameters --------------------------------------- */

bool virt_host_module_name_valid(const char *name)
{
    size_t len;

    if (!name)
        return false;
    len = strlen(name);
    if (len == 0 || len > VIRT_HOST_MODULE_MAX_NAME)
        return false;
    if (len < 4 || strcmp(name + len - 3, ".ko") != 0)
        return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL)
        return false;
    if (strstr(name, "..") != NULL)
        return false;
    return true;
}

bool virt_host_module_params_valid(const char *params)
{
    size_t len;

    if (!params)
        return true;
    len = strlen(params);
    if (len > VIRT_HOST_MODULE_MAX_PARAM)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = params[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '=' ||
              c == '.' || c == ',' || c == ' '))
            return false;
    }
    return true;
}

/* ---- load and unload --------------------------------------------------- */

static int open_module(const char *name)
{
    struct utsname uts;
    char path[512];

    if (g_configured) {
        for (size_t i = 0; i < g_module_dir_count; i++) {
            int fd;

            snprintf(path, sizeof(path), "%.255s/%.63s", g_module_dirs[i], name);
            fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0)
                return fd;
        }
        return -1;
    }
    if (uname(&uts) != 0)
        return -1;
    snprintf(path, sizeof(path), "/lib/modules/%.65s/extra/%.63s", uts.release, name);
    return open(path, O_RDONLY | O_CLOEXEC);
}

/* ---- parameters a loaded module carries ------------------------------- */

static void module_base(const char *name, char *base, size_t cap)
{
    char *dot;

    snprintf(base, cap, "%s", name);
    dot = strrchr(base, '.');
    if (dot)
        *dot = '\0';
}

/* mkdir -p: every missing directory on the way to path. */
static void make_dirs(const char *path)
{
    char tmp[256];
    char *q;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (q = tmp + 1; *q; q++) {
        if (*q == '/') {
            *q = '\0';
            mkdir(tmp, 0755);
            *q = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void record_params(const char *base, const char *p)
{
    char path[512];
    int fd;

    make_dirs(g_state_dir);
    snprintf(path, sizeof(path), "%.255s/%.63s.params", g_state_dir, base);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write(fd, p, strlen(p)) < 0) {
        /* Without the record the sysfs check still applies. */
    }
    close(fd);
}

/* The parameter string recorded for base, or -1 when there is none. */
static int read_state(const char *base, char *out, size_t cap)
{
    char path[512];
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), "%.255s/%.63s.params", g_state_dir, base);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = '\0';
    return (int)n;
}

static bool as_number(const char *s, unsigned long long *v)
{
    char *end = NULL;

    if (s[0] == '\0')
        return false;
    if (s[1] == '\0' && (s[0] == 'Y' || s[0] == 'y')) {
        *v = 1;
        return true;
    }
    if (s[1] == '\0' && (s[0] == 'N' || s[0] == 'n')) {
        *v = 0;
        return true;
    }
    *v = strtoull(s, &end, 0);
    return end && *end == '\0';
}

/* The value a module reports and the value asked for, as numbers when both are, else as text. */
static bool values_equal(const char *actual, const char *want)
{
    unsigned long long a, w;

    if (as_number(actual, &a) && as_number(want, &w))
        return a == w;
    return strcmp(actual, want) == 0;
}

static int verify_via_sysfs(const char *base, const char *p)
{
    char copy[VIRT_HOST_MODULE_MAX_PARAM + 1];
    char *save = NULL;
    char *tok;

    snprintf(copy, sizeof(copy), "%s", p);
    for (tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        char path[512];
        char actual[128];
        const char *want;
        bool readable;
        ssize_t n = -1;
        int fd;

        if (!eq) {
            HLOG("module %s: parameter %s has no value to check", base, tok);
            return -ENODATA;
        }
        *eq = '\0';
        want = eq + 1;
        snprintf(path, sizeof(path), "%.255s/%.63s/parameters/%.63s", g_sysfs_dir, base, tok);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            n = read(fd, actual, sizeof(actual) - 1);
            close(fd);
        }
        readable = n >= 0;
        if (n < 0)
            n = 0;
        if (!readable) {
            HLOG("module %s is already loaded and parameter %s cannot be read", base, tok);
            return -ENODATA;
        }
        actual[n] = '\0';
        while (n > 0 && (actual[n - 1] == '\n' || actual[n - 1] == '\r' || actual[n - 1] == ' '))
            actual[--n] = '\0';
        if (!values_equal(actual, want)) {
            HLOG("module %s is already loaded with %s=%s, not %s", base, tok, actual, want);
            return -EEXIST;
        }
    }
    return 0;
}

static int verify_loaded_params(const char *name, const char *p)
{
    char base[VIRT_HOST_MODULE_MAX_NAME + 1];
    char recorded[VIRT_HOST_MODULE_MAX_PARAM + 1];

    if (p[0] == '\0')
        return 0;
    module_base(name, base, sizeof(base));
    if (read_state(base, recorded, sizeof(recorded)) >= 0) {
        if (strcmp(recorded, p) != 0) {
            HLOG("module %s is already loaded with parameters '%s', not '%s'", base, recorded, p);
            return -EEXIST;
        }
        return 0;
    }
    return verify_via_sysfs(base, p);
}

int virt_host_module_load(const char *name, const char *params)
{
    const char *p = (params && params[0] != '\0') ? params : "";
    int fd, ret, err;

    if (!virt_host_module_name_valid(name) || !virt_host_module_params_valid(params))
        return -EINVAL;

    fd = open_module(name);
    if (fd < 0) {
        HLOG("module %s not found in the image", name);
        return -ENOENT;
    }

    errno = 0;
    ret = g_sys->finit_module(fd, p);
    err = errno;
    close(fd);

    if (ret < 0) {
        if (err == EEXIST) {
            HLOG("module %s already loaded", name);
            return verify_loaded_params(name, p);
        }
        HLOG("finit_module %s failed: errno=%d (%s)", name, err, strerror(err));
        return -err;
    }
    if (p[0] != '\0') {
        char base[VIRT_HOST_MODULE_MAX_NAME + 1];

        module_base(name, base, sizeof(base));
        record_params(base, p);
    }
    HLOG("loaded module %s", name);
    return 0;
}

int virt_host_module_unload(const char *name)
{
    char base[VIRT_HOST_MODULE_MAX_NAME + 1];
    char *dot;
    int ret, err;

    if (!virt_host_module_name_valid(name))
        return -EINVAL;

    snprintf(base, sizeof(base), "%s", name);
    dot = strrchr(base, '.');
    if (dot)
        *dot = '\0';

    errno = 0;
    ret = g_sys->delete_module(base);
    err = errno;
    if (ret < 0) {
        if (err == ENOENT) {
            HLOG("module %s already unloaded", base);
            return 0;
        }
        HLOG("delete_module %s failed: errno=%d (%s)", base, err, strerror(err));
        return -err;
    }
    HLOG("unloaded module %s", base);
    return 0;
}

/* ---- module state ------------------------------------------------------ */

uint32_t virt_host_module_mask(void)
{
    FILE *fp = fopen(g_proc_modules, "r");
    char line[256];
    uint32_t mask = 0;

    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp)) {
        char mod_name[64];

        if (sscanf(line, "%63s", mod_name) != 1)
            continue;
        for (size_t i = 0; i < DRIVER_MATRIX_COUNT; i++) {
            char base_name[64];
            char *dot;

            snprintf(base_name, sizeof(base_name), "%s", g_driver_matrix[i].module_name);
            dot = strrchr(base_name, '.');
            if (dot)
                *dot = '\0';
            if (strcmp(mod_name, base_name) == 0)
                mask |= g_driver_matrix[i].module_mask;
        }
    }
    fclose(fp);
    return mask;
}

static pthread_t g_watch_thread;
static bool g_watch_running;
static virt_host_state_cb g_watch_cb;
static uint32_t g_watch_mask;
static unsigned int g_watch_ms = VIRT_HOST_WATCH_MS_DEFAULT;
static pthread_mutex_t g_watch_lock = PTHREAD_MUTEX_INITIALIZER;

void virt_host_set_watch_interval_ms(unsigned int ms)
{
    pthread_mutex_lock(&g_watch_lock);
    g_watch_ms = ms ? ms : VIRT_HOST_WATCH_MS_DEFAULT;
    pthread_mutex_unlock(&g_watch_lock);
}

static void *watch_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts;
        virt_host_state_cb cb;
        uint32_t now;
        bool changed = false;
        unsigned int ms;

        pthread_mutex_lock(&g_watch_lock);
        ms = g_watch_ms;
        pthread_mutex_unlock(&g_watch_lock);
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&g_watch_lock);
        if (!g_watch_running) {
            pthread_mutex_unlock(&g_watch_lock);
            break;
        }
        pthread_mutex_unlock(&g_watch_lock);

        now = virt_host_module_mask();
        pthread_mutex_lock(&g_watch_lock);
        cb = g_watch_cb;
        if (now != g_watch_mask) {
            HLOG("module state changed: 0x%08x -> 0x%08x", g_watch_mask, now);
            g_watch_mask = now;
            changed = true;
        }
        pthread_mutex_unlock(&g_watch_lock);
        if (changed && cb)
            cb(now);
    }
    return NULL;
}

int virt_host_watch_start(virt_host_state_cb cb)
{
    uint32_t mask;

    pthread_mutex_lock(&g_watch_lock);
    if (g_watch_running) {
        pthread_mutex_unlock(&g_watch_lock);
        return -EBUSY;
    }
    g_watch_running = true;
    g_watch_cb = cb;
    mask = virt_host_module_mask();
    g_watch_mask = mask;
    pthread_mutex_unlock(&g_watch_lock);

    if (cb)
        cb(mask);
    if (pthread_create(&g_watch_thread, NULL, watch_main, NULL) != 0) {
        pthread_mutex_lock(&g_watch_lock);
        g_watch_running = false;
        pthread_mutex_unlock(&g_watch_lock);
        return -EAGAIN;
    }
    return 0;
}

void virt_host_watch_stop(void)
{
    pthread_mutex_lock(&g_watch_lock);
    if (!g_watch_running) {
        pthread_mutex_unlock(&g_watch_lock);
        return;
    }
    g_watch_running = false;
    pthread_mutex_unlock(&g_watch_lock);
    pthread_join(g_watch_thread, NULL);
}

/* ---- firmware ---------------------------------------------------------- */

/* Every component of p is non-empty and is neither "." nor "..". */
static bool components_clean(const char *p)
{
    const char *comp = p;

    for (;;) {
        const char *end = strchr(comp, '/');
        size_t clen = end ? (size_t)(end - comp) : strlen(comp);

        if (clen == 0)
            return false;
        if (clen == 1 && comp[0] == '.')
            return false;
        if (clen == 2 && comp[0] == '.' && comp[1] == '.')
            return false;
        if (!end)
            break;
        comp = end + 1;
    }
    return true;
}

static bool no_control_or_backslash(const char *p)
{
    if (strchr(p, '\\') != NULL)
        return false;
    for (size_t i = 0; p[i] != '\0'; i++) {
        if ((unsigned char)p[i] < 0x20 || p[i] == 0x7f)
            return false;
    }
    return true;
}

bool virt_host_firmware_path_valid(const char *rel)
{
    size_t len;

    if (!rel)
        return false;
    len = strlen(rel);
    if (len == 0 || len > VIRT_HOST_FW_MAX_RELPATH)
        return false;
    if (rel[0] == '/')
        return false;
    if (!no_control_or_backslash(rel))
        return false;
    return components_clean(rel);
}

int virt_host_firmware_select(const char *dir)
{
    char cur[VIRT_HOST_FW_MAX_DIR + 1];
    size_t len;
    ssize_t n;
    int fd;

    if (!dir)
        return -EINVAL;
    len = strlen(dir);
    if (len < 2 || len > VIRT_HOST_FW_MAX_DIR || dir[0] != '/')
        return -EINVAL;
    if (!no_control_or_backslash(dir) || !components_clean(dir + 1))
        return -EINVAL;

    fd = open(g_fw_node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    n = read(fd, cur, sizeof(cur) - 1);
    if (n < 0) {
        int err = errno;

        close(fd);
        return -err;
    }
    cur[n] = '\0';
    while (n > 0 && (cur[n - 1] == '\n' || cur[n - 1] == '\r'))
        cur[--n] = '\0';

    if (strcmp(cur, dir) != 0) {
        /* A regular file needs truncating; a sysfs attribute rejects it. */
        if (lseek(fd, 0, SEEK_SET) < 0 || (ftruncate(fd, 0) < 0 && errno != EINVAL)) {
            int err = errno;

            close(fd);
            return -err;
        }
        if (write(fd, dir, len) != (ssize_t)len) {
            int err = errno ? errno : EIO;

            close(fd);
            return -err;
        }
        HLOG("firmware_class search path set to %s", dir);
    }
    close(fd);
    return 0;
}

int virt_host_get_firmware(struct virt_host_firmware_resp *resp)
{
    if (!resp)
        return -EINVAL;
    memset(resp, 0, sizeof(*resp));
    resp->count = 0;
    return 0;
}

/* ---- hardware reset ---------------------------------------------------- */

/*
 * No reset operation exists. The cavalry headers in this tree name none, and the daemon this
 * replaced only opened and closed /dev/cavalry, which resets nothing. Report that, and open no
 * device.
 */
int virt_host_hardware_reset(uint32_t dev_id)
{
    HLOG("accelerator reset for dev_id=%u is not supported", dev_id);
    return -EOPNOTSUPP;
}

/*
 * A cavalry module that still has requests in flight after the drain timeout needs a hardware
 * reset before it can be removed. With no reset available the module stays loaded.
 */
int virt_host_module_unload_drained(const char *name, int drained)
{
    if (name && strstr(name, "cavalry") != NULL && drained < 0) {
        int rr = virt_host_hardware_reset(AMBA_VIRT_DEV_TYPE_CAVALRY);

        if (rr < 0) {
            HLOG("%s still has requests in flight and cannot be reset: not unloading", name);
            return rr;
        }
    }
    return virt_host_module_unload(name);
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
