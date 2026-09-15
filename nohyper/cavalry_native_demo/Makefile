# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Cavalry Native Baseline Benchmark Makefile
# Copyright (C) 2026, Ambarella International LLC

CROSS_COMPILE ?= aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc

ROOT_DIR ?= $(CURDIR)/../..
COOPER_DIR ?= $(ROOT_DIR)/../ambarella/cooper/ambarella

MEM_DIR = $(COOPER_DIR)/boot/fastapp/cavalry/packages/cavalry_mem
NNCTRL_DIR = $(COOPER_DIR)/boot/fastapp/cavalry/packages/nnctrl/cavalry_v3

CFLAGS = -O2 -Wall \
	-DAMBA_AMYOC_BUILD \
	-DAMBA_SOC_N1_655 \
	-I$(ROOT_DIR)/drivers/cavalry/include/cavalry_v3 \
	-I$(ROOT_DIR)/drivers/amba_virt/include \
	-I$(MEM_DIR)/inc \
	-I$(MEM_DIR)/src \
	-I$(NNCTRL_DIR)/inc \
	-I$(NNCTRL_DIR)/src \
	-I$(NNCTRL_DIR)/dep/ambacnn

LDFLAGS = -lpthread -lm

SRCS = cavalry_native_demo.c \
	$(MEM_DIR)/src/cavalry_mem.c \
	$(wildcard $(NNCTRL_DIR)/src/*.c)

TARGET = cavalry_native_demo

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o
