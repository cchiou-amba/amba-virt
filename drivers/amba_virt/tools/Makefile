# SPDX-License-Identifier: GPL-2.0
#
# Ambarella Virtualization Host Daemon Makefile

CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
IAV_CFLAGS := -I../../../build/pristine-dsp_v6/include -I../../../build/pristine-dsp_v6/include/uapi -I../../../build/pristine-dsp_v6/include/uapi/specific
NANOPB_DIR := ../../../third_party/nanopb
PROTO_DIR := ../../../protocol/amba_virt/generated
NANOPB_CFLAGS := -I$(NANOPB_DIR) -I$(PROTO_DIR)

CFLAGS ?= -O2 -Wall -Wextra -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I.. -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3 $(IAV_CFLAGS) $(NANOPB_CFLAGS)

LDFLAGS ?= -static

# amba-virt-server dlopen()s libamba-virt-camera.so, and a statically linked glibc cannot do that
# safely, so the server is dynamic. It runs in EVE Dom0, which is musl, against the glibc the image
# carries in /usr/lib/amba-virt/lib.
SERVER_LDFLAGS ?= -Wl,--dynamic-linker=/usr/lib/amba-virt/lib/ld-linux-aarch64.so.1 \
	-Wl,--disable-new-dtags -Wl,-rpath,/usr/lib/amba-virt/lib

TARGETS := amba-virt-server amba-virt-ctl iav_frame_tap iav_proxy_test
SERVER_SRCS := amba-virt-server.c cavalry_proxy.c amba_virt_slice.c iav_proxy.c virt_mem_pool.c virt_acl.c virt_admin_ipc.c virt_slice_lease.c virt_query.c virt_host_ops.c virt_daemon.c virt_ready.c virt_dma_broker.c hmac_sha256.c virt_module_loader.c virt_camera_conf.c virt_camera_policy.c virt_gpio.c virt_camera_pipeline.c iav_broker.c iav_policy.c iav_broker_server.c iav_slice_rpc.c iav_low_pool.c virt_low_lease.c virt_slice_arena.c virt_iav_guest_slice.c virt_pool_inventory.c virt_iav_aperture.c iav_new_parts.c
SERVER_OBJS := $(SERVER_SRCS:.c=.o)
NANOPB_OBJS := pb_common.o pb_encode.o pb_decode.o amba_virt.pb.o

CTL_SRCS := amba-virt-ctl.c
CTL_OBJS := $(CTL_SRCS:.c=.o)

IAV_CFLAGS := -I../../../build/pristine-dsp_v6/include -I../../../build/pristine-dsp_v6/include/uapi -I../../../build/pristine-dsp_v6/include/uapi/specific

NNCTRL_SRCS := ../../../guest-os/userspace/cavalry_mem/src/cavalry_mem.c \
	$(wildcard ../../../guest-os/userspace/nnctrl/cavalry_v3/src/*.c)

NNCTRL_CFLAGS := -I../../../guest-os/userspace/cavalry_mem/inc \
	-I../../../guest-os/userspace/cavalry_mem/src \
	-I../../../guest-os/userspace/nnctrl/cavalry_v3/inc \
	-I../../../guest-os/userspace/nnctrl/cavalry_v3/src \
	-I../../../guest-os/userspace/nnctrl/cavalry_v3/dep/ambacnn

TESTS := test_virt_host_ops test_virt_daemon test_virt_mem_pool test_admin_ipc test_virt_query test_virt_driver_matrix test_virt_module_loader test_virt_camera test_drain_protocol test_boundary_stress test_state_desync test_fuzz_corruption test_security_exploits test_hardware_safety test_virt_dma_acl test_virt_uart test_virt_dma_broker test_hmac_sha256 test_host_iav_proxy test_host_cavalry_eve_sync

IAV_SUPPORT_OUT := ../../../build/iav-policy
IAV_SUPPORT_DUMP := $(IAV_SUPPORT_OUT)/iav_ioctl_dump

.PHONY: test_host_iav_arena_run test_iav_arena_negctl test_host_low_lease_run test_low_lease_negctl
.PHONY: all clean test test_admin_ipc_run test_host_cavalry_eve_sync_run test_host_camera_policy_run test_host_camera_policy_tsan_run test_camera_negctl test_iav_qual_host_loopback_run test_iav_qual_host_loopback_bench_run test_host_iav_broker_tsan_run test_host_iav_broker_run fuzz_iav_broker_run test_iav_broker_negctl amba-virt-server-qual test_iav_qual_host_run test_iav_qual_host_bench_run test_iav_qual_host_negctl test_iav_support_table_run iav-support-table test_iav_support_table_negctl

all: $(TARGETS)

amba-virt-server: $(SERVER_OBJS) $(NANOPB_OBJS)
	$(CC) $(CFLAGS) $(IAV_CFLAGS) $(SERVER_LDFLAGS) -o $@ $^ -lpthread -lm -ldl

amba-virt-ctl: $(CTL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

iav_frame_tap: iav_frame_tap.c
	$(CC) $(CFLAGS) $(IAV_CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread

iav_proxy_test: iav_proxy_test.c iav_proxy.c cavalry_proxy.c ../amba_virt_slice.c virt_mem_pool.c virt_acl.c $(NNCTRL_SRCS) $(NANOPB_OBJS)
	$(CC) $(CFLAGS) $(IAV_CFLAGS) $(NNCTRL_CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread -lm

amba_virt_slice.o: ../amba_virt_slice.c
	$(CC) $(CFLAGS) -c -o $@ $<

pb_common.o: $(NANOPB_DIR)/pb_common.c
	$(CC) $(CFLAGS) -c -o $@ $<

pb_encode.o: $(NANOPB_DIR)/pb_encode.c
	$(CC) $(CFLAGS) -c -o $@ $<

pb_decode.o: $(NANOPB_DIR)/pb_decode.c
	$(CC) $(CFLAGS) -c -o $@ $<

amba_virt.pb.o: $(PROTO_DIR)/amba_virt.pb.c
	$(CC) $(CFLAGS) -c -o $@ $<


test: $(TESTS)
	./test_virt_host_ops
	./test_virt_daemon
	./test_virt_mem_pool
	./test_admin_ipc
	./test_virt_query
	./test_virt_driver_matrix
	./test_virt_module_loader
	./test_virt_camera
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
	./test_host_cavalry_eve_sync
	$(MAKE) test_host_iav_broker_run

TEST_CFLAGS := -O2 -Wall -Wextra -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I.. -I../../../guest-os/linux/amba-cavalry/include -I../../cavalry/include/cavalry_v3

test_virt_mem_pool: test_virt_mem_pool.c virt_mem_pool.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_admin_ipc: test_admin_ipc.c virt_admin_ipc.c virt_slice_lease.c virt_camera_policy.c virt_low_lease.c virt_slice_arena.c virt_pool_inventory.c iav_slice_rpc.c virt_acl.c virt_mem_pool.c virt_host_ops.c hmac_sha256.c cavalry_proxy.c ../amba_virt_slice.c virt_query.c $(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
	gcc $(TEST_CFLAGS) $(NANOPB_CFLAGS) -o $@ $^ -lpthread

test_admin_ipc_run:
	$(MAKE) -B test_admin_ipc
	./test_admin_ipc

test_virt_query: test_virt_query.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c ../amba_virt_slice.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_virt_driver_matrix: test_virt_driver_matrix.c
	gcc $(TEST_CFLAGS) -o $@ $^

# The host-operations unit tests. HOST_OPS_SRC and HOST_OPS_TEST_OUT let a broken copy of
# virt_host_ops.c be built for negative controls.
HOST_OPS_SRC ?= virt_host_ops.c
HOST_OPS_TEST_OUT ?= test_virt_host_ops

$(HOST_OPS_TEST_OUT): test_virt_host_ops.c $(HOST_OPS_SRC) virt_module_loader.c
	gcc $(TEST_CFLAGS) -I. -fsanitize=address,undefined -o $@ $^ -lpthread

# The daemon-mode and readiness tests. DAEMON_SRC and DAEMON_TEST_OUT let a broken copy of
# virt_daemon.c be built for negative controls.
DAEMON_SRC ?= virt_daemon.c
DAEMON_TEST_OUT ?= test_virt_daemon

$(DAEMON_TEST_OUT): test_virt_daemon.c $(DAEMON_SRC) virt_ready.c
	gcc $(TEST_CFLAGS) -I. -fsanitize=address,undefined -o $@ $^ -lpthread

# The same host-operations tests under ThreadSanitizer, for the watcher thread.
test_virt_host_ops_tsan: test_virt_host_ops.c virt_host_ops.c virt_module_loader.c
	gcc $(TEST_CFLAGS) -I. -fsanitize=thread -o $@ $^ -lpthread

test_virt_module_loader: test_virt_module_loader.c virt_module_loader.c virt_host_ops.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_virt_camera: test_virt_camera.c virt_camera_conf.c virt_module_loader.c virt_gpio.c virt_camera_pipeline.c virt_host_ops.c
	gcc $(TEST_CFLAGS) $(IAV_CFLAGS) -fsanitize=address,undefined -o $@ $^ -lpthread -ldl

test_drain_protocol: test_drain_protocol.c cavalry_proxy.c ../amba_virt_slice.c virt_mem_pool.c virt_acl.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_boundary_stress: test_boundary_stress.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c ../amba_virt_slice.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_state_desync: test_state_desync.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c ../amba_virt_slice.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_fuzz_corruption: test_fuzz_corruption.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c ../amba_virt_slice.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_security_exploits: test_security_exploits.c virt_query.c virt_mem_pool.c virt_acl.c cavalry_proxy.c ../amba_virt_slice.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_hardware_safety: test_hardware_safety.c cavalry_proxy.c ../amba_virt_slice.c virt_mem_pool.c virt_acl.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

test_virt_dma_acl: test_virt_dma_acl.c
	gcc $(TEST_CFLAGS) -o $@ $^

test_virt_uart: test_virt_uart.c
	gcc $(TEST_CFLAGS) -I../../../ -o $@ $^

DMA_EP_SRC ?= ../amba_virt_dma.c
DMA_EP_OUT := ../../../build/dma-ep

test_virt_dma_broker: test_virt_dma_broker.c virt_dma_broker.c virt_acl.c hmac_sha256.c $(DMA_EP_SRC)
	gcc $(TEST_CFLAGS) -DBUILD_HOST_UNIT_TEST -fsanitize=address,undefined -o $@ $^ -lpthread

MUT_first_come_ownership_EXPR := s/if \(lease->bound_endpoint_id == AMBA_DMA_ENDPOINT_NONE\)/if (0)/
MUT_guest_chosen_endpoint_EXPR := s/if \(req->endpoint_id != AMBA_DMA_ENDPOINT_NONE &&\n\t    req->endpoint_id != lease->bound_endpoint_id\)/if (0)/

test_dma_ep_negctl:
	test -n "$(MUTANT)"
	test -n "$(MUT_$(MUTANT)_EXPR)"
	rm -rf $(DMA_EP_OUT)/mut-$(MUTANT)
	mkdir -p $(DMA_EP_OUT)/mut-$(MUTANT)
	cp ../amba_virt_dma.c $(DMA_EP_OUT)/mut-$(MUTANT)/amba_virt_dma.c
	perl -0pi -e '$(MUT_$(MUTANT)_EXPR)' $(DMA_EP_OUT)/mut-$(MUTANT)/amba_virt_dma.c
	@if cmp -s ../amba_virt_dma.c $(DMA_EP_OUT)/mut-$(MUTANT)/amba_virt_dma.c; then echo "MUTATION DID NOT APPLY: $(MUTANT)"; exit 99; fi
	gcc $(TEST_CFLAGS) -DBUILD_HOST_UNIT_TEST -fsanitize=address,undefined -o $(DMA_EP_OUT)/mut-$(MUTANT)/test_virt_dma_broker test_virt_dma_broker.c virt_dma_broker.c virt_acl.c hmac_sha256.c $(DMA_EP_OUT)/mut-$(MUTANT)/amba_virt_dma.c -lpthread
	timeout 60 $(DMA_EP_OUT)/mut-$(MUTANT)/test_virt_dma_broker

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
	gcc $(TEST_CFLAGS) -I. -I.. -fsanitize=address,undefined -c -o $@ $<

virt_mem_pool.host.o: virt_mem_pool.c
	gcc $(TEST_CFLAGS) -I. -I.. -fsanitize=address,undefined -c -o $@ $<

amba_virt_slice.host.o: ../amba_virt_slice.c
	gcc $(TEST_CFLAGS) -I. -I.. -fsanitize=address,undefined -c -o $@ $<

virt_slice_lease.host.o: virt_slice_lease.c virt_camera_policy.h
	gcc $(TEST_CFLAGS) -I. -I.. -fsanitize=address,undefined -c -o $@ $<

virt_camera_policy.host.o: virt_camera_policy.c virt_camera_policy.h
	gcc $(TEST_CFLAGS) -I. -I.. -fsanitize=address,undefined -c -o $@ $<

test_host_cavalry_eve_sync: test/test_main.cxx test/TestHostCavalryEveSync.cxx test/TestHostSliceLease.cxx cavalry_proxy.host.o virt_mem_pool.host.o amba_virt_slice.host.o virt_slice_lease.host.o virt_camera_policy.host.o
	g++ $(CPPUTEST_CFLAGS) -o $@ $^ $(CPPUTEST_LDFLAGS)

test_host_cavalry_eve_sync_run:
	$(MAKE) -B test_host_cavalry_eve_sync
	./test_host_cavalry_eve_sync

SLICES_OUT := ../../../build/slices-negctl
SLICES_MUT_truncate_32bit_offset_DEFINE := -DCAVALRY_PROXY_MUTANT_TRUNCATE_32BIT_OFFSET
SLICES_MUT_saturate_32bit_window_DEFINE := -DCAVALRY_PROXY_MUTANT_SATURATE_32BIT_WINDOW

test_slices_negctl: virt_mem_pool.host.o amba_virt_slice.host.o virt_slice_lease.host.o virt_camera_policy.host.o
	test -n "$(MUTANT)"
	test -n "$(SLICES_MUT_$(MUTANT)_DEFINE)"
	rm -rf $(SLICES_OUT)/mut-$(MUTANT)
	mkdir -p $(SLICES_OUT)/mut-$(MUTANT)
	gcc $(TEST_CFLAGS) -I. -I.. $(SLICES_MUT_$(MUTANT)_DEFINE) -fsanitize=address,undefined -c -o $(SLICES_OUT)/mut-$(MUTANT)/cavalry_proxy.host.o cavalry_proxy.c
	g++ $(CPPUTEST_CFLAGS) -o $(SLICES_OUT)/mut-$(MUTANT)/test_host_cavalry_eve_sync test/test_main.cxx test/TestHostCavalryEveSync.cxx test/TestHostSliceLease.cxx $(SLICES_OUT)/mut-$(MUTANT)/cavalry_proxy.host.o virt_mem_pool.host.o amba_virt_slice.host.o virt_slice_lease.host.o virt_camera_policy.host.o $(CPPUTEST_LDFLAGS)
	timeout 60 $(SLICES_OUT)/mut-$(MUTANT)/test_host_cavalry_eve_sync

test_iav_slice_rpc: test_iav_slice_rpc.c iav_slice_rpc.c iav_slice_client.c $(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
	gcc -O1 -g -Wall -Wextra -Werror -I../include/uapi -I. $(NANOPB_CFLAGS) -o $@ $^

test_host_iav_low_pool: test/test_main.cxx test/TestHostIavLowPool.cxx iav_low_pool.c
	gcc -O1 -g -Wall -Wextra -Werror -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 $(IAV_CFLAGS) -I. -include stdint.h -c -o iav_low_pool.host.o iav_low_pool.c
	g++ -O1 -g -Wall -Wextra -Werror -std=c++17 -I. -Itest $(CPPUTEST_CFLAGS) -o $@ test/test_main.cxx test/TestHostIavLowPool.cxx iav_low_pool.host.o $(CPPUTEST_LDFLAGS)

test_host_iav_low_slice: test/test_main.cxx test/TestHostIavLowSlice.cxx ../iav_low_slice.c
	gcc -O1 -g -Wall -Wextra -Werror -I.. -c -o iav_low_slice.host.o ../iav_low_slice.c
	g++ -O1 -g -Wall -Wextra -Werror -std=c++17 -I.. -Itest $(CPPUTEST_CFLAGS) -o $@ test/test_main.cxx test/TestHostIavLowSlice.cxx iav_low_slice.host.o $(CPPUTEST_LDFLAGS)

test_host_low_lease: test/test_main.cxx test/TestHostLowLease.cxx virt_low_lease.c virt_slice_arena.c virt_slice_lease.c virt_camera_policy.c iav_slice_rpc.c virt_host_ops.c $(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
	gcc -O1 -g -Wall -Wextra -Werror -I../include/uapi -I. $(NANOPB_CFLAGS) -c -o virt_low_lease.low.o virt_low_lease.c
	gcc -O1 -g -Wall -Wextra -Werror -I../include/uapi -I. -c -o virt_slice_arena.low.o virt_slice_arena.c
	gcc -O1 -g -Wall -Wextra -Werror -I../include/uapi -I. $(NANOPB_CFLAGS) -c -o virt_slice_lease.low.o virt_slice_lease.c
	gcc -O1 -g -Wall -Wextra -Werror -I../include/uapi -I. -c -o virt_camera_policy.low.o virt_camera_policy.c
	gcc -O1 -g -Wall -Wextra -Werror -I../include/uapi -I. $(NANOPB_CFLAGS) -c -o iav_slice_rpc.low.o iav_slice_rpc.c
	gcc -O1 -g -Wall -Wextra -Werror -I../include/uapi -I. -c -o virt_host_ops.low.o virt_host_ops.c
	gcc -O1 -g -Wall -Wextra -Werror $(NANOPB_CFLAGS) -c -o pb_common.low.o $(NANOPB_DIR)/pb_common.c
	gcc -O1 -g -Wall -Wextra -Werror $(NANOPB_CFLAGS) -c -o pb_encode.low.o $(NANOPB_DIR)/pb_encode.c
	gcc -O1 -g -Wall -Wextra -Werror $(NANOPB_CFLAGS) -c -o pb_decode.low.o $(NANOPB_DIR)/pb_decode.c
	gcc -O1 -g -Wall -Wextra -Werror $(NANOPB_CFLAGS) -c -o amba_virt.pb.low.o $(PROTO_DIR)/amba_virt.pb.c
	g++ -O1 -g -Wall -Wextra -Werror -std=c++17 -I. -I../include/uapi $(CPPUTEST_CFLAGS) -o $@ test/test_main.cxx test/TestHostLowLease.cxx virt_low_lease.low.o virt_slice_arena.low.o virt_slice_lease.low.o virt_camera_policy.low.o iav_slice_rpc.low.o virt_host_ops.low.o pb_common.low.o pb_encode.low.o pb_decode.low.o amba_virt.pb.low.o $(CPPUTEST_LDFLAGS)

iav_low_pool_pin: iav_low_pool_main.c iav_low_pool.c
	$(CC) $(CFLAGS) -static -include stdint.h -o $@ iav_low_pool_main.c iav_low_pool.c

iav_low_pool_show: iav_low_pool_show.c iav_low_pool.c
	$(CC) $(CFLAGS) -static -include stdint.h -o $@ iav_low_pool_show.c iav_low_pool.c

iav_slice_isolation: iav_slice_isolation.c
	$(CC) $(CFLAGS) -static -o $@ iav_slice_isolation.c

test_host_iav_physical_frame_arm64: $(HOST_CPPUTEST_SRCS) iav_proxy.c $(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
	aarch64-linux-gnu-g++ $(TEST_CFLAGS) $(IAV_CFLAGS) $(NANOPB_CFLAGS) -I. -std=c++17 -static -o $@ $^ -L../../../third_party/cpputest/aarch64/lib -lCppUTest -lCppUTestExt -lpthread -lm

# ---- IAV broker host tests (plan_iav_virtualization, Envelope 2) ----
# The broker is C, so it is built with gcc and linked into the CppUTest binary. Sanitizers on.
IAV_BROKER_C := iav_broker.c iav_policy.c virt_iav_guest_slice.c
IAV_BROKER_OUT := ../../../build/iav-broker
IAV_BROKER_CFLAGS := -O1 -g -Wall -Wextra -Werror -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. $(IAV_CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined
IAV_BROKER_CXXFLAGS := -O1 -g -Wall -Wextra -std=c++17 -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. -Itest $(IAV_CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined
IAV_BROKER_TESTS := $(wildcard test/TestHostIavBroker*.cxx)
IAV_BROKER_OBJS := $(addprefix $(IAV_BROKER_OUT)/,$(IAV_BROKER_C:.c=.o))
IAV_BROKER_TEST_BIN := $(IAV_BROKER_OUT)/test_host_iav_broker
# ThreadSanitizer build of the same tests. TSan on this host needs address randomization off.
IAV_BROKER_TSAN_BIN := $(IAV_BROKER_OUT)/test_host_iav_broker_tsan

$(IAV_BROKER_TSAN_BIN): test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_BROKER_C) iav_broker.h iav_policy.h iav_allow_primary.inc $(wildcard test/*.hxx)
	mkdir -p $(IAV_BROKER_OUT)/tsanobj
	gcc -O1 -g -Wall -Wextra -Werror -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. $(IAV_CFLAGS) -fsanitize=thread -c -o $(IAV_BROKER_OUT)/tsanobj/iav_broker.o iav_broker.c
	gcc -O1 -g -Wall -Wextra -Werror -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. $(IAV_CFLAGS) -fsanitize=thread -c -o $(IAV_BROKER_OUT)/tsanobj/iav_policy.o iav_policy.c
	gcc -O1 -g -Wall -Wextra -Werror -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. $(IAV_CFLAGS) -fsanitize=thread -c -o $(IAV_BROKER_OUT)/tsanobj/virt_iav_guest_slice.o virt_iav_guest_slice.c
	g++ -O1 -g -Wall -Wextra -std=c++17 -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. -Itest $(IAV_CFLAGS) -fsanitize=thread -o $@ test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_BROKER_OUT)/tsanobj/iav_broker.o $(IAV_BROKER_OUT)/tsanobj/iav_policy.o $(IAV_BROKER_OUT)/tsanobj/virt_iav_guest_slice.o -lCppUTest -lCppUTestExt -lpthread

test_host_iav_broker_tsan_run: $(IAV_BROKER_TSAN_BIN)
	setarch $(shell uname -m) -R $(IAV_BROKER_TSAN_BIN) -v

# ---- Negative controls: the broker with one deliberate bug must fail the tests ----
# MUTANT names one of the mutations below. Each is a perl substitution on a copy of the source.
# The target fails if the mutation did not change the file (a control that changes nothing proves
# nothing), and otherwise runs the tests, which must fail: a surviving mutant makes this succeed.
MUT_foreign_handle_FILE := iav_broker.c
MUT_foreign_handle_EXPR := s/else if \(s->cid != cid\)/else if (0)/
MUT_stale_epoch_FILE := iav_broker.c
MUT_stale_epoch_EXPR := s/epoch != b->epoch \|\| //
MUT_closed_handle_FILE := iav_broker.c
MUT_closed_handle_EXPR := s/ \|\| s->closing \|\| s->gen != gen/ || s->gen != gen/
MUT_generation_FILE := iav_broker.c
MUT_generation_EXPR := s/ \|\| s->gen != gen\)/)/
MUT_arg_length_FILE := iav_broker.c
MUT_arg_length_EXPR := s/if \(\(e->dir & IAV_DIR_W\) \? arg_len != size : arg_len != 0\)/if (0)/
MUT_policy_denied_FILE := iav_broker.c
MUT_policy_denied_EXPR := s/if \(!\(e->profiles & IAV_PROFILE_BIT\(effective_profile\(b, cid\)\)\)\)/if (0)/
MUT_prot_exec_FILE := iav_broker.c
MUT_prot_exec_EXPR := s/if \(q->cmd & ~\(unsigned int\)\(PROT_READ \| PROT_WRITE\)\)/if (0)/
MUT_ring_once_FILE := iav_broker.c
MUT_ring_once_EXPR := s/limit = 2 \* w->part_len;/limit = w->part_len;/
MUT_no_quota_FILE := iav_broker.c
MUT_no_quota_EXPR := s/cs->sessions >= b->cfg.max_sessions_per_cid/0/
MUT_leak_on_gone_FILE := iav_broker.c
MUT_leak_on_gone_EXPR := s/if \(s->in_use && !s->closing && s->cid == cid\) \{/if (0) {/
MUT_dmabuf_fd_FILE := iav_policy.c
MUT_dmabuf_fd_EXPR := s/if \(q->arg.canvas.yuv_use_dma_buf_fd \|\| q->arg.canvas.me_use_dma_buf_fd \|\|/if (0 \&\& q->arg.canvas.yuv_use_dma_buf_fd || q->arg.canvas.me_use_dma_buf_fd ||/
MUT_poll_unbounded_FILE := iav_broker.c
MUT_poll_unbounded_EXPR := s/q->arg_value > AMBA_VIRT_IAV_POLL_SLICE_MS \? \(int\)AMBA_VIRT_IAV_POLL_SLICE_MS :/0 ? 0 :/
MUT_no_rate_limit_FILE := iav_broker.c
MUT_no_rate_limit_EXPR := s/if \(b->cfg.rate_per_sec == 0\)/if (1)/
MUT_open_race_FILE := iav_broker.c
MUT_open_race_EXPR := s/if \(s->closing\) \{\n\t\t\/\* The guest went away/if (0) {\n\t\t\/* The guest went away/
MUT_cid_table_full_FILE := iav_broker.c
MUT_cid_table_full_EXPR := s/if \(free_slot == NULL\) \{\n\t\t\/\*\n\t\t \* The table is full\./if (0) {\n\t\t\/*\n\t\t * The table is full./
MUT_state_change_allowed_FILE := iav_policy.c
MUT_state_change_allowed_EXPR := s/\{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL, NULL \},/{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL, NULL },\n\t{ IAV_IOC_ENTER_IDLE, BOTH, 0, NULL, NULL },/
MUT_refused_isp_nn_allowed_FILE := iav_policy.c
MUT_refused_isp_nn_allowed_EXPR := s/\{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL, NULL \},/{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL, NULL },\n\t{ IAV_IOC_SET_ISP_NN_CFG, PRIMARY_SET, 0, NULL, NULL },/
MUT_frame_wait_unclamped_FILE := iav_policy.c
MUT_frame_wait_unclamped_EXPR := s/q->arg.frame.time_ms = call->block_max_ms;/;/
MUT_decode_range_unchecked_FILE := iav_policy.c
MUT_decode_range_unchecked_EXPR := s/return off <= limit && len <= limit - off;/return 1;/
MUT_decode_range_wraps_FILE := iav_policy.c
MUT_decode_range_wraps_EXPR := s/return off <= limit && len <= limit - off;/return off + len <= limit;/
MUT_decode_eos_unclamped_FILE := iav_policy.c
MUT_decode_eos_unclamped_EXPR := s/w->timeout_ticks = 255;/;/
MUT_decode_status_leaks_FILE := iav_policy.c
MUT_decode_status_leaks_EXPR := s/st->yuv422_y_addr = 0;/;/
MUT_decode_for_consumers_FILE := iav_policy.c
MUT_decode_for_consumers_EXPR := s/\{ IAV_IOC_DECODE_JPEG, PRIMARY_SET, IAV_F_WINDOW, check_decode_jpeg, NULL \},/{ IAV_IOC_DECODE_JPEG, BOTH, IAV_F_WINDOW, check_decode_jpeg, NULL },/
MUT_decode_post_not_run_FILE := iav_broker.c
MUT_decode_post_not_run_EXPR := s/e->post\(&call\);/;/
MUT_decode_window_for_consumers_FILE := iav_broker.c
MUT_decode_window_for_consumers_EXPR := s/if \(window_primary_only\(w->id\) && effective_profile\(b, cid\) == IAV_PROFILE_CONSUMER\) \{/if (0) {/
MUT_decode_window_unknown_FILE := iav_broker.c
MUT_decode_window_unknown_EXPR := s/call.dec_len = b->windows\[i\].part_len;/;/
MUT_overlay_clut_unchecked_FILE := iav_policy.c
MUT_overlay_clut_unchecked_EXPR := s/ \|\|\n\t\t    !range_ok\(a->clut_addr_offset, OVERLAY_CLUT_BYTES, limit\)//
MUT_overlay_pitch_ignored_FILE := iav_policy.c
MUT_overlay_pitch_ignored_EXPR := s/need = \(uint64_t\)a->pitch \* a->height;/need = a->total_size;/
MUT_overlay_canvas_allowed_FILE := iav_policy.c
MUT_overlay_canvas_allowed_EXPR := s/\tif \(limit == 0\)\n\t\treturn -EPERM;\n\treturn overlay_areas_ok/\treturn overlay_areas_ok/
MUT_overlay_later_areas_unchecked_FILE := iav_policy.c
MUT_overlay_later_areas_unchecked_EXPR := s/i < MAX_NUM_OVERLAY_AREA/i < 1/
MUT_overlay_window_unknown_FILE := iav_broker.c
MUT_overlay_window_unknown_EXPR := s/call.ovl_len = b->windows\[i\].part_len;/;/
MUT_overlay_for_consumers_FILE := iav_policy.c
MUT_overlay_for_consumers_EXPR := s/\{ IAV_IOC_SET_OVERLAY_INSERT, PRIMARY_SET,/{ IAV_IOC_SET_OVERLAY_INSERT, BOTH,/

# Envelope C of plan_iav_usb_yolo: the negotiated wire, window pinning, OP_SLICE admission and the wire size.
MUT_pin_chan_off_FILE := iav_policy.c
MUT_pin_chan_off_EXPR := s/static int check_chan_cfg_pinned\(struct iav_call \*call\)\n\{/static int check_chan_cfg_pinned(struct iav_call *call)\n{\n\tif (call) return 0;/
MUT_pin_resource_off_FILE := iav_policy.c
MUT_pin_resource_off_EXPR := s/if \(have.chan_map != want->chan_map\)/if (0)/
MUT_stale_ignored_FILE := iav_broker.c
MUT_stale_ignored_EXPR := s/b->windows_stale = stale;/b->windows_stale = 0;/
MUT_slice_admission_skipped_FILE := iav_broker.c
MUT_slice_admission_skipped_EXPR := s/if \(virt_iav_guest_slice_admit\(&pool, &req, &dma\) != 0\)\n\t\treturn refuse\(r, EPERM\);/(void)virt_iav_guest_slice_admit(&pool, &req, &dma);/
MUT_wire_unaudited_FILE := iav_broker.c
MUT_wire_unaudited_EXPR := s/\t\tif \(size > e->size && !\(e->flags & IAV_F_WIRE_AUDITED\)\)\n\t\t\tmemset\(buf \+ e->size, 0, size - e->size\);\n\t\tbreak;/\t\tbreak;/
MUT_info_unfiltered_FILE := iav_broker.c
MUT_info_unfiltered_EXPR := s/if \(window_primary_only\(w->id\) && prof == IAV_PROFILE_CONSUMER\)\n\t\t\tcontinue;//
MUT_hello_grants_all_FILE := iav_broker.c
MUT_hello_grants_all_EXPR := s/granted = in.features & \(BROKER_FEATURES \| b->host_features \| \(arena_ok \? AMBA_VIRT_IAV_F_ARENA : 0\) \|\n\t\t\t\t \(aperture_ok \? AMBA_VIRT_IAV_F_APERTURE : 0\)\);/granted = in.features;/

# Envelope D of plan_iav_usb_yolo: the image calls of inventory class W.
MUT_img_scale_phys_FILE := iav_policy.c
MUT_img_scale_phys_EXPR := s/if \(b->use_phys \|\| b->use_dma_buf_fd\)\n\t\treturn -EPERM;\n\tif \(b->pid != IAV_PART_DEC_BSB/if (b->pid != IAV_PART_DEC_BSB/
MUT_img_scale_any_pid_FILE := iav_policy.c
MUT_img_scale_any_pid_EXPR := s/\tif \(b->pid != IAV_PART_DEC_BSB && !is_arena_slot\(call, slot, b->pid\)\)\n\t\treturn -EPERM;\n//
MUT_img_scale_uv_unchecked_FILE := iav_policy.c
MUT_img_scale_uv_unchecked_EXPR := s/if \(uv_bytes && !span_ok\(b->uv_offset, uv_bytes, lo, hi\)\)/if (0)/
MUT_img_scale_sum_wraps_FILE := iav_policy.c
MUT_img_scale_sum_wraps_EXPR := s/if \(x0 \+ w > in->pitch \|\| y0 \+ h > in->height\)/if ((uint32_t)(x0 + w) > in->pitch || (uint32_t)(y0 + h) > in->height)/
MUT_img_scale_out_window_unchecked_FILE := iav_policy.c
MUT_img_scale_out_window_unchecked_EXPR := s/if \(\(uint64_t\)s->output.x \+ ow > out->pitch \|\| \(uint64_t\)s->output.y \+ oh > out->height\)/if (0)/
MUT_img_for_consumers_FILE := iav_policy.c
MUT_img_for_consumers_EXPR := s/\{ IAV_IOC_SET_IMG_SCALE, PRIMARY_SET,/{ IAV_IOC_SET_IMG_SCALE, BOTH,/
MUT_gdma_dst_any_FILE := iav_policy.c
MUT_gdma_dst_any_EXPR := s/if \(dst && !pid_is_primary_content\(call, pid\) && !is_arena_slot\(call, slot, pid\)\)/if (0)/
MUT_gdma_range_unchecked_FILE := iav_policy.c
MUT_gdma_range_unchecked_EXPR := s/if \(!span_ok\(offset, pitch \* height, lo, hi\)\)\n\t\treturn -EINVAL;//
MUT_gdma_phys_FILE := iav_policy.c
MUT_gdma_phys_EXPR := s/if \(g->src_use_phys \|\| g->dst_use_phys \|\| g->src_use_dma_buf_fd \|\| g->dst_use_dma_buf_fd\)/if (0)/
MUT_gdma_zero_size_FILE := iav_policy.c
MUT_gdma_zero_size_EXPR := s/if \(g->width == 0 \|\| g->height == 0\)\n\t\treturn -EINVAL;//
MUT_sync_invalidate_any_FILE := iav_policy.c
MUT_sync_invalidate_any_EXPR := s/if \(opt == IAV_CACHE_INVALIDATE && !pid_is_primary_content\(call, c->pid\) && !is_arena_slot\(call, 0, c->pid\)\)/if (0)/
MUT_blur_partition_unchecked_FILE := iav_policy.c
MUT_blur_partition_unchecked_EXPR := s/if \(!blur_partition_present\(call,/if (0 \&\& !blur_partition_present(call,/
MUT_blur_arb_allowed_FILE := iav_policy.c
MUT_blur_arb_allowed_EXPR := s/uint64_t limit = smooth \? 0 : aux_len_of\(call, IAV_PART_ARB_BLUR\);/uint64_t limit = ~0ull;/
MUT_canvas_overlay_allowed_FILE := iav_policy.c
MUT_canvas_overlay_allowed_EXPR := s/\t\treturn overlay_ok\(call, &c->arg.overlay, c->id, 1\);/\t\treturn 0;/
MUT_vproc_blend_allowed_FILE := iav_policy.c
MUT_vproc_blend_allowed_EXPR := s/if \(cid == IAV_VIDEO_PROC_MULTI_BLEND \|\| cid == IAV_VIDEO_PROC_BLEND_WARP\)/if (0)/
MUT_vin_idsp_shift_FILE := iav_policy.c
MUT_vin_idsp_shift_EXPR := s/if \(c->idsp_cfg.chan_id >= 32u \|\| c->vin_cfg.vsrc_id >= 32u\)/if (0)/
MUT_isp_nn_leaks_FILE := iav_policy.c
MUT_isp_nn_leaks_EXPR := s/p->arg.nn_cfg.nn_info\[i\].dag_addr_phys_addr = 0;/;/
MUT_efm_stream_unchecked_FILE := iav_policy.c
MUT_efm_stream_unchecked_EXPR := s/\|\| !\(\(res.stream_map >> stream_id\) & 1u\)\)/)/
MUT_pyramid_release_unbounded_FILE := iav_policy.c
MUT_pyramid_release_unbounded_EXPR := s/return d->chan_id < CONFIG_AMBARELLA_MAX_CHANNEL_NUM \? 0 : -EINVAL;/return 0;/
MUT_raw_release_unaudited_FILE := iav_policy.c
MUT_raw_release_unaudited_EXPR := s/\{ IAV_IOC_RELEASE_RAW_BUF, PRIMARY_SET, IAV_F_WIRE_AUDITED,/{ IAV_IOC_RELEASE_RAW_BUF, PRIMARY_SET, 0,/

# Envelope E of plan_iav_usb_yolo: the per-guest arena (OP_ARENA, the arena entry, quarantine accounting).
MUT_arena_admit_skipped_FILE := iav_broker.c
MUT_arena_admit_skipped_EXPR := s/err = arena_admit\(&h, pool.slice_bytes - PAGE_BYTES\);/err = 0;/
MUT_arena_slice_bound_FILE := iav_broker.c
MUT_arena_slice_bound_EXPR := s/if \(bk->offset >= usable \|\| bk->length > usable - bk->offset\)/if (0)/
MUT_arena_claim_page_FILE := iav_broker.c
MUT_arena_claim_page_EXPR := s/err = arena_admit\(&h, pool.slice_bytes - PAGE_BYTES\);/err = arena_admit(&h, pool.slice_bytes);/
MUT_arena_misaligned_FILE := iav_broker.c
MUT_arena_misaligned_EXPR := s/bk->offset % AMBA_VIRT_IAV_ARENA_ALIGN \|\| //
MUT_arena_overlap_allowed_FILE := iav_broker.c
MUT_arena_overlap_allowed_EXPR := s/if \(bk->offset < o->offset \+ o->length && o->offset < bk->offset \+ bk->length\)\n\t\t\t\treturn -EINVAL;//
MUT_arena_unused_block_FILE := iav_broker.c
MUT_arena_unused_block_EXPR := s/if \(used\[k\] == 0\)\n\t\t\trc = -EINVAL;//
MUT_arena_pool_named_directly_FILE := iav_policy.c
MUT_arena_pool_named_directly_EXPR := s/\*lo = call->arena\[slot\].start;\n\t\t\*hi = call->arena\[slot\].start \+ call->arena\[slot\].len;\n\t\treturn 0;/*lo = 0;\n\t\t*hi = ~0ull;\n\t\treturn 0;/; s/if \(slot >= 2 \|\| !call->arena\[slot\].valid \|\| call->arena\[slot\].len == 0\)\n\t\t\treturn -EPERM;//; s/return call->arena_pid != 0 && pid == call->arena_pid && slot < 2 && call->arena\[slot\].valid;/return call->arena_pid != 0 \&\& pid == call->arena_pid;/
MUT_hello_arena_for_consumers_FILE := iav_broker.c
MUT_hello_arena_for_consumers_EXPR := s/arena_ok = prof != IAV_PROFILE_CONSUMER && /arena_ok = /
MUT_arena_quarantine_ignored_FILE := iav_broker.c
MUT_arena_quarantine_ignored_EXPR := s/if \(b->ops.arena_begin\(b->ops.ctx, x->index\) != 0\) \{/if (b->ops.arena_begin(b->ops.ctx, x->index) != 0 \&\& 0) {/
MUT_arena_lease_recheck_skipped_FILE := iav_broker.c
MUT_arena_lease_recheck_skipped_EXPR := s/if \(b->ops.slice_of == NULL \|\| b->ops.slice_of\(b->ops.ctx, cid, &window\) != 0 \|\| window != x->index\) \{/if (0) {/
MUT_arena_output_not_restored_FILE := iav_broker.c
MUT_arena_output_not_restored_EXPR := s/\t\tif \(x != NULL\)\n\t\t\tarena_restore\(cmd, buf, orig\);\n//
MUT_arena_info_wrong_slice_FILE := iav_broker.c
MUT_arena_info_wrong_slice_EXPR := s/ent.base = pool.parent_base \+ \(uint64_t\)index \* pool.slice_bytes;/ent.base = pool.parent_base;/
MUT_arena_pool_recheck_skipped_FILE := iav_broker.c
MUT_arena_pool_recheck_skipped_EXPR := s/\tif \(pool_set\) \{\n\t\tstruct iav_querymem q;/\tif (0) {\n\t\tstruct iav_querymem q;/

# Envelope F of plan_iav_usb_yolo: the aux aperture and the calls it unlocks.
MUT_aux_map_for_consumers_FILE := iav_broker.c
MUT_aux_map_for_consumers_EXPR := s/if \(!hello.done \|\| !\(hello.features & AMBA_VIRT_IAV_F_APERTURE\) \|\|\n\t\t    effective_profile\(b, cid\) == IAV_PROFILE_CONSUMER\)\n\t\t\treturn refuse\(r, EFAULT\);//
MUT_aux_map_ring_unbounded_FILE := iav_broker.c
MUT_aux_map_ring_unbounded_EXPR := s/if \(length > 2 \* ap.e\[k\].len\)/if (0)/
MUT_aux_info_for_consumers_FILE := iav_broker.c
MUT_aux_info_for_consumers_EXPR := s/if \(ext && \(hello.features & AMBA_VIRT_IAV_F_APERTURE\) && prof != IAV_PROFILE_CONSUMER\) \{/if (ext \&\& (hello.features \& AMBA_VIRT_IAV_F_APERTURE)) {/
MUT_hello_aperture_for_consumers_FILE := iav_broker.c
MUT_hello_aperture_for_consumers_EXPR := s/aperture_ok = prof != IAV_PROFILE_CONSUMER && /aperture_ok = /
MUT_aperture_unvalidated_FILE := iav_broker.c
MUT_aperture_unvalidated_EXPR := s/\t\tfor \(j = 0; j < i; j\+\+\)\n\t\t\tif \(ap->e\[j\].pid == e->pid \|\|/\t\tfor (j = 0; j < 0; j++)\n\t\t\tif (ap->e[j].pid == e->pid ||/
MUT_aux_pinning_skipped_FILE := iav_broker.c
MUT_aux_pinning_skipped_EXPR := s/\t\tfor \(i = 0; i < na && i < IAV_BROKER_AUX_MAX; i\+\+\) \{\n\t\t\tstruct iav_querymem q;/\t\tfor (i = 0; i < 0; i++) {\n\t\t\tstruct iav_querymem q;/
MUT_warp_offset_unchecked_FILE := iav_policy.c
MUT_warp_offset_unchecked_EXPR := s/return m->data_addr_offset < limit;/return 1;/
MUT_ce_index_unbounded_FILE := iav_policy.c
MUT_ce_index_unbounded_EXPR := s/if \(h->chan_id >= CHANNELS \|\| h->buffer_idx >= CE_GO_MAP_BUFFERS\)/if (h->chan_id >= CHANNELS)/
MUT_qpm_raw_allowed_FILE := iav_policy.c
MUT_qpm_raw_allowed_EXPR := s/if \(q->qpm_raw\)\n\t\t\treturn -EPERM;//
MUT_remap_clut_unchecked_FILE := iav_policy.c
MUT_remap_clut_unchecked_EXPR := s/if \(r->area\[i\].enable && !range_ok\(r->area\[i\].clut_addr_offset, YUV_REMAP_CLUT_BYTES, limit\)\)/if (0)/
MUT_arb_blur_unchecked_FILE := iav_policy.c
MUT_arb_blur_unchecked_EXPR := s/if \(!range_ok\(\(uint64_t\)a->arb_blur_offset \+ \(uint64_t\)\(a->height - 1u\) \* a->arb_blur_pitch, row, limit\)\)/if (0)/
MUT_arb_blur_pitch_blind_FILE := iav_policy.c
MUT_arb_blur_pitch_blind_EXPR := s/\(uint64_t\)a->arb_blur_offset \+ \(uint64_t\)\(a->height - 1u\) \* a->arb_blur_pitch, row, limit/(uint64_t)a->arb_blur_offset, (uint16_t)(row * a->height), limit/
MUT_overlay_stream_id_unbounded_FILE := iav_policy.c
MUT_overlay_stream_id_unbounded_EXPR := s/if \(canvas \? id >= IAV_MAX_CANVAS_BUF_NUM : id >= STREAM_IDS\)/if (canvas \&\& id >= IAV_MAX_CANVAS_BUF_NUM)/
MUT_canvas_overlay_against_overlay_FILE := iav_policy.c
MUT_canvas_overlay_against_overlay_EXPR := s/limit = canvas \? aux_len_of\(call, IAV_PART_CANVAS_OVERLAY\) : call->ovl_len;/limit = call->ovl_len;/
MUT_vproc_any_cid_FILE := iav_policy.c
MUT_vproc_any_cid_EXPR := s/\tcase IAV_VIDEO_PROC_CAWARP:\n\t\treturn v->arg.cawarp.chan_id < CHANNELS \? 0 : -EINVAL;\n\tdefault:\n\t\treturn -EPERM;/\tcase IAV_VIDEO_PROC_CAWARP:\n\t\treturn v->arg.cawarp.chan_id < CHANNELS ? 0 : -EINVAL;\n\tdefault:\n\t\treturn 0;/
MUT_mask_without_entry_FILE := iav_policy.c
MUT_mask_without_entry_EXPR := s/return aux_len_of\(call, IAV_PART_MASK\) \? 0 : -EPERM;/return 0;/
MUT_vsrc_update_unpinned_FILE := iav_policy.c
MUT_vsrc_update_unpinned_EXPR := s/\t\tif \(have.pre_dec_enable\) \{\n\t\t\tcall->pinned = 1;\n\t\t\treturn -EPERM;\n\t\t\}//
MUT_canvas_order_shift_FILE := iav_policy.c
MUT_canvas_order_shift_EXPR := s/if \(v->arg.canvas_order.chan_order\[i\] >= 32u\)/if (0)/
MUT_stream_custom_sei_FILE := iav_policy.c
MUT_stream_custom_sei_EXPR := s/\tcase IAV_STMCFG_CUSTOM_SEI:\n\t\treturn -EPERM;\n\tcase IAV_H264_CFG_QP_ROI:/\tcase IAV_H264_CFG_QP_ROI:/
MUT_stream_id_unbounded_FILE := iav_policy.c
MUT_stream_id_unbounded_EXPR := s/\tif \(c->id >= STREAM_IDS\)\n\t\treturn -EINVAL;\n\tmemcpy\(&cid, &c->cid, sizeof\(cid\)\);\n\tswitch \(cid\) \{\n\tcase IAV_STMCFG_CUSTOM_SEI:/\tmemcpy(\&cid, \&c->cid, sizeof(cid));\n\tswitch (cid) {\n\tcase IAV_STMCFG_CUSTOM_SEI:/
MUT_misc_pointers_forwarded_FILE := iav_policy.c
MUT_misc_pointers_forwarded_EXPR := s/if \(f.query > 1 \|\| f.valid > 1 \|\| f.extra > 1 \|\| t->reserved \|\| t->count > AMBA_VIRT_IAV_INLINE_MISC_CHANS\)\n\t\treturn -EINVAL;/if (t->count > AMBA_VIRT_IAV_INLINE_MISC_CHANS)\n\t\treturn -EINVAL;/; s/m->query_extra_raw = f.query \? t->query : NULL;/m->query_extra_raw = f.query > 1 ? (u8 *)f.query : f.query ? t->query : NULL;/
MUT_misc_inline_unnegotiated_FILE := iav_broker.c
MUT_misc_inline_unnegotiated_EXPR := s/if \(!hello.done \|\| !\(hello.features & AMBA_VIRT_IAV_F_INLINE\) \|\| !\(e->dir & IAV_DIR_W\) \|\| x != NULL\)/if (!(e->dir \& IAV_DIR_W) || x != NULL)/
MUT_misc_out_leaks_FILE := iav_policy.c
MUT_misc_out_leaks_EXPR := s/\tmemcpy\(&m->query_extra_raw, &q, sizeof\(q\)\);\n//
MUT_apply_vproc_any_FILE := iav_policy.c
MUT_apply_vproc_any_EXPR := s/\t\t\tbreak;\n\t\tdefault:\n\t\t\treturn -EPERM;\n\t\t\}\n\t\}\n\treturn 0;/\t\t\tbreak;\n\t\tdefault:\n\t\t\tbreak;\n\t\t}\n\t}\n\treturn 0;/
MUT_apply_vproc_chan_unbounded_FILE := iav_policy.c
MUT_apply_vproc_chan_unbounded_EXPR := s/if \(!CHAN_MAP_OK\(f\[k\].param\)\)/if (0)/
MUT_apply_vproc_canvas_unbounded_FILE := iav_policy.c
MUT_apply_vproc_canvas_unbounded_EXPR := s/if \(!CANVAS_MAP_OK\(f\[k\].param\)\)/if (0)/
MUT_apply_warp_any_FILE := iav_policy.c
MUT_apply_warp_any_EXPR := s/if \(f->apply != IAV_WARP_CTRL_MAIN && f->apply != IAV_WARP_CTRL_DPTZ\)\n\t\treturn -EPERM;\n//
MUT_apply_canvas_sync_unbounded_FILE := iav_policy.c
MUT_apply_canvas_sync_unbounded_EXPR := s/return CANVAS_MAP_OK\(a->canvas_updated_map\) \? 0 : -EINVAL;/return 0;/

MUT_jpeg_unchecked_FILE := iav_policy.c
MUT_jpeg_unchecked_EXPR := s/return iav_jpeg_admit\(head, n\);/return 0;/
MUT_jpeg_422_allowed_FILE := iav_policy.c
MUT_jpeg_422_allowed_EXPR := s/if \(s\[7\] != 0x22 \|\| s\[10\] != 0x11 \|\| s\[13\] != 0x11\)/if (0)/
MUT_jpeg_progressive_allowed_FILE := iav_policy.c
MUT_jpeg_progressive_allowed_EXPR := s/if \(m == 0xC0\) \{/if (m == 0xC0 || m == 0xC2) {/

IAV_MUT_DIR = $(IAV_BROKER_OUT)/mut-$(MUTANT)

test_iav_broker_negctl:
	test -n "$(MUTANT)"
	test -n "$(MUT_$(MUTANT)_FILE)"
	rm -rf $(IAV_MUT_DIR)
	mkdir -p $(IAV_MUT_DIR)
	cp iav_broker.c iav_policy.c $(IAV_MUT_DIR)/
	perl -0pi -e '$(MUT_$(MUTANT)_EXPR)' $(IAV_MUT_DIR)/$(MUT_$(MUTANT)_FILE)
	@if cmp -s $(MUT_$(MUTANT)_FILE) $(IAV_MUT_DIR)/$(MUT_$(MUTANT)_FILE); then echo "MUTATION DID NOT APPLY: $(MUTANT)"; exit 99; fi
	gcc $(IAV_BROKER_CFLAGS) -Wno-error -I$(IAV_MUT_DIR) -c -o $(IAV_MUT_DIR)/iav_broker.o $(IAV_MUT_DIR)/iav_broker.c
	gcc $(IAV_BROKER_CFLAGS) -Wno-error -I$(IAV_MUT_DIR) -c -o $(IAV_MUT_DIR)/iav_policy.o $(IAV_MUT_DIR)/iav_policy.c
	gcc $(IAV_BROKER_CFLAGS) -Wno-error -c -o $(IAV_MUT_DIR)/virt_iav_guest_slice.o virt_iav_guest_slice.c
	g++ $(IAV_BROKER_CXXFLAGS) -o $(IAV_MUT_DIR)/test test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_MUT_DIR)/iav_broker.o $(IAV_MUT_DIR)/iav_policy.o $(IAV_MUT_DIR)/virt_iav_guest_slice.o -lCppUTest -lCppUTestExt -lpthread -fsanitize=address,undefined
	timeout 120 $(IAV_MUT_DIR)/test

# Coverage-guided fuzzing with libFuzzer. clang needs the libstdc++ directory of the host gcc.
IAV_FUZZ_BIN := $(IAV_BROKER_OUT)/fuzz_iav_broker
IAV_FUZZ_SECS ?= 60
IAV_FUZZ_LIBDIR := $(shell dirname $$(g++ -print-file-name=libstdc++.so))
IAV_FUZZ_GCCVER := $(shell g++ -dumpversion | cut -d. -f1)
IAV_FUZZ_CXXINC := -isystem /usr/include/c++/$(IAV_FUZZ_GCCVER) -isystem /usr/include/$(shell uname -m)-linux-gnu/c++/$(IAV_FUZZ_GCCVER) -isystem /usr/include/c++/$(IAV_FUZZ_GCCVER)/backward
IAV_FUZZ_COMMON := -O1 -g -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. -Itest $(IAV_CFLAGS)

$(IAV_FUZZ_BIN): fuzz/FuzzIavBroker.cxx test/MockIavDriver.cxx $(IAV_BROKER_C) virt_iav_guest_slice.c test/IavBrokerHarness.hxx iav_broker.h iav_policy.h
	mkdir -p $(IAV_BROKER_OUT)/fuzzobj
	clang -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined $(IAV_FUZZ_COMMON) -c -o $(IAV_BROKER_OUT)/fuzzobj/iav_broker.o iav_broker.c
	clang -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined $(IAV_FUZZ_COMMON) -c -o $(IAV_BROKER_OUT)/fuzzobj/iav_policy.o iav_policy.c
	clang -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined $(IAV_FUZZ_COMMON) -c -o $(IAV_BROKER_OUT)/fuzzobj/virt_iav_guest_slice.o virt_iav_guest_slice.c
	clang++ -std=c++17 $(IAV_FUZZ_CXXINC) -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined $(IAV_FUZZ_COMMON) -o $@ fuzz/FuzzIavBroker.cxx test/MockIavDriver.cxx $(IAV_BROKER_OUT)/fuzzobj/iav_broker.o $(IAV_BROKER_OUT)/fuzzobj/iav_policy.o $(IAV_BROKER_OUT)/fuzzobj/virt_iav_guest_slice.o -L$(IAV_FUZZ_LIBDIR) -lpthread

# Fuzz for IAV_FUZZ_SECS seconds. Crashes are written next to the corpus.
fuzz_iav_broker_run: $(IAV_FUZZ_BIN)
	mkdir -p $(IAV_BROKER_OUT)/corpus
	$(IAV_FUZZ_BIN) $(IAV_BROKER_OUT)/corpus fuzz/seeds -max_total_time=$(IAV_FUZZ_SECS) -artifact_prefix=$(IAV_BROKER_OUT)/ -print_final_stats=1



$(IAV_BROKER_OUT)/%.o: %.c iav_broker.h iav_policy.h iav_ioctl_table.inc iav_allow_primary.inc ../include/uapi/amba_virt_iav.h
	mkdir -p $(IAV_BROKER_OUT)
	gcc $(IAV_BROKER_CFLAGS) -c -o $@ $<

$(IAV_BROKER_TEST_BIN): test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_BROKER_OBJS) $(wildcard test/*.hxx) iav_allow_primary.inc
	g++ $(IAV_BROKER_CXXFLAGS) -o $@ test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_BROKER_OBJS) -lCppUTest -lCppUTestExt -lpthread -fsanitize=address,undefined

test_host_iav_broker_run: $(IAV_BROKER_TEST_BIN)
	$(IAV_BROKER_TEST_BIN) -v

# ---- The aux aperture and the new partitions on the server side (plan_iav_usb_yolo, Envelope F) ----
IAV_APERTURE_OUT := ../../../build/iav-aperture
IAV_APERTURE_CFLAGS := -O1 -g -Wall -Wextra -Werror -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. $(IAV_CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined

test_host_iav_aperture_run: test/test_main.cxx test/TestHostIavAperture.cxx virt_iav_aperture.c virt_iav_aperture.h iav_new_parts.c iav_new_parts.h virt_slice_lease.c virt_camera_policy.c
	mkdir -p $(IAV_APERTURE_OUT)
	gcc $(IAV_APERTURE_CFLAGS) -c -o $(IAV_APERTURE_OUT)/virt_iav_aperture.o virt_iav_aperture.c
	gcc $(IAV_APERTURE_CFLAGS) -c -o $(IAV_APERTURE_OUT)/iav_new_parts.o iav_new_parts.c
	gcc $(IAV_APERTURE_CFLAGS) -c -o $(IAV_APERTURE_OUT)/virt_slice_lease.o virt_slice_lease.c
	gcc $(IAV_APERTURE_CFLAGS) -c -o $(IAV_APERTURE_OUT)/virt_camera_policy.o virt_camera_policy.c
	g++ -O1 -g -Wall -Wextra -std=c++17 -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. -Itest $(IAV_CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $(IAV_APERTURE_OUT)/test_host_iav_aperture test/test_main.cxx test/TestHostIavAperture.cxx $(IAV_APERTURE_OUT)/virt_iav_aperture.o $(IAV_APERTURE_OUT)/iav_new_parts.o $(IAV_APERTURE_OUT)/virt_slice_lease.o $(IAV_APERTURE_OUT)/virt_camera_policy.o -lCppUTest -lCppUTestExt -lpthread
	$(IAV_APERTURE_OUT)/test_host_iav_aperture -v

# ---- The guest module's arena allocator (plan_iav_usb_yolo, Envelope E), on the build host ----
IAV_ARENA_SRC := ../../../guest-os/linux/amba-iav/iav_arena.c
IAV_ARENA_OUT := ../../../build/iav-arena
IAV_ARENA_FLAGS := -O1 -g -Wall -Wextra -I../../../guest-os/linux/amba-iav -fsanitize=address,undefined -fno-sanitize-recover=undefined

test_host_iav_arena_run: test/test_main.cxx test/TestHostIavArena.cxx $(IAV_ARENA_SRC) ../../../guest-os/linux/amba-iav/iav_arena.h
	mkdir -p $(IAV_ARENA_OUT)
	gcc $(IAV_ARENA_FLAGS) -Werror -c -o $(IAV_ARENA_OUT)/iav_arena.o $(IAV_ARENA_SRC)
	g++ $(IAV_ARENA_FLAGS) -std=c++17 -Itest -o $(IAV_ARENA_OUT)/test_host_iav_arena test/test_main.cxx test/TestHostIavArena.cxx $(IAV_ARENA_OUT)/iav_arena.o -lCppUTest -lCppUTestExt -lpthread
	$(IAV_ARENA_OUT)/test_host_iav_arena -v

# Negative controls of the allocator: each mutant must fail the tests.
ARENA_MUT_no_alignment_EXPR := s/span = arena_round_up\(l, IAV_ARENA_ALIGN, &overflow\);/span = l;/
ARENA_MUT_overlap_EXPR := s/if \(b->off >= at && b->off - at >= span\)\n\t\t\tbreak;/break;/
ARENA_MUT_free_mapped_EXPR := s/if \(b->maps\)\n\t\treturn -EFAULT;//
ARENA_MUT_unregister_mapped_EXPR := s/if \(u && u->maps\)\n\t\treturn -EPERM;//
ARENA_MUT_release_frees_held_EXPR := s/if \(b->nusers == 0 && b->maps == 0\)/if (b->maps == 0)/
ARENA_MUT_overflow_unchecked_EXPR := s/if \(overflow \|\| span > a->usable\)/if (span > a->usable)/
ARENA_MUT_past_arena_EXPR := s/if \(at > a->usable \|\| a->usable - at < span\)\n\t\treturn -EFAULT;//

test_iav_arena_negctl:
	test -n "$(MUTANT)"
	test -n "$(ARENA_MUT_$(MUTANT)_EXPR)"
	rm -rf $(IAV_ARENA_OUT)/mut-$(MUTANT)
	mkdir -p $(IAV_ARENA_OUT)/mut-$(MUTANT)
	cp $(IAV_ARENA_SRC) $(IAV_ARENA_OUT)/mut-$(MUTANT)/iav_arena.c
	perl -0pi -e '$(ARENA_MUT_$(MUTANT)_EXPR)' $(IAV_ARENA_OUT)/mut-$(MUTANT)/iav_arena.c
	@if cmp -s $(IAV_ARENA_SRC) $(IAV_ARENA_OUT)/mut-$(MUTANT)/iav_arena.c; then echo "MUTATION DID NOT APPLY: $(MUTANT)"; exit 99; fi
	gcc $(IAV_ARENA_FLAGS) -c -o $(IAV_ARENA_OUT)/mut-$(MUTANT)/iav_arena.o $(IAV_ARENA_OUT)/mut-$(MUTANT)/iav_arena.c
	g++ $(IAV_ARENA_FLAGS) -std=c++17 -Itest -o $(IAV_ARENA_OUT)/mut-$(MUTANT)/test test/test_main.cxx test/TestHostIavArena.cxx $(IAV_ARENA_OUT)/mut-$(MUTANT)/iav_arena.o -lCppUTest -lCppUTestExt -lpthread
	timeout 120 $(IAV_ARENA_OUT)/mut-$(MUTANT)/test

# Negative controls of the slice scrub (Envelope E): each mutant must fail the low-lease tests.
LOW_MUT_scrub_skipped_FILE := virt_slice_arena.c
LOW_MUT_scrub_skipped_EXPR := s/\t\tmemset\(p, 0, bytes\);\n//
LOW_MUT_quarantine_not_leased_FILE := virt_low_lease.c
LOW_MUT_quarantine_not_leased_EXPR := s/return index_leased\(index\) \|\| \(index < VIRT_LOW_LEASE_COUNT && g_scrub\[index\]\);/return index_leased(index);/
LOW_MUT_inflight_ignored_FILE := virt_slice_arena.c
LOW_MUT_inflight_ignored_EXPR := s/if \(g_slot\[index\].quarantined && g_slot\[index\].inflight == 0\) \{/if (g_slot[index].quarantined) {/
LOW_MUT_begin_while_quarantined_FILE := virt_slice_arena.c
LOW_MUT_begin_while_quarantined_EXPR := s/if \(g_slot\[index\].quarantined\)\n\t\trc = -EBUSY;\n\telse\n\t\tg_slot/g_slot/

LOW_MUT_DIR = ../../../build/low-lease/mut-$(MUTANT)

test_low_lease_negctl:
	test -n "$(MUTANT)"
	test -n "$(LOW_MUT_$(MUTANT)_FILE)"
	rm -rf $(LOW_MUT_DIR)
	mkdir -p $(LOW_MUT_DIR)
	cp virt_low_lease.c virt_slice_arena.c $(LOW_MUT_DIR)/
	perl -0pi -e '$(LOW_MUT_$(MUTANT)_EXPR)' $(LOW_MUT_DIR)/$(LOW_MUT_$(MUTANT)_FILE)
	@if cmp -s $(LOW_MUT_$(MUTANT)_FILE) $(LOW_MUT_DIR)/$(LOW_MUT_$(MUTANT)_FILE); then echo "MUTATION DID NOT APPLY: $(MUTANT)"; exit 99; fi
	gcc -O1 -g -Wall -Wextra -I../include/uapi -I. $(NANOPB_CFLAGS) -c -o $(LOW_MUT_DIR)/virt_low_lease.o $(LOW_MUT_DIR)/virt_low_lease.c
	gcc -O1 -g -Wall -Wextra -I../include/uapi -I. -c -o $(LOW_MUT_DIR)/virt_slice_arena.o $(LOW_MUT_DIR)/virt_slice_arena.c
	$(MAKE) test_host_low_lease >/dev/null
	g++ -O1 -g -Wall -Wextra -std=c++17 -I. -I../include/uapi $(CPPUTEST_CFLAGS) -o $(LOW_MUT_DIR)/test test/test_main.cxx test/TestHostLowLease.cxx $(LOW_MUT_DIR)/virt_low_lease.o $(LOW_MUT_DIR)/virt_slice_arena.o virt_slice_lease.low.o virt_camera_policy.low.o iav_slice_rpc.low.o virt_host_ops.low.o pb_common.low.o pb_encode.low.o pb_decode.low.o amba_virt.pb.low.o $(CPPUTEST_LDFLAGS)
	timeout 120 $(LOW_MUT_DIR)/test

test_host_low_lease_run: test_host_low_lease
	./test_host_low_lease -v

# ---- IAV qualification suite (plan_iav_virtualization, Envelope 1) ----
# The shared suite lives in ../iav_qual. The host build runs it against the mock driver, under
# ASan and UBSan, and builds the broken-driver variants that must make it fail.
IAV_QUAL_DIR := ../iav_qual
IAV_QUAL_OUT := ../../../build/iav-qual
IAV_QUAL_CORE := $(addprefix $(IAV_QUAL_DIR)/,IavQualConfig.cxx IavQualResults.cxx IavQualContext.cxx IavQualOutput.cxx IavQualMain.cxx RealBackend.cxx LoopbackBackend.cxx)
IAV_QUAL_TESTS := $(wildcard $(IAV_QUAL_DIR)/TestIav*.cxx)
IAV_QUAL_HOST_SRCS := $(IAV_QUAL_CORE) $(IAV_QUAL_TESTS) $(IAV_QUAL_DIR)/MockBackend.cxx $(IAV_QUAL_DIR)/IavQualHostMain.cxx
IAV_QUAL_CXXFLAGS := -O1 -g -Wall -Wextra -std=c++17 -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 $(IAV_CFLAGS) -I$(IAV_QUAL_DIR) -I. -I../include/uapi -DIAVQUAL_WITH_MOCK -fsanitize=address,undefined
IAV_QUAL_HOST_BIN := $(IAV_QUAL_OUT)/iav-qual-host

$(IAV_QUAL_HOST_BIN): $(IAV_QUAL_HOST_SRCS) $(wildcard $(IAV_QUAL_DIR)/*.hxx) $(IAV_BROKER_OBJS)
	mkdir -p $(IAV_QUAL_OUT)
	g++ $(IAV_QUAL_CXXFLAGS) -o $@ $(IAV_QUAL_HOST_SRCS) $(IAV_BROKER_OBJS) $(CPPUTEST_LDFLAGS)

test_iav_qual_host_run: $(IAV_QUAL_HOST_BIN)
	$(IAV_QUAL_HOST_BIN) --mode tests --backend mock --board host --results $(IAV_QUAL_OUT)/host-tests.jsonl

test_iav_qual_host_loopback_run: $(IAV_QUAL_HOST_BIN)
	$(IAV_QUAL_HOST_BIN) --mode tests --backend loopback-mock --board host --results $(IAV_QUAL_OUT)/host-loopback-tests.jsonl

test_iav_qual_host_loopback_bench_run: $(IAV_QUAL_HOST_BIN)
	$(IAV_QUAL_HOST_BIN) --mode bench --backend loopback-mock --board host --results $(IAV_QUAL_OUT)/host-loopback-bench.jsonl

test_iav_qual_host_bench_run: $(IAV_QUAL_HOST_BIN)
	$(IAV_QUAL_HOST_BIN) --mode bench --backend mock --board host --results $(IAV_QUAL_OUT)/host-bench.jsonl

# The aarch64 qualification build of amba-virt-server. It is the production server source plus
# the suite, linked with a static CppUTest and a static C++ runtime, and run by hand from the
# development tree on a board. It is never part of the image.
IAV_QUAL_AARCH64_DIR := $(IAV_QUAL_OUT)/aarch64
IAV_QUAL_TARGET_SRCS := $(IAV_QUAL_CORE) $(IAV_QUAL_TESTS) $(IAV_QUAL_DIR)/IavQualServer.cxx
IAV_QUAL_TARGET_OBJS := $(patsubst $(IAV_QUAL_DIR)/%.cxx,$(IAV_QUAL_AARCH64_DIR)/%.o,$(IAV_QUAL_TARGET_SRCS))
IAV_QUAL_TARGET_CXXFLAGS := -O2 -g -Wall -Wextra -std=c++17 -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 $(IAV_CFLAGS) -I$(IAV_QUAL_DIR) -I. -I../include/uapi
IAV_QUAL_SERVER_BIN := $(IAV_QUAL_AARCH64_DIR)/amba-virt-server-qual

$(IAV_QUAL_AARCH64_DIR)/%.o: $(IAV_QUAL_DIR)/%.cxx $(wildcard $(IAV_QUAL_DIR)/*.hxx) $(IAV_QUAL_DIR)/iav_qual_server.h
	mkdir -p $(IAV_QUAL_AARCH64_DIR)
	$(CROSS_COMPILE)g++ $(IAV_QUAL_TARGET_CXXFLAGS) -c -o $@ $<

amba-virt-server.qual.o: amba-virt-server.c $(IAV_QUAL_DIR)/iav_qual_server.h
	$(CC) $(CFLAGS) $(IAV_CFLAGS) -DAMBA_VIRT_IAV_QUAL -c -o $@ $<

$(IAV_QUAL_SERVER_BIN): amba-virt-server.qual.o $(filter-out amba-virt-server.o,$(SERVER_OBJS)) $(NANOPB_OBJS) $(IAV_QUAL_TARGET_OBJS)
	$(CROSS_COMPILE)g++ $(SERVER_LDFLAGS) -static-libstdc++ -static-libgcc -o $@ $^ -L../../../third_party/cpputest/aarch64/lib -lCppUTest -lCppUTestExt -lpthread -lm -ldl

amba-virt-server-qual: $(IAV_QUAL_SERVER_BIN)

# Negative control: VARIANT=mock-<name> runs the suite against a deliberately broken driver. The
# suite must fail, so this target must fail.
test_iav_qual_host_negctl: $(IAV_QUAL_HOST_BIN)
	test -n "$(VARIANT)"
	$(IAV_QUAL_HOST_BIN) --mode tests --backend $(VARIANT) --board host --results $(IAV_QUAL_OUT)/negctl-$(VARIANT).jsonl

# Track header dependencies, so a changed header (the guest role in amba_virt.h, for one) rebuilds
# every object that includes it.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(wildcard *.d)

clean:
	rm -f amba-virt-server.qual.o $(TARGETS) $(TESTS) test_host_iav_proxy test_host_multitenant_broadcast test_host_cavalry_eve_sync test_host_iav_physical_frame_arm64 *.o *.d

# The ioctl support table of doc/IavVirtualization.md section 6.1. The dump program reads the real policy table
# with the allow-list applied; the generator joins it with iav_ioctl_review.tsv. iav-support-table rewrites the
# section of the document; the test fails when the document, the review record and the allow-list disagree.
$(IAV_SUPPORT_DUMP): iav_ioctl_dump.c iav_policy.c iav_policy.h iav_ioctl_table.inc
	@mkdir -p $(IAV_SUPPORT_OUT)
	gcc -O1 -Wall -Wextra -Werror -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. $(IAV_CFLAGS) -o $@ iav_ioctl_dump.c iav_policy.c

$(IAV_SUPPORT_OUT)/iav_ioctl_dump.tsv: $(IAV_SUPPORT_DUMP)
	$(IAV_SUPPORT_DUMP) > $@

iav-support-table: $(IAV_SUPPORT_OUT)/iav_ioctl_dump.tsv
	python3 ../../../tools/iav-policy/gen_iav_support_table.py --dump $< --review iav_ioctl_review.tsv --doc ../../../doc/IavVirtualization.md --write

test_iav_support_table_run: $(IAV_SUPPORT_OUT)/iav_ioctl_dump.tsv
	python3 ../../../tools/iav-policy/gen_iav_support_table.py --dump $< --review iav_ioctl_review.tsv --doc ../../../doc/IavVirtualization.md --check

test_iav_support_table_negctl: $(IAV_SUPPORT_OUT)/iav_ioctl_dump.tsv
	python3 ../../../tools/iav-policy/check_support_table_negctl.py --dump $< --review iav_ioctl_review.tsv --doc ../../../doc/IavVirtualization.md

# ---- Camera policy: verdict matrix, grammar, persistence, concurrency ----
CAM_OUT := ../../../build/camera-policy
CAM_CFLAGS := -O1 -g -Wall -Wextra -Werror -I../include/uapi -I.
CAM_CXXFLAGS := -O1 -g -Wall -Wextra -Werror -std=c++17 -I. -I../include/uapi -Itest

test_host_camera_policy: test/test_main.cxx test/TestHostCameraPolicy.cxx virt_camera_policy.c virt_slice_lease.c virt_camera_policy.h virt_slice_lease.h
	mkdir -p $(CAM_OUT)
	gcc $(CAM_CFLAGS) -fsanitize=address,undefined -c -o $(CAM_OUT)/virt_camera_policy.o virt_camera_policy.c
	gcc $(CAM_CFLAGS) -fsanitize=address,undefined -c -o $(CAM_OUT)/virt_slice_lease.o virt_slice_lease.c
	g++ $(CAM_CXXFLAGS) -fsanitize=address,undefined -o $@ test/test_main.cxx test/TestHostCameraPolicy.cxx $(CAM_OUT)/virt_camera_policy.o $(CAM_OUT)/virt_slice_lease.o -lCppUTest -lCppUTestExt -lpthread

test_host_camera_policy_run:
	$(MAKE) -B test_host_camera_policy
	./test_host_camera_policy

test_host_camera_policy_tsan: test/test_main.cxx test/TestHostCameraPolicy.cxx virt_camera_policy.c virt_slice_lease.c virt_camera_policy.h virt_slice_lease.h
	mkdir -p $(CAM_OUT)/tsan
	gcc $(CAM_CFLAGS) -fsanitize=thread -c -o $(CAM_OUT)/tsan/virt_camera_policy.o virt_camera_policy.c
	gcc $(CAM_CFLAGS) -fsanitize=thread -c -o $(CAM_OUT)/tsan/virt_slice_lease.o virt_slice_lease.c
	g++ $(CAM_CXXFLAGS) -fsanitize=thread -o $@ test/test_main.cxx test/TestHostCameraPolicy.cxx $(CAM_OUT)/tsan/virt_camera_policy.o $(CAM_OUT)/tsan/virt_slice_lease.o -lCppUTest -lCppUTestExt -lpthread

test_host_camera_policy_tsan_run:
	$(MAKE) -B test_host_camera_policy_tsan
	setarch $(shell uname -m) -R ./test_host_camera_policy_tsan

# Negative controls: the policy with one deliberate bug must fail the tests. Each mutation is a perl
# substitution on a copy of virt_camera_policy.c. The target fails with code 99 if the mutation did not
# change the file, and otherwise runs the tests, which must fail: a surviving mutant makes this succeed.
MUT_ignore_gate_EXPR := s/\(g_windows_ready && on\)/(on)/
MUT_ignore_override_EXPR := s/\(ov == OV_INHERIT\) \?/(1) ?/
MUT_dec_for_consumer_EXPR := s/if \(index == 0\) \{/if (1) {/
MUT_nonatomic_persist_EXPR := s/"%s\.tmp", g_store/"%s", g_store/

CAM_MUT_DIR = $(CAM_OUT)/mut-$(MUTANT)

test_camera_negctl:
	test -n "$(MUTANT)"
	test -n "$(MUT_$(MUTANT)_EXPR)"
	rm -rf $(CAM_MUT_DIR)
	mkdir -p $(CAM_MUT_DIR)
	cp virt_camera_policy.c $(CAM_MUT_DIR)/
	perl -0pi -e '$(MUT_$(MUTANT)_EXPR)' $(CAM_MUT_DIR)/virt_camera_policy.c
	@if cmp -s virt_camera_policy.c $(CAM_MUT_DIR)/virt_camera_policy.c; then echo "MUTATION DID NOT APPLY: $(MUTANT)"; exit 99; fi
	gcc $(CAM_CFLAGS) -Wno-error -fsanitize=address,undefined -c -o $(CAM_MUT_DIR)/virt_camera_policy.o $(CAM_MUT_DIR)/virt_camera_policy.c
	gcc $(CAM_CFLAGS) -fsanitize=address,undefined -c -o $(CAM_MUT_DIR)/virt_slice_lease.o virt_slice_lease.c
	g++ $(CAM_CXXFLAGS) -fsanitize=address,undefined -o $(CAM_MUT_DIR)/test test/test_main.cxx test/TestHostCameraPolicy.cxx $(CAM_MUT_DIR)/virt_camera_policy.o $(CAM_MUT_DIR)/virt_slice_lease.o -lCppUTest -lCppUTestExt -lpthread
	timeout 120 $(CAM_MUT_DIR)/test
