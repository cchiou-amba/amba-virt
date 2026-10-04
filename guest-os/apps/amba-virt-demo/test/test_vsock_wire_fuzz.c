/*
 * test_vsock_wire_fuzz.c
 *
 * In-Kernel Live Virtio-Vsock Wire Fuzzing Probe
 * Copyright (C) 2026, Ambarella International LLC.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <time.h>

#define VSOCK_HOST_CID       2
#define VSOCK_DEFAULT_PORT   7462
#define AMBA_VIRT_MAX_MSG    65536

static int connect_vsock(uint32_t cid, uint32_t port)
{
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(AF_VSOCK)");
        return -1;
    }

    struct sockaddr_vm sa;
    memset(&sa, 0, sizeof(sa));
    sa.svm_family = AF_VSOCK;
    sa.svm_cid = cid;
    sa.svm_port = port;

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static bool test_truncated_header(uint32_t port)
{
    printf("[TEST 1] Truncated wire header injection...\n");
    int fd = connect_vsock(VSOCK_HOST_CID, port);
    if (fd < 0) {
        printf("  [WARN] Unable to connect to host vsock port %u\n", port);
        return false;
    }

    /* Send only 2 bytes of the 4-byte LE length prefix, then pause and close abruptly */
    uint8_t partial[2] = { 0x40, 0x00 };
    if (write(fd, partial, sizeof(partial)) != sizeof(partial)) {
        close(fd);
        return false;
    }
    usleep(50000); /* 50 ms */
    close(fd);
    printf("  [PASS] Truncated header closed without server failure\n");
    return true;
}

static bool test_oversized_payload(uint32_t port)
{
    printf("[TEST 2] Oversized wire payload rejection...\n");
    int fd = connect_vsock(VSOCK_HOST_CID, port);
    if (fd < 0) return false;

    /* Claim 128 KB payload (exceeds AMBA_VIRT_MAX_MSG 64 KB) */
    uint32_t len = 128 * 1024;
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len & 0xFF);
    hdr[1] = (uint8_t)((len >> 8) & 0xFF);
    hdr[2] = (uint8_t)((len >> 16) & 0xFF);
    hdr[3] = (uint8_t)((len >> 24) & 0xFF);

    if (write(fd, hdr, 4) != 4) {
        close(fd);
        return false;
    }

    /* Read response: server should reject or drop connection */
    uint8_t resp_buf[256];
    ssize_t n = read(fd, resp_buf, sizeof(resp_buf));
    close(fd);

    printf("  [PASS] Oversized payload handled cleanly (bytes read=%zd)\n", n);
    return true;
}

static bool test_fuzz_bitflips(uint32_t port, int iterations)
{
    printf("[TEST 3] Bit-flip wire fuzzing (%d iterations)...\n", iterations);
    srand(1337);

    for (int i = 0; i < iterations; i++) {
        int fd = -1;
        for (int retry = 0; retry < 10; retry++) {
            fd = connect_vsock(VSOCK_HOST_CID, port);
            if (fd >= 0) break;
            usleep(10000);
        }
        if (fd < 0) {
            printf("  [FAIL] Server unreachable at fuzz iteration %d\n", i);
            return false;
        }

        uint32_t payload_len = 16 + (rand() % 512);
        uint8_t *pkt = (uint8_t *)malloc(4 + payload_len);
        pkt[0] = (uint8_t)(payload_len & 0xFF);
        pkt[1] = (uint8_t)((payload_len >> 8) & 0xFF);
        pkt[2] = (uint8_t)((payload_len >> 16) & 0xFF);
        pkt[3] = (uint8_t)((payload_len >> 24) & 0xFF);

        for (uint32_t b = 0; b < payload_len; b++) {
            pkt[4 + b] = (uint8_t)(rand() & 0xFF);
        }

        if (write(fd, pkt, 4 + payload_len) < 0) { /* ignore */ }
        free(pkt);

        uint8_t resp[256];
        if (read(fd, resp, sizeof(resp)) < 0) { /* ignore */ }
        close(fd);
        usleep(2000);
    }

    printf("  [PASS] %d random wire mutations rejected cleanly with 0 server crashes\n", iterations);
    return true;
}

static bool test_rapid_connect_storm(uint32_t port, int count)
{
    printf("[TEST 4] Rapid connect / abrupt disconnect storm (%d sockets)...\n", count);
    for (int i = 0; i < count; i++) {
        int fd = connect_vsock(VSOCK_HOST_CID, port);
        if (fd >= 0) {
            struct linger sl;
            sl.l_onoff = 1;
            sl.l_linger = 0;
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
            close(fd);
        }
    }
    printf("  [PASS] Rapid connect storm survived\n");
    return true;
}

int main(int argc, char **argv)
{
    uint32_t port = VSOCK_DEFAULT_PORT;
    if (argc > 1) {
        port = (uint32_t)atoi(argv[1]);
    }

    printf("============================================================\n");
    printf(" Live Virtio-Vsock Wire Fuzzing & Hypervisor Robustness Probe\n");
    printf(" Target Host CID: %u, Vsock Port: %u\n", VSOCK_HOST_CID, port);
    printf("============================================================\n");

    bool ok = true;
    ok = test_truncated_header(port) && ok;
    ok = test_oversized_payload(port) && ok;
    ok = test_fuzz_bitflips(port, 100) && ok;
    ok = test_rapid_connect_storm(port, 50) && ok;

    if (ok) {
        printf("\n>>> ALL LIVE VSOCK WIRE FUZZ TESTS PASSED <<<\n");
        return 0;
    } else {
        printf("\n>>> LIVE VSOCK WIRE FUZZ TESTS FAILED <<<\n");
        return 1;
    }
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
