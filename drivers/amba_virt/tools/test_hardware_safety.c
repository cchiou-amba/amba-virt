/*
 * test_hardware_safety.c
 *
 * Suite E: Hardware Safety, Deadlock & Timeout Suite
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cavalry_proxy.h"

static void test_drain_safety_state(void)
{
	printf("[SUITE E] Testing hardware accelerator drain safety state...\n");
	cavalry_proxy_start_drain();
	assert(cavalry_proxy_is_draining());

	/* New RPC dispatches during drain should be rejected */
	struct amba_virt_cavalry_rpc req = { .opcode = 10 /* VCAV_OP_RUN_DAGS */ };
	struct amba_virt_cavalry_rpc resp;
	memset(&resp, 0, sizeof(resp));

	cavalry_proxy_handle_rpc(&req, &resp, 2);
	assert(resp.status == -EHOSTDOWN);

	cavalry_proxy_finish_drain();
	assert(!cavalry_proxy_is_draining());
	printf("[PASS] Drain mode strictly rejects incoming hardware operations with -EHOSTDOWN\n");
}

int main(void)
{
	printf("=== Running Suite E: Hardware Safety & Timeouts ===\n");
	test_drain_safety_state();
	printf("=== Suite E: All Tests Passed ===\n\n");
	return 0;
}
