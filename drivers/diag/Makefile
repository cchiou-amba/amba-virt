# SPDX-License-Identifier: GPL-2.0
#
# Ambarella host diagnostic and validation kernel modules

KDIR ?= $(CURDIR)/../../build/usr/src/linux-headers-*
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

.PHONY: all modules clean

CAVALRY_SYMVERS := $(wildcard $(CURDIR)/../cavalry/cavalry_v3/Module.symvers)

all modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		$(if $(CAVALRY_SYMVERS),KBUILD_EXTRA_SYMBOLS=$(CAVALRY_SYMVERS)) \
		modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order
	rm -rf .tmp_versions
