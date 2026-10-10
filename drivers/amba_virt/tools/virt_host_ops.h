/*
 * virt_host_ops.h
 *
 * Direct Dom0 host operations for amba-virt-server: kernel module load and
 * unload, module state, the firmware search path, and the accelerator reset.
 * Each is a local call into the Dom0 kernel.
 *
 * The syscalls and the file locations can be replaced, so the unit tests can
 * drive every kernel answer (EEXIST, ENOEXEC, EKEYREJECTED, EBUSY) without a
 * kernel.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_HOST_OPS_H_
#define _VIRT_HOST_OPS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "amba_virt.h"

#define VIRT_HOST_MAX_MODULE_DIRS   4
#define VIRT_HOST_MODULE_MAX_NAME   63u
#define VIRT_HOST_MODULE_MAX_PARAM  255u
#define VIRT_HOST_FW_MAX_RELPATH    127u
#define VIRT_HOST_FW_MAX_DIR        255u
#define VIRT_HOST_FW_NODE_DEFAULT   "/sys/module/firmware_class/parameters/path"
#define VIRT_HOST_PROC_MODULES      "/proc/modules"
#define VIRT_HOST_WATCH_MS_DEFAULT  2000u
#define VIRT_HOST_STATE_DIR         "/run/amba-virt/modules"
#define VIRT_HOST_SYSFS_MODULE_DIR  "/sys/module"

/* One firmware image, as the admin FIRMWARE command reports it. */
struct virt_host_firmware_entry {
    char     name[32];
    uint32_t size;
    char     sha256[64];
    uint32_t present;
};

struct virt_host_firmware_resp {
    uint32_t count;
    struct virt_host_firmware_entry entries[8];
};

/* A syscall returns 0, or -1 with errno set, exactly as the libc wrapper does. */
struct virt_host_sys {
    int (*finit_module)(int fd, const char *params);
    int (*delete_module)(const char *name);
};

struct virt_host_config {
    const char *module_dirs[VIRT_HOST_MAX_MODULE_DIRS]; /* searched in order */
    size_t module_dir_count;
    const char *proc_modules;
    const char *fw_node;
    const char *state_dir;          /* where a load records the parameters it passed */
    const char *sysfs_module_dir;   /* where a loaded module's parameters can be read */
};

/* NULL restores the real syscalls. */
void virt_host_set_sys(const struct virt_host_sys *sys);
/* NULL restores the defaults: /lib/modules/<release>/extra, /proc/modules, the kernel's firmware_class path node. */
void virt_host_configure(const struct virt_host_config *cfg);

/* A module file name: "<base>.ko", at most 63 bytes, no '/', no '\\', no "..". */
bool virt_host_module_name_valid(const char *name);
/* Module parameters: letters, digits, '_', '=', '.', ',' and ' ', at most 255 bytes. NULL is empty. */
bool virt_host_module_params_valid(const char *params);

/*
 * Loads name from the first module directory that holds it. 0 on success and
 * when the module is already loaded. -EINVAL for a bad name or parameters
 * without touching the kernel, -ENOENT when no directory has the file, and
 * the kernel's own refusal otherwise (-ENOEXEC for a vermagic mismatch,
 * -EKEYREJECTED for a bad signature).
 *
 * A module that is already loaded counts as loaded only when it carries the parameters asked for.
 * With no parameters asked for, that is always so. Otherwise the parameters this process recorded
 * when it loaded the module must be the same string. With no record, each parameter is read from
 * the module's sysfs directory and compared. A mismatch returns -EEXIST, and a parameter that
 * cannot be read returns -ENODATA, so the boot fails instead of running with the wrong values.
 */
int virt_host_module_load(const char *name, const char *params);
/* 0 on success and when the module is not loaded. -EBUSY when it is in use. */
int virt_host_module_unload(const char *name);

/* Bitmask of the loaded modules named by the driver matrix. */
uint32_t virt_host_module_mask(void);

typedef void (*virt_host_state_cb)(uint32_t mask);
/*
 * Calls cb once with the current mask, then again from a watcher thread each
 * time the mask changes, for a change made by anything on the host. -EBUSY
 * when a watcher is already running. virt_host_watch_stop() is a no-op when
 * none is, and the watcher may be started again after it. One owner starts and
 * stops it; the calls are not safe from two threads at once.
 */
int virt_host_watch_start(virt_host_state_cb cb);
void virt_host_watch_stop(void);
void virt_host_set_watch_interval_ms(unsigned int ms);

/* A path relative to a firmware root: at most 127 bytes, no leading '/', no '\\', no empty, "." or ".." component. */
bool virt_host_firmware_path_valid(const char *rel);
/*
 * Points the kernel's firmware_class search path at dir, an absolute path in
 * the image, and writes the node only when it holds something else.
 */
int virt_host_firmware_select(const char *dir);
/* The firmware inventory. Dom0 manages no firmware inventory, so the count is zero. */
int virt_host_get_firmware(struct virt_host_firmware_resp *resp);

/*
 * No accelerator reset exists, so this returns -EOPNOTSUPP and opens no device. A real reset waits
 * for a header in the tree that names the operation.
 */
int virt_host_hardware_reset(uint32_t dev_id);
/*
 * Unloads name. drained is the result of the cavalry drain wait, negative when requests were still
 * in flight at the timeout. Such a cavalry unload needs a reset; it returns that error and leaves
 * the module loaded.
 */
int virt_host_module_unload_drained(const char *name, int drained);

#endif /* _VIRT_HOST_OPS_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
