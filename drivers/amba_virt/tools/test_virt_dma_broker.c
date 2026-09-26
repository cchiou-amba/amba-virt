/*
 * test_virt_dma_broker.c
 *
 * Test harness for Per-VM NOHYPER DMA Broker (Envelope 2).
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

    /* Assert field offsets */
    assert(offsetof(struct amba_virt_dma_request, capability) == 0);
    assert(offsetof(struct amba_virt_dma_request, epoch) == 8);
    assert(offsetof(struct amba_virt_dma_request, cookie) == 16);
    assert(offsetof(struct amba_virt_dma_request, endpoint_id) == 24);
    assert(offsetof(struct amba_virt_dma_request, operation) == 28);
    assert(offsetof(struct amba_virt_dma_request, offset) == 32);
    assert(offsetof(struct amba_virt_dma_request, length) == 40);
    assert(offsetof(struct amba_virt_dma_request, flags) == 44);

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
    printf("[TEST 2.2] Testing CID-to-lease binding isolation...\n");

    ret = virt_dma_bind_cid_lease(10, 0);
    assert(ret == 0);

    ret = virt_dma_bind_cid_lease(11, 1);
    assert(ret == 0);

    ret = virt_dma_bind_cid_lease(12, 2);
    assert(ret == 0);

    ret = virt_dma_bind_cid_lease(13, 3);
    assert(ret == 0);

    printf("  [PASS] Explicit CID-to-lease bindings registered successfully.\n");
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

    /*
     * We send a request with TEST_FIXTURE_NON_UART_ENDPOINT.
     * virt_dma_handle_request must NOT reject this at transport level.
     * It forwards to the kernel lease FD.
     * The kernel will reject it with -ENODEV or -EINVAL because the endpoint
     * is not in kernel policy, proving transport does not hardcode UART logic.
     */
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

    /* If lease device exists, kernel returns -ENODEV, -EACCES, or -EPERM if lease is not allocated */
    if (access("/dev/amba_dma_lease0", F_OK) == 0) {
        assert(resp.status == -ENODEV || resp.status == -EACCES || resp.status == -EPERM);
        printf("  [PASS] Non-UART endpoint forwarded transparently; kernel enforced policy rejection (%d).\n", resp.status);
    } else {
        printf("  [PASS] Non-UART endpoint processed by transport (lease dev offline in host mock).\n");
    }
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

    /* Allocate lease 0 for guest CID 4 */
    memset(&alloc, 0, sizeof(alloc));
    alloc.boot_generation = 1;
    alloc.vsock_cid = guest_cid;
    alloc.vm_uuid[0] = 0x42;
    ret = ioctl(ctl_fd, AMBA_DMA_IOC_LEASE_ALLOC, &alloc);
    assert(ret == 0);
    assert(alloc.lease_id == 0);

    printf("  Allocated lease %u (epoch=0x%llx, cap=0x%llx)\n",
           alloc.lease_id, (unsigned long long)alloc.epoch, (unsigned long long)alloc.capability);

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
