/*
 * IavQualServer.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Holds and resumes the production amba-virt-server worker around a qualification run. This is
 * the documented development loop: SIGUSR1 to the supervisor stops the worker and keeps it
 * stopped, SIGUSR2 starts it again.
 */

#include "iav_qual_server.h"

#include "IavQualConfig.hxx"
#include "IavFrameHash.hxx"
#include "IavQualMain.hxx"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <iav_ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

const char *PidPath = "/run/amba-virt/server.pid";
const char *DaemonLogPath = "/run/amba-virt/server-daemon.log";
const char *HoldMarker = "holding: the worker stays stopped";
const int HoldTimeoutMs = 60000;
const int SettleMs = 15000;
const int SettleTimeoutMs = 120000;

/* Supervisor pid of the production server, or 0 when none is running. */
pid_t g_supervisor = 0;
/* The write end of the sentinel's pipe, or -1. */
int g_sentinelFd = -1;
pid_t g_sentinel = 0;
bool g_held = false;

pid_t readSupervisorPid()
{
    FILE *f = fopen(PidPath, "r");
    long pid = 0;

    if (f == NULL) {
        return 0;
    }
    if (fscanf(f, "%ld", &pid) != 1) {
        pid = 0;
    }
    fclose(f);
    if (pid <= 0 || kill((pid_t)pid, 0) != 0) {
        return 0;
    }
    return (pid_t)pid;
}

off_t fileSize(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    return st.st_size;
}

/* True when text appears in the file after offset from. */
bool logHasMarkerAfter(const char *path, off_t from, const char *text)
{
    FILE *f = fopen(path, "r");
    char line[512];
    bool found = false;

    if (f == NULL) {
        return false;
    }
    if (fseeko(f, from, SEEK_SET) == 0) {
        while (fgets(line, sizeof(line), f) != NULL) {
            if (strstr(line, text) != NULL) {
                found = true;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

void sleepMs(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/*
 * Milliseconds since boot, the clock the supervisor stamps its log with, or -1.
 */
long long uptimeMs()
{
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0;

    if (f == NULL) {
        return -1;
    }
    if (fscanf(f, "%lf", &up) != 1) {
        up = -1;
    }
    fclose(f);
    return up < 0 ? -1 : (long long)(up * 1000.0);
}

/*
 * True when the worker's last log line says it is ready and that line is at least SettleMs old.
 * Holding a worker that is still bringing the camera up leaves the camera services' sessions
 * unfinished, and the next bring-up then times out and can wedge the IAV state machine.
 */
bool workerSettled()
{
    FILE *f = fopen(DaemonLogPath, "r");
    char line[512];
    char last[512] = "";
    long long stamp = 0, now;

    if (f == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '[') {
            memcpy(last, line, sizeof(last));
        }
    }
    fclose(f);
    if (strstr(last, " is ready") == NULL || sscanf(last, "[%lld]", &stamp) != 1) {
        return false;
    }
    now = uptimeMs();
    return now >= 0 && now - stamp >= SettleMs;
}

/*
 * The sentinel waits on a pipe. A byte means the parent resumed the worker itself. End of file
 * means the parent died first, so the sentinel resumes the worker.
 */
void sentinelMain(int readFd, pid_t supervisor)
{
    char c = 0;
    ssize_t n;

    do {
        n = read(readFd, &c, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
        kill(supervisor, SIGUSR2);
        fprintf(stderr, "iav-qual: sentinel resumed the production worker\n");
    }
    _exit(0);
}

} // namespace

namespace {

void *witnessMain(void *)
{
    int fd = -1;
    struct iav_querymem qm;
    unsigned long long length = 0;
    const volatile unsigned char *map = NULL;
    unsigned long long last = 0;
    bool haveLast = false;

    for (int i = 0; i < 600 && fd < 0; i++) {
        fd = open("/dev/iav", O_RDWR);
        if (fd < 0) {
            sleepMs(500);
        }
    }
    if (fd < 0) {
        fprintf(stderr, "witness: /dev/iav never appeared\n");
        return NULL;
    }
    memset(&qm, 0, sizeof(qm));
    qm.mid = IAV_MEM_PARTITION;
    qm.arg.partition.pid = IAV_PART_DSP;
    if (ioctl(fd, IAV_IOC_QUERY_MEMBLOCK, &qm) != 0) {
        fprintf(stderr, "witness: cannot query the DSP partition: %s\n", strerror(errno));
        return NULL;
    }
    length = qm.arg.partition.mem.length;
    map = (const volatile unsigned char *)mmap(NULL, (size_t)length, PROT_READ, MAP_SHARED, fd,
                                                (off_t)qm.arg.partition.mem.addr);
    if (map == (const volatile unsigned char *)MAP_FAILED) {
        fprintf(stderr, "witness: cannot map the DSP partition: %s\n", strerror(errno));
        return NULL;
    }
    printf("witness: started\n");
    for (;;) {
        struct iav_querydesc q;
        size_t yBytes;

        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_CANVAS;
        q.arg.canvas.canvas_id = 0;
        /* blocking: every frame, as the guest's blocking reads see them (the driver bounds the wait) */
        q.arg.canvas.non_block_flag = 0;
        if (ioctl(fd, IAV_IOC_QUERY_DESC, &q) == 0 && (!haveLast || q.arg.canvas.yuv.mono_pts != last)) {
            yBytes = (size_t)q.arg.canvas.yuv.pitch * q.arg.canvas.yuv.height;
            if (yBytes > 0 && q.arg.canvas.yuv.y_addr_offset + yBytes <= length) {
                printf("witness: pts=%llu hash=%lld\n", (unsigned long long)q.arg.canvas.yuv.mono_pts,
                       (long long)iavFrameHash(map + q.arg.canvas.yuv.y_addr_offset, yBytes));
                last = q.arg.canvas.yuv.mono_pts;
                haveLast = true;
            }
        } else if (errno != EAGAIN) {
            sleepMs(50);
        }
    }
    return NULL;
}

} // namespace

extern "C" void iav_qual_server_witness_start(void)
{
    pthread_t t;

    if (pthread_create(&t, NULL, witnessMain, NULL) == 0) {
        pthread_detach(t);
    }
}

extern "C" int iav_qual_server_precheck(int argc, char **argv)
{
    return iavQualPrecheck(argc, argv, 2);
}

extern "C" int iav_qual_server_hold(void)
{
    int fds[2];
    off_t logOffset;
    int waited;

    g_supervisor = readSupervisorPid();
    if (g_supervisor == 0) {
        fprintf(stderr, "iav-qual: no production server is running, nothing to hold\n");
        return 0;
    }

    for (waited = 0; !workerSettled(); waited += 500) {
        if (waited >= SettleTimeoutMs) {
            fprintf(stderr, "iav-qual: the production worker is not ready and settled after %d ms; "
                    "not touching the pipeline\n", SettleTimeoutMs);
            return IavQualExitEnvironment;
        }
        sleepMs(500);
    }

    if (pipe(fds) != 0) {
        fprintf(stderr, "iav-qual: pipe: %s\n", strerror(errno));
        return IavQualExitEnvironment;
    }
    g_sentinel = fork();
    if (g_sentinel < 0) {
        fprintf(stderr, "iav-qual: fork: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return IavQualExitEnvironment;
    }
    if (g_sentinel == 0) {
        close(fds[1]);
        sentinelMain(fds[0], g_supervisor);
    }
    close(fds[0]);
    g_sentinelFd = fds[1];

    logOffset = fileSize(DaemonLogPath);
    if (kill(g_supervisor, SIGUSR1) != 0) {
        fprintf(stderr, "iav-qual: cannot signal the supervisor %d: %s\n", (int)g_supervisor,
                strerror(errno));
        return IavQualExitEnvironment;
    }
    g_held = true;
    for (waited = 0; waited < HoldTimeoutMs; waited += 200) {
        if (logHasMarkerAfter(DaemonLogPath, logOffset, HoldMarker)) {
            printf("iav-qual: production worker held (supervisor %d)\n", (int)g_supervisor);
            return 0;
        }
        sleepMs(200);
    }
    fprintf(stderr, "iav-qual: the production worker did not report holding within %d ms\n",
            HoldTimeoutMs);
    return IavQualExitEnvironment;
}

extern "C" void iav_qual_server_resume(void)
{
    if (!g_held) {
        return;
    }
    g_held = false;
    if (g_supervisor != 0 && kill(g_supervisor, SIGUSR2) == 0) {
        printf("iav-qual: production worker resumed (supervisor %d)\n", (int)g_supervisor);
    }
    if (g_sentinelFd >= 0) {
        /* Tell the sentinel the resume is done, so it exits without signaling again. */
        if (write(g_sentinelFd, "R", 1) < 0) {
            fprintf(stderr, "iav-qual: sentinel notify: %s\n", strerror(errno));
        }
        close(g_sentinelFd);
        g_sentinelFd = -1;
    }
    if (g_sentinel > 0) {
        waitpid(g_sentinel, NULL, 0);
        g_sentinel = 0;
    }
}

extern "C" int iav_qual_server_run(int argc, char **argv)
{
    return iavQualMain(argc, argv, 2, NULL);
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
