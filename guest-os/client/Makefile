# Multi-OS amba-virt-client Makefile (Linux + QNX Neutrino)

OS ?= $(shell if which qcc >/dev/null 2>&1; then echo qnx; else echo linux; fi)

TARGET := amba-virt-client
SRCS := amba-virt-client.cxx
INCLUDES := -I. -Iinclude/uapi

CROSS_COMPILE ?=
STATIC ?= 0

ifeq ($(OS),qnx)
QNX_TARGET_CXX ?= gcc_ntoaarch64le_cxx
CXX := qcc
INCLUDES += -Icompat
CXXFLAGS ?= -V$(QNX_TARGET_CXX) -D_QNX_SOURCE -O2 -Wall -Wextra -std=c++17 $(INCLUDES)
LDFLAGS ?= -V$(QNX_TARGET_CXX)



else
ifneq ($(CROSS_COMPILE),)
CXX := $(CROSS_COMPILE)g++
else
CXX ?= g++
endif

HAVE_CPPUTEST := $(shell if printf '%b\n' '\043include <CppUTest/CommandLineTestRunner.h>' | $(CXX) $(INCLUDES) -E -x c++ - >/dev/null 2>&1; then echo 1; else echo 0; fi)

ifeq ($(HAVE_CPPUTEST),1)
LDFLAGS += -lCppUTest -lCppUTestExt
else
INCLUDES += -Icompat
endif

CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17 $(INCLUDES)
LDFLAGS ?= -lpthread

ifeq ($(STATIC),1)
LDFLAGS += -static
endif
endif



TARGETS := amba-virt-client amba-virt-cli

.PHONY: all help clean test bench

all: $(TARGETS)

amba-virt-client: $(SRCS) amba_virt_dev.hxx
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

amba-virt-cli: amba-virt-cli.cxx amba_virt_dev.hxx
	$(CXX) $(CXXFLAGS) -o $@ amba-virt-cli.cxx -lpthread

test: amba-virt-client
	./amba-virt-client -v

bench: amba-virt-client
	./amba-virt-client -bench

clean:
	rm -f $(TARGETS) *.o

help:
	@echo "amba-virt-client Multi-OS Build Targets:"
	@echo "  all (default)  Build amba-virt-client (OS=$(OS))"
	@echo "  test           Run CppUTest functional test suite"
	@echo "  bench          Run IPC latency and throughput benchmark"
	@echo "  clean          Remove build artifacts"
	@echo "  help           Show this help message"
	@echo
	@echo "Supported OS options:"
	@echo "  make OS=linux  (uses g++ or CROSS_COMPILE=aarch64-linux-gnu-g++)"
	@echo "  make OS=qnx    (uses QNX SDP qcc compiler)"
