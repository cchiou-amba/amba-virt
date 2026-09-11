# QNX 8.0 Resource Manager Makefile for amba_virt
# Requires QNX SDP 8.0 environment (source qnxsdp-env.sh)

CC := qcc
CFLAGS ?= -Vgcc_ntoarm64le -O2 -Wall -Wextra -I../../../drivers/amba_virt/include/uapi
LDFLAGS ?= -Vgcc_ntoarm64le -lpci

TARGET := amba-virt-resmgr

.PHONY: all help clean

all: $(TARGET)

$(TARGET): amba_virt_resmgr.c
	@if ! which $(CC) >/dev/null 2>&1; then \
		echo "Error: QNX compiler '$(CC)' not found in PATH." >&2; \
		echo "Please source your QNX SDP environment: source ~/qnx800/qnxsdp-env.sh" >&2; \
		exit 1; \
	fi
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

help:
	@echo "QNX amba-virt Resource Manager Build Targets:"
	@echo "  all (default)  Build amba-virt-resmgr using QNX qcc"
	@echo "  clean          Remove build artifacts"
	@echo "  help           Show this help message"
