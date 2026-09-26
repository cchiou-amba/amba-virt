#
# Makefile
#
# QNX Neutrino 8.0 AArch64 Serial Driver for Ambarella UART Passthrough (devc-seramb)
#
# Copyright (C) 2026, Ambarella International LLC
#

CC = qcc -Vgcc_ntoaarch64le
CFLAGS = -O2 -Wall -Wextra -D_QNX_SOURCE -I.
LDFLAGS = -lio-char -lsecpol -lsmmu -lsocket

TARGETS = devc-seramb qnx-getty

all: $(TARGETS)

devc-seramb: devc-seramb.o
	$(CC) -o $@ devc-seramb.o $(LDFLAGS)

qnx-getty: qnx-getty.o
	$(CC) -o $@ qnx-getty.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean
