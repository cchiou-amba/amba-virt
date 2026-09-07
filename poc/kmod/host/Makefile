# Makefile
#
# Copyright (C) 2026, Ambarella International LLC

ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(src)/../../include/uapi -I$(src)/../common
obj-m := amba_virt.o
amba_virt-y := amba_virt_host.o amba_virt_core.o
else
# NOHYPER / EVE host kernel. Defaults from ../../eve-flags.mk.
-include $(CURDIR)/../../eve-flags.mk
KDIR ?= $(KDIR_HOST)
KDIR ?= /lib/modules/$(shell uname -r)/build
KSRC ?= $(EVE_KERNEL)
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
PWD  := $(CURDIR)

# Extracted kernel-dev.tar is an O= tree (stub Makefile includes /kernel-src).
# Drive kbuild from eve-kernel and put generated files in KDIR.
ifneq ($(wildcard $(KSRC)/Makefile),)
ifneq ($(abspath $(KSRC)),$(abspath $(KDIR)))
KBUILD_O := O=$(KDIR)
KBUILD_C := $(KSRC)
endif
endif
KBUILD_C ?= $(KDIR)

.PHONY: all prepare clean
.DEFAULT_GOAL := all

all: prepare
	ln -sfn ../common/amba_virt_core.c amba_virt_core.c
	ln -sfn ../common/amba_virt_core.h amba_virt_core.h
	$(MAKE) -C $(KBUILD_C) $(KBUILD_O) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	@if [ -f "$(KDIR)/certs/signing_key.pem" ] && [ -f "$(KDIR)/certs/signing_key.x509" ] && [ -x "$(KDIR)/scripts/sign-file" ]; then \
		echo "Signing amba_virt.ko with $(KDIR)/certs/signing_key.pem..."; \
		$(KDIR)/scripts/sign-file sha256 $(KDIR)/certs/signing_key.pem $(KDIR)/certs/signing_key.x509 amba_virt.ko; \
	fi

# kernel-dev.tar omits Docker host tools (or ships Alpine/musl binaries).
# modules_prepare builds fixdep/modpost with this machine's HOSTCC.
ifneq ($(KBUILD_O),)
prepare:
	@if [ ! -x "$(KDIR)/scripts/mod/modpost" ]; then \
		rel=$$(basename $(KDIR) | sed 's|^linux-headers-||'); \
		$(MAKE) -C $(KBUILD_C) $(KBUILD_O) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) olddefconfig && \
		$(MAKE) -C $(KBUILD_C) $(KBUILD_O) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules_prepare && \
		if [ -n "$$rel" ]; then \
			echo "#define UTS_RELEASE \"$$rel\"" > $(KDIR)/include/generated/utsrelease.h; \
			echo "$$rel" > $(KDIR)/include/config/kernel.release; \
		fi; \
	fi
else
prepare:
endif

clean:
	$(MAKE) -C $(KBUILD_C) $(KBUILD_O) M=$(PWD) clean || true
	rm -f amba_virt_core.c amba_virt_core.h *.o *.ko *.mod *.mod.c .*.cmd *.o.d .*.o.d Module.symvers modules.order
	rm -rf .tmp_versions
endif
