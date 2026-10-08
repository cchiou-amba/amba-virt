/*
 * virt_module_loader.h
 *
 * Config-driven module loader for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_MODULE_LOADER_H_
#define _VIRT_MODULE_LOADER_H_

#include <stddef.h>
#include <stdint.h>

#define MODULES_CONF_DEFAULT_PATH "/etc/amba-virt/modules.conf"

/*
 * Parses a single line of modules.conf.
 * Returns:
 *   1 if a valid module entry was parsed (out_name and out_params populated)
 *   0 if the line was empty or a comment (no entry, out_name[0] = '\0')
 *  <0 (-EINVAL) if parsing or validation failed
 */
int virt_module_loader_parse_line(const char *line, char *out_name, size_t name_sz,
                                 char *out_params, size_t params_sz);

/*
 * Reads config_path and loads each module in sequence through the backend client.
 * Returns 0 on success, or the first probe/store/load error encountered.
 */
int virt_module_loader_run(const char *config_path);

#endif /* _VIRT_MODULE_LOADER_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
