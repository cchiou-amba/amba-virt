# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Cavalry HVM Benchmark & Comparison Suite Makefile
# Copyright (C) 2026, Ambarella International LLC

CROSS_COMPILE ?= aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc

ROOT_DIR ?= $(CURDIR)/../../..
USERSPACE_DIR ?= $(ROOT_DIR)/guest-os/userspace
MEM_DIR ?= $(USERSPACE_DIR)/cavalry_mem
NNCTRL_DIR ?= $(USERSPACE_DIR)/nnctrl/cavalry_v3
CAVALRY_DIR ?= $(ROOT_DIR)/guest-os/linux/amba-cavalry

CFLAGS = -O2 -Wall \
	-DAMBA_AMYOC_BUILD \
	-DAMBA_SOC_N1_655 \
	-I$(ROOT_DIR)/drivers/cavalry/include/cavalry_v3 \
	-I$(ROOT_DIR)/drivers/amba_virt/include \
	-I$(ROOT_DIR)/drivers/amba_virt/include/uapi \
	-I$(CAVALRY_DIR)/include \
	-I$(MEM_DIR)/inc \
	-I$(MEM_DIR)/src \
	-I$(NNCTRL_DIR)/inc \
	-I$(NNCTRL_DIR)/src \
	-I$(NNCTRL_DIR)/dep/ambacnn

LDFLAGS = -lpthread -lm

COMMON_SRCS = $(MEM_DIR)/src/cavalry_mem.c \
	$(wildcard $(NNCTRL_DIR)/src/*.c)

TARGETS = cavalry_hvm_demo cavalry_hvm_demo-musl cavalry_hvm_demo-alpine

.PHONY: all clean

all: $(TARGETS)

cavalry_hvm_demo: cavalry_hvm_demo.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cavalry_hvm_demo-musl: cavalry_hvm_demo.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -static -o $@ $^ $(LDFLAGS)

cavalry_hvm_demo-alpine: cavalry_hvm_demo-musl
	ln -sf $< $@

clean:
	rm -f $(TARGETS) *.o
