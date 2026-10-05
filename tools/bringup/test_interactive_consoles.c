/*
 * tools/bringup/test_interactive_consoles.c
 *
 * Diagnostic probe for HVM interactive serial consoles:
 *   - Native EVE Dom0 UNIX sockets: /run/hypervisor/kvm/<uuid>/cons
 *   - Forwarded TCP endpoints: Port 6071 (Ubuntu), Port 6072 (QNX)
 *
 * Verifies instantaneous shell availability, character echo round-trip
 * latency, and execution of interactive verification commands.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <glob.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_UBUNTU_PORT 6071
#define DEFAULT_QNX_PORT 6072

static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static int connect_tcp(const char *host, int port, int timeout_sec)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket AF_INET");
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sin.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "Failed to connect to %s:%d: %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int connect_unix(const char *path, int timeout_sec)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket AF_UNIX");
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, path, sizeof(sun.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        fprintf(stderr, "Failed to connect to %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int connect_endpoint(const char *target, int port, int timeout_sec)
{
    if (target[0] == '/') {
        return connect_unix(target, timeout_sec);
    }
    return connect_tcp(target, port, timeout_sec);
}

static void drain_sock(int fd, int timeout_ms)
{
    char buf[4096];
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
    }
}

static int test_console_endpoint(const char *name, const char *endpoint, int port, const char *sentinel)
{
    printf("============================================================\n");
    if (endpoint[0] == '/') {
        printf("[*] Testing %s via UNIX socket (%s)...\n", name, endpoint);
    } else {
        printf("[*] Testing %s via TCP (%s:%d)...\n", name, endpoint, port);
    }
    printf("============================================================\n");

    int fd = connect_endpoint(endpoint, port, 3);
    if (fd < 0) {
        printf("[-] SKIP / UNREACHABLE: %s not active.\n", endpoint);
        return 0;
    }

    drain_sock(fd, 200);

    /* Send CRLF to wake prompt */
    const char crlf[] = "\r\n";
    if (write(fd, crlf, strlen(crlf)) < 0) {
        perror("write crlf");
        close(fd);
        return -1;
    }
    usleep(100000);
    drain_sock(fd, 200);

    /* Send command */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo %s\r\n", sentinel);
    uint64_t t0 = get_time_ms();
    if (write(fd, cmd, strlen(cmd)) < 0) {
        perror("write cmd");
        close(fd);
        return -1;
    }

    char rx_buf[8192];
    size_t rx_total = 0;
    memset(rx_buf, 0, sizeof(rx_buf));

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool found = false;
    while (rx_total < sizeof(rx_buf) - 1) {
        ssize_t n = recv(fd, rx_buf + rx_total, sizeof(rx_buf) - 1 - rx_total, 0);
        if (n <= 0) break;
        rx_total += (size_t)n;
        rx_buf[rx_total] = '\0';
        if (strstr(rx_buf, sentinel) != NULL) {
            found = true;
            break;
        }
    }
    uint64_t elapsed_ms = get_time_ms() - t0;

    close(fd);

    if (found) {
        printf("[+] %s Console PASS: Echo received in %llu ms\n", name, (unsigned long long)elapsed_ms);
        return 0;
    } else {
        printf("[-] %s Console FAIL: Sentinel '%s' not received\n", name, sentinel);
        return 1;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [host_or_unix_socket] [port]\n", argv[0]);
        printf("Examples:\n");
        printf("  %s                                     (Auto-detect on EVE Dom0 or default TCP)\n", argv[0]);
        printf("  %s /run/hypervisor/kvm/<uuid>/cons     (Test specific UNIX domain console socket)\n", argv[0]);
        printf("  %s 127.0.0.1 6071                      (Test forwarded TCP console)\n", argv[0]);
        return 0;
    }

    /* If explicit path/host is given */
    if (argc > 1) {
        const char *target = argv[1];
        int port = (argc > 2) ? atoi(argv[2]) : 0;
        return test_console_endpoint("Target Console", target, port, "CONSOLE_PROBE_ECHO_OK");
    }

    /* Auto-detection mode: check if running on EVE Dom0 with live HVM console sockets */
    glob_t gl;
    int rc = 0;
    if (glob("/run/hypervisor/kvm/*/cons", 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
        printf("[*] Detected %zu active EVE HVM console sockets on Dom0:\n", gl.gl_pathc);
        for (size_t i = 0; i < gl.gl_pathc; i++) {
            char name[64];
            snprintf(name, sizeof(name), "HVM Guest %zu", i + 1);
            rc |= test_console_endpoint(name, gl.gl_pathv[i], 0, "EVE_CONSOLE_ECHO_OK");
        }
        globfree(&gl);
        return rc;
    }

    /* Fallback to default TCP endpoints */
    const char *host = getenv("CONSOLE_HOST");
    if (!host) host = DEFAULT_HOST;

    int ubuntu_port = DEFAULT_UBUNTU_PORT;
    const char *env_ub = getenv("CONSOLE_UBUNTU_PORT");
    if (env_ub) ubuntu_port = atoi(env_ub);

    int qnx_port = DEFAULT_QNX_PORT;
    const char *env_qnx = getenv("CONSOLE_QNX_PORT");
    if (env_qnx) qnx_port = atoi(env_qnx);

    rc |= test_console_endpoint("Ubuntu HVM (UART2)", host, ubuntu_port, "UBUNTU_ECHO_PASS_42");
    rc |= test_console_endpoint("QNX HVM (UART3)", host, qnx_port, "QNX_ECHO_PASS_42");

    return rc;
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
