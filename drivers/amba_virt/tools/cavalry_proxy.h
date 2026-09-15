/*
 * cavalry_proxy.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _CAVALRY_PROXY_H_
#define _CAVALRY_PROXY_H_

#include <stdint.h>
#include <stddef.h>
#include "amba_virt.h"

#define CAVALRY_POOL_BASE        0x02000000U    /* 32 MiB */
#define CAVALRY_POOL_SIZE        0x3E000000U    /* 992 MiB (extends to 1 GiB) */
#define CAVALRY_RPC_ARENA_OFFSET 0x01F00000U    /* 31 MiB (1 MiB control arena) */
#define CAVALRY_RPC_ARENA_SIZE   0x00100000U    /* 1 MiB */

int cavalry_proxy_init(int fd_amba_virt, unsigned char *shm_map, size_t shm_size);
void cavalry_proxy_cleanup(void);

int cavalry_proxy_handle_rpc(const struct amba_virt_cavalry_rpc *req,
			     struct amba_virt_cavalry_rpc *resp,
			     uint32_t client_cid);

int cavalry_proxy_close_session(uint32_t client_cid, uint32_t session_id);
void cavalry_proxy_client_disconnect(uint32_t client_cid);

#endif /* _CAVALRY_PROXY_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
