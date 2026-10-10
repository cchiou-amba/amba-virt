/*
 * virt_camera_conf.h
 *
 * Parser for the live camera config, /etc/amba-virt/camera.conf.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_CAMERA_CONF_H_
#define _VIRT_CAMERA_CONF_H_

#include <stddef.h>

#define CAMERA_CONF_DEFAULT_PATH "/etc/amba-virt/camera.conf"

#define VIRT_CAMERA_PATH_MAX            256
#define VIRT_CAMERA_MAX_POC_LINES       16
#define VIRT_CAMERA_APP_IMG_PROFILE_MAX 10
#define VIRT_CAMERA_SETTLE_MS_MAX       60000

struct virt_camera_conf {
    char firmware_dir[VIRT_CAMERA_PATH_MAX];
    char resource_script[VIRT_CAMERA_PATH_MAX];
    char vout_script[VIRT_CAMERA_PATH_MAX];
    int app_img_profile;
    char poc_chip[VIRT_CAMERA_PATH_MAX];
    unsigned int poc_lines[VIRT_CAMERA_MAX_POC_LINES];
    size_t poc_line_count;
    unsigned int poc_settle_ms;
    char early_modules[VIRT_CAMERA_PATH_MAX];
    char late_modules[VIRT_CAMERA_PATH_MAX];
    char aaa_bin[VIRT_CAMERA_PATH_MAX];
    char monitor_bin[VIRT_CAMERA_PATH_MAX];
};

/*
 * Parses one line of the live config into conf. *seen accumulates the keys
 * already set, so a repeated key is rejected.
 * Returns:
 *   1 if a key was set
 *   0 if the line was blank or a comment
 *  <0 (-EINVAL) if the key is unknown or repeated or the value is invalid
 */
int virt_camera_conf_parse_line(struct virt_camera_conf *conf, unsigned int *seen,
                                const char *line);

/*
 * Reads path into conf. Every key of Section 2.3 is required.
 * Returns 0 on success, -errno if the file cannot be read, -EINVAL if any
 * line is invalid or a key is missing.
 */
int virt_camera_conf_load(const char *path, struct virt_camera_conf *conf);

#endif /* _VIRT_CAMERA_CONF_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
