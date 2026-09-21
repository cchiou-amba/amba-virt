# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Cavalry HVM YOLOX Edge AI Service Makefile
# Copyright (C) 2026, Ambarella International LLC

OS ?= $(shell if which qcc >/dev/null 2>&1; then echo qnx; else echo linux; fi)

ROOT_DIR ?= $(CURDIR)/../../..
USERSPACE_DIR ?= $(ROOT_DIR)/guest-os/userspace
MEM_DIR ?= $(USERSPACE_DIR)/cavalry_mem
NNCTRL_DIR ?= $(USERSPACE_DIR)/nnctrl/cavalry_v3
CAVALRY_DIR ?= $(ROOT_DIR)/guest-os/linux/amba-cavalry

COMMON_SRCS = $(MEM_DIR)/src/cavalry_mem.c \
	$(wildcard $(NNCTRL_DIR)/src/*.c)

ifeq ($(OS),qnx)
CC := qcc
QNX_TARGET_ARCH ?= gcc_ntoaarch64le
CFLAGS := -V$(QNX_TARGET_ARCH) -D_QNX_SOURCE -O2 -Wall \
	-DAMBA_AMYOC_BUILD \
	-DAMBA_SOC_N1 \
	-DAMBA_SOC_N1_655 \
	-I$(CAVALRY_DIR)/include \
	-I$(ROOT_DIR)/drivers/amba_virt/include \
	-I$(ROOT_DIR)/drivers/amba_virt/include/uapi \
	-I$(ROOT_DIR)/drivers/cavalry/include/cavalry_v3 \
	-I$(MEM_DIR)/inc \
	-I$(MEM_DIR)/src \
	-I$(NNCTRL_DIR)/inc \
	-I$(NNCTRL_DIR)/src \
	-I$(NNCTRL_DIR)/dep/ambacnn

LDFLAGS := -V$(QNX_TARGET_ARCH) -lsocket -lm
TARGETS := cavalry_hvm_yolo
else
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS := -O2 -Wall \
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

LDFLAGS := -lpthread -lm
TARGETS := cavalry_hvm_yolo cavalry_hvm_yolo-musl cavalry_hvm_yolo-alpine
endif

.PHONY: all clean model

all: $(TARGETS)

cavalry_hvm_yolo: cavalry_hvm_yolo.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cavalry_hvm_yolo-musl: cavalry_hvm_yolo.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -static -o $@ $^ $(LDFLAGS)

cavalry_hvm_yolo-alpine: cavalry_hvm_yolo-musl
	ln -sf $< $@

# YOLOX-S compiled model from Ambarella Hugging Face repository
YOLOX_MODEL = n1-655_yolox_s_amba_optimized.bin
YOLOX_MODEL_URL = https://huggingface.co/Ambarella/YOLOX/resolve/main/$(YOLOX_MODEL)

$(YOLOX_MODEL):
	@echo "Downloading $(YOLOX_MODEL) from Hugging Face..."
	curl -sSL -o $@ $(YOLOX_MODEL_URL)

model: $(YOLOX_MODEL)

clean:
	rm -f $(TARGETS) *.o
