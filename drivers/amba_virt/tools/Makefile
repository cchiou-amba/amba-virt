# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Host Daemon Makefile

CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3

LDFLAGS ?= -static

TARGETS := amba-virt-server amba-virt-ctl amba-virt-backend
SERVER_SRCS := amba-virt-server.c cavalry_proxy.c virt_mem_pool.c virt_acl.c virt_admin_ipc.c virt_query.c virt_backend_client.c virt_dma_broker.c
SERVER_OBJS := $(SERVER_SRCS:.c=.o)

CTL_SRCS := amba-virt-ctl.c
CTL_OBJS := $(CTL_SRCS:.c=.o)

BACKEND_SRCS := amba-virt-backend.c
BACKEND_OBJS := $(BACKEND_SRCS:.c=.o)

TESTS := test_virt_mem_pool test_admin_ipc test_virt_query test_virt_driver_matrix test_backend_ipc test_drain_protocol test_boundary_stress test_state_desync test_fuzz_corruption test_security_exploits test_hardware_safety test_virt_dma_acl test_virt_uart test_virt_dma_broker

.PHONY: all clean test

all: $(TARGETS)

amba-virt-server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread

amba-virt-ctl: $(CTL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

amba-virt-backend: $(BACKEND_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread


test: $(TESTS)
	./test_virt_mem_pool
	./test_admin_ipc
	./test_virt_query
	./test_virt_driver_matrix
	./test_backend_ipc
	./test_drain_protocol
	./test_boundary_stress
	./test_state_desync
	./test_fuzz_corruption
	./test_security_exploits
	./test_hardware_safety
	./test_virt_dma_acl
	./test_virt_uart

TEST_CFLAGS := -O2 -Wall -Wextra -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3

test_virt_mem_pool: test_virt_mem_pool.c virt_mem_pool.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_admin_ipc: test_admin_ipc.c virt_admin_ipc.c virt_acl.c virt_mem_pool.c virt_backend_client.c cavalry_proxy.c virt_query.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_virt_query: test_virt_query.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_virt_driver_matrix: test_virt_driver_matrix.c
	gcc $(TEST_CFLAGS) -o $@ $^

test_backend_ipc: test_backend_ipc.c virt_backend_client.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_drain_protocol: test_drain_protocol.c cavalry_proxy.c virt_mem_pool.c virt_acl.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_boundary_stress: test_boundary_stress.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_state_desync: test_state_desync.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_fuzz_corruption: test_fuzz_corruption.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_security_exploits: test_security_exploits.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_hardware_safety: test_hardware_safety.c cavalry_proxy.c virt_mem_pool.c virt_acl.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_virt_dma_acl: test_virt_dma_acl.c
	gcc $(TEST_CFLAGS) -o $@ $^

test_virt_uart: test_virt_uart.c
	gcc $(TEST_CFLAGS) -I../../../ -o $@ $^

test_virt_dma_broker: test_virt_dma_broker.c virt_dma_broker.c virt_acl.c
	$(CC) $(CFLAGS) -static -o $@ $^ -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGETS) $(TESTS) *.o

