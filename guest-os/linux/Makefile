# SPDX-License-Identifier: GPL-2.0
#
# guest-os/linux/Makefile
#
# Copyright (C) 2026, Ambarella International LLC
#
# Each directory is an out-of-tree guest module with its own Makefile,
# built the same way as amba-virt, amba-uart, and amba-dma.

MODULES := amba-virt amba-gdma test-gdma amba-cavalry amba-iav amba-uart amba-dma amba-virt-usb

.PHONY: all modules clean $(MODULES)

all modules: $(MODULES)

$(MODULES):
	$(MAKE) -C $@ $(if $(KDIR_HVM),KDIR_HVM=$(KDIR_HVM)) \
		$(if $(ARCH),ARCH=$(ARCH)) \
		$(if $(CROSS_COMPILE),CROSS_COMPILE=$(CROSS_COMPILE)) \
		modules

clean:
	for dir in $(MODULES); do $(MAKE) -C $$dir clean; done
