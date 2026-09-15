# SPDX-License-Identifier: GPL-2.0
#
# Ambarella GDMA Linux HVM guest driver Makefile

KDIR_HVM ?= /lib/modules/$(shell uname -r)/build
KDIR ?= $(KDIR_HVM)
PWD := $(CURDIR)
AMBA_VIRT_DIR ?= $(CURDIR)/../amba-virt

.PHONY: all modules clean

all modules:
	@test -d "$(KDIR)" || { echo "KDIR=$(KDIR) not found" >&2; exit 1; }
	$(MAKE) -C $(KDIR) M=$(PWD) \
		KBUILD_EXTRA_SYMBOLS=$(AMBA_VIRT_DIR)/Module.symvers \
		$(if $(ARCH),ARCH=$(ARCH)) \
		$(if $(CROSS_COMPILE),CROSS_COMPILE=$(CROSS_COMPILE)) modules

clean:
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true; \
	fi
	rm -f *.o *.ko *.mod *.mod.c .*.cmd *.o.d .*.o.d \
		Module.symvers modules.order
	rm -rf .tmp_versions
