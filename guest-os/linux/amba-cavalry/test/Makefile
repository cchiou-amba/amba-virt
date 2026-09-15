# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Cavalry HVM Guest Inference Test Makefile
# Copyright (C) 2026, Ambarella International LLC

CROSS_COMPILE ?= aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc

ROOT_DIR ?= $(CURDIR)/../../../..
COOPER_DIR ?= $(ROOT_DIR)/../ambarella/cooper/ambarella

MEM_DIR = $(COOPER_DIR)/boot/fastapp/cavalry/packages/cavalry_mem
NNCTRL_DIR = $(COOPER_DIR)/boot/fastapp/cavalry/packages/nnctrl/cavalry_v3

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

TARGETS = cavalry_hvm_inference \
	cavalry_hvm_dags_denial \
	cavalry_hvm_mt_stress \
	cavalry_hvm_victim_loop \
	cavalry_hvm_path_b_test \
	cavalry_hvm_path_b_security \
	cavalry_hvm_demo

.PHONY: all clean

all: $(TARGETS)

cavalry_hvm_inference: cavalry_hvm_inference.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cavalry_hvm_dags_denial: cavalry_hvm_dags_denial.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cavalry_hvm_mt_stress: cavalry_hvm_mt_stress.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cavalry_hvm_victim_loop: cavalry_hvm_victim_loop.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cavalry_hvm_path_b_test: cavalry_hvm_path_b_test.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cavalry_hvm_path_b_security: cavalry_hvm_path_b_security.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cavalry_hvm_demo: cavalry_hvm_demo.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o
