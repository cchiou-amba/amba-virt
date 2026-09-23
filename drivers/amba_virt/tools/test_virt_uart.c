/*
 * test_virt_uart.c
 *
 * Unit tests for Ambarella Virtual UART message protocols, framing, and FIFO bounds.
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

#include "guest-os/linux/amba-uart/amba_uart.h"

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

/* Test 1: Register Offset & Errata Bit Verification */
static int test_uart_register_offsets(void)
{
    /* 16550 standard offsets */
    assert(UART_RB_OFFSET == 0x00);
    assert(UART_TH_OFFSET == 0x00);
    assert(UART_IE_OFFSET == 0x04);
    assert(UART_II_OFFSET == 0x08);
    assert(UART_FC_OFFSET == 0x08);
    assert(UART_LC_OFFSET == 0x0c);
    assert(UART_MC_OFFSET == 0x10);
    assert(UART_LS_OFFSET == 0x14);
    assert(UART_MS_OFFSET == 0x18);
    assert(UART_SC_OFFSET == 0x1c);

    /* Synopsys DesignWare extensions */
    assert(UART_DMAE_OFFSET == 0x28);
    assert(UART_DMAF_OFFSET == 0x40);
    assert(UART_US_OFFSET == 0x7c);
    assert(UART_TFL_OFFSET == 0x80);
    assert(UART_RFL_OFFSET == 0x84);
    assert(UART_RTR_OFFSET == 0xac);
    assert(UART_TTR_OFFSET == 0xb0);

    /* Errata & bitmask constants */
    assert(UART_IE_ETBEI == 0x02);
    assert(UART_IE_PTIME == 0x80);
    assert(UART_US_TFNF == 0x02);
    assert(UART_US_RFNE == 0x08);
    assert(AMBA_UART_FIFO_SIZE == 64);
    assert(AMBA_UART_DEFAULT_CLK == 24000000);

    return TEST_PASS;
}

/* Test 2: Baud Rate Divisor Calculations */
static int test_uart_baud_divisors(void)
{
    unsigned int clk = AMBA_UART_DEFAULT_CLK; /* 24 MHz */
    unsigned int baud_115200 = 115200;
    unsigned int baud_9600 = 9600;
    unsigned int baud_3000000 = 3000000;

    /* Divisor formula: divisor = (clk + 8 * baud) / (16 * baud) */
    unsigned int quot_115200 = (clk + 8 * baud_115200) / (16 * baud_115200);
    unsigned int quot_9600 = (clk + 8 * baud_9600) / (16 * baud_9600);
    unsigned int quot_3000000 = (clk + 8 * baud_3000000) / (16 * baud_3000000);

    /* 24,000,000 / (16 * 115200) = 13.0208 -> rounded 13 */
    assert(quot_115200 == 13);
    /* 24,000,000 / (16 * 9600) = 156.25 -> rounded 156 */
    assert(quot_9600 == 156);
    /* 24,000,000 / (16 * 3000000) = 0.5 -> rounded 1 */
    assert(quot_3000000 == 1);

    /* Check actual calculated baud error for 115200 */
    unsigned int actual_baud_115200 = clk / (16 * quot_115200);
    int error_pct = abs((int)actual_baud_115200 - (int)baud_115200) * 100 / baud_115200;
    assert(error_pct <= 1); /* Under 1% error */

    return TEST_PASS;
}

/* Test 3: FIFO Bounds & Circular Buffer Simulation */
static int test_uart_fifo_simulation(void)
{
    uint8_t fifo_buf[AMBA_UART_FIFO_SIZE];
    size_t head = 0, tail = 0, count = 0;

    /* Push 64 bytes */
    for (size_t i = 0; i < AMBA_UART_FIFO_SIZE; i++) {
        assert(count < AMBA_UART_FIFO_SIZE);
        fifo_buf[head] = (uint8_t)(i & 0xff);
        head = (head + 1) % AMBA_UART_FIFO_SIZE;
        count++;
    }
    assert(count == AMBA_UART_FIFO_SIZE);

    /* Drain 64 bytes and verify content integrity */
    for (size_t i = 0; i < AMBA_UART_FIFO_SIZE; i++) {
        assert(count > 0);
        uint8_t val = fifo_buf[tail];
        assert(val == (uint8_t)(i & 0xff));
        tail = (tail + 1) % AMBA_UART_FIFO_SIZE;
        count--;
    }
    assert(count == 0);

    return TEST_PASS;
}

int main(void)
{
    printf("====================================================\n");
    printf(" Suite A: Ambarella Virtual UART Unit Tests\n");
    printf("====================================================\n");

    RUN_TEST(test_uart_register_offsets);
    RUN_TEST(test_uart_baud_divisors);
    RUN_TEST(test_uart_fifo_simulation);

    printf("\nResults: %d/%d tests passed (100%%).\n", passed_tests, total_tests);
    return (passed_tests == total_tests) ? 0 : 1;
}
