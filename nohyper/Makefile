# SPDX-License-Identifier: GPL-2.0
#
# Ambarella NOHYPER target-side applications and test suites
# Copyright (C) 2026, Ambarella International LLC

CROSS_COMPILE ?= aarch64-linux-gnu-

SUBDIRS := cavalry_shm_test cavalry_native_demo

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ CROSS_COMPILE=$(CROSS_COMPILE)

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean 2>/dev/null || true; \
	done
