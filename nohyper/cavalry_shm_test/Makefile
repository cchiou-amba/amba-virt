# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Cavalry ivshmem shared window validation test
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

SRCS = cavalry_shm_dag_test.c \
	$(MEM_DIR)/src/cavalry_mem.c \
	$(wildcard $(NNCTRL_DIR)/src/*.c)

OBJS = $(SRCS:.c=.o)
TARGET = cavalry_shm_dag_test
PROBE_TARGET = cavalry_host_dvi_probe

PROBE_SRCS = cavalry_host_dvi_probe.c \
	$(MEM_DIR)/src/cavalry_mem.c \
	$(wildcard $(NNCTRL_DIR)/src/*.c)

.PHONY: all clean

all: $(TARGET) $(PROBE_TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(PROBE_TARGET): $(PROBE_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET) $(PROBE_TARGET) *.o

