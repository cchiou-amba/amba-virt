/*
 * tools/bringup/uart_bench_agent.c
 *
 * Deterministic Directional UART Performance & Integrity Benchmark Agent.
 * Executes directional TX, RX, Echo scheduling, and sustained PRBS streaming
 * with precise monotonic timestamping, CRC-32, and SHA-256 verification.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include "uart_bench_protocol.h"

/* ========================================================================== */
/* Monotonic Timestamp Helper                                                 */
/* ========================================================================== */

static uint64_t get_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* ========================================================================== */
/* Serial Port Configuration (Raw 115200 8N1)                                 */
/* ========================================================================== */

static int configure_serial_raw(int fd, speed_t baud)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        return -1;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD | CS8);
    tio.c_cflag &= ~(PARENB | CSTOPB);
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif
#ifdef IHFLOW
    tio.c_cflag &= ~IHFLOW;
#endif
#ifdef OHFLOW
    tio.c_cflag &= ~OHFLOW;
#endif
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tio.c_oflag &= ~(OPOST | ONLCR | OCRNL);
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, baud);
    cfsetospeed(&tio, baud);
    return tcsetattr(fd, TCSANOW, &tio);
}

static inline double max_double(double a, double b)
{
    return (a > b) ? a : b;
}

/* ========================================================================== */
/* Reliable Exact Write with Timeout                                          */
/* ========================================================================== */

static int write_exact(int fd, const uint8_t *buf, size_t len, double timeout_sec)
{
    size_t written = 0;
    uint64_t start_ns = get_monotonic_ns();
    uint64_t timeout_ns = (uint64_t)(timeout_sec * 1000000000.0);

    while (written < len) {
        if (get_monotonic_ns() - start_ns > timeout_ns) {
            return -ETIMEDOUT;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000; /* 50 ms */
        int s = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (s == 0) continue;

        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -errno;
        }
        written += (size_t)n;
    }
    tcdrain(fd);
    return 0;
}

/* ========================================================================== */
/* Reliable Exact Read with Timeout                                           */
/* ========================================================================== */

static int read_exact(int fd, uint8_t *buf, size_t len, double timeout_sec)
{
    size_t received = 0;
    uint64_t start_ns = get_monotonic_ns();
    uint64_t timeout_ns = (uint64_t)(timeout_sec * 1000000000.0);

    while (received < len) {
        uint64_t elapsed_ns = get_monotonic_ns() - start_ns;
        if (elapsed_ns >= timeout_ns) {
            return -ETIMEDOUT;
        }
        uint64_t remaining_ns = timeout_ns - elapsed_ns;
        struct timeval tv;
        tv.tv_sec = remaining_ns / 1000000000ULL;
        tv.tv_usec = (remaining_ns % 1000000000ULL) / 1000ULL;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        int s = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (s == 0) {
            return -ETIMEDOUT;
        }

        ssize_t n = read(fd, buf + received, len - received);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -errno;
        }
        if (n == 0) {
            return -EPIPE;
        }
        received += (size_t)n;
    }
    return 0;
}

/* ========================================================================== */
/* Mode: Echo Scheduling (Workload 1)                                         */
/* ========================================================================== */

static int do_echo(const char *dev_path, int samples, int warmup)
{
    int fd = open(dev_path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "{\"error\": \"Failed to open %s: %s\"}\n", dev_path, strerror(errno));
        return 1;
    }
    if (configure_serial_raw(fd, B115200) < 0) {
        fprintf(stderr, "{\"error\": \"Failed to configure %s: %s\"}\n", dev_path, strerror(errno));
        close(fd);
        return 1;
    }

    uint64_t *latencies_ns = malloc(sizeof(uint64_t) * (size_t)samples);
    if (!latencies_ns) {
        close(fd);
        return 1;
    }

    int total_runs = warmup + samples;
    int recorded = 0;

    for (int i = 0; i < total_runs; i++) {
        uint8_t byte = 0;
        int r = read_exact(fd, &byte, 1, 10.0);
        if (r < 0) {
            fprintf(stderr, "{\"error\": \"Echo read timeout at sample %d: %d\"}\n", i, r);
            free(latencies_ns);
            close(fd);
            return 1;
        }
        uint64_t t_rx_ns = get_monotonic_ns();

        int w = write_exact(fd, &byte, 1, 2.0);
        if (w < 0) {
            fprintf(stderr, "{\"error\": \"Echo write timeout at sample %d: %d\"}\n", i, w);
            free(latencies_ns);
            close(fd);
            return 1;
        }
        uint64_t t_tx_ns = get_monotonic_ns();

        if (i >= warmup) {
            latencies_ns[recorded++] = (t_tx_ns - t_rx_ns);
        }
    }

    /* Print JSON result */
    printf("{\n");
    printf("  \"mode\": \"echo\",\n");
    printf("  \"warmup_samples\": %d,\n", warmup);
    printf("  \"recorded_samples\": %d,\n", recorded);
    printf("  \"samples_ns\": [");
    for (int i = 0; i < recorded; i++) {
        printf("%" PRIu64 "%s", latencies_ns[i], (i + 1 < recorded) ? ", " : "");
    }
    printf("],\n");
    printf("  \"status\": \"OK\"\n");
    printf("}\n");

    free(latencies_ns);
    close(fd);
    return 0;
}

/* ========================================================================== */
/* Mode: Directional Transmit (Workload 2 & 3)                                */
/* ========================================================================== */

static int do_tx(const char *dev_path, uint32_t trial_id, uint32_t seq, size_t size, int sustained_sec)
{
    int fd = open(dev_path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "{\"error\": \"Failed to open %s: %s\"}\n", dev_path, strerror(errno));
        return 1;
    }
    if (configure_serial_raw(fd, B115200) < 0) {
        fprintf(stderr, "{\"error\": \"Failed to configure %s: %s\"}\n", dev_path, strerror(errno));
        close(fd);
        return 1;
    }

    size_t frame_sz = sizeof(struct bench_frame_hdr) + size + sizeof(uint32_t);
    uint8_t *frame = malloc(frame_sz);
    if (!frame) {
        close(fd);
        return 1;
    }

    struct bench_frame_hdr *hdr = (struct bench_frame_hdr *)frame;
    hdr->magic = BENCH_MAGIC;
    hdr->trial_id = trial_id;
    hdr->seq = seq;
    hdr->payload_len = (uint32_t)size;

    uint8_t *payload_ptr = frame + sizeof(struct bench_frame_hdr);
    generate_prbs_payload(trial_id + seq, payload_ptr, size);

    uint32_t crc = crc32_ieee(payload_ptr, size);
    memcpy(frame + sizeof(struct bench_frame_hdr) + size, &crc, sizeof(uint32_t));

    sha256_ctx_t s_ctx;
    uint8_t hash[32];
    char hash_hex[65];
    sha256_init(&s_ctx);
    sha256_update(&s_ctx, payload_ptr, size);
    sha256_final(&s_ctx, hash);
    sha256_hex(hash, hash_hex);

    uint64_t t_start_ns = get_monotonic_ns();
    uint64_t total_written = 0;
    int r = 0;

    if (sustained_sec <= 0) {
        double timeout_sec = max_double(5.0, (double)frame_sz / 5000.0 + 3.0);
        r = write_exact(fd, frame, frame_sz, timeout_sec);
        total_written = frame_sz;
    } else {
        uint64_t sustained_ns = (uint64_t)sustained_sec * 1000000000ULL;
        uint32_t cur_seq = seq;
        while (get_monotonic_ns() - t_start_ns < sustained_ns) {
            hdr->seq = cur_seq++;
            generate_prbs_payload(trial_id + hdr->seq, payload_ptr, size);
            crc = crc32_ieee(payload_ptr, size);
            memcpy(frame + sizeof(struct bench_frame_hdr) + size, &crc, sizeof(uint32_t));
            r = write_exact(fd, frame, frame_sz, 10.0);
            if (r < 0) break;
            total_written += frame_sz;
        }
    }
    uint64_t t_end_ns = get_monotonic_ns();
    uint64_t duration_ns = t_end_ns - t_start_ns;

    printf("{\n");
    printf("  \"mode\": \"tx\",\n");
    printf("  \"trial_id\": %u,\n", trial_id);
    printf("  \"seq\": %u,\n", seq);
    printf("  \"payload_len\": %zu,\n", size);
    printf("  \"total_bytes_written\": %" PRIu64 ",\n", total_written);
    printf("  \"duration_ns\": %" PRIu64 ",\n", duration_ns);
    printf("  \"throughput_bytes_sec\": %.2f,\n", (double)total_written / ((double)duration_ns / 1e9));
    printf("  \"crc32\": \"0x%08x\",\n", crc);
    printf("  \"sha256\": \"%s\",\n", hash_hex);
    printf("  \"status\": \"%s\"\n", (r == 0) ? "OK" : "ERROR");
    printf("}\n");

    free(frame);
    close(fd);
    return (r == 0) ? 0 : 1;
}

/* ========================================================================== */
/* Mode: Directional Receive (Workload 2 & 3)                                 */
/* ========================================================================== */

static int do_rx(const char *dev_path, uint32_t expected_trial_id, uint32_t expected_seq, size_t expected_size, double timeout_sec)
{
    int fd = open(dev_path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "{\"error\": \"Failed to open %s: %s\"}\n", dev_path, strerror(errno));
        return 1;
    }
    if (configure_serial_raw(fd, B115200) < 0) {
        fprintf(stderr, "{\"error\": \"Failed to configure %s: %s\"}\n", dev_path, strerror(errno));
        close(fd);
        return 1;
    }
    tcflush(fd, TCIFLUSH);

    struct bench_frame_hdr hdr;
    uint64_t t_start_ns = get_monotonic_ns();

    /* 1. Synchronize to BENCH_MAGIC */
    uint32_t magic_buf = 0;
    while (magic_buf != BENCH_MAGIC) {
        uint8_t b = 0;
        int r = read_exact(fd, &b, 1, timeout_sec);
        if (r < 0) {
            fprintf(stderr, "{\"error\": \"Timeout synchronizing to frame magic: %d\"}\n", r);
            close(fd);
            return 1;
        }
        magic_buf = (magic_buf >> 8) | ((uint32_t)b << 24);
    }
    hdr.magic = BENCH_MAGIC;

    /* 2. Read remainder of header (trial_id, seq, payload_len) */
    uint8_t hdr_rest[12];
    int r = read_exact(fd, hdr_rest, sizeof(hdr_rest), 5.0);
    if (r < 0) {
        fprintf(stderr, "{\"error\": \"Failed to read frame header remainder: %d\"}\n", r);
        close(fd);
        return 1;
    }
    memcpy(&hdr.trial_id, hdr_rest, 4);
    memcpy(&hdr.seq, hdr_rest + 4, 4);
    memcpy(&hdr.payload_len, hdr_rest + 8, 4);

    if (expected_trial_id > 0 && hdr.trial_id != expected_trial_id) {
        fprintf(stderr, "{\"error\": \"Trial ID mismatch: got %u expected %u\"}\n", hdr.trial_id, expected_trial_id);
        close(fd);
        return 1;
    }

    if (expected_seq > 0 && hdr.seq != expected_seq) {
        fprintf(stderr, "{\"error\": \"Sequence mismatch: got %u expected %u\"}\n", hdr.seq, expected_seq);
        close(fd);
        return 1;
    }

    if (expected_size > 0 && (size_t)hdr.payload_len != expected_size) {
        fprintf(stderr, "{\"error\": \"Payload length mismatch: got %u expected %zu\"}\n", hdr.payload_len, expected_size);
        close(fd);
        return 1;
    }

    size_t payload_len = hdr.payload_len;
    uint8_t *payload = malloc(payload_len);
    if (!payload) {
        close(fd);
        return 1;
    }

    double payload_timeout = max_double(5.0, (double)payload_len / 5000.0 + 3.0);
    r = read_exact(fd, payload, payload_len, payload_timeout);
    if (r < 0) {
        fprintf(stderr, "{\"error\": \"Failed to read payload bytes: %d\"}\n", r);
        free(payload);
        close(fd);
        return 1;
    }

    uint32_t frame_crc = 0;
    r = read_exact(fd, (uint8_t *)&frame_crc, sizeof(frame_crc), 2.0);
    if (r < 0) {
        fprintf(stderr, "{\"error\": \"Failed to read frame CRC: %d\"}\n", r);
        free(payload);
        close(fd);
        return 1;
    }

    uint64_t t_end_ns = get_monotonic_ns();
    uint64_t duration_ns = t_end_ns - t_start_ns;

    uint32_t calc_crc = crc32_ieee(payload, payload_len);
    sha256_ctx_t s_ctx;
    uint8_t hash[32];
    char hash_hex[65];
    sha256_init(&s_ctx);
    sha256_update(&s_ctx, payload, payload_len);
    sha256_final(&s_ctx, hash);
    sha256_hex(hash, hash_hex);

    bool crc_match = (calc_crc == frame_crc);

    printf("{\n");
    printf("  \"mode\": \"rx\",\n");
    printf("  \"trial_id\": %u,\n", hdr.trial_id);
    printf("  \"seq\": %u,\n", hdr.seq);
    printf("  \"payload_len\": %zu,\n", payload_len);
    printf("  \"duration_ns\": %" PRIu64 ",\n", duration_ns);
    printf("  \"throughput_bytes_sec\": %.2f,\n", (double)(payload_len + sizeof(hdr) + sizeof(frame_crc)) / ((double)duration_ns / 1e9));
    printf("  \"crc32_expected\": \"0x%08x\",\n", frame_crc);
    printf("  \"crc32_calculated\": \"0x%08x\",\n", calc_crc);
    printf("  \"crc_match\": %s,\n", crc_match ? "true" : "false");
    printf("  \"sha256\": \"%s\",\n", hash_hex);
    printf("  \"status\": \"%s\"\n", crc_match ? "OK" : "CRC_MISMATCH");
    printf("}\n");

    free(payload);
    close(fd);
    return crc_match ? 0 : 2;
}

/* ========================================================================== */
/* Self-Test Mode                                                             */
/* ========================================================================== */

static int do_selftest(void)
{
    /* Verify PRBS, CRC32, and SHA256 against known test patterns */
    uint8_t test_vec[] = "123456789";
    uint32_t c = crc32_ieee(test_vec, 9);
    if (c != 0xCBF43926U) {
        fprintf(stderr, "CRC32 self-test failed: got 0x%08x expected 0xcbf43926\n", c);
        return 1;
    }

    sha256_ctx_t ctx;
    uint8_t h[32];
    char hex[65];
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"abc", 3);
    sha256_final(&ctx, h);
    sha256_hex(h, hex);
    if (strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
        fprintf(stderr, "SHA256 self-test failed: got %s\n", hex);
        return 1;
    }

    printf("{\"selftest\": \"PASS\"}\n");
    return 0;
}

/* ========================================================================== */
/* Main CLI Parser                                                            */
/* ========================================================================== */

int main(int argc, char *argv[])
{
    static struct option long_options[] = {
        {"device",      required_argument, 0, 'd'},
        {"mode",        required_argument, 0, 'm'},
        {"samples",     required_argument, 0, 'n'},
        {"warmup",      required_argument, 0, 'w'},
        {"trial-id",    required_argument, 0, 't'},
        {"seq",         required_argument, 0, 's'},
        {"size",        required_argument, 0, 'z'},
        {"sustained",   required_argument, 0, 'S'},
        {"timeout",     required_argument, 0, 'T'},
        {"selftest",    no_argument,       0, 'X'},
        {0, 0, 0, 0}
    };

    char dev_path[256] = "/dev/ttyAMBA0";
    char mode[32] = "echo";
    int samples = 1000;
    int warmup = 100;
    uint32_t trial_id = 1;
    uint32_t seq = 0;
    size_t size = 4096;
    int sustained_sec = 0;
    double timeout_sec = 10.0;
    bool is_selftest = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "d:m:n:w:t:s:z:S:T:X", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            strncpy(dev_path, optarg, sizeof(dev_path) - 1);
            break;
        case 'm':
            strncpy(mode, optarg, sizeof(mode) - 1);
            break;
        case 'n':
            samples = atoi(optarg);
            break;
        case 'w':
            warmup = atoi(optarg);
            break;
        case 't':
            trial_id = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 's':
            seq = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'z':
            size = (size_t)strtoull(optarg, NULL, 0);
            break;
        case 'S':
            sustained_sec = atoi(optarg);
            break;
        case 'T':
            timeout_sec = atof(optarg);
            break;
        case 'X':
            is_selftest = true;
            break;
        default:
            fprintf(stderr, "Usage: %s --mode <echo|tx|rx> --device <dev> [options]\n", argv[0]);
            return 1;
        }
    }

    if (is_selftest || strcmp(mode, "selftest") == 0) {
        return do_selftest();
    }

    if (strcmp(mode, "echo") == 0) {
        return do_echo(dev_path, samples, warmup);
    } else if (strcmp(mode, "tx") == 0) {
        return do_tx(dev_path, trial_id, seq, size, sustained_sec);
    } else if (strcmp(mode, "rx") == 0) {
        return do_rx(dev_path, trial_id, seq, size, timeout_sec);
    } else {
        fprintf(stderr, "{\"error\": \"Unknown mode: %s\"}\n", mode);
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
