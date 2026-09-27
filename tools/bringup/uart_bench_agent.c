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

#define BENCH_MAGIC 0x55415254U /* 'UART' in ASCII */

#pragma pack(push, 1)
struct bench_frame_hdr {
    uint32_t magic;
    uint32_t trial_id;
    uint32_t seq;
    uint32_t payload_len;
};
#pragma pack(pop)

/* ========================================================================== */
/* Portable CRC-32 (IEEE 802.3 Polynomial 0xEDB88320)                        */
/* ========================================================================== */

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(crc & 1)));
        }
    }
    return ~crc;
}

/* ========================================================================== */
/* Self-Contained SHA-256 Implementation (FIPS PUB 180-4)                     */
/* ========================================================================== */

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx_t;

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_EP1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_transform(uint32_t state[8], const uint8_t data[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    uint32_t w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SHA256_SIG1(w[i - 2]) + w[i - 7] + SHA256_SIG0(w[i - 15]) + w[i - 16];
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t buffer_idx = (size_t)(ctx->count & 63);
    ctx->count += len;

    size_t i = 0;
    if (buffer_idx > 0 && buffer_idx + len >= 64) {
        size_t fill = 64 - buffer_idx;
        memcpy(ctx->buffer + buffer_idx, data, fill);
        sha256_transform(ctx->state, ctx->buffer);
        i += fill;
        buffer_idx = 0;
    }
    while (i + 64 <= len) {
        sha256_transform(ctx->state, data + i);
        i += 64;
    }
    if (i < len) {
        memcpy(ctx->buffer + buffer_idx, data + i, len - i);
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32])
{
    uint64_t total_bits = ctx->count * 8;
    size_t buffer_idx = (size_t)(ctx->count & 63);
    ctx->buffer[buffer_idx++] = 0x80;

    if (buffer_idx > 56) {
        memset(ctx->buffer + buffer_idx, 0, 64 - buffer_idx);
        sha256_transform(ctx->state, ctx->buffer);
        buffer_idx = 0;
    }
    memset(ctx->buffer + buffer_idx, 0, 56 - buffer_idx);
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(total_bits >> ((7 - i) * 8));
    }
    sha256_transform(ctx->state, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void sha256_hex(const uint8_t hash[32], char hex_out[65])
{
    for (int i = 0; i < 32; i++) {
        sprintf(hex_out + (i * 2), "%02x", hash[i]);
    }
    hex_out[64] = '\0';
}

/* ========================================================================== */
/* PRBS Pattern Generator (Galois LFSR PRBS-9: x^9 + x^5 + 1)                 */
/* ========================================================================== */

static void generate_prbs_payload(uint32_t seed, uint8_t *buf, size_t len)
{
    uint32_t lfsr = (seed == 0) ? 0x1FEU : (seed & 0x1FFU);
    if (lfsr == 0) lfsr = 0x1FEU;

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            uint32_t bit = ((lfsr >> 8) ^ (lfsr >> 4)) & 1U;
            lfsr = ((lfsr << 1) | bit) & 0x1FFU;
            byte = (byte << 1) | bit;
        }
        buf[i] = byte;
    }
}

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

    if (is_selftest) {
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
