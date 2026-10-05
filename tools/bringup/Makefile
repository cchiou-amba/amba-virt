# Makefile for Ambarella bringup & hardware diagnostic utilities
#
# Copyright (C) 2026, Ambarella International LLC
#

CROSS_COMPILE ?= aarch64-linux-gnu-
CC            := $(CROSS_COMPILE)gcc
CFLAGS        ?= -Wall -O2
LDFLAGS       ?= -static

BUILD_DIR     ?= build
PROGS         := check_locks iav_status mmio_read scan_i2c1 read_max_regs test_oneshot_ser set_uart_pinmux uart_bench_agent test_interactive_consoles
TARGETS       := $(addprefix $(BUILD_DIR)/, $(PROGS))

all: $(BUILD_DIR) $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_interactive_consoles: test_interactive_consoles.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/uart_bench_agent: uart_bench_agent.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/set_uart_pinmux: set_uart_pinmux.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/check_locks: check_locks.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/iav_status: iav_status.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/mmio_read: mmio_read.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/scan_i2c1: scan_i2c1.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/read_max_regs: read_max_regs.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/test_oneshot_ser: test_oneshot_ser.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD_DIR) *.o

# Host Unit Testing (CppUTest with ASan/UBSan)
HOST_CXX     ?= g++
TEST_CFLAGS  ?= -Wall -Wextra -O2 -I. -std=c++17 -fsanitize=address,undefined
TEST_LDFLAGS ?= -lCppUTest -lCppUTestExt -lpthread -lm -fsanitize=address,undefined
TEST_SRCS    := test/test_main.cxx test/TestUartBenchAgent.cxx
TEST_BIN     := $(BUILD_DIR)/test_uart_bench_agent

test: $(TEST_BIN)
	./$(TEST_BIN) -v

$(TEST_BIN): $(TEST_SRCS) uart_bench_protocol.h | $(BUILD_DIR)
	$(HOST_CXX) $(TEST_CFLAGS) -o $@ $(TEST_SRCS) $(TEST_LDFLAGS)

.PHONY: all clean test
