/*
 * virt_camera_pipeline.c
 *
 * The hands-off camera pipeline of amba-virt-server. The order is the order of
 * tools/bringup/scripts/run_sensor_bringup_fresh.sh, sections 2 through 11.
 *
 * Every ioctl issued here is in the accepted Envelope 3 citation or is one of
 * the three step 10 ioctls. The ioctls of step 6 (microcode load) and step 7
 * (idle) follow load_ucode() and `test_encode --idle --nopreview`. Step 11 is
 * done by libamba-virt-camera.so, which this file reaches with dlopen().
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <basetypes.h>
#include <iav_ioctl.h>
#include <iav_ucode_ioctl.h>
#include <iav_vin_ioctl.h>

#include "amba_virt_camera.h"
#include "virt_host_ops.h"
#include "virt_camera_conf.h"
#include "virt_camera_pipeline.h"
#include "virt_gpio.h"
#include "virt_module_loader.h"

#define PLOG(fmt, ...) printf("amba-virt-server: camera: " fmt "\n", ##__VA_ARGS__)

#define MONITOR_ALIVE_MS      3000
#define AAA_ALIVE_MS          2000
#define AAA_HANDSHAKE_MS      15000
#define CHILD_TERM_MS         5000
#define CHILD_KILL_MS         2000
#define AAA_LOG_CAP           (1024 * 1024)
#define FW_WALK_MAX_DEPTH     8
#define MAX_LATE_MODULES      32
#define VSRC_SCAN_MAX         32
#define SENSOR_NAME           "os08a10"

struct pipeline {
    pthread_mutex_t lock;
    struct virt_camera_conf conf;
    pid_t monitor_pid;
    pid_t aaa_pid;
    int aaa_master;
    pthread_t aaa_reader;
    bool aaa_reader_started;
    char *aaa_log;
    size_t aaa_log_len;
    struct virt_gpio gpio;
    char late_names[MAX_LATE_MODULES][64];
    size_t late_count;
    bool late_attempted;
    int kmsg_fd;
    bool shutting_down;
    bool shutdown_done;
    virt_camera_encode_stop_fn encode_stop;
    volatile sig_atomic_t signaled;
};

static struct pipeline g_p = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .monitor_pid = -1,
    .aaa_pid = -1,
    .aaa_master = -1,
    .gpio = VIRT_GPIO_INIT,
    .kmsg_fd = -1,
};

extern char **environ;

static void msleep(unsigned int ms)
{
    usleep(ms * 1000u);
}

/* ---- text checks ------------------------------------------------------- */

int virt_camera_log_has_handshake(const char *log)
{
    return log && strstr(log, "AAA prepare done") && strstr(log, "ADJ parameter version") &&
           strstr(log, "AEB parameter version");
}

int virt_camera_log_fell_back(const char *log)
{
    const char *p = log;

    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];

        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        if (strstr(line, "Can't find file:") &&
            (strstr(line, "os08a10.rgb.linear.liso.adj_param") ||
             strstr(line, "os08a10.rgb.linear.liso.aeb_param")))
            return 1;
        if (!eol)
            break;
        p = eol + 1;
    }
    return 0;
}

int virt_camera_kmsg_has_failure(const char *text)
{
    return text && (strstr(text, "no active user") || strstr(text, "empty ISO cfg"));
}

int virt_camera_fw_rel_path(const char *root, const char *path, char *out, size_t cap)
{
    size_t rl;
    const char *rel;

    if (!root || !path || !out || cap == 0)
        return -EINVAL;
    rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/')
        rl--;
    if (strncmp(path, root, rl) != 0 || path[rl] != '/')
        return -EINVAL;
    rel = path + rl + 1;
    if (*rel == '\0' || strlen(rel) >= cap)
        return -EINVAL;
    memcpy(out, rel, strlen(rel) + 1);
    return 0;
}

/* ---- child processes --------------------------------------------------- */

/* True if pid is still running after ms. A child that exited is reaped. */
static bool child_alive_after(pid_t *pid, unsigned int ms)
{
    for (unsigned int waited = 0; waited <= ms; waited += 100) {
        int status;
        pid_t r = waitpid(*pid, &status, WNOHANG);

        if (r == *pid) {
            if (WIFEXITED(status))
                PLOG("child %d exited with status %d", (int)*pid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                PLOG("child %d killed by signal %d", (int)*pid, WTERMSIG(status));
            *pid = -1;
            return false;
        }
        if (r < 0 && errno != EINTR) {
            *pid = -1;
            return false;
        }
        if (waited < ms)
            msleep(100);
    }
    return true;
}

static void stop_child(pid_t *pid, const char *name)
{
    int status;

    if (*pid <= 0)
        return;
    PLOG("stopping %s (pid %d)", name, (int)*pid);
    kill(-*pid, SIGTERM);
    kill(*pid, SIGTERM);
    for (unsigned int waited = 0; waited < CHILD_TERM_MS; waited += 100) {
        if (waitpid(*pid, &status, WNOHANG) == *pid || kill(*pid, 0) < 0) {
            *pid = -1;
            return;
        }
        msleep(100);
    }
    PLOG("%s did not exit after SIGTERM, sending SIGKILL", name);
    kill(-*pid, SIGKILL);
    kill(*pid, SIGKILL);
    for (unsigned int waited = 0; waited < CHILD_KILL_MS; waited += 100) {
        if (waitpid(*pid, &status, WNOHANG) == *pid) {
            *pid = -1;
            return;
        }
        msleep(100);
    }
    PLOG("%s is still not gone", name);
    *pid = -1;
}

/*
 * Starts a Cooper binary the way the bring-up script does: through the private
 * loader with --library-path, so the golden binaries need no RPATH. A pty slave
 * path, when given, becomes the child's stdin, stdout and stderr, which makes
 * its stdio line-buffered. Only async-signal-safe calls run between fork and
 * exec, so everything is prepared first.
 */
static pid_t spawn_cooper(const char *bin, const char *arg1, char *const envp[], const char *pty_slave)
{
    char *argv[7];
    int argc = 0;
    long maxfd = sysconf(_SC_OPEN_MAX);
    pid_t pid;

    if (maxfd < 0 || maxfd > 4096)
        maxfd = 4096;
    argv[argc++] = (char *)VIRT_CAMERA_LOADER;
    argv[argc++] = (char *)"--library-path";
    argv[argc++] = (char *)VIRT_CAMERA_LIB_DIR;
    argv[argc++] = (char *)bin;
    if (arg1)
        argv[argc++] = (char *)arg1;
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int fd;

        setsid();
        if (pty_slave) {
            fd = open(pty_slave, O_RDWR | O_NOCTTY);
            if (fd < 0)
                _exit(126);
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        } else {
            fd = open("/dev/null", O_RDONLY);
            if (fd >= 0)
                dup2(fd, STDIN_FILENO);
        }
        for (int c = 3; c < maxfd; c++)
            close(c);
        execve(VIRT_CAMERA_LOADER, argv, envp);
        _exit(127);
    }
    return pid;
}

static char **env_with_profile(int profile)
{
    size_t n = 0;
    char **env;
    size_t out = 0;

    while (environ && environ[n])
        n++;
    env = calloc(n + 2, sizeof(*env));
    if (!env)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], "APP_IMG_PROFILE=", 16) == 0)
            continue;
        env[out] = strdup(environ[i]);
        if (!env[out])
            goto fail;
        out++;
    }
    if (asprintf(&env[out], "APP_IMG_PROFILE=%d", profile) < 0) {
        env[out] = NULL;
        goto fail;
    }
    env[++out] = NULL;
    return env;
fail:
    for (size_t i = 0; i < out; i++)
        free(env[i]);
    free(env);
    return NULL;
}

static void free_env(char **env)
{
    for (size_t i = 0; env && env[i]; i++)
        free(env[i]);
    free(env);
}

static void *aaa_reader_thread(void *arg)
{
    char buf[4096];

    (void)arg;
    for (;;) {
        ssize_t n = read(g_p.aaa_master, buf, sizeof(buf));

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (write(STDOUT_FILENO, buf, (size_t)n) < 0) {
            /* The log buffer below is what the checks read; a lost echo is harmless. */
        }
        pthread_mutex_lock(&g_p.lock);
        if (g_p.aaa_log && g_p.aaa_log_len + (size_t)n < AAA_LOG_CAP) {
            memcpy(g_p.aaa_log + g_p.aaa_log_len, buf, (size_t)n);
            g_p.aaa_log_len += (size_t)n;
            g_p.aaa_log[g_p.aaa_log_len] = '\0';
        }
        pthread_mutex_unlock(&g_p.lock);
    }
    return NULL;
}

/* ---- step 2: firmware -------------------------------------------------- */

static int fw_walk(const char *root, const char *dir, int depth, unsigned int *files)
{
    struct dirent **names = NULL;
    int n = scandir(dir, &names, NULL, alphasort);
    int rc = 0;

    if (n < 0)
        return -errno;
    for (int i = 0; i < n && rc == 0; i++) {
        const char *name = names[i]->d_name;
        char path[VIRT_CAMERA_PATH_MAX + 128];
        struct stat st;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
            rc = -ENAMETOOLONG;
            break;
        }
        if (lstat(path, &st) < 0) {
            rc = -errno;
            break;
        }
        if (S_ISLNK(st.st_mode)) {
            /* The host copy holds regular files. The load reads symlinks through the package tree. */
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth + 1 > FW_WALK_MAX_DEPTH)
                rc = -ELOOP;
            else
                rc = fw_walk(root, path, depth + 1, files);
        } else if (S_ISREG(st.st_mode)) {
            char rel[VIRT_HOST_FW_MAX_RELPATH + 1];

            rc = virt_camera_fw_rel_path(root, path, rel, sizeof(rel));
            if (rc == 0 && !virt_host_firmware_path_valid(rel))
                rc = -EINVAL;
            if (rc < 0) {
                PLOG("firmware %s is not a valid image path: %d", path, rc);
                break;
            }
            (*files)++;
        }
    }
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
    return rc;
}

static int step_firmware(const struct virt_camera_conf *c)
{
    unsigned int files = 0;
    int rc = fw_walk(c->firmware_dir, c->firmware_dir, 0, &files);

    if (rc < 0)
        return rc;
    rc = virt_host_firmware_select(c->firmware_dir);
    if (rc < 0) {
        PLOG("firmware search path %s failed: %d", c->firmware_dir, rc);
        return rc;
    }
    PLOG("firmware: %u files in %s, kernel search path set", files, c->firmware_dir);
    return 0;
}

/* ---- step 4: device nodes ---------------------------------------------- */

static int proc_devices_major(const char *name)
{
    FILE *fp = fopen("/proc/devices", "r");
    char line[256];
    bool in_char = false;
    int major = -1;

    if (!fp)
        return -errno;
    while (fgets(line, sizeof(line), fp)) {
        int num;
        char dev[64];

        if (strstr(line, "Character devices:")) {
            in_char = true;
            continue;
        }
        if (strstr(line, "Block devices:"))
            break;
        if (in_char && sscanf(line, "%d %63s", &num, dev) == 2 && strcmp(dev, name) == 0) {
            major = num;
            break;
        }
    }
    fclose(fp);
    return major >= 0 ? major : -ENODEV;
}

static int ensure_chrdev(const char *path, const char *dev_name, unsigned int minor, mode_t mode)
{
    int major = proc_devices_major(dev_name);
    struct stat st;

    if (major < 0) {
        PLOG("no character device %s in /proc/devices for %s", dev_name, path);
        return major;
    }
    if (stat(path, &st) == 0) {
        if (S_ISCHR(st.st_mode) && (int)major(st.st_rdev) == major && minor(st.st_rdev) == minor)
            return 0;
        /* A node from an earlier boot can carry a stale major. */
        if (unlink(path) < 0)
            return -errno;
    }
    if (mknod(path, S_IFCHR | mode, makedev((unsigned int)major, minor)) < 0)
        return -errno;
    if (chmod(path, mode) < 0)
        return -errno;
    PLOG("created %s (c %d %u)", path, major, minor);
    return 0;
}

static int step_device_nodes(void)
{
    int rc = ensure_chrdev("/dev/ucode", "iav_ucode", 0, 0666);

    if (rc == 0)
        rc = ensure_chrdev("/dev/iav", "iav_ucode", 1, 0666);
    if (rc == 0)
        rc = ensure_chrdev("/dev/cavalry", "cavalry", 0, 0666);
    if (rc == 0)
        rc = ensure_chrdev("/dev/amba_otp", "otp", 0, 0600);
    return rc;
}

/* ---- step 6: microcode ------------------------------------------------- */

static bool ucode_name_ok(const char *name)
{
    return name[0] != '\0' && name[0] != '/' && strstr(name, "..") == NULL;
}

static int load_microcode(const char *fw_dir)
{
    int fd_ucode = -1, fd_iav = -1;
    ucode_load_info_t info;
    ucode_fw_size_t fw_size;
    ucode_version_t version;
    u32 state = 0;
    uint8_t *mem = MAP_FAILED;
    size_t map_size = 0;
    int rc = 0;

    fd_ucode = open("/dev/ucode", O_RDWR | O_CLOEXEC);
    if (fd_ucode < 0)
        return -errno;
    fd_iav = open("/dev/iav", O_RDWR | O_CLOEXEC);
    if (fd_iav < 0) {
        rc = -errno;
        goto out;
    }

    if (ioctl(fd_iav, IAV_IOC_GET_IAV_STATE, &state) < 0) {
        rc = -errno;
        goto out;
    }
    if (state != (u32)IAV_STATE_INIT) {
        PLOG("DSP is already booted up to state %u, microcode not loaded", (unsigned int)state);
        goto out;
    }

    memset(&info, 0, sizeof(info));
    if (ioctl(fd_ucode, IAV_IOC_GET_UCODE_INFO, &info) < 0) {
        rc = -errno;
        goto out;
    }
    if (info.nr_item == 0 || info.nr_item > UCODE_LOAD_ITEM_MAX || info.map_size == 0) {
        rc = -EPROTO;
        goto out;
    }

    if (info.is_compact) {
        memset(&fw_size, 0, sizeof(fw_size));
        fw_size.nr_item = info.nr_item;
        for (unsigned int i = 0; i < info.nr_item; i++) {
            char name[sizeof(info.items[i].filename) + 1];
            char path[VIRT_CAMERA_PATH_MAX + 64];
            struct stat st;

            memcpy(name, info.items[i].filename, sizeof(info.items[i].filename));
            name[sizeof(info.items[i].filename)] = '\0';
            if (!ucode_name_ok(name)) {
                rc = -EPROTO;
                goto out;
            }
            snprintf(path, sizeof(path), "%s/%s", fw_dir, name);
            if (stat(path, &st) < 0) {
                rc = -errno;
                PLOG("cannot stat %s: %d", path, rc);
                goto out;
            }
            memcpy(fw_size.items[i].filename, info.items[i].filename, sizeof(fw_size.items[i].filename));
            fw_size.items[i].size = (unsigned long)st.st_size;
        }
        if (ioctl(fd_ucode, IAV_IOC_SET_UCODE_FW_SIZE, &fw_size) < 0) {
            rc = -errno;
            goto out;
        }
        /* The offsets change once the sizes are set. */
        if (ioctl(fd_ucode, IAV_IOC_GET_UCODE_INFO, &info) < 0) {
            rc = -errno;
            goto out;
        }
    }

    map_size = info.map_size;
    mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_ucode, 0);
    if (mem == MAP_FAILED) {
        rc = -errno;
        goto out;
    }

    for (unsigned int i = 0; i < info.nr_item; i++) {
        char name[sizeof(info.items[i].filename) + 1];
        char path[VIRT_CAMERA_PATH_MAX + 64];
        struct stat st;
        size_t off = info.items[i].addr_offset;
        size_t done = 0;
        int fd;

        memcpy(name, info.items[i].filename, sizeof(info.items[i].filename));
        name[sizeof(info.items[i].filename)] = '\0';
        if (!ucode_name_ok(name)) {
            rc = -EPROTO;
            goto out;
        }
        snprintf(path, sizeof(path), "%s/%s", fw_dir, name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            rc = -errno;
            PLOG("cannot open %s: %d", path, rc);
            goto out;
        }
        if (fstat(fd, &st) < 0 || st.st_size < 0) {
            rc = -errno ? -errno : -EIO;
            close(fd);
            goto out;
        }
        /* The driver names the offset; do not trust it past the mapping. */
        if (off > map_size || (size_t)st.st_size > map_size - off) {
            PLOG("%s (%lld bytes at offset %zu) does not fit the %zu byte ucode map", name,
                 (long long)st.st_size, off, map_size);
            close(fd);
            rc = -E2BIG;
            goto out;
        }
        while (done < (size_t)st.st_size) {
            ssize_t n = read(fd, mem + off + done, (size_t)st.st_size - done);

            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                rc = n < 0 ? -errno : -EIO;
                close(fd);
                goto out;
            }
            done += (size_t)n;
        }
        close(fd);
        PLOG("microcode %s: %lld bytes at offset 0x%zx", name, (long long)st.st_size, off);
    }

    if (ioctl(fd_ucode, IAV_IOC_UPDATE_UCODE, 0) < 0) {
        rc = -errno;
        goto out;
    }

    /* Only to log the version. */
    memset(&version, 0, sizeof(version));
    if (ioctl(fd_ucode, IAV_IOC_GET_UCODE_VERSION, &version) == 0)
        PLOG("ucode version %04x/%02x/%02x %02x:%02x:%02x (hash 0x%x)", version.year, version.month,
             version.day, version.hour, version.minute, version.second, version.edition_ver);
out:
    if (mem != MAP_FAILED)
        munmap(mem, map_size);
    if (fd_iav >= 0)
        close(fd_iav);
    if (fd_ucode >= 0)
        close(fd_ucode);
    return rc;
}

/* ---- step 7: idle ------------------------------------------------------ */

static int enter_idle(void)
{
    struct iav_idle_params idle;
    int fd = open("/dev/iav", O_RDWR | O_CLOEXEC);
    int rc = 0;

    if (fd < 0)
        return -errno;
    /* `test_encode --idle --nopreview` powers the VIN off and leaves the VOUT reset alone. */
    memset(&idle, 0, sizeof(idle));
    idle.poweroff_vin = 1;
    if (ioctl(fd, IAV_IOC_ENTER_IDLE, &idle) < 0)
        rc = -errno;
    close(fd);
    return rc;
}

/* ---- step 10: the sensor is active ------------------------------------- */

/*
 * The only ioctls of this function are the three read-only ones step 10
 * authorizes, from show_vsrc_info(): GET_GLOBAL_INFO, GET_DEVINFO and
 * GET_CHIP_STATUS. A vsrc whose name contains os08a10 and whose access_status
 * is 0 passes.
 */
static int virt_camera_step10_sensor_active(int fd_iav)
{
    struct vin_global_info info;
    unsigned int total;
    int found = -ENODEV;

    memset(&info, 0, sizeof(info));
    if (ioctl(fd_iav, IAV_IOC_VIN_GET_GLOBAL_INFO, &info) < 0)
        return -errno;

    total = info.total_vsrc_num;
    if (total > VSRC_SCAN_MAX)
        total = VSRC_SCAN_MAX;
    for (unsigned int i = 0; i < total; i++) {
        struct vindev_devinfo dev;
        struct vindev_chip_status chip;

        memset(&dev, 0, sizeof(dev));
        dev.vsrc_id = i;
        if (ioctl(fd_iav, IAV_IOC_VIN_GET_DEVINFO, &dev) < 0)
            return -errno;
        memset(&chip, 0, sizeof(chip));
        chip.vsrc_id = i;
        if (ioctl(fd_iav, IAV_IOC_VIN_GET_CHIP_STATUS, &chip) < 0)
            return -errno;
        dev.name[sizeof(dev.name) - 1] = '\0';
        PLOG("vsrc[%u] %s access_status=%d", i, dev.name, chip.access_status);
        if (strstr(dev.name, SENSOR_NAME) && chip.access_status == 0)
            found = 0;
    }
    return found;
}

static int step_sensor_check(void)
{
    int fd = open("/dev/iav", O_RDWR | O_CLOEXEC);
    int rc;

    if (fd < 0)
        return -errno;
    rc = virt_camera_step10_sensor_active(fd);
    close(fd);
    return rc;
}

/* ---- step 11: apply the resource and vout scripts ---------------------- */

static int step_apply(const struct virt_camera_conf *c)
{
    amba_virt_camera_apply_fn apply = NULL;
    void *lib = dlopen(VIRT_CAMERA_APPLY_LIB, RTLD_NOW | RTLD_LOCAL);
    int rc;

    if (!lib) {
        PLOG("dlopen %s failed: %s", VIRT_CAMERA_APPLY_LIB, dlerror());
        return -ENOENT;
    }
    *(void **)(&apply) = dlsym(lib, AMBA_VIRT_CAMERA_APPLY_SYMBOL);
    if (!apply) {
        PLOG("dlsym %s failed: %s", AMBA_VIRT_CAMERA_APPLY_SYMBOL, dlerror());
        dlclose(lib);
        return -ENOSYS;
    }
    rc = apply(c->resource_script, c->vout_script, c->app_img_profile);
    dlclose(lib);
    if (rc > 0)
        rc = -EIO;
    if (rc < 0)
        PLOG("amba_virt_camera_apply failed: %d", rc);
    return rc;
}

/* ---- the 3A and kernel checks after preview ---------------------------- */

static void kmsg_open_at_end(void)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd >= 0 && lseek(fd, 0, SEEK_END) < 0) {
        close(fd);
        fd = -1;
    }
    g_p.kmsg_fd = fd;
    if (fd < 0)
        PLOG("/dev/kmsg is not readable: the kernel message check will be skipped");
}

/* Returns 1 if the kernel logged a failure since kmsg_open_at_end(), else 0. */
static int kmsg_failure_since_start(void)
{
    char *text;
    size_t len = 0;
    int bad = 0;

    if (g_p.kmsg_fd < 0)
        return 0;
    text = malloc(AAA_LOG_CAP);
    if (!text)
        return 0;
    for (;;) {
        char rec[8192];
        ssize_t n = read(g_p.kmsg_fd, rec, sizeof(rec) - 1);

        if (n < 0 && (errno == EINTR || errno == EPIPE))
            continue;
        if (n <= 0)
            break;
        if (len + (size_t)n + 1 >= AAA_LOG_CAP)
            break;
        memcpy(text + len, rec, (size_t)n);
        len += (size_t)n;
    }
    text[len] = '\0';
    bad = virt_camera_kmsg_has_failure(text);
    if (bad)
        PLOG("the kernel reported a dropped 3A message");
    free(text);
    return bad;
}

static int step_check_3a(void)
{
    bool ok = false;
    char *copy;

    for (unsigned int waited = 0; waited <= AAA_HANDSHAKE_MS; waited += 200) {
        pthread_mutex_lock(&g_p.lock);
        ok = virt_camera_log_has_handshake(g_p.aaa_log);
        pthread_mutex_unlock(&g_p.lock);
        if (ok)
            break;
        if (g_p.aaa_pid > 0 && kill(g_p.aaa_pid, 0) < 0)
            break;
        msleep(200);
    }
    if (!ok) {
        PLOG("the 3A service never printed its handshake");
        return -ETIMEDOUT;
    }

    pthread_mutex_lock(&g_p.lock);
    copy = g_p.aaa_log ? strdup(g_p.aaa_log) : NULL;
    pthread_mutex_unlock(&g_p.lock);
    if (copy && virt_camera_log_fell_back(copy)) {
        PLOG("the 3A service fell back to the default ADJ or AEB tables");
        free(copy);
        return -ENOENT;
    }
    free(copy);

    if (kmsg_failure_since_start())
        return -EIO;
    PLOG("3A handshake complete");
    return 0;
}

/* ---- run and shutdown -------------------------------------------------- */

void virt_camera_pipeline_set_encode_stop(virt_camera_encode_stop_fn fn)
{
    g_p.encode_stop = fn;
}

static int collect_late_names(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[1024];

    g_p.late_count = 0;
    if (!fp)
        return -errno;
    while (fgets(line, sizeof(line), fp)) {
        char name[64], params[256];
        int r = virt_module_loader_parse_line(line, name, sizeof(name), params, sizeof(params));

        if (r < 0) {
            fclose(fp);
            return -EINVAL;
        }
        if (r == 0)
            continue;
        if (g_p.late_count >= MAX_LATE_MODULES) {
            fclose(fp);
            return -E2BIG;
        }
        snprintf(g_p.late_names[g_p.late_count++], sizeof(g_p.late_names[0]), "%s", name);
    }
    fclose(fp);
    return 0;
}

static int start_monitor(const struct virt_camera_conf *c)
{
    pid_t pid = spawn_cooper(c->monitor_bin, NULL, environ, NULL);

    if (pid < 0)
        return -errno;
    g_p.monitor_pid = pid;
    PLOG("dsp_monitor_service started (pid %d)", (int)pid);
    if (!child_alive_after(&g_p.monitor_pid, MONITOR_ALIVE_MS)) {
        PLOG("dsp_monitor_service did not stay up");
        return -ECHILD;
    }
    return 0;
}

static int start_aaa(const struct virt_camera_conf *c)
{
    char slave[128];
    char **env = env_with_profile(c->app_img_profile);
    int master;
    pid_t pid;

    if (!env)
        return -ENOMEM;
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
        ptsname_r(master, slave, sizeof(slave)) != 0) {
        int err = errno;

        if (master >= 0)
            close(master);
        free_env(env);
        return -err;
    }

    g_p.aaa_log = calloc(1, AAA_LOG_CAP);
    if (!g_p.aaa_log) {
        close(master);
        free_env(env);
        return -ENOMEM;
    }
    g_p.aaa_master = master;
    pid = spawn_cooper(c->aaa_bin, "-a", env, slave);
    free_env(env);
    if (pid < 0) {
        int err = errno;

        close(master);
        g_p.aaa_master = -1;
        return -err;
    }
    g_p.aaa_pid = pid;
    if (pthread_create(&g_p.aaa_reader, NULL, aaa_reader_thread, NULL) == 0)
        g_p.aaa_reader_started = true;
    PLOG("amba-virt-aaa started (pid %d, APP_IMG_PROFILE=%d)", (int)pid, c->app_img_profile);
    if (!child_alive_after(&g_p.aaa_pid, AAA_ALIVE_MS)) {
        PLOG("amba-virt-aaa did not stay up");
        return -ECHILD;
    }
    return 0;
}

#define STEP(num, text, call)                                                                    \
    do {                                                                                         \
        PLOG("step %d: %s", (num), (text));                                                      \
        rc = (call);                                                                             \
        if (rc < 0) {                                                                            \
            PLOG("step %d failed: %s (%d)", (num), (text), rc);                                  \
            return rc;                                                                           \
        }                                                                                        \
    } while (0)

static int require_readable(const char *path)
{
    return access(path, R_OK) == 0 ? 0 : -errno;
}

static int step_early_modules(const struct virt_camera_conf *c)
{
    int rc = require_readable(c->early_modules);

    return rc < 0 ? rc : virt_module_loader_run(c->early_modules);
}

static int step_power(const struct virt_camera_conf *c)
{
    int rc = virt_gpio_acquire(&g_p.gpio, c->poc_chip, c->poc_lines, c->poc_line_count, 1);

    if (rc == 0)
        msleep(c->poc_settle_ms);
    return rc;
}

static int step_late_modules(const struct virt_camera_conf *c)
{
    int rc = require_readable(c->late_modules);

    /* From here on the shutdown has late modules to unload. */
    g_p.late_attempted = true;
    if (rc == 0)
        rc = collect_late_names(c->late_modules);
    if (rc == 0)
        rc = virt_module_loader_run(c->late_modules);
    if (rc == 0)
        rc = step_sensor_check();
    return rc;
}

int virt_camera_pipeline_run(const struct virt_camera_conf *conf)
{
    int rc;

    if (!conf)
        return -EINVAL;
    g_p.conf = *conf;
    kmsg_open_at_end();

    STEP(2, "select firmware", step_firmware(&g_p.conf));
    STEP(3, "load the early modules", step_early_modules(&g_p.conf));
    STEP(4, "create the device nodes", step_device_nodes());
    STEP(5, "start dsp_monitor_service", start_monitor(&g_p.conf));
    STEP(6, "load the microcode", load_microcode(g_p.conf.firmware_dir));
    STEP(7, "enter idle", enter_idle());
    STEP(8, "start amba-virt-aaa", start_aaa(&g_p.conf));
    STEP(9, "power the camera and wait for it to settle", step_power(&g_p.conf));
    STEP(10, "load the late modules and confirm the sensor", step_late_modules(&g_p.conf));
    STEP(11, "apply the resource and vout scripts", step_apply(&g_p.conf));
    PLOG("checking the 3A service after preview");
    rc = step_check_3a();
    if (rc < 0) {
        PLOG("step 11 failed: the 3A checks after preview (%d)", rc);
        return rc;
    }
    return 0;
}

void virt_camera_pipeline_shutdown(void)
{
    pthread_mutex_lock(&g_p.lock);
    if (g_p.shutting_down) {
        pthread_mutex_unlock(&g_p.lock);
        return;
    }
    g_p.shutting_down = true;
    pthread_mutex_unlock(&g_p.lock);

    PLOG("shutdown: stopping encode and the IAV tap");
    if (g_p.encode_stop)
        g_p.encode_stop();

    /*
     * Leave preview while the sensor and the 3A service are still up. Once the
     * sensor modules are gone, goto_idle from preview times out waiting for VIN
     * idle and leaves IAV in EXITING_PREVIEW until the next boot.
     */
    if (g_p.late_attempted) {
        int rc = enter_idle();

        PLOG("shutdown: enter idle: %d", rc);
    }

    PLOG("shutdown: stopping the 3A service");
    stop_child(&g_p.aaa_pid, "amba-virt-aaa");

    if (g_p.late_attempted) {
        PLOG("shutdown: unloading the late modules");
        for (size_t i = g_p.late_count; i > 0; i--) {
            int rc = virt_host_module_unload(g_p.late_names[i - 1]);

            PLOG("unload %s: %d", g_p.late_names[i - 1], rc);
        }
    }

    if (g_p.gpio.line_fd >= 0) {
        int rc = virt_gpio_set(&g_p.gpio, 0);

        PLOG("shutdown: camera power lines low: %d", rc);
    }

    PLOG("shutdown: stopping dsp_monitor_service");
    stop_child(&g_p.monitor_pid, "dsp_monitor_service");

    if (g_p.aaa_master >= 0) {
        close(g_p.aaa_master);
        g_p.aaa_master = -1;
    }
    if (g_p.aaa_reader_started) {
        pthread_join(g_p.aaa_reader, NULL);
        g_p.aaa_reader_started = false;
    }
    g_p.shutdown_done = true;
}

static void on_signal(int sig)
{
    g_p.signaled = sig;
}

static void *supervisor_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int status;
        bool dead = false;
        const char *who = NULL;

        msleep(100);
        if (g_p.signaled) {
            PLOG("signal %d: shutting the pipeline down", (int)g_p.signaled);
            virt_camera_pipeline_shutdown();
            exit(0);
        }
        if (g_p.monitor_pid > 0 && waitpid(g_p.monitor_pid, &status, WNOHANG) == g_p.monitor_pid) {
            who = "dsp_monitor_service";
            g_p.monitor_pid = -1;
            dead = true;
        } else if (g_p.aaa_pid > 0 && waitpid(g_p.aaa_pid, &status, WNOHANG) == g_p.aaa_pid) {
            who = "amba-virt-aaa";
            g_p.aaa_pid = -1;
            dead = true;
        }
        if (dead) {
            PLOG("%s exited: the pipeline has failed, shutting down", who);
            virt_camera_pipeline_shutdown();
            exit(1);
        }
    }
    return NULL;
}

int virt_camera_pipeline_supervise(void)
{
    struct sigaction sa;
    pthread_t tid;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (pthread_create(&tid, NULL, supervisor_thread, NULL) != 0)
        return -errno;
    pthread_detach(tid);
    return 0;
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
