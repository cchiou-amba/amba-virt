/*
 * test_state_desync.c
 *
 * Suite B: Inconsistency, State Desync & Self-Healing Suite
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amba_virt.h"
#include "virt_acl.h"
#include "virt_driver_matrix.h"
#include "virt_query.h"

static void test_out_of_band_mask_reconciliation(void)
{
	printf("[SUITE B] Testing out-of-band host module bitmask dynamic reconciliation...\n");
	virt_acl_init();

	/* Initial state: no modules */
	virt_query_set_host_mod_mask(0);
	struct amba_virt_query_req req = { .query_op = AMBA_VIRT_QUERY_DRIVER_CAPS };
	struct amba_virt_query_resp resp;
	int ret = virt_query_handle_req(2, &req, &resp);
	assert(ret == 0);
	struct amba_virt_driver_caps_resp *caps = (struct amba_virt_driver_caps_resp *)resp.payload;
	for (uint32_t i = 0; i < caps->count; i++) {
		assert(caps->entries[i].state == AMBA_VIRT_DEV_STATE_OFFLINE);
	}

	/* Simulate out-of-band load of cavalry + ambcma */
	virt_query_set_host_mod_mask(HOST_MOD_AMBCMA | HOST_MOD_CAVALRY);
	ret = virt_query_handle_req(2, &req, &resp);
	assert(ret == 0);
	caps = (struct amba_virt_driver_caps_resp *)resp.payload;
	for (uint32_t i = 0; i < caps->count; i++) {
		if (caps->entries[i].dev_id == 1) { /* Cavalry */
			assert(caps->entries[i].state == AMBA_VIRT_DEV_STATE_ONLINE);
		} else {
			assert(caps->entries[i].state == AMBA_VIRT_DEV_STATE_OFFLINE);
		}
	}
	printf("[PASS] Out-of-band state changes dynamically transitioned virtual driver readiness\n");
}

int main(void)
{
	printf("=== Running Suite B: Inconsistency & State Desync ===\n");
	test_out_of_band_mask_reconciliation();
	printf("=== Suite B: All Tests Passed ===\n\n");
	return 0;
}
