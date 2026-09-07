# eve-flags.mk
#
# Copyright (C) 2026, Ambarella International LLC

# NOHYPER / EVE host kmod (kmod-host). Not for the Ubuntu HVM.
# Headers from: make -C $(EVE)/build eve-kernel-headers
# Must match uname -r on the board that will insmod this module.
# Override EVE if the eve tree is not a sibling of amba-virt.
#
# Docker builds with O=/kernel-out, source at /kernel-src. kernel-dev.tar is
# that O= tree. kbuild still needs eve-kernel:
#   make -C $(EVE_KERNEL) O=$(KDIR_HOST) modules_prepare
#   make -C $(EVE_KERNEL) O=$(KDIR_HOST) M=... modules
#
# uname -r / linux-headers dir (Makefile.eve LOCALVERSION + eve_defconfig):
#   <KERNEL_TAG without v>-linuxkit-<git12>              CI (BRANCH on cmdline)
#   <KERNEL_TAG without v>-linuxkit-<git12>-<user>       local make
#   <KERNEL_TAG without v>-linuxkit-<git12>-<user>-dirty local + dirty tree

ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
# Userspace for NOHYPER (amba-virt-server). Guest CLI stays native $(CC).
HOST_CC ?= $(CROSS_COMPILE)gcc

_eve_mk := $(dir $(lastword $(MAKEFILE_LIST)))
EVE ?= $(abspath $(_eve_mk)/../../eve)
EVE_KERNEL ?= $(EVE)/eve-kernel
EVE_BUILD ?= $(EVE)/build

ifeq ($(wildcard $(EVE_KERNEL)/Makefile.eve),)
EVE_KVER :=
EVE_KREV :=
else
EVE_KVER := $(patsubst v%,%,$(shell sed -n 's/^KERNEL_TAG=//p' $(EVE_KERNEL)/Makefile.eve))
EVE_KREV := $(shell git -C $(EVE_KERNEL) rev-parse --short=12 HEAD 2>/dev/null)
endif

# Same layout eve-kernel-headers unpacks: usr/src/linux-headers-$(uname -r)
EVE_HDR_PREFIX := $(EVE_BUILD)/usr/src/linux-headers-$(EVE_KVER)-linuxkit-$(EVE_KREV)
KDIR_HOST ?= $(or \
	$(wildcard $(EVE_HDR_PREFIX)), \
	$(wildcard $(EVE_HDR_PREFIX)-$(USER)), \
	$(wildcard $(EVE_HDR_PREFIX)-$(USER)-dirty), \
	$(firstword $(sort $(wildcard $(EVE_HDR_PREFIX)-*))), \
	$(firstword $(sort $(wildcard $(EVE_BUILD)/usr/src/linux-headers-*-linuxkit-*))))
