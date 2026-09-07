# Makefile
#
# Copyright (C) 2026, Ambarella International LLC

# Two kernels, two KDIRs. Do not share eve-kernel with the guest.
#
#   KDIR_GUEST  Ubuntu HVM (EL1). linux-headers of the kernel that boots
#               in the VM. Never eve-kernel (wrong vermagic / CONFIG, and
#               Ambarella cavalry.ko must not load in the HVM).
#   KDIR_HOST   NOHYPER on the EVE host (EL2). From eve-flags.mk (extracted
#               linux-headers). On the device, override to
#               /lib/modules/$(uname -r)/build. A mismatch can panic the host.
#
# Local QEMU on a generic kernel:
#   make kmod-host KDIR_HOST=/lib/modules/$(uname -r)/build CROSS_COMPILE=
#
# Guest UAPI lives in this repo (include/uapi/). Do not mount eve-kernel
# into the HVM; copy headers here if a guest frontend needs them.

ifdef KDIR
$(error KDIR is no longer used. Set KDIR_GUEST (Ubuntu HVM) and/or KDIR_HOST (eve-kernel / EVE).)
endif

include eve-flags.mk

KDIR_GUEST ?= /lib/modules/$(shell uname -r)/build

PREFIX ?= .
BINDIR := $(PREFIX)/bin
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Iinclude/uapi

.PHONY: all help on-guest on-host userspace userspace-guest userspace-host \
	kmod-guest kmod-host maybe-kmod-guest maybe-kmod-host \
	clean clean-guest clean-host distclean distclean-guest distclean-host

all: userspace
	@$(MAKE) --no-print-directory maybe-kmod-guest
	@$(MAKE) --no-print-directory maybe-kmod-host

# Build on the Ubuntu HVM (native). Guest CLI + guest kmod only.
on-guest: userspace-guest kmod-guest

# Build or cross-compile the host/NOHYPER side (eve-flags.mk: ARCH, CROSS_COMPILE, KDIR_HOST).
on-host: userspace-host kmod-host

help:
	@echo "Targets:"
	@echo "  on-guest      Ubuntu HVM: amba-virt-cli + kmod-guest"
	@echo "  on-host       NOHYPER / cross: amba-virt-server + kmod-host"
	@echo "  userspace     both userspace binaries"
	@echo "  kmod-guest    amba_virt.ko for the Ubuntu HVM (KDIR_GUEST)"
	@echo "  kmod-host     amba_virt.ko for NOHYPER / EVE (KDIR_HOST)"
	@echo "  clean-guest / distclean-guest   HVM objects + amba-virt-cli"
	@echo "  clean-host / distclean-host     host objects + amba-virt-server"
	@echo "  all           userspace + whichever kmods have a usable KDIR"
	@echo
	@echo "KDIR_GUEST=$(KDIR_GUEST)"
	@echo "EVE=$(EVE)"
	@echo "EVE_KERNEL=$(EVE_KERNEL)"
	@echo "KDIR_HOST=$(KDIR_HOST)"
	@echo "ARCH=$(ARCH)  CROSS_COMPILE=$(CROSS_COMPILE)  HOST_CC=$(HOST_CC)"
	@echo
	@echo "Guest: Ubuntu linux-headers. Host: eve-flags.mk (NOHYPER / EVE)."

userspace: userspace-guest userspace-host
userspace-guest: $(BINDIR)/amba-virt-cli
userspace-host: $(BINDIR)/amba-virt-server

$(BINDIR)/amba-virt-cli: userspace/guest/amba-virt-cli.c include/uapi/amba_virt.h
	mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ userspace/guest/amba-virt-cli.c

$(BINDIR)/amba-virt-server: userspace/host/amba-virt-server.c include/uapi/amba_virt.h
	mkdir -p $(BINDIR)
	$(HOST_CC) $(CFLAGS) -o $@ userspace/host/amba-virt-server.c

# Fail hard: explicit guest build must not use eve-kernel.
kmod-guest:
	@if [ ! -d "$(KDIR_GUEST)" ]; then \
		echo "kmod-guest: KDIR_GUEST=$(KDIR_GUEST) not found." >&2; \
		echo "Install Ubuntu linux-headers or set KDIR_GUEST to that guest kernel." >&2; \
		exit 1; \
	fi
	@if [ -d "$(KDIR_GUEST)/ambarella" ]; then \
		echo "kmod-guest: refusing eve-kernel / Ambarella tree ($(KDIR_GUEST))." >&2; \
		echo "The HVM runs Ubuntu. Set KDIR_GUEST to that kernel's linux-headers." >&2; \
		exit 1; \
	fi
	$(MAKE) -C kmod/guest KDIR=$(KDIR_GUEST)

# Fail hard: explicit host build needs a prepared kbuild tree.
kmod-host:
	@if [ ! -d "$(KDIR_HOST)" ]; then \
		echo "kmod-host: KDIR_HOST=$(KDIR_HOST) not found." >&2; \
		echo "Set EVE to the EVE source tree (sibling of amba-virt), then:" >&2; \
		echo "  make -C \$$(EVE)/build eve-kernel-headers" >&2; \
		echo "Or: make on-host EVE=/path/to/eve" >&2; \
		echo "On device / local QEMU: KDIR_HOST=/lib/modules/\$$(uname -r)/build" >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(KDIR_HOST)/.config" ] && [ ! -f "$(KDIR_HOST)/include/generated/autoconf.h" ]; then \
		echo "kmod-host: $(KDIR_HOST) is not a prepared kbuild tree (no .config)." >&2; \
		echo "On EVE/NOHYPER: KDIR_HOST=/lib/modules/\$$(uname -r)/build" >&2; \
		echo "From eve-kernel: make ARCH=arm64 modules_prepare, or set EVE_KERNEL." >&2; \
		echo "Local QEMU: make kmod-host KDIR_HOST=/lib/modules/\$$(uname -r)/build" >&2; \
		exit 1; \
	fi
	$(MAKE) -C kmod/host all KDIR=$(KDIR_HOST) KSRC=$(EVE_KERNEL) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

# Used by 'all': skip a side that is missing or the wrong tree.
maybe-kmod-guest:
	@if [ ! -d "$(KDIR_GUEST)" ]; then \
		echo "Skipping kmod-guest: KDIR_GUEST=$(KDIR_GUEST) not found (install Ubuntu linux-headers or set KDIR_GUEST)."; \
	elif [ -d "$(KDIR_GUEST)/ambarella" ]; then \
		echo "Skipping kmod-guest: $(KDIR_GUEST) is eve-kernel. Guest modules need Ubuntu headers (KDIR_GUEST)."; \
	else \
		$(MAKE) kmod-guest; \
	fi

maybe-kmod-host:
	@if [ ! -d "$(KDIR_HOST)" ]; then \
		echo "Skipping kmod-host: KDIR_HOST=$(KDIR_HOST) not found (set EVE= to the EVE tree; make -C \$$(EVE)/build eve-kernel-headers)."; \
	elif [ ! -f "$(KDIR_HOST)/.config" ] && [ ! -f "$(KDIR_HOST)/include/generated/autoconf.h" ]; then \
		echo "Skipping kmod-host: $(KDIR_HOST) is not prepared (no .config). On device use KDIR_HOST=/lib/modules/\$$(uname -r)/build; for local QEMU use that path on this machine."; \
	else \
		$(MAKE) kmod-host; \
	fi

clean-guest:
	$(MAKE) -C kmod/guest clean KDIR=$(KDIR_GUEST)
	rm -f $(BINDIR)/amba-virt-cli

distclean-guest: clean-guest
	rm -f kmod/guest/.*.o.d kmod/guest/*.o.d
	rmdir $(BINDIR) 2>/dev/null || true

clean-host:
	$(MAKE) -C kmod/host clean KDIR=$(KDIR_HOST) KSRC=$(EVE_KERNEL) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	rm -f $(BINDIR)/amba-virt-server

distclean-host: clean-host
	rm -f kmod/host/.*.o.d kmod/host/*.o.d
	rmdir $(BINDIR) 2>/dev/null || true

clean: clean-guest clean-host
	rmdir $(BINDIR) 2>/dev/null || true

distclean: distclean-guest distclean-host
	rm -rf $(BINDIR)

-include local.install.mk
