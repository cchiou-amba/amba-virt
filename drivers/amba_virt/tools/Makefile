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
SERVER_SRCS := amba-virt-server.c cavalry_proxy.c amba_virt_slice.c iav_proxy.c virt_mem_pool.c virt_acl.c virt_admin_ipc.c virt_query.c virt_host_ops.c virt_daemon.c virt_ready.c virt_dma_broker.c hmac_sha256.c virt_module_loader.c virt_camera_conf.c virt_gpio.c virt_camera_pipeline.c iav_broker.c iav_policy.c iav_broker_server.c
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

.PHONY: all clean test test_iav_qual_host_loopback_run test_iav_qual_host_loopback_bench_run test_host_iav_broker_tsan_run test_host_iav_broker_run fuzz_iav_broker_run test_iav_broker_negctl amba-virt-server-qual test_iav_qual_host_run test_iav_qual_host_bench_run test_iav_qual_host_negctl test_iav_support_table_run iav-support-table test_iav_support_table_negctl

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

test_admin_ipc: test_admin_ipc.c virt_admin_ipc.c virt_acl.c virt_mem_pool.c virt_host_ops.c hmac_sha256.c cavalry_proxy.c ../amba_virt_slice.c virt_query.c
	gcc $(TEST_CFLAGS) -o $@ $^ -lpthread

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
	gcc $(TEST_CFLAGS) -I. -I.. -fsanitize=address,undefined -c -o $@ $<

virt_mem_pool.host.o: virt_mem_pool.c
	gcc $(TEST_CFLAGS) -I. -I.. -fsanitize=address,undefined -c -o $@ $<

amba_virt_slice.host.o: ../amba_virt_slice.c
	gcc $(TEST_CFLAGS) -I. -I.. -fsanitize=address,undefined -c -o $@ $<

test_host_cavalry_eve_sync: test/test_main.cxx test/TestHostCavalryEveSync.cxx cavalry_proxy.host.o virt_mem_pool.host.o amba_virt_slice.host.o
	g++ $(CPPUTEST_CFLAGS) -I.. -o $@ $^ $(CPPUTEST_LDFLAGS)

test_host_iav_physical_frame_arm64: $(HOST_CPPUTEST_SRCS) iav_proxy.c $(NANOPB_DIR)/pb_common.c $(NANOPB_DIR)/pb_encode.c $(NANOPB_DIR)/pb_decode.c $(PROTO_DIR)/amba_virt.pb.c
	aarch64-linux-gnu-g++ $(TEST_CFLAGS) $(IAV_CFLAGS) $(NANOPB_CFLAGS) -I. -std=c++17 -static -o $@ $^ -L../../../third_party/cpputest/aarch64/lib -lCppUTest -lCppUTestExt -lpthread -lm

# ---- IAV broker host tests (plan_iav_virtualization, Envelope 2) ----
# The broker is C, so it is built with gcc and linked into the CppUTest binary. Sanitizers on.
IAV_BROKER_C := iav_broker.c iav_policy.c
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
	g++ -O1 -g -Wall -Wextra -std=c++17 -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. -Itest $(IAV_CFLAGS) -fsanitize=thread -o $@ test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_BROKER_OUT)/tsanobj/iav_broker.o $(IAV_BROKER_OUT)/tsanobj/iav_policy.o -lCppUTest -lCppUTestExt -lpthread

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
MUT_arg_length_EXPR := s/if \(\(e->dir & IAV_DIR_W\) \? q->arg_len != e->size : q->arg_len != 0\)/if (0)/
MUT_policy_denied_FILE := iav_broker.c
MUT_policy_denied_EXPR := s/if \(!\(e->profiles & IAV_PROFILE_BIT\(b->cfg.profile\)\)\)/if (0)/
MUT_prot_exec_FILE := iav_broker.c
MUT_prot_exec_EXPR := s/if \(q->cmd & ~\(unsigned int\)\(PROT_READ \| PROT_WRITE\)\)/if (0)/
MUT_ring_everywhere_FILE := iav_broker.c
MUT_ring_everywhere_EXPR := s/limit = w->ring \? 2 \* w->part_len : w->part_len;/limit = 2 * w->part_len;/
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
MUT_state_change_allowed_EXPR := s/\{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL \},/{ IAV_IOC_WAIT_NEXT_IDSP_FRAME, BOTH, 0, NULL },\n\t{ IAV_IOC_ENTER_IDLE, BOTH, 0, NULL },/
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
MUT_decode_for_consumers_EXPR := s/\{ IAV_IOC_DECODE_JPEG, PRIMARY_SET, 0, check_decode_jpeg, NULL \},/{ IAV_IOC_DECODE_JPEG, BOTH, 0, check_decode_jpeg, NULL },/
MUT_decode_post_not_run_FILE := iav_broker.c
MUT_decode_post_not_run_EXPR := s/e->post\(&call\);/;/
MUT_decode_window_for_consumers_FILE := iav_broker.c
MUT_decode_window_for_consumers_EXPR := s/if \(\(w->id == AMBA_VIRT_IAV_WIN_DEC \|\| w->id == AMBA_VIRT_IAV_WIN_OVL\) &&\n\t    effective_profile\(b, cid\) == IAV_PROFILE_CONSUMER\) \{/if (0) {/
MUT_decode_window_unknown_FILE := iav_broker.c
MUT_decode_window_unknown_EXPR := s/call.dec_len = b->windows\[i\].part_len;/;/
MUT_overlay_clut_unchecked_FILE := iav_policy.c
MUT_overlay_clut_unchecked_EXPR := s/ \|\|\n\t\t    !range_ok\(a->clut_addr_offset, OVERLAY_CLUT_BYTES, call->ovl_len\)//
MUT_overlay_pitch_ignored_FILE := iav_policy.c
MUT_overlay_pitch_ignored_EXPR := s/need = \(uint64_t\)a->pitch \* a->height;/need = a->total_size;/
MUT_overlay_canvas_allowed_FILE := iav_policy.c
MUT_overlay_canvas_allowed_EXPR := s/if \(o->id_for_canvas\)\n\t\treturn -EPERM;\n\tif \(!o->enable\)/if (!o->enable)/
MUT_overlay_later_areas_unchecked_FILE := iav_policy.c
MUT_overlay_later_areas_unchecked_EXPR := s/i < MAX_NUM_OVERLAY_AREA/i < 1/
MUT_overlay_window_unknown_FILE := iav_broker.c
MUT_overlay_window_unknown_EXPR := s/call.ovl_len = b->windows\[i\].part_len;/;/
MUT_overlay_for_consumers_FILE := iav_policy.c
MUT_overlay_for_consumers_EXPR := s/\{ IAV_IOC_SET_OVERLAY_INSERT, PRIMARY_SET,/{ IAV_IOC_SET_OVERLAY_INSERT, BOTH,/

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
	g++ $(IAV_BROKER_CXXFLAGS) -o $(IAV_MUT_DIR)/test test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_MUT_DIR)/iav_broker.o $(IAV_MUT_DIR)/iav_policy.o -lCppUTest -lCppUTestExt -lpthread -fsanitize=address,undefined
	timeout 120 $(IAV_MUT_DIR)/test

# Coverage-guided fuzzing with libFuzzer. clang needs the libstdc++ directory of the host gcc.
IAV_FUZZ_BIN := $(IAV_BROKER_OUT)/fuzz_iav_broker
IAV_FUZZ_SECS ?= 60
IAV_FUZZ_LIBDIR := $(shell dirname $$(g++ -print-file-name=libstdc++.so))
IAV_FUZZ_GCCVER := $(shell g++ -dumpversion | cut -d. -f1)
IAV_FUZZ_CXXINC := -isystem /usr/include/c++/$(IAV_FUZZ_GCCVER) -isystem /usr/include/$(shell uname -m)-linux-gnu/c++/$(IAV_FUZZ_GCCVER) -isystem /usr/include/c++/$(IAV_FUZZ_GCCVER)/backward
IAV_FUZZ_COMMON := -O1 -g -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655 -I../include/uapi -I. -Itest $(IAV_CFLAGS)

$(IAV_FUZZ_BIN): fuzz/FuzzIavBroker.cxx test/MockIavDriver.cxx $(IAV_BROKER_C) test/IavBrokerHarness.hxx iav_broker.h iav_policy.h
	mkdir -p $(IAV_BROKER_OUT)/fuzzobj
	clang -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined $(IAV_FUZZ_COMMON) -c -o $(IAV_BROKER_OUT)/fuzzobj/iav_broker.o iav_broker.c
	clang -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined $(IAV_FUZZ_COMMON) -c -o $(IAV_BROKER_OUT)/fuzzobj/iav_policy.o iav_policy.c
	clang++ -std=c++17 $(IAV_FUZZ_CXXINC) -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined $(IAV_FUZZ_COMMON) -o $@ fuzz/FuzzIavBroker.cxx test/MockIavDriver.cxx $(IAV_BROKER_OUT)/fuzzobj/iav_broker.o $(IAV_BROKER_OUT)/fuzzobj/iav_policy.o -L$(IAV_FUZZ_LIBDIR) -lpthread

# Fuzz for IAV_FUZZ_SECS seconds. Crashes are written next to the corpus.
fuzz_iav_broker_run: $(IAV_FUZZ_BIN)
	mkdir -p $(IAV_BROKER_OUT)/corpus
	$(IAV_FUZZ_BIN) $(IAV_BROKER_OUT)/corpus -max_total_time=$(IAV_FUZZ_SECS) -artifact_prefix=$(IAV_BROKER_OUT)/ -print_final_stats=1



$(IAV_BROKER_OUT)/%.o: %.c iav_broker.h iav_policy.h iav_ioctl_table.inc iav_allow_primary.inc ../include/uapi/amba_virt_iav.h
	mkdir -p $(IAV_BROKER_OUT)
	gcc $(IAV_BROKER_CFLAGS) -c -o $@ $<

$(IAV_BROKER_TEST_BIN): test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_BROKER_OBJS) $(wildcard test/*.hxx) iav_allow_primary.inc
	g++ $(IAV_BROKER_CXXFLAGS) -o $@ test/test_main.cxx test/MockIavDriver.cxx $(IAV_BROKER_TESTS) $(IAV_BROKER_OBJS) -lCppUTest -lCppUTestExt -lpthread -fsanitize=address,undefined

test_host_iav_broker_run: $(IAV_BROKER_TEST_BIN)
	$(IAV_BROKER_TEST_BIN) -v

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
