# QNX 8.0 Resource Manager & Library Makefile for amba_virt
# Requires QNX SDP 8.0 environment (source qnxsdp-env.sh)

QNX_TARGET_ARCH ?= gcc_ntoaarch64le
CC := qcc
AR ?= ntoaarch64-ar
CFLAGS ?= -V$(QNX_TARGET_ARCH) -O2 -Wall -Wextra -Iinclude -I../../../drivers/amba_virt/include/uapi
LDFLAGS ?= -V$(QNX_TARGET_ARCH) -lpci -lsocket

BIN_TARGET := amba-virt-resmgr
LIB_SO_TARGET := libamba_virt.so
LIB_A_TARGET := libamba_virt.a
TARGETS := $(BIN_TARGET) $(LIB_SO_TARGET) $(LIB_A_TARGET)

.PHONY: all help clean

all: $(TARGETS)

check_compiler:
	@if ! which $(CC) >/dev/null 2>&1; then \
		echo "Error: QNX compiler '$(CC)' not found in PATH." >&2; \
		echo "Please source your QNX SDP environment: source ~/qnx800/qnxsdp-env.sh" >&2; \
		exit 1; \
	fi

$(BIN_TARGET): amba_virt_resmgr.c check_compiler
	$(CC) $(CFLAGS) -o $@ amba_virt_resmgr.c $(LDFLAGS)

$(LIB_SO_TARGET): libamba_virt.c include/amba_virt_qnx.h check_compiler
	$(CC) $(CFLAGS) -shared -fPIC -o $@ libamba_virt.c -V$(QNX_TARGET_ARCH)

$(LIB_A_TARGET): libamba_virt.c include/amba_virt_qnx.h check_compiler
	$(CC) $(CFLAGS) -c -o libamba_virt.o libamba_virt.c
	@if which $(AR) >/dev/null 2>&1; then \
		$(AR) rcs $@ libamba_virt.o; \
	else \
		ar rcs $@ libamba_virt.o; \
	fi

clean:
	rm -f $(TARGETS) *.o

help:
	@echo "QNX amba-virt Resource Manager Build Targets:"
	@echo "  all (default)  Build amba-virt-resmgr and libamba_virt using QNX qcc"
	@echo "  clean          Remove build artifacts"
	@echo "  help           Show this help message"
