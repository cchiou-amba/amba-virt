# Makefile for host_mkimage tool
#
# Copyright (C) 2026, Ambarella International LLC

CC ?= gcc
CFLAGS ?= -Wall -O2

all: host_mkimage

host_mkimage: host_mkimage.c crc32.c firmware.h
	$(CC) $(CFLAGS) -o $@ host_mkimage.c crc32.c

clean:
	rm -f host_mkimage *.o

.PHONY: all clean
