# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Host Daemon Makefile

CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3

TARGETS := amba-virt-server amba-virt-ctl
SERVER_SRCS := amba-virt-server.c cavalry_proxy.c virt_mem_pool.c virt_acl.c virt_admin_ipc.c virt_query.c
SERVER_OBJS := $(SERVER_SRCS:.c=.o)

CTL_SRCS := amba-virt-ctl.c
CTL_OBJS := $(CTL_SRCS:.c=.o)

TESTS := test_virt_mem_pool test_admin_ipc test_virt_query

.PHONY: all clean test

all: $(TARGETS)

amba-virt-server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

amba-virt-ctl: $(CTL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TESTS)
	./test_virt_mem_pool
	./test_admin_ipc
	./test_virt_query

test_virt_mem_pool: test_virt_mem_pool.c virt_mem_pool.c
	gcc -O2 -Wall -Wextra -I../include/uapi -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3 -o $@ $^ -lpthread

test_admin_ipc: test_admin_ipc.c virt_admin_ipc.c virt_acl.c virt_mem_pool.c
	gcc -O2 -Wall -Wextra -I../include/uapi -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3 -o $@ $^ -lpthread

test_virt_query: test_virt_query.c virt_query.c virt_mem_pool.c virt_acl.c
	gcc -O2 -Wall -Wextra -I../include/uapi -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3 -o $@ $^ -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGETS) $(TESTS) *.o

