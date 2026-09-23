/*
 * test_fuzz_corruption.c
 *
 * Suite C: Fuzzing, Corruption & Tampering Test Suite
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amba_virt.h"
#include "virt_acl.h"
#include "virt_query.h"

static void test_fuzzed_magic_and_lengths(void)
{
	printf("[SUITE C] Testing fuzzed magic numbers and corrupted message lengths...\n");
	virt_acl_init();

	/* Corrupted query req */
	struct amba_virt_query_req req;
	struct amba_virt_query_resp resp;

	memset(&req, 0xFF, sizeof(req));
	int ret = virt_query_handle_req(2, &req, &resp);
	assert(ret == 0);
	assert(resp.status == -EOPNOTSUPP);

	/* Unregistered query opcode */
	req.query_op = 0x9999;
	ret = virt_query_handle_req(2, &req, &resp);
	assert(ret == 0);
	assert(resp.status == -EOPNOTSUPP);

	printf("[PASS] Fuzzed and invalid requests safely rejected\n");
}

int main(void)
{
	printf("=== Running Suite C: Fuzzing & Corruption ===\n");
	test_fuzzed_magic_and_lengths();
	printf("=== Suite C: All Tests Passed ===\n\n");
	return 0;
}
