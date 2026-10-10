/*
 * virt_module_loader.c
 *
 * Config-driven module loader for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amba_virt.h"
#include "virt_host_ops.h"
#include "virt_module_loader.h"

static bool is_valid_param_char(char c)
{
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '=' || c == '.' || c == ',' || c == ' ') {
        return true;
    }
    return false;
}

int virt_module_loader_parse_line(const char *line, char *out_name, size_t name_sz,
                                 char *out_params, size_t params_sz)
{
    if (!line || !out_name || !out_params || name_sz == 0 || params_sz == 0)
        return -EINVAL;

    out_name[0] = '\0';
    out_params[0] = '\0';

    const char *p = line;
    while (*p && isspace((unsigned char)*p))
        p++;

    if (*p == '\0' || *p == '#')
        return 0;

    const char *name_start = p;
    while (*p && !isspace((unsigned char)*p) && *p != '#')
        p++;

    size_t name_len = (size_t)(p - name_start);
    if (name_len == 0 || name_len > VIRT_HOST_MODULE_MAX_NAME || name_len >= name_sz)
        return -EINVAL;

    char temp_name[64];
    memcpy(temp_name, name_start, name_len);
    temp_name[name_len] = '\0';

    if (name_len < 4 || strcmp(temp_name + name_len - 3, ".ko") != 0)
        return -EINVAL;

    if (strchr(temp_name, '/') != NULL || strchr(temp_name, '\\') != NULL ||
        strstr(temp_name, "..") != NULL)
        return -EINVAL;

    while (*p && isspace((unsigned char)*p))
        p++;

    char temp_params[256] = {0};
    size_t param_len = 0;

    if (*p && *p != '#') {
        const char *param_start = p;
        const char *comment_pos = strchr(param_start, '#');
        const char *param_end = comment_pos ? comment_pos : (param_start + strlen(param_start));

        while (param_end > param_start && isspace((unsigned char)*(param_end - 1)))
            param_end--;

        param_len = (size_t)(param_end - param_start);
        if (param_len > VIRT_HOST_MODULE_MAX_PARAM || param_len >= params_sz)
            return -EINVAL;

        for (size_t i = 0; i < param_len; i++) {
            if (!is_valid_param_char(param_start[i]))
                return -EINVAL;
            temp_params[i] = param_start[i];
        }
        temp_params[param_len] = '\0';
    }

    strncpy(out_name, temp_name, name_sz - 1);
    out_name[name_sz - 1] = '\0';

    strncpy(out_params, temp_params, params_sz - 1);
    out_params[params_sz - 1] = '\0';

    return 1;
}

int virt_module_loader_run(const char *config_path)
{
    if (!config_path)
        return -EINVAL;

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            printf("[virt_module_loader] %s not present, continuing without modules.conf\n", config_path);
            return 0;
        }
        perror(config_path);
        return -errno;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char mod_name[64];
        char mod_params[256];
        int ret = virt_module_loader_parse_line(line, mod_name, sizeof(mod_name),
                                                mod_params, sizeof(mod_params));
        if (ret < 0) {
            fprintf(stderr, "[virt_module_loader] Invalid line in %s: %s\n", config_path, line);
            fclose(fp);
            return ret;
        }
        if (ret == 0)
            continue;

        ret = virt_host_module_load(mod_name, mod_params);
        if (ret < 0) {
            fprintf(stderr, "[virt_module_loader] Load failed for %s (params='%s'): %d\n",
                    mod_name, mod_params, ret);
            fclose(fp);
            return ret;
        }
    }

    fclose(fp);
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
