# Makefile
#
# Copyright (C) 2026, Ambarella International LLC

ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(src)/../../include/uapi -I$(src)/../common
obj-m := amba_virt.o
amba_virt-y := amba_virt_guest.o amba_virt_core.o
else
# Ubuntu HVM kernel only. Never eve-kernel (see ../../Makefile KDIR_GUEST).
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(CURDIR)

.PHONY: all clean

all:
	@if [ ! -d "$(KDIR)" ]; then \
		echo "kmod/guest: KDIR=$(KDIR) not found (Ubuntu linux-headers)." >&2; \
		exit 1; \
	fi
	@if [ -d "$(KDIR)/ambarella" ]; then \
		echo "kmod/guest: refusing Ambarella eve-kernel ($(KDIR))." >&2; \
		echo "Build the guest module against the Ubuntu HVM kernel (KDIR_GUEST)." >&2; \
		exit 1; \
	fi
	ln -sfn ../common/amba_virt_core.c amba_virt_core.c
	ln -sfn ../common/amba_virt_core.h amba_virt_core.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean || true
	rm -f amba_virt_core.c amba_virt_core.h *.o *.ko *.mod *.mod.c .*.cmd *.o.d .*.o.d Module.symvers modules.order
	rm -rf .tmp_versions
endif
