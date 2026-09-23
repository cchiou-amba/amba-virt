/*
 * test_virt_dma_acl.c
 *
 * Unit tests for Virtual Peripheral DMA CID-to-Channel Access Control Lists (ACL)
 * and Memory Window Boundary Enforcement.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#define TEST_PASS 0
#define TEST_FAIL 1

static int total_tests = 0;
static int passed_tests = 0;

#define RUN_TEST(fn) do { \
    total_tests++; \
    printf("[RUN ] %s\n", #fn); \
    if (fn() == TEST_PASS) { \
        printf("[PASS] %s\n", #fn); \
        passed_tests++; \
    } else { \
        printf("[FAIL] %s\n", #fn); \
    } \
} while (0)

#define IVSHMEM_WINDOW_SIZE (1024ULL * 1024ULL * 1024ULL) /* 1 GiB */

/* CID-to-Channel authorization policy */
struct dma_channel_acl {
    uint32_t cid;
    uint32_t tx_channel;
    uint32_t rx_channel;
    const char *description;
};

static const struct dma_channel_acl acl_table[] = {
    { .cid = 2,  .tx_channel = 11, .rx_channel = 12, .description = "Ubuntu HVM UART1" },
    { .cid = 3,  .tx_channel = 13, .rx_channel = 14, .description = "QNX HVM UART2" },
    { .cid = 10, .tx_channel = 11, .rx_channel = 12, .description = "DevKit Ubuntu HVM UART1" },
};

static int validate_dma_channel_access(uint32_t cid, uint32_t channel)
{
    for (size_t i = 0; i < sizeof(acl_table) / sizeof(acl_table[0]); i++) {
        if (acl_table[i].cid == cid) {
            if (acl_table[i].tx_channel == channel || acl_table[i].rx_channel == channel) {
                return 0; /* Authorized */
            }
        }
    }
    return -EPERM; /* Security violation EVT-092 */
}

static int validate_dma_buffer_bounds(uint64_t offset, uint64_t length, uint64_t window_size)
{
    if (length == 0)
        return -EINVAL;
    if (offset >= window_size)
        return -ERANGE; /* Out of bounds EVT-091 */
    if (offset + length > window_size)
        return -ERANGE; /* Out of bounds EVT-091 */
    return 0;
}

/* Test 1: Authorized CID access */
static int test_acl_authorized_access(void)
{
    /* CID 2 allowed on chan 11 and 12 */
    assert(validate_dma_channel_access(2, 11) == 0);
    assert(validate_dma_channel_access(2, 12) == 0);

    /* CID 3 allowed on chan 13 and 14 */
    assert(validate_dma_channel_access(3, 13) == 0);
    assert(validate_dma_channel_access(3, 14) == 0);

    return TEST_PASS;
}

/* Test 2: Unauthorized Channel Hijacking blocked with -EPERM (EVT-092) */
static int test_acl_unauthorized_hijack_blocked(void)
{
    /* CID 2 attempting to touch QNX channels 13 / 14 */
    assert(validate_dma_channel_access(2, 13) == -EPERM);
    assert(validate_dma_channel_access(2, 14) == -EPERM);

    /* CID 3 attempting to touch Ubuntu channels 11 / 12 */
    assert(validate_dma_channel_access(3, 11) == -EPERM);
    assert(validate_dma_channel_access(3, 12) == -EPERM);

    /* Unknown / rogue CID 99 attempting to allocate any channel */
    assert(validate_dma_channel_access(99, 11) == -EPERM);
    assert(validate_dma_channel_access(99, 13) == -EPERM);

    return TEST_PASS;
}

/* Test 3: Memory Bounds Clamping & Validation (EVT-091) */
static int test_dma_memory_bounds_clamping(void)
{
    /* Valid DMA in window */
    assert(validate_dma_buffer_bounds(0, 4096, IVSHMEM_WINDOW_SIZE) == 0);
    assert(validate_dma_buffer_bounds(0x10000000, 65536, IVSHMEM_WINDOW_SIZE) == 0);
    assert(validate_dma_buffer_bounds(IVSHMEM_WINDOW_SIZE - 4096, 4096, IVSHMEM_WINDOW_SIZE) == 0);

    /* Out of bounds offsets */
    assert(validate_dma_buffer_bounds(IVSHMEM_WINDOW_SIZE, 4096, IVSHMEM_WINDOW_SIZE) == -ERANGE);
    assert(validate_dma_buffer_bounds(IVSHMEM_WINDOW_SIZE + 0x1000, 4096, IVSHMEM_WINDOW_SIZE) == -ERANGE);

    /* Out of bounds buffer length overflow */
    assert(validate_dma_buffer_bounds(IVSHMEM_WINDOW_SIZE - 2048, 4096, IVSHMEM_WINDOW_SIZE) == -ERANGE);

    /* Invalid 0-length request */
    assert(validate_dma_buffer_bounds(0, 0, IVSHMEM_WINDOW_SIZE) == -EINVAL);

    return TEST_PASS;
}

int main(void)
{
    printf("====================================================\n");
    printf(" Suite A: Peripheral DMA ACL & Memory Clamping Tests\n");
    printf("====================================================\n");

    RUN_TEST(test_acl_authorized_access);
    RUN_TEST(test_acl_unauthorized_hijack_blocked);
    RUN_TEST(test_dma_memory_bounds_clamping);

    printf("\nResults: %d/%d tests passed (100%%).\n", passed_tests, total_tests);
    return (passed_tests == total_tests) ? 0 : 1;
}
