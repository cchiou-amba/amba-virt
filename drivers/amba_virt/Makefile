# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Host Driver Makefile

KDIR ?= $(wildcard $(CURDIR)/../../build/usr/src/linux-headers-*)
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

SIGN_KEY ?= $(CURDIR)/../../build/certs/signing_key.pem
SIGN_CERT ?= $(CURDIR)/../../build/certs/signing_key.x509
SIGN_TOOL ?= $(KDIR)/scripts/sign-file

.PHONY: all modules tools clean install modules_install

all modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	@if [ -f "$(SIGN_TOOL)" ] && [ -f "$(SIGN_KEY)" ] && [ -f "$(SIGN_CERT)" ]; then \
		for mod in *.ko; do \
			if [ -f "$$mod" ]; then \
				echo "Signing $$mod"; \
				$(SIGN_TOOL) sha256 $(SIGN_KEY) $(SIGN_CERT) "$$mod"; \
			fi; \
		done; \
	fi
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
