# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Host Daemon Makefile

CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
IAV_CFLAGS := -I../../../build/pristine-dsp_v6/include -I../../../build/pristine-dsp_v6/include/uapi -I../../../build/pristine-dsp_v6/include/uapi/specific
NANOPB_DIR := ../../../third_party/nanopb
PROTO_DIR := ../../../protocol/amba_virt/generated
NANOPB_CFLAGS := -I$(NANOPB_DIR) -I$(PROTO_DIR)

CFLAGS ?= -O2 -Wall -Wextra -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3 $(IAV_CFLAGS) $(NANOPB_CFLAGS)

LDFLAGS ?= -static

TARGETS := amba-virt-server amba-virt-ctl amba-virt-backend iav_frame_tap iav_proxy_test
SERVER_SRCS := amba-virt-server.c cavalry_proxy.c iav_proxy.c virt_mem_pool.c virt_acl.c virt_admin_ipc.c virt_query.c virt_backend_client.c virt_dma_broker.c hmac_sha256.c
SERVER_OBJS := $(SERVER_SRCS:.c=.o)
NANOPB_OBJS := pb_common.o pb_encode.o pb_decode.o amba_virt.pb.o

CTL_SRCS := amba-virt-ctl.c
CTL_OBJS := $(CTL_SRCS:.c=.o)

BACKEND_SRCS := amba-virt-backend.c
BACKEND_OBJS := $(BACKEND_SRCS:.c=.o)

IAV_CFLAGS := -I../../../build/pristine-dsp_v6/include -I../../../build/pristine-dsp_v6/include/uapi -I../../../build/pristine-dsp_v6/include/uapi/specific

NNCTRL_SRCS := ../../../guest-os/userspace/cavalry_mem/src/cavalry_mem.c \
	$(wildcard ../../../guest-os/userspace/nnctrl/cavalry_v3/src/*.c)

NNCTRL_CFLAGS := -I../../../guest-os/userspace/cavalry_mem/inc \
	-I../../../guest-os/userspace/cavalry_mem/src \
	-I../../../guest-os/userspace/nnctrl/cavalry_v3/inc \
	-I../../../guest-os/userspace/nnctrl/cavalry_v3/src \
	-I../../../guest-os/userspace/nnctrl/cavalry_v3/dep/ambacnn

TESTS := test_virt_mem_pool test_admin_ipc test_virt_query test_virt_driver_matrix test_backend_ipc test_drain_protocol test_boundary_stress test_state_desync test_fuzz_corruption test_security_exploits test_hardware_safety test_virt_dma_acl test_virt_uart test_virt_dma_broker test_hmac_sha256 test_host_iav_proxy

.PHONY: all clean test

all: $(TARGETS)

amba-virt-server: $(SERVER_OBJS) $(NANOPB_OBJS)
	$(CC) $(CFLAGS) $(IAV_CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread -lm

amba-virt-ctl: $(CTL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

amba-virt-backend: $(BACKEND_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread

iav_frame_tap: iav_frame_tap.c
	$(CC) $(CFLAGS) $(IAV_CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread

iav_proxy_test: iav_proxy_test.c iav_proxy.c cavalry_proxy.c virt_mem_pool.c virt_acl.c $(NNCTRL_SRCS) $(NANOPB_OBJS)
	$(CC) $(CFLAGS) $(IAV_CFLAGS) $(NNCTRL_CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread -lm

pb_common.o: $(NANOPB_DIR)/pb_common.c
	$(CC) $(CFLAGS) -c -o $@ $<

pb_encode.o: $(NANOPB_DIR)/pb_encode.c
	$(CC) $(CFLAGS) -c -o $@ $<

pb_decode.o: $(NANOPB_DIR)/pb_decode.c
	$(CC) $(CFLAGS) -c -o $@ $<

amba_virt.pb.o: $(PROTO_DIR)/amba_virt.pb.c
	$(CC) $(CFLAGS) -c -o $@ $<


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
	./test_virt_dma_broker
	./test_hmac_sha256
	./test_host_iav_proxy -v

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

test_virt_dma_broker: test_virt_dma_broker.c virt_dma_broker.c virt_acl.c hmac_sha256.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_hmac_sha256: test_hmac_sha256.c hmac_sha256.c
	gcc $(TEST_CFLAGS) -o $@ $^

HOST_CPPUTEST_SRCS := test/test_main.cxx test/TestHostNv12Geometry.cxx test/TestHostIavProxy.cxx test/TestHostIavPhysicalFrame.cxx
CPPUTEST_CFLAGS := $(TEST_CFLAGS) $(IAV_CFLAGS) $(NANOPB_CFLAGS) -DBUILD_HOST_UNIT_TEST -I. -std=c++17 -fsanitize=address,undefined
CPPUTEST_LDFLAGS := -lCppUTest -lCppUTestExt -lpthread -lm -fsanitize=address,undefined

test_host_iav_proxy: $(HOST_CPPUTEST_SRCS) iav_proxy.c $(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
	g++ $(CPPUTEST_CFLAGS) -o $@ $^ $(CPPUTEST_LDFLAGS)

test_host_multitenant_broadcast: test/test_main.cxx test/TestHostIavMultiTenantBroadcast.cxx iav_proxy.c $(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
	g++ $(CPPUTEST_CFLAGS) -o $@ $^ $(CPPUTEST_LDFLAGS)

cavalry_proxy.host.o: cavalry_proxy.c
	gcc $(TEST_CFLAGS) -I. -fsanitize=address,undefined -c -o $@ $<

test_host_cavalry_eve_sync: test/test_main.cxx test/TestHostCavalryEveSync.cxx cavalry_proxy.host.o
	g++ $(CPPUTEST_CFLAGS) -o $@ $^ $(CPPUTEST_LDFLAGS)

test_host_iav_physical_frame_arm64: $(HOST_CPPUTEST_SRCS) iav_proxy.c $(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
	aarch64-linux-gnu-g++ $(TEST_CFLAGS) $(IAV_CFLAGS) $(NANOPB_CFLAGS) -I. -std=c++17 -static -o $@ $^ -L../../../third_party/cpputest/aarch64/lib -lCppUTest -lCppUTestExt -lpthread -lm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGETS) $(TESTS) test_host_iav_proxy test_host_multitenant_broadcast test_host_cavalry_eve_sync test_host_iav_physical_frame_arm64 *.o


