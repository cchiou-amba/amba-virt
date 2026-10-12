# Makefile for the IAV reference applications (plan_iav_virtualization_phase2, section 4a).
#
# Static aarch64 executables built from this tree alone, against the IAV UAPI headers in the build tree
# (build/pristine-dsp_v6; not tracked). The same binary runs on the bare board and in a KVM guest.
#
# Copyright (C) 2026, Ambarella International LLC.

CROSS_COMPILE ?= aarch64-linux-gnu-
CXX := $(CROSS_COMPILE)g++
ROOT := ../../..
OUT ?= $(ROOT)/build/iav-ref
IAV := $(ROOT)/build/pristine-dsp_v6/include
CXXFLAGS ?= -O2 -Wall -Wextra -Werror -std=c++11 -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 \
	-I$(IAV) -I$(IAV)/uapi -I$(IAV)/uapi/specific -include stdint.h
LDFLAGS ?= -static

APPS := IavRefStream IavRefCapture IavRefEvents IavRefPipeline IavRefDecode IavRefOverlay
BINS := $(addprefix $(OUT)/,$(APPS))

all: $(BINS)

$(OUT)/%: %.cxx IavRefCommon.hxx
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
