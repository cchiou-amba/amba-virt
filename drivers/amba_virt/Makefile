# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Host Driver Makefile

KDIR ?= $(CURDIR)/../../build/usr/src/linux-headers-*
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

.PHONY: all modules tools clean install modules_install

all modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	$(MAKE) -C tools ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

tools:
	$(MAKE) -C tools ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean 2>/dev/null || true
	$(MAKE) -C tools clean || true
	rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order
	rm -rf .tmp_versions

install modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules_install
