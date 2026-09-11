# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Host Daemon Makefile

CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra -I../include/uapi

TARGET := amba-virt-server

.PHONY: all clean

all: $(TARGET)

$(TARGET): amba-virt-server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET) *.o
