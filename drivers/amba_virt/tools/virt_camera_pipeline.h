/*
 * virt_camera_pipeline.h
 *
 * The hands-off camera pipeline of amba-virt-server (Section 2.2 steps 2
 * through 11 and the Section 2.7 shutdown).
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_CAMERA_PIPELINE_H_
#define _VIRT_CAMERA_PIPELINE_H_

#include <stddef.h>

#include "virt_camera_conf.h"

/* The Cooper children are started through this loader and library directory. */
#define VIRT_CAMERA_LIB_DIR   "/usr/lib/amba-virt/lib"
#define VIRT_CAMERA_LOADER    VIRT_CAMERA_LIB_DIR "/ld-linux-aarch64.so.1"
#define VIRT_CAMERA_APPLY_LIB VIRT_CAMERA_LIB_DIR "/libamba-virt-camera.so"

typedef void (*virt_camera_encode_stop_fn)(void);

/* Called by the shutdown to stop encode and the IAV tap. Optional. */
void virt_camera_pipeline_set_encode_stop(virt_camera_encode_stop_fn fn);

/*
 * Runs steps 2 through 11 for conf. Returns 0, or a negative errno from the
 * first step that failed. The caller runs virt_camera_pipeline_shutdown()
 * after a failure.
 */
int virt_camera_pipeline_run(const struct virt_camera_conf *conf);

/*
 * Starts the supervisor: a child that exits fails the pipeline, and SIGTERM or
 * SIGINT ends it. Either way the shutdown runs and the process exits, with
 * status 1 for a failed child and 0 for a signal.
 */
int virt_camera_pipeline_supervise(void);

/* Section 2.7, in reverse of the startup. Safe to call more than once. */
void virt_camera_pipeline_shutdown(void);

/*
 * Text checks, exposed so they can be tested on the host. Each takes a
 * NUL-terminated string and returns 1 or 0.
 */
/* All of "AAA prepare done", "ADJ parameter version" and "AEB parameter version". */
int virt_camera_log_has_handshake(const char *log);
/* A line that cannot find the os08a10 ADJ or AEB tuning file. */
int virt_camera_log_fell_back(const char *log);
/* "no active user" or "empty ISO cfg" in kernel messages. */
int virt_camera_kmsg_has_failure(const char *text);
/* Writes path relative to root into out. Returns 0, or -EINVAL if path is not under root. */
int virt_camera_fw_rel_path(const char *root, const char *path, char *out, size_t cap);

#endif /* _VIRT_CAMERA_PIPELINE_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
