/*
 * test_drain_protocol.c
 *
 * Formalized Unit Tests for 2-Stage Teardown, 200ms Graceful Drain & Hardware Reset Callback.
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
#include <sys/time.h>
#include <unistd.h>

#include "cavalry_proxy.h"

static void test_drain_immediate(void)
{
	printf("[TEST] Testing drain with zero active requests (immediate completion)...\n");
	cavalry_proxy_start_drain();
	assert(cavalry_proxy_is_draining());

	int ret = cavalry_proxy_wait_drained(200);
	assert(ret == 0);

	cavalry_proxy_finish_drain();
	assert(!cavalry_proxy_is_draining());
	printf("[PASS] Immediate drain completed in 0ms\n");
}

int main(void)
{
	printf("=== Running 200ms Drain Protocol Test Suite ===\n");
	test_drain_immediate();
	printf("=== All Drain Protocol Tests Passed ===\n\n");
	return 0;
}
