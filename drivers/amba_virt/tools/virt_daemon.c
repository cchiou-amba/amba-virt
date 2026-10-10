/*
 * virt_daemon.c
 *
 * Daemon mode for amba-virt-server. See virt_daemon.h.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "virt_daemon.h"
#include "virt_ready.h"

#define STOP_GRACE_MS 5000u

enum ready_state { READY_NONE, READY_OK, READY_TIMEOUT, READY_FAILED };

static volatile sig_atomic_t g_sig_term;
static volatile sig_atomic_t g_sig_hold;
static volatile sig_atomic_t g_sig_resume;
static int g_self_pipe[2] = { -1, -1 };

static void on_signal(int sig)
{
    int saved = errno;
    char b = 'x';

    if (sig == SIGTERM || sig == SIGINT)
        g_sig_term = 1;
    else if (sig == SIGUSR1)
        g_sig_hold = 1;
    else if (sig == SIGUSR2)
        g_sig_resume = 1;
    if (g_self_pipe[1] >= 0 && write(g_self_pipe[1], &b, 1) < 0) {
        /* The pipe is full: a wake-up is already pending. */
    }
    errno = saved;
}

static unsigned long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static void dlog(const struct virt_daemon_cfg *cfg, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    syslog(LOG_INFO, "%s", msg);
    if (cfg->diag_path) {
        int fd = open(cfg->diag_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

        if (fd >= 0) {
            char line[300];
            int n = snprintf(line, sizeof(line), "[%llu] %s\n", now_ms(), msg);

            if (n > 0 && write(fd, line, (size_t)n) < 0) {
                /* Diagnostics are best effort. */
            }
            close(fd);
        }
    }
}

static pid_t start_worker(const struct virt_daemon_cfg *cfg, int *ready_rd)
{
    int pfd[2];
    pid_t pid;

    if (pipe(pfd) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int rc;

        close(pfd[0]);
        close(g_self_pipe[0]);
        close(g_self_pipe[1]);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGUSR1, SIG_DFL);
        signal(SIGUSR2, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        rc = cfg->worker(pfd[1], cfg->ctx);
        _exit(rc & 0xff);
    }
    close(pfd[1]);
    *ready_rd = pfd[0];
    return pid;
}

/* SIGTERM, then SIGKILL after the grace period, and wait for the worker. */
static void stop_worker(const struct virt_daemon_cfg *cfg, pid_t worker)
{
    unsigned int waited = 0;
    int status;

    if (worker <= 0)
        return;
    kill(worker, SIGTERM);
    while (waited < STOP_GRACE_MS) {
        struct timespec step = { .tv_sec = 0, .tv_nsec = 20 * 1000000L };

        if (waitpid(worker, &status, WNOHANG) == worker) {
            dlog(cfg, "worker %d stopped", (int)worker);
            return;
        }
        nanosleep(&step, NULL);
        waited += 20;
    }
    kill(worker, SIGKILL);
    waitpid(worker, &status, 0);
    dlog(cfg, "worker %d killed after %u ms", (int)worker, STOP_GRACE_MS);
}

static void report_result(int result_fd, enum ready_state ready)
{
    char c;

    if (ready == READY_OK) {
        c = 'R';
    } else if (ready == READY_TIMEOUT) {
        c = 'T';
    } else {
        c = 'F';
    }
    if (result_fd >= 0 && write(result_fd, &c, 1) < 0) {
        /* The original process is gone; nothing to tell. */
    }
}

static int daemon_main(const struct virt_daemon_cfg *cfg, int result_fd)
{
    struct sigaction sa;
    pid_t worker = 0;
    int ready_rd = -1;
    enum ready_state ready = READY_NONE;
    bool reported = false;
    bool held = false;
    bool start_now = true;
    unsigned int delay = cfg->restart_min_ms;
    unsigned long long restart_at = 0;
    bool restart_pending = false;
    unsigned long long deadline = now_ms() + cfg->ready_timeout_ms;

    openlog("amba-virt-server", LOG_PID, LOG_DAEMON);
    setsid();
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    if (pipe(g_self_pipe) < 0)
        return 1;
    fcntl(g_self_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_self_pipe[1], F_SETFL, O_NONBLOCK);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sa.sa_handler = on_signal;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (cfg->pid_path) {
        FILE *fp = fopen(cfg->pid_path, "w");

        if (fp) {
            fprintf(fp, "%d\n", (int)getpid());
            fclose(fp);
        }
    }

    for (;;) {
        struct pollfd fds[2];
        int nfds = 0, timeout = -1;
        unsigned long long now = now_ms();
        int status;
        pid_t pid;

        if (start_now && worker == 0 && !held) {
            worker = start_worker(cfg, &ready_rd);
            start_now = false;
            restart_pending = false;
            if (worker < 0) {
                dlog(cfg, "fork of the worker failed: %s", strerror(errno));
                worker = 0;
                if (!reported) {
                    reported = true;
                    report_result(result_fd, READY_FAILED);
                }
                return 1;
            }
            dlog(cfg, "worker started pid=%d", (int)worker);
        }

        if (!reported) {
            timeout = deadline > now ? (int)(deadline - now) : 0;
        }
        if (restart_pending) {
            int t = restart_at > now ? (int)(restart_at - now) : 0;

            if (timeout < 0 || t < timeout)
                timeout = t;
        }
        fds[nfds].fd = g_self_pipe[0];
        fds[nfds].events = POLLIN;
        nfds++;
        if (ready_rd >= 0) {
            fds[nfds].fd = ready_rd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        if (poll(fds, (nfds_t)nfds, timeout) < 0 && errno != EINTR)
            return 1;

        if (fds[0].revents & POLLIN) {
            char drain[64];

            while (read(g_self_pipe[0], drain, sizeof(drain)) > 0) {
            }
        }
        if (nfds > 1 && (fds[1].revents & (POLLIN | POLLHUP))) {
            char b;
            ssize_t n = read(ready_rd, &b, 1);

            if (n == 1 && b == VIRT_READY_BYTE) {
                ready = READY_OK;
                delay = cfg->restart_min_ms;
                dlog(cfg, "worker %d is ready", (int)worker);
                if (!reported) {
                    reported = true;
                    report_result(result_fd, ready);
                }
            }
            if (n <= 0 || ready == READY_OK) {
                close(ready_rd);
                ready_rd = -1;
            }
        }
        now = now_ms();
        if (!reported && now >= deadline) {
            ready = READY_TIMEOUT;
            dlog(cfg, "worker not ready after %u ms; boot goes on and the daemon keeps supervising",
                 cfg->ready_timeout_ms);
            reported = true;
            report_result(result_fd, ready);
        }

        if (g_sig_term) {
            dlog(cfg, "stopping on a signal");
            stop_worker(cfg, worker);
            while (waitpid(-1, &status, WNOHANG) > 0) {
            }
            if (cfg->pid_path)
                unlink(cfg->pid_path);
            return 0;
        }
        if (g_sig_hold) {
            g_sig_hold = 0;
            held = true;
            restart_pending = false;
            dlog(cfg, "holding: the worker stays stopped");
            stop_worker(cfg, worker);
            worker = 0;
            if (ready_rd >= 0) {
                close(ready_rd);
                ready_rd = -1;
            }
        }
        if (g_sig_resume) {
            g_sig_resume = 0;
            if (held) {
                held = false;
                start_now = true;
                dlog(cfg, "resuming");
            }
        }

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (pid != worker)
                continue;
            if (WIFEXITED(status))
                dlog(cfg, "worker %d exited status=%d", (int)pid, WEXITSTATUS(status));
            else
                dlog(cfg, "worker %d killed by signal %d", (int)pid, WTERMSIG(status));
            worker = 0;
            if (ready_rd >= 0) {
                close(ready_rd);
                ready_rd = -1;
            }
            if (!reported) {
                reported = true;
                report_result(result_fd, ready == READY_OK ? READY_OK : READY_FAILED);
            }
            if (cfg->restart && !held) {
                restart_at = now_ms() + delay;
                restart_pending = true;
                dlog(cfg, "restarting the worker in %u ms", delay);
                delay = delay * 2 > cfg->restart_max_ms ? cfg->restart_max_ms : delay * 2;
            }
        }
        if (restart_pending && worker == 0 && !held && now_ms() >= restart_at)
            start_now = true;
        if (!cfg->restart && worker == 0 && !held && !start_now)
            return 0;
    }
}

int virt_daemon_start(const struct virt_daemon_cfg *cfg, pid_t *daemon_pid)
{
    int rp[2];
    pid_t d;
    struct pollfd pfd;
    char c = 0;
    ssize_t n;

    if (!cfg || !cfg->worker)
        return -EINVAL;
    if (pipe(rp) < 0)
        return -errno;
    d = fork();
    if (d < 0) {
        int err = errno;

        close(rp[0]);
        close(rp[1]);
        return -err;
    }
    if (d == 0) {
        int rc;

        close(rp[0]);
        rc = daemon_main(cfg, rp[1]);
        _exit(rc);
    }
    close(rp[1]);
    if (daemon_pid)
        *daemon_pid = d;
    pfd.fd = rp[0];
    pfd.events = POLLIN;
    /* The daemon reports within ready_timeout_ms; the extra time only guards a hung daemon. */
    if (poll(&pfd, 1, (int)(cfg->ready_timeout_ms + 5000u)) <= 0) {
        close(rp[0]);
        return -ETIMEDOUT;
    }
    n = read(rp[0], &c, 1);
    close(rp[0]);
    if (n == 1 && c == 'R')
        return 0;
    if (n == 1 && c == 'T')
        return -ETIMEDOUT;
    return -EIO;
}

int virt_daemon_run_foreground(const struct virt_daemon_cfg *cfg)
{
    if (!cfg || !cfg->worker)
        return -EINVAL;
    return cfg->worker(-1, cfg->ctx);
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
