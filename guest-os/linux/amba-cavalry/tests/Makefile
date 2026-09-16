# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Cavalry HVM Guest Driver Test Suite Makefile
# Copyright (C) 2026, Ambarella International LLC

CROSS_COMPILE ?= aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc

ROOT_DIR ?= $(CURDIR)/../../../..
USERSPACE_DIR ?= $(ROOT_DIR)/guest-os/userspace
MEM_DIR ?= $(USERSPACE_DIR)/cavalry_mem
NNCTRL_DIR ?= $(USERSPACE_DIR)/nnctrl/cavalry_v3

CFLAGS = -O2 -Wall \
	-DAMBA_AMYOC_BUILD \
	-DAMBA_SOC_N1_655 \
	-I$(ROOT_DIR)/drivers/cavalry/include/cavalry_v3 \
	-I$(ROOT_DIR)/drivers/amba_virt/include \
	-I$(ROOT_DIR)/drivers/amba_virt/include/uapi \
	-I../include \
	-I$(MEM_DIR)/inc \
	-I$(MEM_DIR)/src \
	-I$(NNCTRL_DIR)/inc \
	-I$(NNCTRL_DIR)/src \
	-I$(NNCTRL_DIR)/dep/ambacnn

LDFLAGS = -lpthread -lm

COMMON_SRCS = $(MEM_DIR)/src/cavalry_mem.c \
	$(wildcard $(NNCTRL_DIR)/src/*.c)

TARGETS = test_basic \
	test_inference \
	test_path_b \
	test_path_b_security \
	test_dags_denial \
	test_mt_stress \
	test_victim_loop \
	test_multi_tenant

.PHONY: all clean

all: $(TARGETS)

test_basic: test_basic.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_inference: test_inference.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_path_b: test_path_b.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_path_b_security: test_path_b_security.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_dags_denial: test_dags_denial.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_mt_stress: test_mt_stress.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_victim_loop: test_victim_loop.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_multi_tenant: test_multi_tenant.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -static -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o
