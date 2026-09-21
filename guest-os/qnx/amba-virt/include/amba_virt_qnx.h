/*
 * amba_virt_qnx.h
 *
 * Ambarella Virtualization QNX Client & Sibling Driver C API.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef AMBA_VIRT_QNX_H
#define AMBA_VIRT_QNX_H

#include <stdint.h>
#include <stddef.h>
#include "amba_virt.h"
#include "amba_virt_test.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * amba_virt_get_window - Query and map the shared ivshmem DRAM window.
 * @phys: Output pointer for the 64-bit physical base address.
 * @virt: Output pointer for the process virtual address mapping.
 * @size: Output pointer for the total window size in bytes.
 *
 * Returns 0 on success, or a negative errno code on failure.
 */
int amba_virt_get_window(uint64_t *phys, void **virt, size_t *size);

/**
 * amba_virt_rpc - Send a synchronous RPC request to the host virtualization server.
 * @request: Pointer to the request payload buffer.
 * @request_len: Length of the request in bytes (<= 4096).
 * @response: Pointer to the response buffer.
 * @response_len: In: capacity of response buffer; Out: actual bytes received.
 * @timeout_ms: Timeout in milliseconds (0 = default 5000 ms).
 *
 * Returns 0 on success, or a negative errno code on failure.
 */
int amba_virt_rpc(const void *request, uint32_t request_len,
                  void *response, uint32_t *response_len,
                  unsigned int timeout_ms);

/**
 * amba_virt_close - Close cached driver handles and unmap shared memory.
 */
void amba_virt_close(void);

#ifdef __cplusplus
}
#endif

#endif /* AMBA_VIRT_QNX_H */
