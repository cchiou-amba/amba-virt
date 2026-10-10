# Top-level Makefile for Ambarella EVE BaseOS and out-of-tree drivers
#
# Copyright (C) 2026, Ambarella International LLC

ROOT_DIR := $(CURDIR)
BUILD_DIR := $(ROOT_DIR)/build
EVE_SYSTEM_DIR ?= $(or $(wildcard $(ROOT_DIR)/eve),$(wildcard $(ROOT_DIR)/../eve))
EVE_KERNEL_DIR ?= $(or $(wildcard $(ROOT_DIR)/eve-kernel),$(wildcard $(ROOT_DIR)/../eve-kernel))
DRIVERS_DIR    ?= $(or $(wildcard $(ROOT_DIR)/drivers),$(wildcard $(ROOT_DIR)/../drivers))
BOOT_DIR       ?= $(or $(wildcard $(ROOT_DIR)/boot),$(wildcard $(ROOT_DIR)/../boot))
EVE_DIR        ?= $(EVE_SYSTEM_DIR)
EVE_KERNEL_BUILD_USER ?= custom

# Parallel build configuration
MAKE_JOBS = $(shell echo "$(MAKEFLAGS)" | sed -n -E 's/.*-j([0-9]+).*/\1/p')
NCORES ?= $(if $(MAKE_JOBS),$(MAKE_JOBS),$(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1))

EVE_MAKE_DONE = touch $(BUILD_DIR)/.$@.done

LINUXKIT_VERSION := $(shell sed -n 's/^LINUXKIT_VERSION=//p' $(EVE_KERNEL_DIR)/Makefile.eve 2>/dev/null)
LINUXKIT := /tmp/linuxkit-$(LINUXKIT_VERSION)/linuxkit

MODE_FILE := $(ROOT_DIR)/.mode
MODE ?= $(shell cat $(MODE_FILE) 2>/dev/null || echo development)

# Mode configuration: development (default) vs. production
ifeq ($(filter prod production,$(MODE)),)
    CURRENT_MODE := development
    EVE_KERNEL_TARGET := kernel-gcc
    EVE_KERNEL_TAG_CMD := docker-tag-gcc
    DRIVER_DEPENDENCY := drivers
    MODE_DESC := DEVELOPMENT (Host Out-of-Tree / Rapid Iteration)
else
    CURRENT_MODE := production
    EVE_KERNEL_TARGET := kernel-ambarella
    EVE_KERNEL_TAG_CMD := docker-tag-ambarella
    DRIVER_DEPENDENCY :=
    MODE_DESC := PRODUCTION (Hermetic In-Tree / Zero-Trust Appliance)
endif

OOT_DRIVERS := $(filter-out private platform,$(notdir $(patsubst %/,%,$(wildcard $(DRIVERS_DIR)/*/))))
ifeq ($(wildcard $(DRIVERS_DIR)/ambvideo),)
    OOT_DRIVERS := $(filter-out dsplog,$(OOT_DRIVERS))
endif

CURRENT_KERNEL_TAG = $(shell $(MAKE) -C $(EVE_KERNEL_DIR) -s --no-print-directory \
	-f Makefile.eve BUILD_USER=$(EVE_KERNEL_BUILD_USER) $(EVE_KERNEL_TAG_CMD) 2>/dev/null)

define my-depend
$(if $(and $(filter eve-kernel,$(1)),$(filter-out $(shell cat $(BUILD_DIR)/.eve-kernel.done 2>/dev/null),$(CURRENT_KERNEL_TAG))),$(1),$(if $(wildcard $(BUILD_DIR)/.$(1).done),,$(if $(and $(filter eve-kernel-headers,$(1)),$(wildcard $(BUILD_DIR)/usr/src/linux-headers-*)),,$(if $(and $(filter eve-kernel-keys,$(1)),$(wildcard $(BUILD_DIR)/certs/signing_key.pem)),,$(1)))))
endef

V ?= 1
EVE_MAKE = env -u MAKEFLAGS $(MAKE) V=$(V)

# TLS-intercepting proxy support; all no-ops when tools/misc/ca holds no
# certificates. See tools/misc/ca/README.md. linuxkit resolves ADD <git-url>
# and ADD <https-url> inside its own buildkit container rather than in a build
# stage, so it needs an image that trusts them. LINUXKIT_BUILDER_VERSION must
# track the default reported by 'linuxkit pkg build --help'.
CA_CERTS_DIR ?= $(ROOT_DIR)/tools/misc/ca
CA_CERTS := $(wildcard $(CA_CERTS_DIR)/*.crt)
LINUXKIT_BUILDER_VERSION ?= v0.26.3
ifneq ($(CA_CERTS),)
export LINUXKIT_BUILDER_IMAGE := moby/buildkit:$(LINUXKIT_BUILDER_VERSION)-localca
endif

.PHONY: linuxkit-ca-image
linuxkit-ca-image:
ifneq ($(CA_CERTS),)
	@$(CA_CERTS_DIR)/mkbuildkit.sh $(LINUXKIT_BUILDER_VERSION) $(LINUXKIT_BUILDER_IMAGE)
endif

.PHONY: all help eve eve-kernel eve-kernel-headers eve-kernel-keys \
	amba-virt-image \
	drivers camera-drivers $(OOT_DRIVERS) everything clean distclean \
	diag test-gdma \
	mode set-mode-development set-mode-production mode-dev mode-prod \
	guest guest-all guest-ubuntu guest-ubuntu-image guest-ubuntu-install \
	guest-alpine guest-qnx guest-qnx-image \
	guest-windows clean-guest distclean-guest \
	u-boot host-mkimage host_mkimage u-boot-pkg u-boot-package clean-uboot

.DEFAULT_GOAL := all

all: eve $(DRIVER_DEPENDENCY)

everything: drivers guest


help:
	@echo "Ambarella N1-655 EVE-OS Firmware & Driver Build Targets:"
	@echo
	@echo "  <empty> / all       Build EVE BaseOS + all out-of-tree drivers (default)"
	@echo "  everything          Build all out-of-tree drivers + all HVM guests"
	@echo "  u-boot              Build U-Boot bootloader (u-boot.bin)"
	@echo "  host-mkimage        Build host firmware packaging tool (host_mkimage)"
	@echo "  u-boot-pkg          Package u-boot.bin into bld.img firmware container"
	@echo "  clean-uboot         Clean U-Boot and host_mkimage build outputs"
	@echo "  guest               Build all HVM guest side artifacts"
	@echo "  guest-ubuntu        Build Ubuntu 24.04 HVM driver & client"
	@echo "  guest-ubuntu-image  Build lean Ubuntu 24.04 ARM64 HVM QCOW2 image"
	@echo "  guest-ubuntu-install Deploy & qualify Ubuntu 24.04 HVM image on EVE nodes"
	@echo "  guest-alpine        Build Alpine 3.20 HVM driver & client"
	@echo "  guest-qnx           Build QNX 8.0 HVM resource manager & client"
	@echo "  guest-qnx-image     Build bootable QNX 8.0 QCOW2 disk image"
	@echo "  drivers             Build and sign all detected out-of-tree drivers (dev mode)"
	@echo "  diag                Build host diagnostic drivers (diag_stage2_pte, diag_gdma)"
	@echo "  eve                 Build EVE BaseOS live installer image for active mode"
	@echo "  amba-virt-image     Build the amba-virt Dom0 init layer (eve/pkg/amba-virt)"
	@echo "  eve-kernel          Build EVE kernel package via Docker ($(EVE_KERNEL_TARGET))"
	@echo "  eve-kernel-headers  Extract linux-headers and signing keys to build/"
	@echo "  eve-kernel-keys     Alias for extracting signing keys to build/certs/"
	@echo "  <driver-name>       Build and sign a specific driver (e.g. cavalry, amba_otp)"
	@echo "  clean-guest         Clean guest build staging (build/guest/)"
	@echo "  distclean-guest     Full clean of guest build cache (including headers)"
	@echo "  deploy-guest        Deploy guest artifacts (TARGET=<host-alias>)"
	@echo "  deploy-hvm-ubuntu   Deploy & reload Ubuntu HVM (TARGET=n1-655-*-ubuntu)"
	@echo "  deploy-hvm-qnx      Deploy & reload QNX 8.0 HVM (TARGET=n1-655-*-qnx)"
	@echo "  clean               Clean local build outputs and driver artifacts"
	@echo "  distclean           Full clean of build outputs and EVE system artifacts"

	@echo
	@echo "Active Configuration:"
	@echo "  Mode:           $(CURRENT_MODE)"
	@echo "  Kernel Target:  $(EVE_KERNEL_TARGET)"
	@echo "  Driver Build:   $(if $(DRIVER_DEPENDENCY),Host out-of-tree (make drivers),In-tree inside Docker)"
	@echo
	@echo "Detected Out-of-Tree Drivers:"
	@echo "  $(if $(OOT_DRIVERS),$(OOT_DRIVERS),<none detected>)"
	@echo
	@echo "Paths & Options:"
	@echo "  ROOT_DIR:       $(ROOT_DIR)"
	@echo "  BUILD_DIR:      $(BUILD_DIR)"
	@echo "  EVE_SYSTEM_DIR: $(EVE_SYSTEM_DIR)"
	@echo "  EVE_KERNEL_DIR: $(EVE_KERNEL_DIR)"
	@echo "  DRIVERS_DIR:    $(DRIVERS_DIR)"
	@echo "  BOOT_DIR:       $(BOOT_DIR)"
	@echo "  NCORES:         $(NCORES)"

mode:
	@echo "EVE Compile Mode: $(CURRENT_MODE)"
	@echo "  Kernel:    $(EVE_KERNEL_TARGET)"
	@if [ "$(CURRENT_MODE)" = "development" ]; then \
		echo "  Drivers:   Host out-of-tree (make drivers -> build/modules/)"; \
		echo "  Storage:   Target node '/persist/modules/' (deployed via SSH)"; \
		echo "  Signing:   Persistent host key (build/certs/signing_key.pem)"; \
		echo "  Workflow:  Fast turnaround (~3s reload via deploy_and_insmod.sh)"; \
		echo "  Switch:    make set-mode-production"; \
	else \
		echo "  Drivers:   In-tree inside Docker BuildKit using driver contexts"; \
		echo "  Storage:   Sealed inside 'rootfs.img' (/lib/modules/<ver>/extra/)"; \
		echo "  Signing:   Ephemeral 4096-bit RSA key inside Docker (zero-trust)"; \
		echo "  Workflow:  Hermetic appliance image (make eve -> OTA update)"; \
		echo "  Switch:    make set-mode-development"; \
	fi

set-mode-development:
	@echo "development" > $(MODE_FILE)
	@rm -f $(BUILD_DIR)/.eve-kernel.done $(BUILD_DIR)/.eve.done $(BUILD_DIR)/.drivers.done
	@echo "Mode set to 'development' (host out-of-tree drivers on /persist)."
	@echo "Run 'make all' to build BaseOS and drivers."

set-mode-production:
	@echo "production" > $(MODE_FILE)
	@rm -f $(BUILD_DIR)/.eve-kernel.done $(BUILD_DIR)/.eve.done $(BUILD_DIR)/.drivers.done
	@echo "Mode set to 'production' (hermetic in-tree drivers in rootfs.img)."
	@echo "Run 'make eve' to build production BaseOS image."

mode-dev: set-mode-development
mode-prod: set-mode-production

# Dom0 payload, the eve/pkg/amba-virt init layer: amba-virt-server and amba-virt-ctl, the glibc
# they and the camera userspace run on under /usr/lib/amba-virt/lib, the camera userspace, firmware
# and assets (guest-os/userspace/amba-virt-camera), the /etc/amba-virt configs, the signed modules
# the configs name, and /etc/init.d/021-amba-virt-server. rootfs/ is generated and gitignored, so
# the git-tree tag linuxkit would compute never changes with it: the package is force-built under
# a tag derived from rootfs/ itself, which 'eve' passes to the rootfs as AMBAVIRT_TAG.
AMBA_VIRT_CAMERA   := $(ROOT_DIR)/guest-os/userspace/amba-virt-camera
AMBA_VIRT_TOOLS    := $(DRIVERS_DIR)/amba_virt/tools
AMBA_VIRT_ROOTFS   := $(EVE_SYSTEM_DIR)/pkg/amba-virt/rootfs
AMBA_VIRT_LIBDIR   := /usr/lib/amba-virt/lib
AMBA_VIRT_TAG_FILE := $(BUILD_DIR)/amba-virt-pkg.tag
AMBA_VIRT_SYSROOT_LIBS := ld-linux-aarch64.so.1 libc.so.6 libm.so.6

.PHONY: amba-virt-image
amba-virt-image: linuxkit-ca-image $(DRIVER_DEPENDENCY)
	@test -f $(AMBA_VIRT_CAMERA)/Makefile || { echo "Error: $(AMBA_VIRT_CAMERA) is not checked out" >&2; exit 1; }
	@test -f $(BUILD_DIR)/modules/amba_virt.ko || { echo "Error: no signed modules in $(BUILD_DIR)/modules (run make drivers)" >&2; exit 1; }
	rm -f $(AMBA_VIRT_TOOLS)/amba-virt-server $(AMBA_VIRT_TOOLS)/amba-virt-ctl
	$(MAKE) -C $(AMBA_VIRT_TOOLS) CROSS_COMPILE=aarch64-linux-gnu- amba-virt-server amba-virt-ctl
	rm -rf $(AMBA_VIRT_ROOTFS)
	$(MAKE) -C $(AMBA_VIRT_CAMERA) CROSS_COMPILE=aarch64-linux-gnu- install DESTDIR=$(AMBA_VIRT_ROOTFS)
	install -m 0755 $(AMBA_VIRT_TOOLS)/amba-virt-server $(AMBA_VIRT_TOOLS)/amba-virt-ctl $(AMBA_VIRT_ROOTFS)/usr/bin/
	set -e; for l in $(AMBA_VIRT_SYSROOT_LIBS); do \
		src=$$(readlink -f $$(aarch64-linux-gnu-gcc -print-file-name=$$l)); \
		install -m 0755 $$src $(AMBA_VIRT_ROOTFS)$(AMBA_VIRT_LIBDIR)/$$l; \
		so=$$(aarch64-linux-gnu-readelf -dW $$src | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p'); \
		id=$$(aarch64-linux-gnu-readelf -nW $$src | sed -n 's/.*Build ID: *//p'); \
		pkg=$$(dpkg-query -S $$src 2>/dev/null | cut -d: -f1 | head -1); \
		printf '%s sha256=%s soname=%s build-id=%s origin=%s license=-\n' \
			$(patsubst /%,%,$(AMBA_VIRT_LIBDIR))/$$l $$(sha256sum < $$src | cut -c1-64) $${so:--} $${id:--} \
			$${pkg:-toolchain}:$$src >> $(AMBA_VIRT_ROOTFS)/usr/share/amba-virt/PREBUILTS; \
	done
	set -e; d=$(AMBA_VIRT_ROOTFS)/etc/amba-virt; install -d $$d; \
	printf '%s\n' '# Loaded by amba-virt-server when it starts, before any guest connects.' \
		'ambcma.ko ama_enable=1 dsp_buf_size=0x40000000' 'cavalry.ko' 'amba_virt.ko' > $$d/modules.conf; \
	for c in early-modules late-modules camera; do install -m 0644 $(AMBA_VIRT_TOOLS)/$$c.conf.example $$d/$$c.conf; done; \
	rel=$$(/sbin/modinfo -F vermagic $(BUILD_DIR)/modules/amba_virt.ko | cut -d' ' -f1); \
	m=$(AMBA_VIRT_ROOTFS)/lib/modules/$$rel/extra; install -d $$m; \
	for k in $$(cat $$d/modules.conf $$d/early-modules.conf $$d/late-modules.conf | sed 's/#.*//' | awk 'NF {print $$1}' | sort -u); do \
		install -m 0644 $(BUILD_DIR)/modules/$$k $$m/$$k; \
	done
	install -d $(AMBA_VIRT_ROOTFS)/etc/init.d
	ln -s /usr/bin/amba-virt-server $(AMBA_VIRT_ROOTFS)/etc/init.d/021-amba-virt-server
	set -e; hash=$$(tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -C $(AMBA_VIRT_ROOTFS) -cf - . | sha256sum | cut -c1-40); \
	$(EVE_MAKE) -C $(EVE_SYSTEM_DIR) ZARCH=arm64 HV=kvm FORCE_BUILD=--force EVE_HASH=$$hash pkg/amba-virt; \
	cd $(EVE_SYSTEM_DIR) && build-tools/bin/linuxkit pkg show-tag --hash $$hash pkg/amba-virt > $(AMBA_VIRT_TAG_FILE)
	@echo "amba-virt Dom0 layer: $$(cat $(AMBA_VIRT_TAG_FILE))"

eve: amba-virt-image linuxkit-ca-image $(call my-depend,eve-kernel) $(DRIVER_DEPENDENCY)
	$(EVE_MAKE) -C $(EVE_SYSTEM_DIR) NCORES=$(NCORES) ZARCH=arm64 HV=kvm pkg/storage-init pkg/dom0-ztools pkg/pillar pkg/grub pkg/mkimage-raw-efi pkg/mkrootfs-squash
	$(EVE_MAKE) -C $(EVE_SYSTEM_DIR) NCORES=$(NCORES) ZARCH=arm64 HV=kvm \
		KERNEL_TAG=$(if $(CURRENT_KERNEL_TAG),$(CURRENT_KERNEL_TAG),$$($(MAKE) -C $(EVE_KERNEL_DIR) -s --no-print-directory \
			-f Makefile.eve BUILD_USER=$(EVE_KERNEL_BUILD_USER) $(EVE_KERNEL_TAG_CMD))) \
		AMBAVIRT_TAG=$$(cat $(AMBA_VIRT_TAG_FILE)) live && \
		$(EVE_MAKE_DONE)


eve-kernel:
	@mkdir -p $(BUILD_DIR)
	+$(EVE_MAKE) -C $(EVE_KERNEL_DIR) -f Makefile.eve \
		BUILD_USER=$(EVE_KERNEL_BUILD_USER) $(EVE_KERNEL_TARGET) && \
		rm -f $(BUILD_DIR)/.eve-kernel-headers.done $(BUILD_DIR)/.drivers.done && \
		$(MAKE) -C $(EVE_KERNEL_DIR) -s --no-print-directory \
			-f Makefile.eve BUILD_USER=$(EVE_KERNEL_BUILD_USER) \
			$(EVE_KERNEL_TAG_CMD) > $(BUILD_DIR)/.eve-kernel.done

eve-kernel-headers: $(call my-depend,eve-kernel)
	@mkdir -p $(BUILD_DIR)/certs $(BUILD_DIR)/bin
	+@$(EVE_MAKE) -C $(EVE_KERNEL_DIR) -f Makefile.eve \
		BUILD_USER=$(EVE_KERNEL_BUILD_USER) linuxkit
	+@TAG=$$($(MAKE) -C $(EVE_KERNEL_DIR) -s --no-print-directory \
		-f Makefile.eve BUILD_USER=$(EVE_KERNEL_BUILD_USER) $(EVE_KERNEL_TAG_CMD)) && \
		TAG=$${TAG#docker.io/} && \
		CLEAN_TAG=$$(echo "$$TAG" | sed 's/-dirty//') && \
		rm -f $(BUILD_DIR)/kernel-dev.tar && rm -rf $(BUILD_DIR)/usr/src/linux-headers-* && \
		echo "Exporting kernel headers and signing keys from linuxkit cache ($$TAG)..." && \
		( $(LINUXKIT) cache export --arch arm64 --format filesystem --outfile - $$TAG 2>/dev/null || \
		  $(LINUXKIT) cache export --arch arm64 --format filesystem --outfile - $$CLEAN_TAG ) | \
			tar -C $(BUILD_DIR) -xf - kernel-dev.tar && \
		tar -C $(BUILD_DIR) -xf $(BUILD_DIR)/kernel-dev.tar && \
		rm -f $(BUILD_DIR)/kernel-dev.tar && \
		hdr=$$(echo $(BUILD_DIR)/usr/src/linux-headers-*) && \
		rel=$$(basename $$hdr | sed 's|^linux-headers-||') && \
		printf '%s\n' \
			"# Redirect Docker O=/kernel-out stub to the local eve-kernel source." \
			"include $(EVE_KERNEL_DIR)/Makefile" > $$hdr/Makefile && \
		$(MAKE) -C $(EVE_KERNEL_DIR) O=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig && \
		$(MAKE) -C $(EVE_KERNEL_DIR) O=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare && \
		if [ -n "$$rel" ]; then \
			echo "#define UTS_RELEASE \"$$rel\"" > $$hdr/include/generated/utsrelease.h && \
			echo "$$rel" > $$hdr/include/config/kernel.release; \
		fi && \
		if [ -f "$$hdr/certs/signing_key.pem" ]; then \
			cp -a $$hdr/certs/signing_key.* $(BUILD_DIR)/certs/ && \
			chmod 600 $(BUILD_DIR)/certs/signing_key.pem && \
			echo "Extracted build-time signing key and cert to $(BUILD_DIR)/certs/"; \
		fi && \
		if [ -f "$(EVE_KERNEL_DIR)/scripts/sign-file.c" ]; then \
			gcc -Wall -O2 -o $$hdr/scripts/sign-file $(EVE_KERNEL_DIR)/scripts/sign-file.c -lcrypto && \
			cp -f $$hdr/scripts/sign-file $(BUILD_DIR)/bin/sign-file; \
		fi && \
		$(EVE_MAKE_DONE)
	@echo "KDIR_HOST=$$(echo $(BUILD_DIR)/usr/src/linux-headers-*)"

eve-kernel-keys: eve-kernel-headers
	@echo "Signing keys available in $(BUILD_DIR)/certs/"

drivers: $(call my-depend,eve-kernel-headers)
	@mkdir -p $(BUILD_DIR)/modules $(BUILD_DIR)/firmware
	@hdr=$$(echo $(BUILD_DIR)/usr/src/linux-headers-*); \
	sign_bin=$(BUILD_DIR)/bin/sign-file; \
	key=$(BUILD_DIR)/certs/signing_key.pem; \
	cert=$(BUILD_DIR)/certs/signing_key.x509; \
	for drv in $(OOT_DRIVERS); do \
		echo "Building out-of-tree driver: $$drv..."; \
		if [ "$$drv" = "pwr_gpu" ]; then \
			if [ -d "$(DRIVERS_DIR)/$$drv" ]; then \
				$(MAKE) -C $(DRIVERS_DIR)/$$drv KERNELDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kbuild || exit 1; \
			else \
				echo "[skip] $$drv: driver source not present in tree"; \
			fi; \
		elif [ "$$drv" = "amba_otp" ]; then \
			if [ -d "$(DRIVERS_DIR)/amba_otp/sec_v2" ]; then \
				$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/amba_otp/sec_v2) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- EXTRA_CFLAGS="-I$$(realpath $(DRIVERS_DIR)/amba_otp/include) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655" modules || exit 1; \
			else \
				echo "[skip] $$drv: driver source not present in tree"; \
			fi; \
		elif [ "$$drv" = "ambvideo" ]; then \
			if [ -d "$(DRIVERS_DIR)/ambvideo/dsp_v6" ]; then \
				$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/ambvideo/dsp_v6) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
			else \
				echo "[skip] $$drv: driver source not present in tree"; \
			fi; \
		elif [ "$$drv" = "dsplog" ]; then \
			if [ -d "$(DRIVERS_DIR)/dsplog" -a -f "$(DRIVERS_DIR)/ambvideo/dsp_v6/Module.symvers" ]; then \
				$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/dsplog) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- EXTRA_CFLAGS="-DAMBA_DSP_ARCH_V6 -DAMBA_SOC_N1_655" KBUILD_EXTRA_SYMBOLS=$$(realpath $(DRIVERS_DIR)/ambvideo/dsp_v6/Module.symvers) modules || exit 1; \
			else \
				echo "[skip] $$drv: driver source or ambvideo prerequisite not present in tree"; \
			fi; \
		elif [ "$$drv" = "pci_platform" ]; then \
			if [ -d "$(DRIVERS_DIR)/pci_platform" ]; then \
				$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/pci_platform) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
			else \
				echo "[skip] $$drv: driver source not present in tree"; \
			fi; \
		elif [ -f "$(DRIVERS_DIR)/$$drv/Makefile" ]; then \
			$(MAKE) -C $(DRIVERS_DIR)/$$drv KDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
		else \
			echo "[skip] $$drv: Makefile not found"; \
		fi; \
		for ko in $$(find $(DRIVERS_DIR)/$$drv/ -name "*.ko" 2>/dev/null); do \
			if [ -x "$$sign_bin" ] && [ -f "$$key" ]; then \
				echo "Signing $$ko with $$(basename $$key)..."; \
				"$$sign_bin" sha256 "$$key" "$$cert" "$$ko"; \
			fi; \
			cp -f "$$ko" $(BUILD_DIR)/modules/; \
		done; \
		for bin in $$(find $(DRIVERS_DIR)/$$drv/ -name "*.bin" 2>/dev/null); do \
			cp -f "$$bin" $(BUILD_DIR)/firmware/; \
		done; \
		if [ -d "$(DRIVERS_DIR)/$$drv/tools" ]; then \
			if [ -f "$(DRIVERS_DIR)/$$drv/tools/Makefile" ]; then \
				$(MAKE) -C $(DRIVERS_DIR)/$$drv/tools CROSS_COMPILE=aarch64-linux-gnu- || exit 1; \
			fi; \
			for exe in $$(find $(DRIVERS_DIR)/$$drv/tools/ -maxdepth 1 -type f -executable ! -name "*.sh" ! -name "*.o" 2>/dev/null); do \
				mkdir -p $(BUILD_DIR)/bin; \
				cp -f "$$exe" $(BUILD_DIR)/bin/; \
			done; \
		fi; \
	done
	@$(MAKE) --no-print-directory camera-drivers
	@$(EVE_MAKE_DONE)
	@echo "Staged modules in $(BUILD_DIR)/modules/:"
	@ls -la $(BUILD_DIR)/modules/

# Camera chain: VIN/VOUT monitor, sensor bridge core, MAX96712 deserializer, OS08A10 sensor,
# and the R9611 MIPI-DSI LCD bridge. They live under drivers/private and drivers/platform,
# which OOT_DRIVERS skips, and each one links against the symbols of the one before it.
CAMERA_DSP := $(DRIVERS_DIR)/private/video/dsp_v6
CAMERA_VIN := $(DRIVERS_DIR)/platform/vin
CAMERA_CFLAGS := -DAMBA_DSP_ARCH_V6 -DAMBA_SOC_N1_655 -I$(CAMERA_DSP)/include \
	-I$(CAMERA_DSP)/include/driver -I$(CAMERA_DSP)/include/driver/specific \
	-I$(CAMERA_DSP)/include/uapi -I$(CAMERA_DSP)/include/uapi/specific \
	-I$(DRIVERS_DIR)/private/vin_vout_monitor

# $(1) source dir, $(2) extra cflags, $(3) Module.symvers files, $(4) module name
camera-module = echo "Building camera-chain driver: $(4)..." && \
	$(MAKE) -C $$hdr M=$(1) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- clean && \
	$(MAKE) -C $$hdr M=$(1) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
		EXTRA_CFLAGS="$(CAMERA_CFLAGS) $(2)" KBUILD_EXTRA_SYMBOLS="$(3)" modules && \
	echo "Signing $(1)/$(4).ko with $$(basename $$key)..." && \
	"$$sign_bin" sha256 "$$key" "$$cert" $(1)/$(4).ko && \
	cp -f $(1)/$(4).ko $(BUILD_DIR)/modules/

camera-drivers: $(call my-depend,eve-kernel-headers)
	@mkdir -p $(BUILD_DIR)/modules
	@hdr=$$(echo $(BUILD_DIR)/usr/src/linux-headers-*); \
	sign_bin=$(BUILD_DIR)/bin/sign-file; \
	key=$(BUILD_DIR)/certs/signing_key.pem; \
	cert=$(BUILD_DIR)/certs/signing_key.x509; \
	if [ ! -x "$$sign_bin" ] || [ ! -f "$$key" ]; then \
		echo "camera-drivers: sign-file or signing key missing in $(BUILD_DIR)" >&2; exit 1; \
	fi; \
	if [ ! -f "$(CAMERA_DSP)/Module.symvers" ]; then \
		echo "camera-drivers: $(CAMERA_DSP)/Module.symvers missing; build ambvideo first" >&2; exit 1; \
	fi; \
	$(call camera-module,$(DRIVERS_DIR)/private/vin_vout_monitor,,$(CAMERA_DSP)/Module.symvers,vio_monitor) && \
	$(call camera-module,$(CAMERA_VIN)/bridges/ambrg,-I$(CAMERA_VIN)/bridges/ambrg,$(CAMERA_DSP)/Module.symvers $(DRIVERS_DIR)/private/vin_vout_monitor/Module.symvers,ambrg) && \
	$(call camera-module,$(CAMERA_VIN)/bridges/maxim_96712,-I$(CAMERA_VIN)/bridges/ambrg -I$(CAMERA_VIN)/bridges/maxim_96712,$(CAMERA_DSP)/Module.symvers $(DRIVERS_DIR)/private/vin_vout_monitor/Module.symvers $(CAMERA_VIN)/bridges/ambrg/Module.symvers,max96712) && \
	$(call camera-module,$(CAMERA_VIN)/sensors/omnivision_os08a10_mipi_brg,-I$(CAMERA_VIN)/bridges/ambrg -I$(CAMERA_VIN)/bridges/maxim_96712 -I$(CAMERA_VIN)/sensors/omnivision_os08a10_mipi_brg,$(CAMERA_DSP)/Module.symvers $(DRIVERS_DIR)/private/vin_vout_monitor/Module.symvers $(CAMERA_VIN)/bridges/ambrg/Module.symvers,os08a10_mipi_brg) && \
	$(call camera-module,$(DRIVERS_DIR)/platform/vout/amblcd/mipi_dsi_lcd_r9611,-I$(CAMERA_DSP)/dsp/include -I$(CAMERA_DSP)/iav/include,$(CAMERA_DSP)/Module.symvers,lcd_r9611)

$(OOT_DRIVERS): %: $(call my-depend,eve-kernel-headers)
	@mkdir -p $(BUILD_DIR)/modules $(BUILD_DIR)/firmware
	@hdr=$$(echo $(BUILD_DIR)/usr/src/linux-headers-*); \
	sign_bin=$(BUILD_DIR)/bin/sign-file; \
	key=$(BUILD_DIR)/certs/signing_key.pem; \
	cert=$(BUILD_DIR)/certs/signing_key.x509; \
	echo "Building out-of-tree driver: $@..."; \
	if [ "$@" = "pwr_gpu" ]; then \
		if [ -d "$(DRIVERS_DIR)/$@" ]; then \
			$(MAKE) -C $(DRIVERS_DIR)/$@ KERNELDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kbuild || exit 1; \
		else \
			echo "[skip] $@: driver source not present in tree"; \
		fi; \
	elif [ "$@" = "amba_otp" ]; then \
		if [ -d "$(DRIVERS_DIR)/amba_otp/sec_v2" ]; then \
			$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/amba_otp/sec_v2) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- EXTRA_CFLAGS="-I$$(realpath $(DRIVERS_DIR)/amba_otp/include) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655" modules || exit 1; \
		else \
			echo "[skip] $@: driver source not present in tree"; \
		fi; \
	elif [ "$@" = "ambvideo" ]; then \
		if [ -d "$(DRIVERS_DIR)/ambvideo/dsp_v6" ]; then \
			$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/ambvideo/dsp_v6) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
		else \
			echo "[skip] $@: driver source not present in tree"; \
		fi; \
	elif [ "$@" = "dsplog" ]; then \
		if [ -d "$(DRIVERS_DIR)/dsplog" -a -f "$(DRIVERS_DIR)/ambvideo/dsp_v6/Module.symvers" ]; then \
			$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/dsplog) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- EXTRA_CFLAGS="-DAMBA_DSP_ARCH_V6 -DAMBA_SOC_N1_655" KBUILD_EXTRA_SYMBOLS=$$(realpath $(DRIVERS_DIR)/ambvideo/dsp_v6/Module.symvers) modules || exit 1; \
		else \
			echo "[skip] $@: driver source or ambvideo prerequisite not present in tree"; \
		fi; \
	elif [ "$@" = "pci_platform" ]; then \
		if [ -d "$(DRIVERS_DIR)/pci_platform" ]; then \
			$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/pci_platform) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
		else \
			echo "[skip] $@: driver source not present in tree"; \
		fi; \
	elif [ -f "$(DRIVERS_DIR)/$@/Makefile" ]; then \
		$(MAKE) -C $(DRIVERS_DIR)/$@ KDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
	else \
		echo "[skip] $@: driver source not present in tree"; \
	fi; \
	for ko in $$(find $(DRIVERS_DIR)/$@/ -name "*.ko"); do \
		if [ -x "$$sign_bin" ] && [ -f "$$key" ]; then \
			echo "Signing $$ko with $$(basename $$key)..."; \
			"$$sign_bin" sha256 "$$key" "$$cert" "$$ko"; \
		fi; \
		cp -f "$$ko" $(BUILD_DIR)/modules/; \
	done; \
	for bin in $$(find $(DRIVERS_DIR)/$@/ -name "*.bin"); do \
		cp -f "$$bin" $(BUILD_DIR)/firmware/; \
	done; \
	if [ -d "$(DRIVERS_DIR)/$@/tools" ]; then \
		if [ -f "$(DRIVERS_DIR)/$@/tools/Makefile" ]; then \
			$(MAKE) -C $(DRIVERS_DIR)/$@/tools CROSS_COMPILE=aarch64-linux-gnu- || exit 1; \
		fi; \
		for exe in $$(find $(DRIVERS_DIR)/$@/tools/ -maxdepth 1 -type f -executable ! -name "*.sh" ! -name "*.o"); do \
			mkdir -p $(BUILD_DIR)/bin; \
			cp -f "$$exe" $(BUILD_DIR)/bin/; \
		done; \
	fi

test-gdma: diag

guest: guest-all

guest-all:
	@$(ROOT_DIR)/guest-os/build_guest.sh --distro=all

guest-ubuntu:
	@$(ROOT_DIR)/guest-os/build_guest.sh --distro=ubuntu

guest-ubuntu-image:
	@$(ROOT_DIR)/guest-os/ubuntu/ubuntu-build/build.sh

guest-ubuntu-install:
	@if [ -z "$$ZCLI_TOKEN" ]; then \
		echo "Error: ZCLI_TOKEN is unset in environment. Export ZCLI_TOKEN before running." >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(ROOT_DIR)/guest-os/ubuntu/ubuntu-build/output/dist/ubuntu-24.04-arm64-cloudimg.qcow2" ]; then \
		echo "Error: Final QCOW2 image not found at guest-os/ubuntu/ubuntu-build/output/dist/ubuntu-24.04-arm64-cloudimg.qcow2." >&2; \
		echo "Run 'make guest-ubuntu-image' first." >&2; \
		exit 1; \
	fi
	@$(ROOT_DIR)/guest-os/ubuntu/ubuntu-build/deploy.sh

guest-alpine:
	@$(ROOT_DIR)/guest-os/build_guest.sh --distro=alpine

guest-qnx:
	@$(ROOT_DIR)/guest-os/build_guest.sh --distro=qnx

guest-qnx-image:
	@$(ROOT_DIR)/guest-os/build_guest.sh --distro=qnx --qnx-image

guest-windows:
	@$(ROOT_DIR)/guest-os/windows/windows-build/build.sh

clean-guest:
	@$(ROOT_DIR)/guest-os/build_guest.sh --clean

distclean-guest:
	@$(ROOT_DIR)/guest-os/build_guest.sh --distclean

deploy-guest:
	@$(ROOT_DIR)/guest-os/deploy_guest.sh $(TARGET)

deploy-hvm-ubuntu:
	@$(ROOT_DIR)/guest-os/deploy_guest.sh $(or $(TARGET),n1-655-devkit-ubuntu) --reload --enable-serial --test

deploy-hvm-qnx:
	@$(ROOT_DIR)/guest-os/deploy_guest.sh $(or $(TARGET),n1-655-devkit-qnx) --reload --enable-serial --test

# ==============================================================================
# U-Boot Bootloader & Host Firmware Packaging Targets
# ==============================================================================

UBOOT_DIR        ?= $(BOOT_DIR)/u-boot
UBOOT_DEFCONFIG  ?= ambarella_n1_655_cooper_pro_clusters_defconfig
UBOOT_DTB        ?= $(or $(wildcard $(ROOT_DIR)/plan/extracted.dtb),$(wildcard $(EVE_KERNEL_DIR)/arch/arm64/boot/dts/ambarella/n1_655.dtb))
UBOOT_BIN        := $(UBOOT_DIR)/u-boot.bin
HOST_MKIMAGE_DIR := $(ROOT_DIR)/tools/host_mkimage
HOST_MKIMAGE_BIN := $(BUILD_DIR)/bin/host_mkimage

host-mkimage: host_mkimage
host_mkimage:
	@mkdir -p $(BUILD_DIR)/bin
	@if [ -d "$(HOST_MKIMAGE_DIR)" ]; then \
		$(MAKE) -C $(HOST_MKIMAGE_DIR) CC=gcc CFLAGS="-Wall -O2" || exit 1; \
		cp -f $(HOST_MKIMAGE_DIR)/host_mkimage $(HOST_MKIMAGE_BIN); \
		echo "Built host_mkimage -> $(HOST_MKIMAGE_BIN)"; \
	else \
		echo "Error: $(HOST_MKIMAGE_DIR) does not exist" >&2; exit 1; \
	fi

u-boot:
	@mkdir -p $(BUILD_DIR)/bin
	@if [ -z "$(UBOOT_DTB)" ] || [ ! -f "$(UBOOT_DTB)" ]; then \
		echo "Building device tree blob in eve-kernel..."; \
		$(MAKE) -C $(EVE_KERNEL_DIR) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- dtbs || exit 1; \
	fi
	@echo "Configuring U-Boot with $(UBOOT_DEFCONFIG)..."
	+$(MAKE) -C $(UBOOT_DIR) ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- $(UBOOT_DEFCONFIG)
	@echo "Building U-Boot with EXT_DTB=$(if $(wildcard $(UBOOT_DTB)),$(UBOOT_DTB),$(EVE_KERNEL_DIR)/arch/arm64/boot/dts/ambarella/n1_655.dtb)..."
	+$(MAKE) -C $(UBOOT_DIR) ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- EXT_DTB=$(if $(wildcard $(UBOOT_DTB)),$(UBOOT_DTB),$(EVE_KERNEL_DIR)/arch/arm64/boot/dts/ambarella/n1_655.dtb) -j$(NCORES)
	@cp -f $(UBOOT_BIN) $(BUILD_DIR)/bin/u-boot.bin
	@echo "U-Boot build complete: $(BUILD_DIR)/bin/u-boot.bin"

u-boot-pkg: u-boot-package
u-boot-package: u-boot host_mkimage
	@mkdir -p $(BUILD_DIR)/firmware
	@$(HOST_MKIMAGE_BIN) -n bld -f "force raw" -l -1 -j -1 -i $(UBOOT_BIN) $(BUILD_DIR)/firmware/bld.img
	@echo "Packaged U-Boot -> $(BUILD_DIR)/firmware/bld.img"

clean-uboot:
	+$(MAKE) -C $(UBOOT_DIR) clean 2>/dev/null || true
	@if [ -d "$(HOST_MKIMAGE_DIR)" ]; then \
		$(MAKE) -C $(HOST_MKIMAGE_DIR) clean 2>/dev/null || true; \
	fi
	@rm -f $(BUILD_DIR)/bin/u-boot.bin $(BUILD_DIR)/bin/host_mkimage $(BUILD_DIR)/firmware/bld.img
	@echo "Cleaned U-Boot and host_mkimage artifacts."

clean: clean-guest clean-uboot
	@rm -rf $(BUILD_DIR)/kmod $(BUILD_DIR)/modules $(BUILD_DIR)/firmware \
		$(BUILD_DIR)/certs $(BUILD_DIR)/usr $(BUILD_DIR)/bin \
		$(BUILD_DIR)/guest/ubuntu $(BUILD_DIR)/guest/alpine $(BUILD_DIR)/guest/qnx $(BUILD_DIR)/.*.done
	@for drv in $(OOT_DRIVERS); do \
		if [ -f "$(DRIVERS_DIR)/$$drv/Makefile" ]; then \
			$(MAKE) -C $(DRIVERS_DIR)/$$drv clean 2>/dev/null || true; \
		fi; \
		if [ -f "$(DRIVERS_DIR)/$$drv/tools/Makefile" ]; then \
			$(MAKE) -C $(DRIVERS_DIR)/$$drv/tools clean 2>/dev/null || true; \
		fi; \
	done
	@echo "Cleaned build artifacts."

distclean: clean distclean-guest
	@+$(MAKE) -C $(EVE_SYSTEM_DIR) clean 2>/dev/null || true

