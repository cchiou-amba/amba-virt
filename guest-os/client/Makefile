# Multi-OS amba-virt-client Makefile (Linux + QNX Neutrino)

OS ?= $(shell if which qcc >/dev/null 2>&1; then echo qnx; else echo linux; fi)

TARGET := amba-virt-client
SRCS := amba-virt-client.cxx
INCLUDES := -I. -Iinclude/uapi

ifeq ($(OS),qnx)
CXX := qcc
CXXFLAGS ?= -Vgcc_ntoarm64le -O2 -Wall -Wextra -std=c++17 $(INCLUDES)
LDFLAGS ?= -Vgcc_ntoarm64le -lCppUTest -lCppUTestExt
else
CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17 $(INCLUDES)
LDFLAGS ?= -lCppUTest -lCppUTestExt -lpthread
endif

.PHONY: all help clean test bench

all: $(TARGET)

$(TARGET): $(SRCS) amba_virt_dev.hxx
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

test: $(TARGET)
	./$(TARGET) -v

bench: $(TARGET)
	./$(TARGET) -bench

clean:
	rm -f $(TARGET) *.o

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
