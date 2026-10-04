# protocol/amba_virt/Makefile
#
# Ambarella Virtualization Protocol Foundation Build & Gate.
# Generates, verifies, and tests Nanopb protocol files.
#
# Copyright (C) 2026, Ambarella International LLC.

NANOPB_DIR ?= ../../third_party/nanopb
NANOPB_GEN := $(NANOPB_DIR)/generator/nanopb_generator.py
PYTHON     ?= python3

GEN_DIR    := generated
TEST_DIR   := tests

CC         ?= gcc
CFLAGS     ?= -O2 -Wall -Wextra -Werror -I$(NANOPB_DIR) -I$(GEN_DIR)

NANOPB_SRCS := $(NANOPB_DIR)/pb_common.c \
               $(NANOPB_DIR)/pb_encode.c \
               $(NANOPB_DIR)/pb_decode.c

GEN_SRCS    := $(GEN_DIR)/amba_virt.pb.c
GEN_HDRS    := $(GEN_DIR)/amba_virt.pb.h
TEST_BIN    := $(TEST_DIR)/test_roundtrip

.PHONY: all generate check-generated test clean

all: generate test

generate: $(GEN_SRCS) $(GEN_HDRS)

$(GEN_SRCS) $(GEN_HDRS): amba_virt.proto amba_virt.options
	mkdir -p $(GEN_DIR)
	$(PYTHON) $(NANOPB_GEN) --error-on-unmatched -I . -D $(GEN_DIR) amba_virt.proto

check-generated:
	@mkdir -p /tmp/check_amba_virt_gen
	$(PYTHON) $(NANOPB_GEN) --error-on-unmatched -I . -D /tmp/check_amba_virt_gen amba_virt.proto
	diff -u $(GEN_DIR)/amba_virt.pb.c /tmp/check_amba_virt_gen/amba_virt.pb.c
	diff -u $(GEN_DIR)/amba_virt.pb.h /tmp/check_amba_virt_gen/amba_virt.pb.h
	@rm -rf /tmp/check_amba_virt_gen
	@echo "[PASS] Generated protocol sources match checked-in files exactly."

$(TEST_BIN): $(TEST_DIR)/test_roundtrip.c $(GEN_SRCS) $(NANOPB_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_BIN)
	@./$(TEST_BIN)

clean:
	rm -f $(TEST_BIN)
	rm -rf /tmp/check_amba_virt_gen
