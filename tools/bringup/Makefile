# Makefile for Ambarella bringup & hardware diagnostic utilities
#
# Copyright (C) 2026, Ambarella International LLC
#

CROSS_COMPILE ?= aarch64-linux-gnu-
CC            := $(CROSS_COMPILE)gcc
CFLAGS        ?= -Wall -O2

BUILD_DIR     ?= build
PROGS         := check_locks iav_status mmio_read scan_i2c1 read_max_regs test_oneshot_ser set_uart_pinmux
TARGETS       := $(addprefix $(BUILD_DIR)/, $(PROGS))

all: $(BUILD_DIR) $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/set_uart_pinmux: set_uart_pinmux.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/check_locks: check_locks.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/iav_status: iav_status.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/mmio_read: mmio_read.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/scan_i2c1: scan_i2c1.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/read_max_regs: read_max_regs.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/test_oneshot_ser: test_oneshot_ser.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD_DIR) *.o

.PHONY: all clean
