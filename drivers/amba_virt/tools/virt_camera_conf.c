/*
 * virt_camera_conf.c
 *
 * Parser for the live camera config, /etc/amba-virt/camera.conf.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virt_camera_conf.h"

enum conf_key {
    KEY_FIRMWARE_DIR,
    KEY_RESOURCE_SCRIPT,
    KEY_VOUT_SCRIPT,
    KEY_APP_IMG_PROFILE,
    KEY_POC_CHIP,
    KEY_POC_LINES,
    KEY_POC_SETTLE_MS,
    KEY_EARLY_MODULES,
    KEY_LATE_MODULES,
    KEY_AAA_BIN,
    KEY_MONITOR_BIN,
    KEY_COUNT
};

static const char *const g_key_names[KEY_COUNT] = {
    [KEY_FIRMWARE_DIR] = "firmware_dir",
    [KEY_RESOURCE_SCRIPT] = "resource_script",
    [KEY_VOUT_SCRIPT] = "vout_script",
    [KEY_APP_IMG_PROFILE] = "app_img_profile",
    [KEY_POC_CHIP] = "poc_chip",
    [KEY_POC_LINES] = "poc_lines",
    [KEY_POC_SETTLE_MS] = "poc_settle_ms",
    [KEY_EARLY_MODULES] = "early_modules",
    [KEY_LATE_MODULES] = "late_modules",
    [KEY_AAA_BIN] = "aaa_bin",
    [KEY_MONITOR_BIN] = "monitor_bin",
};

#define ALL_KEYS_MASK ((1u << KEY_COUNT) - 1u)

/* An absolute path with no control character and no ".." component. */
static bool path_value_ok(const char *v)
{
    size_t len = strlen(v);
    const char *comp = v;

    if (len == 0 || len >= VIRT_CAMERA_PATH_MAX || v[0] != '/')
        return false;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)v[i] < 0x20 || v[i] == 0x7f)
            return false;
    }
    for (;;) {
        const char *end = strchr(comp, '/');
        size_t clen = end ? (size_t)(end - comp) : strlen(comp);

        if (clen == 2 && comp[0] == '.' && comp[1] == '.')
            return false;
        if (!end)
            break;
        comp = end + 1;
    }
    return true;
}

/* Strict unsigned decimal, no sign, no spaces, at most max. */
static bool uint_value_ok(const char *v, unsigned long max, unsigned long *out)
{
    char *end = NULL;
    unsigned long n;

    if (*v == '\0')
        return false;
    for (const char *p = v; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return false;
    }
    errno = 0;
    n = strtoul(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0' || n > max)
        return false;
    *out = n;
    return true;
}

static int set_path(char *dst, const char *v)
{
    if (!path_value_ok(v))
        return -EINVAL;
    snprintf(dst, VIRT_CAMERA_PATH_MAX, "%.*s", VIRT_CAMERA_PATH_MAX - 1, v);
    return 0;
}

static int set_poc_lines(struct virt_camera_conf *conf, const char *v)
{
    unsigned int lines[VIRT_CAMERA_MAX_POC_LINES];
    size_t count = 0;
    const char *p = v;

    while (*p) {
        char tok[16];
        size_t n = 0;
        unsigned long line;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        while (*p && *p != ' ' && *p != '\t') {
            if (n + 1 >= sizeof(tok))
                return -EINVAL;
            tok[n++] = *p++;
        }
        tok[n] = '\0';
        if (!uint_value_ok(tok, 65535, &line) || count >= VIRT_CAMERA_MAX_POC_LINES)
            return -EINVAL;
        for (size_t i = 0; i < count; i++) {
            if (lines[i] == (unsigned int)line)
                return -EINVAL;
        }
        lines[count++] = (unsigned int)line;
    }
    if (count == 0)
        return -EINVAL;

    memcpy(conf->poc_lines, lines, count * sizeof(lines[0]));
    conf->poc_line_count = count;
    return 0;
}

int virt_camera_conf_parse_line(struct virt_camera_conf *conf, unsigned int *seen,
                                const char *line)
{
    char key[32];
    char value[VIRT_CAMERA_PATH_MAX + 64];
    const char *p = line;
    const char *eq;
    const char *vend;
    size_t klen;
    size_t vlen;
    int idx = -1;
    unsigned long n = 0;
    int rc = 0;

    if (!conf || !seen || !line)
        return -EINVAL;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '\0' || *p == '#')
        return 0;

    eq = strchr(p, '=');
    if (!eq)
        return -EINVAL;

    vend = eq;
    while (vend > p && isspace((unsigned char)*(vend - 1)))
        vend--;
    klen = (size_t)(vend - p);
    if (klen == 0 || klen >= sizeof(key))
        return -EINVAL;
    memcpy(key, p, klen);
    key[klen] = '\0';

    for (int i = 0; i < KEY_COUNT; i++) {
        if (strcmp(key, g_key_names[i]) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -EINVAL;
    if (*seen & (1u << idx))
        return -EINVAL;

    /* The value runs to a "#" that follows white space, or to the end of the line. */
    p = eq + 1;
    while (*p && isspace((unsigned char)*p))
        p++;
    vend = p + strlen(p);
    for (const char *q = p; *q; q++) {
        if (*q == '#' && (q == p || isspace((unsigned char)*(q - 1)))) {
            vend = q;
            break;
        }
    }
    while (vend > p && isspace((unsigned char)*(vend - 1)))
        vend--;
    vlen = (size_t)(vend - p);
    if (vlen == 0 || vlen >= sizeof(value))
        return -EINVAL;
    memcpy(value, p, vlen);
    value[vlen] = '\0';

    switch (idx) {
    case KEY_FIRMWARE_DIR:
        rc = set_path(conf->firmware_dir, value);
        break;
    case KEY_RESOURCE_SCRIPT:
        rc = set_path(conf->resource_script, value);
        break;
    case KEY_VOUT_SCRIPT:
        rc = set_path(conf->vout_script, value);
        break;
    case KEY_APP_IMG_PROFILE:
        if (!uint_value_ok(value, VIRT_CAMERA_APP_IMG_PROFILE_MAX, &n))
            return -EINVAL;
        conf->app_img_profile = (int)n;
        break;
    case KEY_POC_CHIP:
        rc = set_path(conf->poc_chip, value);
        break;
    case KEY_POC_LINES:
        rc = set_poc_lines(conf, value);
        break;
    case KEY_POC_SETTLE_MS:
        if (!uint_value_ok(value, VIRT_CAMERA_SETTLE_MS_MAX, &n))
            return -EINVAL;
        conf->poc_settle_ms = (unsigned int)n;
        break;
    case KEY_EARLY_MODULES:
        rc = set_path(conf->early_modules, value);
        break;
    case KEY_LATE_MODULES:
        rc = set_path(conf->late_modules, value);
        break;
    case KEY_AAA_BIN:
        rc = set_path(conf->aaa_bin, value);
        break;
    case KEY_MONITOR_BIN:
        rc = set_path(conf->monitor_bin, value);
        break;
    default:
        return -EINVAL;
    }
    if (rc < 0)
        return rc;

    *seen |= 1u << idx;
    return 1;
}

int virt_camera_conf_load(const char *path, struct virt_camera_conf *conf)
{
    char line[1024];
    unsigned int seen = 0;
    unsigned int lineno = 0;
    FILE *fp;

    if (!path || !conf)
        return -EINVAL;

    fp = fopen(path, "r");
    if (!fp)
        return -errno;

    memset(conf, 0, sizeof(*conf));
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        int rc;

        lineno++;
        /* A line that fills the buffer without a newline is too long. */
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            fprintf(stderr, "[virt_camera_conf] %s:%u: line too long\n", path, lineno);
            fclose(fp);
            return -EINVAL;
        }
        rc = virt_camera_conf_parse_line(conf, &seen, line);
        if (rc < 0) {
            fprintf(stderr, "[virt_camera_conf] %s:%u: invalid line: %s", path, lineno, line);
            fclose(fp);
            return -EINVAL;
        }
    }
    fclose(fp);

    if (seen != ALL_KEYS_MASK) {
        for (int i = 0; i < KEY_COUNT; i++) {
            if (!(seen & (1u << i)))
                fprintf(stderr, "[virt_camera_conf] %s: missing key %s\n", path, g_key_names[i]);
        }
        return -EINVAL;
    }
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
