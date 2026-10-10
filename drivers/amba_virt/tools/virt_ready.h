/*
 * virt_ready.h
 *
 * Readiness for the daemon-mode server: the wait for the shared-memory nodes
 * and the byte the worker sends the parent when it is ready.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_READY_H_
#define _VIRT_READY_H_

#include <stdbool.h>

#define VIRT_READY_DEV_DIR   "/dev"
#define VIRT_READY_SHM_PREFIX "amba_virt_shm"
#define VIRT_READY_BYTE      'R'

/* True when dev_dir holds an entry whose name starts with amba_virt_shm. */
bool virt_ready_shm_present(const char *dev_dir);
/* Waits for such an entry. 0 when present, -ETIMEDOUT after timeout_ms. */
int virt_ready_wait_shm(const char *dev_dir, unsigned int timeout_ms);
/* Writes the readiness byte to fd. A negative fd (foreground mode) does nothing. */
void virt_ready_notify(int fd);

#endif /* _VIRT_READY_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
