/*
 * virt_daemon.h
 *
 * Daemon mode for amba-virt-server, the init entry on EVE Dom0. The process
 * init runs forks a daemon, the daemon forks the worker (the server), and the
 * original process waits for the worker to report ready within a bounded time,
 * then returns so init can go on. The daemon stays behind: it restarts the
 * worker when it dies, reaps what the worker leaves, and can hold the worker
 * stopped so a development copy can run.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_DAEMON_H_
#define _VIRT_DAEMON_H_

#include <stdbool.h>
#include <sys/types.h>

struct virt_daemon_cfg {
    /*
     * The server. It runs in the forked worker with the write end of the readiness pipe
     * (virt_ready_notify() takes it) and returns the worker's exit status.
     */
    int (*worker)(int ready_fd, void *ctx);
    void *ctx;
    unsigned int ready_timeout_ms;   /* the bounded wait before init proceeds */
    unsigned int restart_min_ms;     /* first restart delay */
    unsigned int restart_max_ms;     /* the delay doubles up to this */
    bool restart;                    /* restart the worker when it dies */
    const char *diag_path;           /* diagnostics are appended here, and go to syslog */
    const char *pid_path;            /* the daemon's pid, for amba-virt-ctl; may be NULL */
};

/*
 * Forks the daemon and waits for its result. Returns 0 when the worker reported ready,
 * -ETIMEDOUT when it did not within the bound (the daemon keeps supervising), -EIO when the
 * worker exited before it was ready or the daemon failed. *daemon_pid gets the daemon's pid.
 */
int virt_daemon_start(const struct virt_daemon_cfg *cfg, pid_t *daemon_pid);

/* Foreground mode: runs the worker in this process with no readiness fd. */
int virt_daemon_run_foreground(const struct virt_daemon_cfg *cfg);

/* Signals the daemon understands: SIGTERM stops, SIGUSR1 holds the worker stopped, SIGUSR2 resumes. */

#endif /* _VIRT_DAEMON_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
