# QNX 8.0 Resource Manager Makefile for amba_cavalry (/dev/cavalry)
# Requires QNX SDP 8.0 environment (source qnxsdp-env.sh)

QNX_TARGET_ARCH ?= gcc_ntoaarch64le
CC := qcc

INCLUDES := -I../amba-virt/include \
            -I../../../drivers/amba_virt/include/uapi \
            -I../../linux/amba-cavalry/include

CFLAGS ?= -V$(QNX_TARGET_ARCH) -O2 -Wall -Wextra -D_QNX_SOURCE -DAMBA_SOC_N1 $(INCLUDES)
LDFLAGS ?= -V$(QNX_TARGET_ARCH) -L../amba-virt -lamba_virt -lpci -lsocket

TARGET := amba-cavalry-resmgr

.PHONY: all help clean check_compiler

all: $(TARGET)

check_compiler:
	@if ! which $(CC) >/dev/null 2>&1; then \
		echo "Error: QNX compiler '$(CC)' not found in PATH." >&2; \
		echo "Please source your QNX SDP environment: source ~/qnx800/qnxsdp-env.sh" >&2; \
		exit 1; \
	fi

$(TARGET): amba_cavalry_resmgr.c check_compiler
	$(CC) $(CFLAGS) -o $@ amba_cavalry_resmgr.c $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

help:
	@echo "QNX amba-cavalry Resource Manager Build Targets:"
	@echo "  all (default)  Build amba-cavalry-resmgr using QNX qcc"
	@echo "  clean          Remove build artifacts"
	@echo "  help           Show this help message"
