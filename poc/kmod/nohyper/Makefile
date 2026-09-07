# Makefile
#
# Copyright (C) 2026, Ambarella International LLC

ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(src)/../../include/uapi -I$(src)/../common
obj-m := amba_virt.o
amba_virt-y := amba_virt_nohyper.o amba_virt_core.o
else
# NOHYPER / EVE BaseOS kernel. Defaults from ../../eve-flags.mk.
-include $(CURDIR)/../../eve-flags.mk
KDIR ?= $(KDIR_NOHYPER)
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

KEY_PEM ?= $(firstword $(wildcard $(KDIR)/certs/signing_key.pem $(EVE_BUILD)/certs/signing_key.pem))
KEY_X509 ?= $(firstword $(wildcard $(KDIR)/certs/signing_key.x509 $(EVE_BUILD)/certs/signing_key.x509))
SIGN_FILE ?= $(firstword $(wildcard $(KDIR)/scripts/sign-file $(EVE_BUILD)/usr/src/linux-headers-*/scripts/sign-file))

.PHONY: all prepare clean
.DEFAULT_GOAL := all

all: prepare
	ln -sfn ../common/amba_virt_core.c amba_virt_core.c
	ln -sfn ../common/amba_virt_core.h amba_virt_core.h
	$(MAKE) -C $(KBUILD_C) $(KBUILD_O) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	@if [ -n "$(KEY_PEM)" ] && [ -n "$(KEY_X509)" ] && [ -x "$(SIGN_FILE)" ]; then \
		echo "Signing amba_virt.ko with $(KEY_PEM)..."; \
		$(SIGN_FILE) sha256 $(KEY_PEM) $(KEY_X509) amba_virt.ko; \
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
