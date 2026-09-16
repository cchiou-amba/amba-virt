/*
 * virt_admin_ipc.h
 *
 * Local UNIX domain socket control plane for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_ADMIN_IPC_H_
#define _VIRT_ADMIN_IPC_H_

#include <stdint.h>

#define VIRT_ADMIN_SOCK_PATH  "/run/amba-virt/admin.sock"
#define VIRT_ADMIN_MAX_BUF    4096

int virt_admin_ipc_start(const char *sock_path);
void virt_admin_ipc_stop(void);

#endif /* _VIRT_ADMIN_IPC_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
