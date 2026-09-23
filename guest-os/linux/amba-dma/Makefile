# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtual Peripheral DMA Guest Driver Makefile
#
# Copyright (C) 2026, Ambarella International LLC
#
# Builds amba_dma.ko against guest Linux kernel headers (KDIR_HVM).

KDIR_HVM ?= /lib/modules/$(shell uname -r)/build
KDIR ?= $(KDIR_HVM)
PWD  := $(CURDIR)

.PHONY: all help clean modules

ARCH ?=
CROSS_COMPILE ?=

all modules:
	@if [ ! -d "$(KDIR)" ]; then \
		echo "kmod: KDIR=$(KDIR) not found (guest linux-headers)." >&2; \
		echo "Set KDIR_HVM=/path/to/hvm/linux-headers" >&2; \
		exit 1; \
	fi
	@if [ -d "$(KDIR)/ambarella" ] || [ -f "$(KDIR)/.config" -a -n "$$(grep -s CONFIG_AMBARELLA $(KDIR)/.config 2>/dev/null)" ]; then \
		echo "kmod: Refusing Ambarella eve-kernel ($(KDIR))." >&2; \
		echo "The HVM runs a standard Linux guest (Ubuntu/Alpine). Set KDIR_HVM to the HVM kernel linux-headers." >&2; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) $(if $(ARCH),ARCH=$(ARCH)) $(if $(CROSS_COMPILE),CROSS_COMPILE=$(CROSS_COMPILE)) modules

clean:
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true; \
	fi
	rm -f *.o *.ko *.mod *.mod.c .*.cmd *.o.d .*.o.d Module.symvers modules.order
	rm -rf .tmp_versions

help:
	@echo "Linux Guest amba_dma.ko Build Targets:"
	@echo "  all (default)  Build amba_dma.ko against KDIR_HVM"
	@echo "  clean          Remove build artifacts"
	@echo "  help           Show this help message"
	@echo
	@echo "Configuration:"
	@echo "  KDIR_HVM:      $(KDIR_HVM)"
