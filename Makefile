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

.PHONY: all help eve eve-kernel eve-kernel-headers eve-kernel-keys \
	drivers $(OOT_DRIVERS) nohyper nohyper-apps everything clean distclean \
	diag test-gdma \
	mode set-mode-development set-mode-production mode-dev mode-prod \
	guest guest-all guest-ubuntu guest-alpine guest-qnx guest-qnx-image \
	guest-windows clean-guest distclean-guest \
	u-boot host-mkimage host_mkimage u-boot-pkg u-boot-package clean-uboot

.DEFAULT_GOAL := all

all: eve $(DRIVER_DEPENDENCY)

nohyper: drivers nohyper-apps

nohyper-apps:
	@if [ -f "$(ROOT_DIR)/nohyper/Makefile" ]; then \
		echo "Building NOHYPER target applications and tests..."; \
		$(MAKE) -C $(ROOT_DIR)/nohyper CROSS_COMPILE=aarch64-linux-gnu- || exit 1; \
	fi

everything: nohyper guest


help:
	@echo "Ambarella N1-655 EVE-OS Firmware & Driver Build Targets:"
	@echo
	@echo "  <empty> / all       Build EVE BaseOS + all NOHYPER drivers and apps (default)"
	@echo "  everything          Build all NOHYPER drivers/apps + all HVM guests"
	@echo "  u-boot              Build U-Boot bootloader (u-boot.bin)"
	@echo "  host-mkimage        Build host firmware packaging tool (host_mkimage)"
	@echo "  u-boot-pkg          Package u-boot.bin into bld.img firmware container"
	@echo "  clean-uboot         Clean U-Boot and host_mkimage build outputs"
	@echo "  guest               Build all HVM guest side artifacts"
	@echo "  guest-ubuntu        Build Ubuntu 24.04 HVM driver & client"
	@echo "  guest-alpine        Build Alpine 3.20 HVM driver & client"
	@echo "  guest-qnx           Build QNX 8.0 HVM resource manager & client"
	@echo "  guest-qnx-image     Build bootable QNX 8.0 QCOW2 disk image"
	@echo "  nohyper             Build all NOHYPER host drivers and apps"
	@echo "  drivers             Build and sign all detected out-of-tree drivers (dev mode)"
	@echo "  diag                Build host diagnostic drivers (diag_stage2_pte, diag_gdma)"
	@echo "  eve                 Build EVE BaseOS live installer image for active mode"
	@echo "  eve-kernel          Build EVE kernel package via Docker ($(EVE_KERNEL_TARGET))"
	@echo "  eve-kernel-headers  Extract linux-headers and signing keys to build/"
	@echo "  eve-kernel-keys     Alias for extracting signing keys to build/certs/"
	@echo "  <driver-name>       Build and sign a specific driver (e.g. cavalry, amba_otp)"
	@echo "  clean-guest         Clean guest build staging (build/guest/)"
	@echo "  distclean-guest     Full clean of guest build cache (including headers)"
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


eve: $(call my-depend,eve-kernel) $(DRIVER_DEPENDENCY)
	+$(EVE_MAKE) -C $(EVE_SYSTEM_DIR) NCORES=$(NCORES) ZARCH=arm64 HV=kvm pkg/storage-init pkg/dom0-ztools pkg/pillar
	+$(EVE_MAKE) -C $(EVE_SYSTEM_DIR) NCORES=$(NCORES) ZARCH=arm64 HV=kvm \
		KERNEL_TAG=$$($(MAKE) -C $(EVE_KERNEL_DIR) -s --no-print-directory \
			-f Makefile.eve BUILD_USER=$(EVE_KERNEL_BUILD_USER) $(EVE_KERNEL_TAG_CMD)) live && \
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
			$(MAKE) -C $(DRIVERS_DIR)/$$drv KERNELDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kbuild || exit 1; \
		elif [ "$$drv" = "amba_otp" ]; then \
			$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/amba_otp/sec_v2) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- EXTRA_CFLAGS="-I$$(realpath $(DRIVERS_DIR)/amba_otp/include) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655" modules || exit 1; \
		elif [ "$$drv" = "ambvideo" ]; then \
			$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/ambvideo/dsp_v6) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
		elif [ "$$drv" = "dsplog" ]; then \
			$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/dsplog) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- EXTRA_CFLAGS="-DAMBA_DSP_ARCH_V6 -DAMBA_SOC_N1_655" KBUILD_EXTRA_SYMBOLS=$$(realpath $(DRIVERS_DIR)/ambvideo/dsp_v6/Module.symvers) modules || exit 1; \
		elif [ "$$drv" = "pci_platform" ]; then \
			$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/pci_platform) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
		elif [ -f "$(DRIVERS_DIR)/$$drv/Makefile" ]; then \
			$(MAKE) -C $(DRIVERS_DIR)/$$drv KDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
		fi; \
		for ko in $$(find $(DRIVERS_DIR)/$$drv/ -name "*.ko"); do \
			if [ -x "$$sign_bin" ] && [ -f "$$key" ]; then \
				echo "Signing $$ko with $$(basename $$key)..."; \
				"$$sign_bin" sha256 "$$key" "$$cert" "$$ko"; \
			fi; \
			cp -f "$$ko" $(BUILD_DIR)/modules/; \
		done; \
		for bin in $$(find $(DRIVERS_DIR)/$$drv/ -name "*.bin"); do \
			cp -f "$$bin" $(BUILD_DIR)/firmware/; \
		done; \
		if [ -d "$(DRIVERS_DIR)/$$drv/tools" ]; then \
			if [ -f "$(DRIVERS_DIR)/$$drv/tools/Makefile" ]; then \
				$(MAKE) -C $(DRIVERS_DIR)/$$drv/tools CROSS_COMPILE=aarch64-linux-gnu- || exit 1; \
			fi; \
			for exe in $$(find $(DRIVERS_DIR)/$$drv/tools/ -maxdepth 1 -type f -executable ! -name "*.sh" ! -name "*.o"); do \
				mkdir -p $(BUILD_DIR)/bin; \
				cp -f "$$exe" $(BUILD_DIR)/bin/; \
			done; \
		fi; \
	done
	@$(EVE_MAKE_DONE)
	@echo "Staged modules in $(BUILD_DIR)/modules/:"
	@ls -la $(BUILD_DIR)/modules/

$(OOT_DRIVERS): %: $(call my-depend,eve-kernel-headers)
	@mkdir -p $(BUILD_DIR)/modules $(BUILD_DIR)/firmware
	@hdr=$$(echo $(BUILD_DIR)/usr/src/linux-headers-*); \
	sign_bin=$(BUILD_DIR)/bin/sign-file; \
	key=$(BUILD_DIR)/certs/signing_key.pem; \
	cert=$(BUILD_DIR)/certs/signing_key.x509; \
	echo "Building out-of-tree driver: $@..."; \
	if [ "$@" = "pwr_gpu" ]; then \
		$(MAKE) -C $(DRIVERS_DIR)/$@ KERNELDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kbuild || exit 1; \
	elif [ "$@" = "amba_otp" ]; then \
		$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/amba_otp/sec_v2) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- EXTRA_CFLAGS="-I$$(realpath $(DRIVERS_DIR)/amba_otp/include) -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655" modules || exit 1; \
	elif [ "$@" = "ambvideo" ]; then \
		$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/ambvideo/dsp_v6) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
	elif [ "$@" = "dsplog" ]; then \
		$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/dsplog) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- EXTRA_CFLAGS="-DAMBA_DSP_ARCH_V6 -DAMBA_SOC_N1_655" KBUILD_EXTRA_SYMBOLS=$$(realpath $(DRIVERS_DIR)/ambvideo/dsp_v6/Module.symvers) modules || exit 1; \
	elif [ "$@" = "pci_platform" ]; then \
		$(MAKE) -C $$hdr M=$$(realpath $(DRIVERS_DIR)/pci_platform) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
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
	@if [ -f "$(ROOT_DIR)/nohyper/Makefile" ]; then \
		$(MAKE) -C $(ROOT_DIR)/nohyper clean 2>/dev/null || true; \
	fi
	@echo "Cleaned build artifacts."

distclean: clean distclean-guest
	@+$(MAKE) -C $(EVE_SYSTEM_DIR) clean 2>/dev/null || true

