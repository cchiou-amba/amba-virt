# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Demo Hub Makefile
# Copyright (C) 2026, Ambarella International LLC

OS ?= $(shell if which qcc >/dev/null 2>&1; then echo qnx; else echo linux; fi)

ROOT_DIR ?= $(CURDIR)/../../..
USERSPACE_DIR ?= $(ROOT_DIR)/guest-os/userspace
MEM_DIR ?= $(USERSPACE_DIR)/cavalry_mem
NNCTRL_DIR ?= $(USERSPACE_DIR)/nnctrl/cavalry_v3
CAVALRY_DIR ?= $(ROOT_DIR)/guest-os/linux/amba-cavalry
NANOPB_DIR ?= $(ROOT_DIR)/third_party/nanopb
PROTO_DIR ?= $(ROOT_DIR)/protocol/amba_virt/generated

COMMON_SRCS = \
	$(MEM_DIR)/src/cavalry_mem.c \
	$(wildcard $(NNCTRL_DIR)/src/*.c) \
	$(NANOPB_DIR)/pb_common.c \
	$(NANOPB_DIR)/pb_encode.c \
	$(NANOPB_DIR)/pb_decode.c \
	$(PROTO_DIR)/amba_virt.pb.c

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
	-I$(NNCTRL_DIR)/dep/ambacnn \
	-I$(NANOPB_DIR) \
	-I$(PROTO_DIR)

LDFLAGS := -V$(QNX_TARGET_ARCH) -lsocket -lm
TARGETS := amba_virt_demo
else
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS := -O2 -Wall \
	-DAMBA_AMYOC_BUILD \
	-DAMBA_SOC_N1_655 \
	-I$(ROOT_DIR)/drivers/cavalry/include/cavalry_v3 \
	-I$(ROOT_DIR)/drivers/amba_virt/include \
	-I$(ROOT_DIR)/drivers/amba_virt/include/uapi \
	-I$(ROOT_DIR)/drivers/amba_virt/tools \
	-I$(CAVALRY_DIR)/include \
	-I$(MEM_DIR)/inc \
	-I$(MEM_DIR)/src \
	-I$(NNCTRL_DIR)/inc \
	-I$(NNCTRL_DIR)/src \
	-I$(NNCTRL_DIR)/dep/ambacnn \
	-I$(NANOPB_DIR) \
	-I$(PROTO_DIR)

LDFLAGS := -lpthread -lm
TARGETS := amba_virt_demo amba_virt_demo-musl amba_virt_demo-alpine
endif

.PHONY: all clean test

all: $(TARGETS)

CXX := $(CROSS_COMPILE)g++
CXXFLAGS := $(CFLAGS) -std=c++17

DEMO_CXX_SRCS := AmbaVirtDemo.cxx HttpServer.cxx StreamBroadcaster.cxx YoloGeometry.cxx YoloPostprocess.cxx

amba_virt_demo: $(DEMO_CXX_SRCS) $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lstdc++

amba_virt_demo-musl: $(DEMO_CXX_SRCS) $(COMMON_SRCS)
	$(CC) $(CFLAGS) -static -o $@ $^ $(LDFLAGS) -lstdc++

amba_virt_demo-alpine: amba_virt_demo-musl
	ln -sf $< $@

HOST_CC ?= gcc
HOST_CXX ?= g++

HOST_C_SRCS := $(MEM_DIR)/src/cavalry_mem.c $(wildcard $(NNCTRL_DIR)/src/*.c) \
	$(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
HOST_C_OBJS := $(patsubst %.c, %.host.o, $(HOST_C_SRCS))

HOST_CFLAGS := -O2 -Wall -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 \
	-I. -I$(ROOT_DIR)/drivers/amba_virt/include -I$(ROOT_DIR)/drivers/amba_virt/include/uapi \
	-I$(CAVALRY_DIR)/include -I$(MEM_DIR)/inc -I$(MEM_DIR)/src \
	-I$(NNCTRL_DIR)/inc -I$(NNCTRL_DIR)/src -I$(NNCTRL_DIR)/dep/ambacnn \
	-I$(ROOT_DIR)/drivers/cavalry/include/cavalry_v3 \
	-I$(NANOPB_DIR) -I$(PROTO_DIR) \
	-fsanitize=address,undefined

%.host.o: %.c
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

TEST_CXX_SRCS := test/test_main.cxx \
	test/TestYoloGeometry.cxx \
	test/TestYoloPostprocess.cxx \
	test/TestTargetHardwareInference.cxx \
	test/TestHttpServer.cxx \
	test/TestStreamBroadcaster.cxx \
	test/TestPhysicalSceneAlignment.cxx \
	test/TestNanopbWireRobustness.cxx \
	YoloGeometry.cxx \
	YoloPostprocess.cxx \
	HttpServer.cxx \
	StreamBroadcaster.cxx

TEST_CXX_FLAGS := -O2 -Wall -Wextra -std=c++17 -fsanitize=address,undefined \
	-DAMBA_AMYOC_BUILD \
	-DAMBA_SOC_N1_655 \
	-I. \
	-I$(ROOT_DIR)/drivers/amba_virt/include \
	-I$(ROOT_DIR)/drivers/amba_virt/include/uapi \
	-I$(CAVALRY_DIR)/include \
	-I$(MEM_DIR)/inc \
	-I$(MEM_DIR)/src \
	-I$(NNCTRL_DIR)/inc \
	-I$(NNCTRL_DIR)/src \
	-I$(NNCTRL_DIR)/dep/ambacnn \
	-I$(ROOT_DIR)/drivers/cavalry/include/cavalry_v3 \
	-I$(NANOPB_DIR) -I$(PROTO_DIR)
TEST_LDFLAGS := -lCppUTest -lCppUTestExt -lpthread -lm -fsanitize=address,undefined

test_yolo: $(HOST_C_OBJS) $(TEST_CXX_SRCS)
	$(HOST_CXX) $(TEST_CXX_FLAGS) -o $@ $^ $(TEST_LDFLAGS)

TARGET_C_OBJS := $(patsubst %.c, %.target.o, $(HOST_C_SRCS))

%.target.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_yolo_arm64: $(TARGET_C_OBJS) $(TEST_CXX_SRCS)
	$(CROSS_COMPILE)g++ $(CFLAGS) -DAMBA_HARDWARE_TARGET -std=c++17 -I. -I$(ROOT_DIR)/third_party/cpputest/aarch64/include -static -o $@ $^ -L$(ROOT_DIR)/third_party/cpputest/aarch64/lib -lCppUTest -lCppUTestExt -lpthread -lm

test: test_yolo
	./test_yolo -v

clean:
	rm -f $(TARGETS) test_yolo test_yolo_arm64 *.o
