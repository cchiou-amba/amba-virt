# Makefile for Ambarella bringup & hardware diagnostic utilities
#
# Copyright (C) 2026, Ambarella International LLC
#

CROSS_COMPILE ?= aarch64-linux-gnu-
CC            := $(CROSS_COMPILE)gcc
CFLAGS        ?= -Wall -O2
LDFLAGS       ?= -static

BUILD_DIR     ?= build
PROGS         := check_locks iav_status iav_memblock iav_sysres iav_pin_probe iav_newpart iav_vsrc_list mmio_read scan_i2c1 read_max_regs test_oneshot_ser set_uart_pinmux uart_bench_agent test_interactive_consoles
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

IAV_INC ?= ../../build/pristine-dsp_v6/include
$(BUILD_DIR)/iav_memblock: iav_memblock.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I$(IAV_INC) -I$(IAV_INC)/uapi -I$(IAV_INC)/uapi/specific -include stdint.h -o $@ $<

$(BUILD_DIR)/iav_sysres: iav_sysres.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I$(IAV_INC) -I$(IAV_INC)/uapi -I$(IAV_INC)/uapi/specific -include stdint.h -o $@ $<

$(BUILD_DIR)/iav_pin_probe: iav_pin_probe.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I$(IAV_INC) -I$(IAV_INC)/uapi -I$(IAV_INC)/uapi/specific -include stdint.h -o $@ $<

$(BUILD_DIR)/iav_newpart: iav_newpart.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I$(IAV_INC) -I$(IAV_INC)/uapi -I$(IAV_INC)/uapi/specific -include stdint.h -o $@ $<

$(BUILD_DIR)/iav_vsrc_list: iav_vsrc_list.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I$(IAV_INC) -I$(IAV_INC)/uapi -I$(IAV_INC)/uapi/specific -include stdint.h -o $@ $<

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
