# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Linux Guest IAV frontend (iav_virt.ko) Makefile
#
# Builds against the HVM kernel headers (KDIR_HVM). Refuses to build against the EVE host kernel.

KDIR_HVM ?= /lib/modules/$(shell uname -r)/build
KDIR ?= $(KDIR_HVM)
PWD  := $(CURDIR)
AMBA_VIRT_DIR ?= $(CURDIR)/../amba-virt

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
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(AMBA_VIRT_DIR)/Module.symvers $(if $(ARCH),ARCH=$(ARCH)) $(if $(CROSS_COMPILE),CROSS_COMPILE=$(CROSS_COMPILE)) modules

clean:
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true; \
	fi
	rm -f *.o *.ko *.mod *.mod.c .*.cmd *.o.d .*.o.d Module.symvers modules.order
	rm -rf .tmp_versions

help:
	@echo "Linux Guest iav_virt.ko Build Targets:"
	@echo "  all (default)  Build iav_virt.ko against KDIR_HVM"
	@echo "  clean          Remove build artifacts"
	@echo
	@echo "Configuration:"
	@echo "  KDIR_HVM:      $(KDIR_HVM)"
