/*
 * test_virt_dma_broker.c
 *
 * Test harness for the per-VM DMA broker.
 * Verifies RPC protocol validation, CID-to-lease binding, token-bucket
 * rate limiting, controller-neutral endpoint forwarding, and live kernel bridge.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>

#include "amba_virt.h"
#include "amba_virt_dma.h"
#include "virt_dma_broker.h"

#define CTL_DEV   "/dev/amba_dma_ctl"
#define LEASE_DEV "/dev/amba_dma_lease%u"

/* Non-production endpoint fixture for Task 2.7 */
#define TEST_FIXTURE_NON_UART_ENDPOINT 0xDEADBEEF

static void test_rpc_protocol_structures(void)
{
    printf("[TEST 2.1] Validating RPC protocol structure definitions...\n");

    /* Assert fixed-width sizes */
    assert(sizeof(struct amba_virt_dma_request) == 48);
    assert(sizeof(struct amba_virt_dma_response) == 24);
    assert(sizeof(struct amba_dma_lease_alloc) == 48);
    assert(AMBA_VIRT_DMA32_POOL_BASE == 0x17c00000ULL);

    /* Assert field offsets */
    assert(offsetof(struct amba_virt_dma_request, capability) == 0);
    assert(offsetof(struct amba_virt_dma_request, epoch) == 8);
    assert(offsetof(struct amba_virt_dma_request, cookie) == 16);
    assert(offsetof(struct amba_virt_dma_request, endpoint_id) == 24);
    assert(offsetof(struct amba_virt_dma_request, operation) == 28);
    assert(offsetof(struct amba_virt_dma_request, offset) == 32);
    assert(offsetof(struct amba_virt_dma_request, length) == 40);
    assert(offsetof(struct amba_virt_dma_request, flags) == 44);

    assert(offsetof(struct amba_dma_lease_alloc, vm_uuid) == 0);
    assert(offsetof(struct amba_dma_lease_alloc, boot_generation) == 16);
    assert(offsetof(struct amba_dma_lease_alloc, vsock_cid) == 20);
    assert(offsetof(struct amba_dma_lease_alloc, lease_id) == 24);
    assert(offsetof(struct amba_dma_lease_alloc, endpoint_id) == 28);
    assert(offsetof(struct amba_dma_lease_alloc, capability) == 32);
    assert(offsetof(struct amba_dma_lease_alloc, epoch) == 40);

    assert(offsetof(struct amba_virt_dma_response, cookie) == 0);
    assert(offsetof(struct amba_virt_dma_response, status) == 8);
    assert(offsetof(struct amba_virt_dma_response, transferred) == 12);
    assert(offsetof(struct amba_virt_dma_response, reserved) == 16);

    /* Assert message type IDs */
    assert(AMBA_VIRT_MSG_DMA_REQUEST_REQ == 57u);
    assert(AMBA_VIRT_MSG_DMA_REQUEST_RESP == 58u);

    printf("  [PASS] RPC structures conform strictly to 64-bit aligned fixed ABI.\n");
}

static void test_cid_lease_binding(void)
{
    int ret;
    int opened_stdin = 0;
    printf("[TEST 2.2] Testing CID-to-lease binding isolation...\n");

    if (fcntl(STDIN_FILENO, F_GETFD) < 0) {
        int devnull = open("/dev/null", O_RDONLY);
        assert(devnull >= 0);
        if (devnull != STDIN_FILENO) {
            assert(dup2(devnull, STDIN_FILENO) == STDIN_FILENO);
            close(devnull);
        }
        opened_stdin = 1;
    }
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);

    ret = virt_dma_bind_cid_lease(10, 0);
    assert(ret == 0);
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);

    /* Binding a second CID when unused slots in g_cid_leases have
     * BSS-initialized cid == 0, fd == 0 must not close fd 0 (STDIN_FILENO). */
    ret = virt_dma_bind_cid_lease(11, 1);
    assert(ret == 0);
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);

    ret = virt_dma_bind_cid_lease(12, 2);
    assert(ret == 0);
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);

    ret = virt_dma_bind_cid_lease(13, 3);
    assert(ret == 0);
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);

    if (opened_stdin) {
        close(STDIN_FILENO);
    }

    printf("  [PASS] Explicit CID-to-lease bindings registered without closing fd=0 (STDIN_FILENO).\n");
}

static void test_token_bucket_rate_limiter(void)
{
    struct amba_virt_dma_request req;
    struct amba_virt_dma_response resp;
    int ret, quota_exceeded = 0;
    uint32_t test_cid = 88;

    printf("[TEST 2.3] Testing token-bucket rate limiter (2000 ops/s, 50 MiB/s)...\n");

    virt_dma_bind_cid_lease(test_cid, 0);

    /* Test 1: Excessive single-request byte quota (> 2 MiB burst) */
    memset(&req, 0, sizeof(req));
    req.cookie = 1001;
    req.length = 3u * 1024u * 1024u; /* 3 MiB exceeds burst of 2 MiB */
    ret = virt_dma_handle_request(test_cid, &req, &resp);
    assert(ret == sizeof(resp));
    assert(resp.status == -EDQUOT);
    printf("  [PASS] Oversized byte request correctly rejected with -EDQUOT (EVT-094).\n");

    /* Test 2: Operation burst exhaustion */
    memset(&req, 0, sizeof(req));
    req.cookie = 2001;
    req.length = 64; /* small length */

    /* Consume burst tokens */
    for (int i = 0; i < 100; i++) {
        req.cookie = 3000 + i;
        ret = virt_dma_handle_request(test_cid, &req, &resp);
        if (resp.status == -EDQUOT) {
            quota_exceeded = 1;
            break;
        }
    }
    assert(quota_exceeded == 1);
    printf("  [PASS] Operation flood correctly exhausted burst budget with -EDQUOT.\n");
}

static void test_non_uart_endpoint_policy_fixture(void)
{
    struct amba_virt_dma_request req;
    struct amba_virt_dma_response resp;
    uint32_t test_cid = 99;
    int ret;

    printf("[TEST 2.7] Testing non-UART endpoint fixture neutrality...\n");

    usleep(100000); /* Wait 100ms to recover tokens */

    virt_dma_bind_cid_lease(test_cid, 0);

    memset(&req, 0, sizeof(req));
    req.cookie = 0x55AA;
    req.endpoint_id = TEST_FIXTURE_NON_UART_ENDPOINT;
    req.operation = AMBA_DMA_OP_MEM_TO_DEV;
    req.offset = 0;
    req.length = 128;

    ret = virt_dma_handle_request(test_cid, &req, &resp);
    assert(ret == sizeof(resp));
    assert(resp.cookie == 0x55AA);

    if (access("/dev/amba_dma_lease0", F_OK) == 0) {
        assert(resp.status == -ENODEV || resp.status == -EACCES || resp.status == -EPERM);
        printf("  [PASS] Non-UART endpoint forwarded transparently; kernel enforced policy rejection (%d).\n", resp.status);
    } else {
        printf("  [PASS] Non-UART endpoint processed by transport (lease dev offline in host mock).\n");
    }
}

static void test_host_bound_endpoint_enforcement(void)
{
    struct amba_dma_lease_alloc alloc_u3, alloc_u2, alloc_legacy, alloc_dup, alloc_bad;
    struct amba_virt_dma_request req;
    struct amba_virt_dma_response resp;
    u32 cfg_before;
    int ret;

    printf("[TEST 2.8] Testing D6 host-bound DMA endpoint enforcement & bounds...\n");
    amba_dma_host_test_reset();

    /* 1. Out-of-range endpoint at alloc is refused with -EINVAL */
    memset(&alloc_bad, 0, sizeof(alloc_bad));
    alloc_bad.vsock_cid = 201;
    alloc_bad.boot_generation = 1;
    alloc_bad.endpoint_id = 1;
    ret = amba_dma_alloc_lease(&alloc_bad);
    assert(ret == -EINVAL);
    alloc_bad.endpoint_id = 4;
    ret = amba_dma_alloc_lease(&alloc_bad);
    assert(ret == -EINVAL);

    /* 2. Allocate lease for UART3 (endpoint 3) */
    memset(&alloc_u3, 0, sizeof(alloc_u3));
    alloc_u3.vsock_cid = 103;
    alloc_u3.boot_generation = 1;
    alloc_u3.endpoint_id = AMBA_DMA_ENDPOINT_UART3;
    ret = amba_dma_alloc_lease(&alloc_u3);
    assert(ret == 0);
    assert(alloc_u3.endpoint_id == AMBA_DMA_ENDPOINT_UART3);

    /* Second lease asking for UART3 while bound to alloc_u3 is refused at alloc with -EBUSY */
    memset(&alloc_dup, 0, sizeof(alloc_dup));
    alloc_dup.vsock_cid = 203;
    alloc_dup.boot_generation = 1;
    alloc_dup.endpoint_id = AMBA_DMA_ENDPOINT_UART3;
    ret = amba_dma_alloc_lease(&alloc_dup);
    assert(ret == -EBUSY);

    /* UART3 lease serves UART3 with endpoint 0 ("my endpoint") */
    memset(&req, 0, sizeof(req));
    req.capability = alloc_u3.capability;
    req.epoch = alloc_u3.epoch;
    req.cookie = 10;
    req.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
    req.operation = AMBA_DMA_OP_MEM_TO_DEV;
    req.offset = 4096;
    req.length = 64;
    cfg_before = amba_dma_host_test_config_count();
    ret = amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp);
    assert(ret == 0 && resp.status == 0 && resp.transferred == 64);
    assert(amba_dma_host_test_config_count() == cfg_before + 1);
    assert(amba_dma_host_test_last_configured_dreq() == 15); /* UART3 TX DREQ */

    /* UART3 lease also serves UART3 with explicit endpoint 2 */
    req.cookie = 11;
    req.endpoint_id = AMBA_DMA_ENDPOINT_UART3;
    req.operation = AMBA_DMA_OP_DEV_TO_MEM;
    cfg_before = amba_dma_host_test_config_count();
    ret = amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp);
    assert(ret == 0 && resp.status == 0 && resp.transferred == 64);
    assert(amba_dma_host_test_config_count() == cfg_before + 1);
    assert(amba_dma_host_test_last_configured_dreq() == 16); /* UART3 RX DREQ */

    /* Same UART3 lease asking for endpoint 1 (UART2) gets -EPERM and configures NO channel */
    req.cookie = 12;
    req.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
    cfg_before = amba_dma_host_test_config_count();
    ret = amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp);
    assert(ret == -EPERM);
    assert(amba_dma_host_test_config_count() == cfg_before);

    /* 3. Reverse for UART2 (endpoint 1) */
    memset(&alloc_u2, 0, sizeof(alloc_u2));
    alloc_u2.vsock_cid = 102;
    alloc_u2.boot_generation = 1;
    alloc_u2.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
    ret = amba_dma_alloc_lease(&alloc_u2);
    assert(ret == 0);
    assert(alloc_u2.endpoint_id == AMBA_DMA_ENDPOINT_UART2);

    /* Second lease asking for UART2 is refused at alloc with -EBUSY */
    memset(&alloc_dup, 0, sizeof(alloc_dup));
    alloc_dup.vsock_cid = 202;
    alloc_dup.boot_generation = 1;
    alloc_dup.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
    ret = amba_dma_alloc_lease(&alloc_dup);
    assert(ret == -EBUSY);

    /* UART2 lease serves UART2 with endpoint 0 and endpoint 1 */
    req.capability = alloc_u2.capability;
    req.epoch = alloc_u2.epoch;
    req.cookie = 20;
    req.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
    req.operation = AMBA_DMA_OP_MEM_TO_DEV;
    cfg_before = amba_dma_host_test_config_count();
    ret = amba_dma_dispatch_request(alloc_u2.lease_id, &req, &resp);
    assert(ret == 0 && resp.status == 0 && resp.transferred == 64);
    assert(amba_dma_host_test_config_count() == cfg_before + 1);
    assert(amba_dma_host_test_last_configured_dreq() == 13); /* UART2 TX DREQ */

    req.cookie = 21;
    req.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
    req.operation = AMBA_DMA_OP_DEV_TO_MEM;
    cfg_before = amba_dma_host_test_config_count();
    ret = amba_dma_dispatch_request(alloc_u2.lease_id, &req, &resp);
    assert(ret == 0 && resp.status == 0 && resp.transferred == 64);
    assert(amba_dma_host_test_config_count() == cfg_before + 1);
    assert(amba_dma_host_test_last_configured_dreq() == 14); /* UART2 RX DREQ */

    /* UART2 lease asking for endpoint 2 (UART3) gets -EPERM and configures NO channel */
    req.cookie = 22;
    req.endpoint_id = AMBA_DMA_ENDPOINT_UART3;
    cfg_before = amba_dma_host_test_config_count();
    ret = amba_dma_dispatch_request(alloc_u2.lease_id, &req, &resp);
    assert(ret == -EPERM);
    assert(amba_dma_host_test_config_count() == cfg_before);

    /* 4. Legacy lease with no endpoint (endpoint_id = 0 at alloc) refuses endpoint 0, 1, and 2 */
    memset(&alloc_legacy, 0, sizeof(alloc_legacy));
    alloc_legacy.vsock_cid = 100;
    alloc_legacy.boot_generation = 1;
    alloc_legacy.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
    ret = amba_dma_alloc_lease(&alloc_legacy);
    assert(ret == 0);
    assert(alloc_legacy.endpoint_id == AMBA_DMA_ENDPOINT_NONE);

    req.capability = alloc_legacy.capability;
    req.epoch = alloc_legacy.epoch;
    req.cookie = 30;
    req.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
    cfg_before = amba_dma_host_test_config_count();
    ret = amba_dma_dispatch_request(alloc_legacy.lease_id, &req, &resp);
    assert(ret == -EPERM);
    assert(amba_dma_host_test_config_count() == cfg_before);

    /* Release UART2 so UART2 is unowned, then verify unbound legacy lease STILL cannot claim UART2 first-come */
    assert(amba_dma_release_lease(alloc_u2.lease_id, alloc_u2.epoch) == 0);
    req.cookie = 31;
    req.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
    ret = amba_dma_dispatch_request(alloc_legacy.lease_id, &req, &resp);
    assert(ret == -EPERM);
    assert(amba_dma_host_test_config_count() == cfg_before);

    /* 5. Out-of-range endpoint_id on request is refused with -ENODEV */
    req.capability = alloc_u3.capability;
    req.epoch = alloc_u3.epoch;
    req.cookie = 40;
    req.endpoint_id = 999;
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -ENODEV);

    /* 6. Zero length, max length, overflow, misaligned, and below-bootstrap offsets */
    req.endpoint_id = AMBA_DMA_ENDPOINT_NONE;
    req.cookie = 41;
    req.offset = 4096;
    req.length = 0;
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -EINVAL);

    req.cookie = 42;
    req.length = (4u * 1024u * 1024u) + 1u;
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -EINVAL);

    req.cookie = 43;
    req.length = 64;
    req.offset = 4095; /* below bootstrap */
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -ERANGE);

    req.cookie = 44;
    req.offset = AMBA_VIRT_DMA32_SLICE_SIZE - 32;
    req.length = 64; /* overflows slice */
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -ERANGE);

    req.cookie = 45;
    req.offset = UINT64_MAX - 16;
    req.length = 64; /* 64-bit wrap attempt */
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -ERANGE);

    req.cookie = 46;
    req.offset = 4096 + 3; /* misaligned */
    req.length = 64;
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -EINVAL);

    /* 7. Stale epoch, wrong capability, and replayed cookie */
    req.offset = 4096;
    req.length = 64;
    req.cookie = 10; /* already completed above */
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -EALREADY);

    req.cookie = 47;
    req.epoch = alloc_u3.epoch + 1;
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -ESTALE);

    req.epoch = alloc_u3.epoch;
    req.capability = alloc_u3.capability ^ 1ULL;
    assert(amba_dma_dispatch_request(alloc_u3.lease_id, &req, &resp) == -EACCES);

    assert(amba_dma_release_lease(alloc_u3.lease_id, alloc_u3.epoch) == 0);
    assert(amba_dma_release_lease(alloc_legacy.lease_id, alloc_legacy.epoch) == 0);

    printf("  [PASS] Host-bound endpoint enforcement, bounds, and replay checks verified.\n");
}

static void test_live_kernel_lease_integration(void)
{
    int ctl_fd;
    struct amba_dma_lease_alloc alloc;
    struct amba_virt_dma_request req;
    struct amba_virt_dma_response resp;
    int ret;
    uint32_t guest_cid = 4;

    printf("[TEST 2.4, 2.6 & 2.7] Testing live kernel lease bridge, audit controls, and non-UART policy...\n");

    if (access(CTL_DEV, F_OK) != 0) {
        printf("  [SKIP] %s not accessible (running in non-target or unprivileged environment).\n", CTL_DEV);
        return;
    }

    ctl_fd = open(CTL_DEV, O_RDWR);
    if (ctl_fd < 0) {
        printf("  [SKIP] Failed to open %s: %s\n", CTL_DEV, strerror(errno));
        return;
    }

    /* Release lease 0 if held */
    struct amba_dma_lease_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.lease_id = 0;
    ctrl.epoch = 0;
    ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
    ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);

    /* Allocate lease 0 for guest CID 4 bound to UART2 */
    memset(&alloc, 0, sizeof(alloc));
    alloc.boot_generation = 1;
    alloc.vsock_cid = guest_cid;
    alloc.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
    alloc.vm_uuid[0] = 0x42;
    ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc);
    assert(ret == 0);
    assert(alloc.lease_id == 0);

    printf("  Allocated lease %u (epoch=0x%llx, cap=0x%llx, ep=%u)\n",
           alloc.lease_id, (unsigned long long)alloc.epoch,
           (unsigned long long)alloc.capability, alloc.endpoint_id);

    /* Wait for token replenishment */
    usleep(100000);

    /* Explicitly bind CID 4 to lease 0 */
    virt_dma_bind_cid_lease(guest_cid, alloc.lease_id);

    /* 1. Request with invalid capability token -> kernel must return -EACCES */
    memset(&req, 0, sizeof(req));
    req.capability = 0xBADF00D;
    req.epoch = alloc.epoch;
    req.cookie = 1;
    req.endpoint_id = AMBA_DMA_ENDPOINT_UART2;
    req.operation = AMBA_DMA_OP_MEM_TO_DEV;
    req.offset = 0;
    req.length = 64;

    ret = virt_dma_handle_request(guest_cid, &req, &resp);
    assert(ret == sizeof(resp));
    assert(resp.status == -EACCES);
    printf("  [PASS] Forged capability correctly rejected with -EACCES by kernel reference monitor.\n");

    /* 2. Request with stale epoch -> kernel must return -ESTALE */
    req.capability = alloc.capability;
    req.epoch = alloc.epoch + 100;
    req.cookie = 2;
    ret = virt_dma_handle_request(guest_cid, &req, &resp);
    assert(ret == sizeof(resp));
    assert(resp.status == -ESTALE);
    printf("  [PASS] Stale epoch correctly rejected with -ESTALE.\n");

    /* 3. Request with out-of-bounds offset -> kernel must return -ERANGE */
    req.epoch = alloc.epoch;
    req.cookie = 3;
    req.offset = 16u * 1024u * 1024u; /* offset at 16 MiB boundary */
    req.length = 64;
    ret = virt_dma_handle_request(guest_cid, &req, &resp);
    assert(ret == sizeof(resp));
    assert(resp.status == -ERANGE);
    printf("  [PASS] Out-of-bounds offset rejected with -ERANGE.\n");

    /* 4. Non-UART endpoint fixture with valid lease credentials -> kernel must return -ENODEV */
    req.offset = 0;
    req.length = 64;
    req.cookie = 4;
    req.endpoint_id = TEST_FIXTURE_NON_UART_ENDPOINT;
    ret = virt_dma_handle_request(guest_cid, &req, &resp);
    assert(ret == sizeof(resp));
    assert(resp.status == -ENODEV);
    printf("  [PASS] Non-UART endpoint fixture rejected with -ENODEV by kernel (Task 2.7).\n");

    /* Clean up lease */
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.lease_id = 0;
    ctrl.epoch = alloc.epoch;
    ctrl.command = AMBA_DMA_LEASE_CMD_RELEASE;
    ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_CTRL, &ctrl);
    close(ctl_fd);

    printf("  [PASS] Live kernel lease bridge verified.\n");
}

int main(void)
{
    printf("=== AmbaVirt Per-VM DMA Broker Test Suite (Envelope 2) ===\n\n");

    test_rpc_protocol_structures();
    test_cid_lease_binding();
    test_token_bucket_rate_limiter();
    test_non_uart_endpoint_policy_fixture();
    test_host_bound_endpoint_enforcement();
    test_live_kernel_lease_integration();

    printf("\n=== All Envelope 2 DMA Broker Tests PASSED ===\n");
    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
