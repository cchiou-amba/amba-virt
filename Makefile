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

# Parallel build configuration
MAKE_JOBS = $(shell echo "$(MAKEFLAGS)" | sed -n -E 's/.*-j([0-9]+).*/\1/p')
NCORES ?= $(if $(MAKE_JOBS),$(MAKE_JOBS),$(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1))

EVE_MAKE_DONE = touch $(BUILD_DIR)/.$@.done

LINUXKIT_VERSION := $(shell sed -n 's/^LINUXKIT_VERSION=//p' $(EVE_KERNEL_DIR)/Makefile.eve 2>/dev/null)
LINUXKIT := /tmp/linuxkit-$(LINUXKIT_VERSION)/linuxkit

# Dynamically discover all out-of-tree driver suites in drivers/
OOT_DRIVERS := $(notdir $(patsubst %/,%,$(wildcard $(DRIVERS_DIR)/*/)))

define my-depend
$(if $(wildcard $(BUILD_DIR)/.$(1).done),,$(if $(and $(filter eve-kernel-headers,$(1)),$(wildcard $(BUILD_DIR)/usr/src/linux-headers-*)),,$(if $(and $(filter eve-kernel-keys,$(1)),$(wildcard $(BUILD_DIR)/certs/signing_key.pem)),,$(1))))
endef

V ?= 1
EVE_MAKE = env -u MAKEFLAGS $(MAKE) V=$(V)

.PHONY: all help eve eve-kernel eve-kernel-headers eve-kernel-keys \
	drivers $(OOT_DRIVERS) clean distclean

all: eve drivers

help:
	@echo "Ambarella N1-655 EVE-OS Firmware & Driver Build Targets:"
	@echo
	@echo "  all                 Build full EVE BaseOS image and all drivers (default)"
	@echo "  eve                 Build EVE BaseOS live installer image (depends on drivers)"
	@echo "  drivers             Build and sign all detected out-of-tree drivers"
	@echo "  eve-kernel          Build EVE kernel package via Docker (kernel-gcc)"
	@echo "  eve-kernel-headers  Extract linux-headers and signing keys to build/"
	@echo "  eve-kernel-keys     Alias for extracting signing keys to build/certs/"
	@echo "  <driver-name>       Build and sign a specific driver (e.g. cavalry, amba_otp)"
	@echo "  clean               Clean local build outputs and driver artifacts"
	@echo "  distclean           Full clean of build outputs and EVE system artifacts"
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

eve: $(call my-depend,eve-kernel) drivers
	+$(EVE_MAKE) -C $(EVE_SYSTEM_DIR) NCORES=$(NCORES) ZARCH=arm64 HV=kvm \
		KERNEL_TAG=$$($(MAKE) -C $(EVE_KERNEL_DIR) -s --no-print-directory -f Makefile.eve docker-tag-gcc) live && \
		$(EVE_MAKE_DONE)

eve-kernel:
	@mkdir -p $(BUILD_DIR)
	+$(EVE_MAKE) -C $(EVE_KERNEL_DIR) -f Makefile.eve kernel-gcc && \
		rm -f $(BUILD_DIR)/.eve-kernel-headers.done $(BUILD_DIR)/.drivers.done && \
		$(EVE_MAKE_DONE)

eve-kernel-headers: $(call my-depend,eve-kernel)
	@mkdir -p $(BUILD_DIR)/certs $(BUILD_DIR)/bin
	+@$(EVE_MAKE) -C $(EVE_KERNEL_DIR) -f Makefile.eve linuxkit
	+@TAG=$$($(MAKE) -C $(EVE_KERNEL_DIR) -s --no-print-directory -f Makefile.eve docker-tag-gcc) && \
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

drivers: eve-kernel-headers
	@mkdir -p $(BUILD_DIR)/modules $(BUILD_DIR)/firmware
	@hdr=$$(echo $(BUILD_DIR)/usr/src/linux-headers-*); \
	sign_bin=$(BUILD_DIR)/bin/sign-file; \
	key=$(BUILD_DIR)/certs/signing_key.pem; \
	cert=$(BUILD_DIR)/certs/signing_key.x509; \
	for drv in $(OOT_DRIVERS); do \
		if [ -f "$(DRIVERS_DIR)/$$drv/Makefile" ]; then \
			echo "Building out-of-tree driver: $$drv..."; \
			if [ "$$drv" = "pwr_gpu" ]; then \
				$(MAKE) -C $(DRIVERS_DIR)/$$drv KERNELDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kbuild || exit 1; \
			else \
				$(MAKE) -C $(DRIVERS_DIR)/$$drv KDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
			fi; \
			for ko in $$(find $(DRIVERS_DIR)/$$drv -name "*.ko"); do \
				if [ -x "$$sign_bin" ] && [ -f "$$key" ]; then \
					echo "Signing $$ko with $$(basename $$key)..."; \
					"$$sign_bin" sha256 "$$key" "$$cert" "$$ko"; \
				fi; \
				cp -f "$$ko" $(BUILD_DIR)/modules/; \
			done; \
			for bin in $$(find $(DRIVERS_DIR)/$$drv -name "*.bin"); do \
				cp -f "$$bin" $(BUILD_DIR)/firmware/; \
			done; \
		fi; \
	done
	@if [ -f "scripts/build_kmod_out_of_tree.sh" ]; then \
		./scripts/build_kmod_out_of_tree.sh && \
		cp -f $(BUILD_DIR)/kmod/amba_virt.ko $(BUILD_DIR)/modules/ 2>/dev/null || true; \
	fi
	@$(EVE_MAKE_DONE)
	@echo "Staged modules in $(BUILD_DIR)/modules/:"
	@ls -la $(BUILD_DIR)/modules/

$(OOT_DRIVERS): %: eve-kernel-headers
	@mkdir -p $(BUILD_DIR)/modules $(BUILD_DIR)/firmware
	@hdr=$$(echo $(BUILD_DIR)/usr/src/linux-headers-*); \
	sign_bin=$(BUILD_DIR)/bin/sign-file; \
	key=$(BUILD_DIR)/certs/signing_key.pem; \
	cert=$(BUILD_DIR)/certs/signing_key.x509; \
	if [ -f "$(DRIVERS_DIR)/$@/Makefile" ]; then \
		echo "Building out-of-tree driver: $@..."; \
		if [ "$@" = "pwr_gpu" ]; then \
			$(MAKE) -C $(DRIVERS_DIR)/$@ KERNELDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kbuild || exit 1; \
		else \
			$(MAKE) -C $(DRIVERS_DIR)/$@ KDIR=$$hdr ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules || exit 1; \
		fi; \
		for ko in $$(find $(DRIVERS_DIR)/$@ -name "*.ko"); do \
			if [ -x "$$sign_bin" ] && [ -f "$$key" ]; then \
				echo "Signing $$ko with $$(basename $$key)..."; \
				"$$sign_bin" sha256 "$$key" "$$cert" "$$ko"; \
			fi; \
			cp -f "$$ko" $(BUILD_DIR)/modules/; \
		done; \
		for bin in $$(find $(DRIVERS_DIR)/$@ -name "*.bin"); do \
			cp -f "$$bin" $(BUILD_DIR)/firmware/; \
		done; \
	else \
		echo "[skip] $@: driver source not present in tree"; \
	fi

clean:
	@rm -rf $(BUILD_DIR)/kmod $(BUILD_DIR)/modules $(BUILD_DIR)/firmware \
		$(BUILD_DIR)/certs $(BUILD_DIR)/usr $(BUILD_DIR)/bin $(BUILD_DIR)/.*.done
	@for drv in $(OOT_DRIVERS); do \
		if [ -f "$(DRIVERS_DIR)/$$drv/Makefile" ]; then \
			$(MAKE) -C $(DRIVERS_DIR)/$$drv clean 2>/dev/null || true; \
		fi; \
	done
	@echo "Cleaned build artifacts."

distclean: clean
	@+$(MAKE) -C $(EVE_SYSTEM_DIR) clean 2>/dev/null || true
