# Makefile
#
# Copyright (C) 2026, Ambarella International LLC

# Two kernels, two KDIRs. Do not share eve-kernel with the HVM.
#
#   KDIR_HVM      Ubuntu HVM (EL1). linux-headers of the kernel that boots
#                 in the VM. Never eve-kernel (wrong vermagic / CONFIG, and
#                 Ambarella cavalry.ko must not load in the HVM).
#   KDIR_NOHYPER  NOHYPER on the EVE host (EL2). From eve-flags.mk (extracted
#                 linux-headers). On the device, override to
#                 /lib/modules/$(uname -r)/build. A mismatch can panic the host.
#
# Local QEMU on a generic kernel:
#   make kmod-nohyper KDIR_NOHYPER=/lib/modules/$(uname -r)/build CROSS_COMPILE=
#
# HVM UAPI lives in this repo (include/uapi/). Do not mount eve-kernel
# into the HVM; copy headers here if an HVM frontend needs them.

ifdef KDIR
$(error KDIR is no longer used. Set KDIR_HVM (Ubuntu HVM) and/or KDIR_NOHYPER (eve-kernel / EVE).)
endif

include eve-flags.mk

KDIR_HVM ?= /lib/modules/$(shell uname -r)/build

PREFIX ?= .
BINDIR := $(PREFIX)/bin
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Iinclude/uapi
CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17 -Iinclude -Iinclude/uapi
CPPUTEST_LDFLAGS ?= -lCppUTest -lCppUTestExt -lpthread

.PHONY: all help build-hvm build-nohyper userspace userspace-hvm userspace-nohyper \
	kmod-hvm kmod-nohyper maybe-kmod-hvm maybe-kmod-nohyper \
	test bench \
	clean clean-hvm clean-nohyper distclean distclean-hvm distclean-nohyper

all: userspace
	@$(MAKE) --no-print-directory maybe-kmod-hvm
	@$(MAKE) --no-print-directory maybe-kmod-nohyper

# Build for the Ubuntu HVM. HVM CppUTest Client + HVM kmod.
build-hvm: userspace-hvm kmod-hvm

# Build or cross-compile the NOHYPER side (eve-flags.mk: ARCH, CROSS_COMPILE, KDIR_NOHYPER).
build-nohyper: userspace-nohyper kmod-nohyper

help:
	@echo "Targets:"
	@echo "  build-hvm         Ubuntu HVM: amba-virt-client (CppUTest) + kmod-hvm"
	@echo "  build-nohyper     NOHYPER / cross: amba-virt-server + kmod-nohyper"
	@echo "  userspace         both userspace binaries"
	@echo "  test              run CppUTest suite on HVM"
	@echo "  bench             run IPC benchmark suite on HVM"
	@echo "  kmod-hvm          amba_virt.ko for the Ubuntu HVM (KDIR_HVM)"
	@echo "  kmod-nohyper      amba_virt.ko for NOHYPER / EVE (KDIR_NOHYPER)"
	@echo "  clean-hvm / distclean-hvm         HVM objects + amba-virt-client"
	@echo "  clean-nohyper / distclean-nohyper NOHYPER objects + amba-virt-server"
	@echo "  all               userspace + whichever kmods have a usable KDIR"
	@echo
	@echo "KDIR_HVM=$(KDIR_HVM)"
	@echo "EVE=$(EVE)"
	@echo "EVE_KERNEL=$(EVE_KERNEL)"
	@echo "KDIR_NOHYPER=$(KDIR_NOHYPER)"
	@echo "ARCH=$(ARCH)  CROSS_COMPILE=$(CROSS_COMPILE)  NOHYPER_CC=$(NOHYPER_CC)"
	@echo
	@echo "HVM: Ubuntu linux-headers. NOHYPER: eve-flags.mk (EVE BaseOS)."

userspace: userspace-hvm userspace-nohyper
userspace-hvm: $(BINDIR)/amba-virt-client
userspace-nohyper: $(BINDIR)/amba-virt-server

$(BINDIR)/amba-virt-client: userspace/hvm/amba-virt-client.cxx include/amba_virt_dev.hxx include/uapi/amba_virt.h include/uapi/amba_virt_test.h
	mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ userspace/hvm/amba-virt-client.cxx $(CPPUTEST_LDFLAGS)

$(BINDIR)/amba-virt-server: userspace/nohyper/amba-virt-server.c include/uapi/amba_virt.h include/uapi/amba_virt_test.h
	mkdir -p $(BINDIR)
	$(NOHYPER_CC) $(CFLAGS) -o $@ userspace/nohyper/amba-virt-server.c

test: $(BINDIR)/amba-virt-client
	$(BINDIR)/amba-virt-client -v

bench: $(BINDIR)/amba-virt-client
	$(BINDIR)/amba-virt-client -bench

# Fail hard: explicit HVM build must not use eve-kernel.
kmod-hvm:
	@if [ ! -d "$(KDIR_HVM)" ]; then \
		echo "kmod-hvm: KDIR_HVM=$(KDIR_HVM) not found." >&2; \
		echo "Install Ubuntu linux-headers or set KDIR_HVM to that HVM kernel." >&2; \
		exit 1; \
	fi
	@if [ -d "$(KDIR_HVM)/ambarella" ]; then \
		echo "kmod-hvm: refusing eve-kernel / Ambarella tree ($(KDIR_HVM))." >&2; \
		echo "The HVM runs Ubuntu. Set KDIR_HVM to that kernel's linux-headers." >&2; \
		exit 1; \
	fi
	$(MAKE) -C kmod/hvm KDIR=$(KDIR_HVM)

# Fail hard: explicit NOHYPER build needs a prepared kbuild tree.
kmod-nohyper:
	@if [ ! -d "$(KDIR_NOHYPER)" ]; then \
		echo "kmod-nohyper: KDIR_NOHYPER=$(KDIR_NOHYPER) not found." >&2; \
		echo "Run 'make eve-kernel-headers' at repository root, or set KDIR_NOHYPER." >&2; \
		echo "On device / local QEMU: KDIR_NOHYPER=/lib/modules/\$$(uname -r)/build" >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(KDIR_NOHYPER)/.config" ] && [ ! -f "$(KDIR_NOHYPER)/include/generated/autoconf.h" ]; then \
		echo "kmod-nohyper: $(KDIR_NOHYPER) is not a prepared kbuild tree (no .config)." >&2; \
		echo "On EVE/NOHYPER: KDIR_NOHYPER=/lib/modules/\$$(uname -r)/build" >&2; \
		echo "From eve-kernel: make ARCH=arm64 modules_prepare, or set EVE_KERNEL." >&2; \
		echo "Local QEMU: make kmod-nohyper KDIR_NOHYPER=/lib/modules/\$$(uname -r)/build" >&2; \
		exit 1; \
	fi
	$(MAKE) -C kmod/nohyper all KDIR=$(KDIR_NOHYPER) KSRC=$(EVE_KERNEL) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

# Used by 'all': skip a side that is missing or the wrong tree.
maybe-kmod-hvm:
	@if [ ! -d "$(KDIR_HVM)" ]; then \
		echo "Skipping kmod-hvm: KDIR_HVM=$(KDIR_HVM) not found (install Ubuntu linux-headers or set KDIR_HVM)."; \
	elif [ -d "$(KDIR_HVM)/ambarella" ]; then \
		echo "Skipping kmod-hvm: $(KDIR_HVM) is eve-kernel. HVM modules need Ubuntu headers (KDIR_HVM)."; \
	else \
		$(MAKE) kmod-hvm; \
	fi

maybe-kmod-nohyper:
	@if [ ! -d "$(KDIR_NOHYPER)" ]; then \
		echo "Skipping kmod-nohyper: KDIR_NOHYPER=$(KDIR_NOHYPER) not found (run 'make eve-kernel-headers' at repository root)."; \
	elif [ ! -f "$(KDIR_NOHYPER)/.config" ] && [ ! -f "$(KDIR_NOHYPER)/include/generated/autoconf.h" ]; then \
		echo "Skipping kmod-nohyper: $(KDIR_NOHYPER) is not prepared (no .config). On device use KDIR_NOHYPER=/lib/modules/\$$(uname -r)/build; for local QEMU use that path on this machine."; \
	else \
		$(MAKE) kmod-nohyper; \
	fi

clean-hvm:
	$(MAKE) -C kmod/hvm clean KDIR=$(KDIR_HVM)
	rm -f $(BINDIR)/amba-virt-client $(BINDIR)/amba-virt-cli

distclean-hvm: clean-hvm
	rm -f kmod/hvm/.*.o.d kmod/hvm/*.o.d
	rmdir $(BINDIR) 2>/dev/null || true

clean-nohyper:
	$(MAKE) -C kmod/nohyper clean KDIR=$(KDIR_NOHYPER) KSRC=$(EVE_KERNEL) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	rm -f $(BINDIR)/amba-virt-server

distclean-nohyper: clean-nohyper
	rm -f kmod/nohyper/.*.o.d kmod/nohyper/*.o.d
	rmdir $(BINDIR) 2>/dev/null || true

clean: clean-hvm clean-nohyper
	rmdir $(BINDIR) 2>/dev/null || true

distclean: distclean-hvm distclean-nohyper
	rm -rf $(BINDIR)

-include local.install.mk
