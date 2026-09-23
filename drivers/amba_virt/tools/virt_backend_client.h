/*
 * virt_backend_client.h
 *
 * Outbound client connection manager from amba-virt-server to amba-virt-backend.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_BACKEND_CLIENT_H_
#define _VIRT_BACKEND_CLIENT_H_

#include <stdint.h>
#include <stdbool.h>
#include "amba_virt.h"

typedef void (*backend_state_change_cb)(uint32_t new_mod_mask);

int virt_backend_client_init(const char *host_ip, uint16_t port, const char *token_file, backend_state_change_cb cb);
void virt_backend_client_stop(void);

bool virt_backend_client_is_connected(void);
uint32_t virt_backend_client_get_mod_mask(void);

int virt_backend_client_load_module(const char *module_name);
int virt_backend_client_unload_module(const char *module_name);
int virt_backend_client_hardware_reset(uint32_t dev_id);
int virt_backend_client_get_firmware(struct backend_firmware_resp *resp);
int virt_backend_client_get_full_status(uint32_t *mod_mask);

#endif /* _VIRT_BACKEND_CLIENT_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
