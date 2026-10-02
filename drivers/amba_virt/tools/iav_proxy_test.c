/*
 * drivers/amba_virt/tools/iav_proxy_test.c
 *
 * Envelope 2 Qualification Test: Live Camera Feed to Cavalry Path B
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <uapi/specific/iav_ioctl.h>
#include <cavalry_ioctl.h>
#include <cavalry_mem.h>
#include <nnctrl.h>
#include <nnctrl_priv.h>

#include "iav_tap_abi.h"
#include "iav_proxy.h"

#define IAV_DEVICE_NODE "/dev/iav"
#define CAVALRY_DEVICE_NODE "/dev/cavalry"

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t read_sof_interrupts(void)
{
    FILE *fp = fopen("/proc/interrupts", "r");
    if (!fp) return 0;

    char line[512];
    uint64_t count = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "vin0_idsp_sof")) {
            char *p = strchr(line, ':');
            if (p) {
                unsigned long c0 = 0, c1 = 0, c2 = 0, c3 = 0;
                if (sscanf(p + 1, "%lu %lu %lu %lu", &c0, &c1, &c2, &c3) >= 1) {
                    count = c0 + c1 + c2 + c3;
                }
            }
            break;
        }
    }
    fclose(fp);
    return count;
}

static uint32_t read_dsp_state(void)
{
    FILE *fp = fopen("/proc/ambarella/dsp_state", "r");
    if (!fp) return 0xFFFFFFFF;

    char line[256];
    uint32_t state = 0xFFFFFFFF;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "bitmap:")) {
            char *p = strstr(line, "0x");
            if (p) {
                state = (uint32_t)strtoul(p, NULL, 16);
                break;
            }
        }
    }
    fclose(fp);
    return state;
}

struct test_ctx {
    int fd_iav;
    uint8_t *dsp_base;
    unsigned long dsp_len;

    int fd_cav;
    int net_id;
    struct net_cfg net_cf;
    struct net_mem net_m;
    struct net_input_cfg net_in;
    struct net_output_cfg net_out;

    struct iav_tap_ring ring;
    pthread_t tap_thread;
    bool tap_thread_running;

    uint32_t target_iterations;
    const char *model_path;
    bool dual_client_mode;
};

static void *tap_producer_thread(void *arg)
{
    struct test_ctx *ctx = (struct test_ctx *)arg;
    uint32_t slot_idx = 0;

    while (g_running) {
        struct iav_querydesc query;
        memset(&query, 0, sizeof(query));
        query.qid = IAV_DESC_CANVAS;
        query.arg.canvas.canvas_id = 0;
        query.arg.canvas.non_block_flag = 0;

        if (ioctl(ctx->fd_iav, IAV_IOC_QUERY_DESC, &query) < 0) {
            if (errno == EINTR) continue;
            perror("IAV_IOC_QUERY_DESC in producer thread");
            break;
        }

        struct iav_yuv_cap *yuv = &query.arg.canvas.yuv;
        uint32_t width = yuv->width;
        uint32_t height = yuv->height;
        uint32_t pitch = yuv->pitch;
        uint32_t format = yuv->format;
        uint64_t seq = yuv->seq_num;
        unsigned long y_offset = yuv->y_addr_offset;
        unsigned long uv_offset = yuv->uv_addr_offset;

        uint32_t y_bytes = height * pitch;
        uint32_t uv_bytes = (height / 2) * pitch;
        uint32_t total_bytes = y_bytes + uv_bytes;

        struct iav_tap_slot *slot = &ctx->ring.slots[slot_idx];
        if (slot->state == IAV_TAP_SLOT_HELD) {
            ctx->ring.drop_count++;
            continue;
        }

        slot->state = IAV_TAP_SLOT_WRITING;
        uint64_t t0 = get_time_us();
        memcpy(slot->payload, ctx->dsp_base + y_offset, y_bytes);
        memcpy(slot->payload + y_bytes, ctx->dsp_base + uv_offset, uv_bytes);
        uint64_t t1 = get_time_us();

        slot->copy_time_us = t1 - t0;
        slot->seq = seq;
        slot->generation++;
        slot->sof_count = read_sof_interrupts();
        slot->dsp_pts = yuv->dsp_pts;
        slot->mono_pts = yuv->mono_pts;
        slot->width = width;
        slot->height = height;
        slot->pitch = pitch;
        slot->fourcc = format;
        slot->nbytes = total_bytes;
        slot->refcount = ctx->dual_client_mode ? 2 : 1;

        slot->state = IAV_TAP_SLOT_PUBLISHED;
        ctx->ring.published_count++;
        ctx->ring.active_width = width;
        ctx->ring.active_height = height;
        ctx->ring.active_pitch = pitch;
        ctx->ring.active_fourcc = format;

        slot_idx = (slot_idx + 1) % IAV_TAP_RING_SLOTS;
    }

    return NULL;
}

static int init_pipeline(struct test_ctx *ctx)
{
    /* 1. Open IAV */
    ctx->fd_iav = open(IAV_DEVICE_NODE, O_RDWR);
    if (ctx->fd_iav < 0) {
        perror("open /dev/iav");
        return -1;
    }

    struct iav_querymem qmem;
    memset(&qmem, 0, sizeof(qmem));
    qmem.mid = IAV_MEM_PARTITION;
    qmem.arg.partition.pid = IAV_PART_DSP;
    if (ioctl(ctx->fd_iav, IAV_IOC_QUERY_MEMBLOCK, &qmem) < 0) {
        perror("IAV_IOC_QUERY_MEMBLOCK");
        return -1;
    }

    ctx->dsp_len = qmem.arg.partition.mem.length;
    ctx->dsp_base = mmap(NULL, ctx->dsp_len, PROT_READ, MAP_SHARED,
                         ctx->fd_iav, qmem.arg.partition.mem.addr);
    if (ctx->dsp_base == MAP_FAILED) {
        perror("mmap IAV_PART_DSP");
        return -1;
    }

    /* 2. Open Cavalry & Load Model */
    ctx->fd_cav = open(CAVALRY_DEVICE_NODE, O_RDWR);
    if (ctx->fd_cav < 0) {
        perror("open /dev/cavalry");
        return -1;
    }

    if (cavalry_mem_init(ctx->fd_cav, 0) < 0 || nnctrl_init(ctx->fd_cav, 0) < 0) {
        fprintf(stderr, "Failed to initialize cavalry memory / nnctrl\n");
        return -1;
    }

    ctx->net_cf.net_file = (char *)ctx->model_path;
    ctx->net_cf.net_all_input_no_mem = 0;
    ctx->net_cf.net_all_output_no_mem = 0;
    ctx->net_cf.no_chip_check = 1;

    ctx->net_id = nnctrl_init_net(&ctx->net_cf, NULL, NULL);
    if (ctx->net_id < 0) {
        fprintf(stderr, "nnctrl_init_net failed for %s\n", ctx->model_path);
        return -1;
    }

    if (cavalry_mem_alloc(&ctx->net_cf.net_mem_total, &ctx->net_m.phy_addr,
                          (void **)&ctx->net_m.virt_addr, 0) < 0) {
        perror("cavalry_mem_alloc");
        return -1;
    }
    ctx->net_m.mem_size = ctx->net_cf.net_mem_total;

    if (nnctrl_load_net(ctx->net_id, &ctx->net_m, NULL, NULL) < 0 ||
        nnctrl_get_net_io_cfg(ctx->net_id, &ctx->net_in, &ctx->net_out) < 0) {
        fprintf(stderr, "nnctrl_load_net / get_net_io_cfg failed\n");
        return -1;
    }

    /* 3. Initialize iav_proxy */
    iav_proxy_init(&ctx->ring);
    if (iav_proxy_attach(1, 1) < 0) {
        fprintf(stderr, "iav_proxy_attach failed\n");
        return -1;
    }

    /* 4. Start Tap Producer Thread */
    if (pthread_create(&ctx->tap_thread, NULL, tap_producer_thread, ctx) != 0) {
        perror("pthread_create tap producer");
        return -1;
    }
    ctx->tap_thread_running = true;

    /* Wait for first published frame */
    while (ctx->ring.published_count == 0 && g_running) {
        usleep(10000);
    }

    printf("Live camera pipeline initialized (%ux%u). Cavalry model loaded: %s\n",
           ctx->ring.active_width, ctx->ring.active_height, ctx->model_path);
    return 0;
}

struct sample_record {
    uint64_t seq;
    void *input_copy;
    size_t input_size;
    void *output_copy;
    size_t output_size;
};

#define NUM_SAMPLE_CHECKS 10

static int run_e2_qualification(struct test_ctx *ctx)
{
    uint32_t completed_dags = 0;
    uint32_t bitexact_passes = 0;
    struct sample_record samples[NUM_SAMPLE_CHECKS];
    memset(samples, 0, sizeof(samples));
    int sample_idx = 0;

    size_t in_size = ctx->net_in.in_desc[0].size;
    size_t out_size = ctx->net_out.out_desc[0].size;

    for (int i = 0; i < NUM_SAMPLE_CHECKS; i++) {
        samples[i].input_copy = malloc(in_size);
        samples[i].output_copy = malloc(out_size);
        samples[i].input_size = in_size;
        samples[i].output_size = out_size;
    }

    uint64_t start_sof = read_sof_interrupts();
    uint64_t start_time = get_time_us();

    printf("=== Starting Envelope 2 Live Inference Qualification (%u DAG runs) ===\n",
           ctx->target_iterations);

    while (g_running && completed_dags < ctx->target_iterations) {
        /* 1. Fetch newest published frame from tap ring */
        struct iav_tap_slot *target = NULL;
        uint64_t max_seq = 0;

        for (int s = 0; s < IAV_TAP_RING_SLOTS; s++) {
            if (ctx->ring.slots[s].state == IAV_TAP_SLOT_PUBLISHED &&
                ctx->ring.slots[s].seq >= max_seq) {
                max_seq = ctx->ring.slots[s].seq;
                target = &ctx->ring.slots[s];
            }
        }

        if (!target) {
            usleep(2000);
            continue;
        }

        /* Hold slot briefly to copy payload */
        target->state = IAV_TAP_SLOT_HELD;
        target->refcount++;
        uint64_t seq = target->seq;

        /* Copy into model input buffer */
        size_t copy_bytes = (target->nbytes < in_size) ? target->nbytes : in_size;
        memcpy(ctx->net_in.in_desc[0].virt, target->payload, copy_bytes);

        /* Publish-then-copy contract: Drop refcount BEFORE VisORC execution */
        target->refcount = 0;
        target->state = IAV_TAP_SLOT_EMPTY;

        bool is_sample = (completed_dags % (ctx->target_iterations / NUM_SAMPLE_CHECKS) == 0) &&
                         (sample_idx < NUM_SAMPLE_CHECKS);
        if (is_sample) {
            samples[sample_idx].seq = seq;
            memcpy(samples[sample_idx].input_copy, ctx->net_in.in_desc[0].virt, in_size);
        }

        /* Execute VisORC DAG */
        if (nnctrl_run_net(ctx->net_id, NULL, NULL, NULL, NULL) < 0) {
            fprintf(stderr, "nnctrl_run_net failed at iter %u\n", completed_dags);
            break;
        }

        if (is_sample) {
            memcpy(samples[sample_idx].output_copy, ctx->net_out.out_desc[0].virt, out_size);
            sample_idx++;
        }

        completed_dags++;
        if (completed_dags % 50 == 0) {
            printf("Progress: %u / %u DAG completions on live frames (seq=%lu, drops=%lu)\n",
                   completed_dags, ctx->target_iterations, seq, ctx->ring.drop_count);
        }
    }

    uint64_t end_time = get_time_us();
    uint64_t end_sof = read_sof_interrupts();
    double duration_s = (double)(end_time - start_time) / 1000000.0;
    uint32_t final_dsp = read_dsp_state();

    /* 2. Execute Bit-Exact Parity Check on 10 Saved Samples */
    printf("\n=== Running Bit-Exact Re-run Validation on %d Saved Live Frames ===\n", sample_idx);
    void *re_output = malloc(out_size);

    for (int i = 0; i < sample_idx; i++) {
        /* Feed saved input copy */
        memcpy(ctx->net_in.in_desc[0].virt, samples[i].input_copy, in_size);
        if (nnctrl_run_net(ctx->net_id, NULL, NULL, NULL, NULL) < 0) {
            fprintf(stderr, "Re-run failed for sample %d\n", i);
            continue;
        }
        memcpy(re_output, ctx->net_out.out_desc[0].virt, out_size);

        if (memcmp(samples[i].output_copy, re_output, out_size) == 0) {
            bitexact_passes++;
            printf("  [PASS] Sample #%d (seq=%lu): 100%% bit-exact parity match\n",
                   i + 1, samples[i].seq);
        } else {
            fprintf(stderr, "  [FAIL] Sample #%d (seq=%lu): Output mismatch detected!\n",
                    i + 1, samples[i].seq);
        }
    }
    free(re_output);

    for (int i = 0; i < NUM_SAMPLE_CHECKS; i++) {
        free(samples[i].input_copy);
        free(samples[i].output_copy);
    }

    printf("\n=== Envelope 2 Qualification Report ===\n");
    printf("Total DAG Completions  : %u / %u\n", completed_dags, ctx->target_iterations);
    printf("Bit-Exact Sample Passes: %u / %d\n", bitexact_passes, sample_idx);
    printf("Tap Drop Count         : %lu\n", ctx->ring.drop_count);
    printf("Throughput             : %.2f FPS (Duration: %.3f s)\n",
           duration_s > 0 ? (double)completed_dags / duration_s : 0.0, duration_s);
    printf("SOF Interrupt Delta    : %lu -> %lu (+%lu)\n",
           start_sof, end_sof, end_sof - start_sof);
    printf("DSP Final State Bitmap : 0x%x\n", final_dsp);

    if (completed_dags >= ctx->target_iterations &&
        bitexact_passes >= (uint32_t)sample_idx &&
        final_dsp == 0x0) {
        printf("\n>>> ENVELOPE 2 EXIT GATE: PASSED <<<\n");
        return 0;
    } else {
        printf("\n>>> ENVELOPE 2 EXIT GATE: FAILED <<<\n");
        return 1;
    }
}

static uint32_t calc_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

static pthread_mutex_t g_visorc_hw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;

#define E5_SAMPLE_CHECKS 10

struct client_worker_args {
    struct test_ctx *ctx;
    uint32_t client_id;
    uint32_t client_cid;
    uint32_t iterations;
    uint32_t completed;
    uint64_t last_seq;
    uint8_t *in_buf;
    uint8_t *out_buf;
    size_t in_size;
    size_t out_size;
    uint64_t *recorded_seqs;
    uint32_t *recorded_crcs;
    uint64_t sample_seqs[E5_SAMPLE_CHECKS];
    uint8_t *sample_outputs[E5_SAMPLE_CHECKS];
    int sample_count;
};

static void *e5_client_worker(void *arg)
{
    struct client_worker_args *cargs = (struct client_worker_args *)arg;
    struct test_ctx *ctx = cargs->ctx;

    iav_proxy_attach(cargs->client_cid, 1);

    while (g_running && cargs->completed < cargs->iterations) {
        struct iav_tap_slot *target = NULL;
        uint64_t max_seq = 0;

        pthread_mutex_lock(&g_ring_lock);
        for (int s = 0; s < IAV_TAP_RING_SLOTS; s++) {
            if (ctx->ring.slots[s].state == IAV_TAP_SLOT_PUBLISHED &&
                ctx->ring.slots[s].seq > cargs->last_seq) {
                if (max_seq == 0 || ctx->ring.slots[s].seq < max_seq) {
                    max_seq = ctx->ring.slots[s].seq;
                    target = &ctx->ring.slots[s];
                }
            }
        }

        if (!target) {
            pthread_mutex_unlock(&g_ring_lock);
            usleep(1000);
            continue;
        }

        uint64_t seq = target->seq;
        size_t copy_bytes = (target->nbytes < cargs->in_size) ? target->nbytes : cargs->in_size;
        memcpy(cargs->in_buf, target->payload, copy_bytes);

        if (target->refcount > 0)
            target->refcount--;

        if (target->refcount == 0) {
            target->state = IAV_TAP_SLOT_EMPTY;
        } else {
            target->state = IAV_TAP_SLOT_PUBLISHED;
        }
        pthread_mutex_unlock(&g_ring_lock);

        cargs->last_seq = seq;

        /* Execute VisORC serialized */
        pthread_mutex_lock(&g_visorc_hw_lock);
        memcpy(ctx->net_in.in_desc[0].virt, cargs->in_buf, cargs->in_size);
        if (nnctrl_run_net(ctx->net_id, NULL, NULL, NULL, NULL) < 0) {
            pthread_mutex_unlock(&g_visorc_hw_lock);
            fprintf(stderr, "Client %u nnctrl_run_net failed at iter %u\n",
                    cargs->client_id, cargs->completed);
            break;
        }
        memcpy(cargs->out_buf, ctx->net_out.out_desc[0].virt, cargs->out_size);
        pthread_mutex_unlock(&g_visorc_hw_lock);

        cargs->recorded_seqs[cargs->completed] = seq;
        cargs->recorded_crcs[cargs->completed] = calc_crc32(cargs->out_buf, cargs->out_size);

        if (cargs->sample_count < E5_SAMPLE_CHECKS && (cargs->completed % (cargs->iterations / E5_SAMPLE_CHECKS) == 0)) {
            cargs->sample_seqs[cargs->sample_count] = seq;
            memcpy(cargs->sample_outputs[cargs->sample_count], cargs->out_buf, cargs->out_size);
            cargs->sample_count++;
        }

        cargs->completed++;
        if (cargs->completed % 50 == 0) {
            printf("Client %u (CID %u): %u / %u live DAG completions (seq=%lu, crc=0x%08x)\n",
                   cargs->client_id, cargs->client_cid, cargs->completed, cargs->iterations,
                   seq, cargs->recorded_crcs[cargs->completed - 1]);
        }
    }

    iav_proxy_detach(cargs->client_cid, 1);
    return NULL;
}

static int run_e5_dual_client_qualification(struct test_ctx *ctx)
{
    printf("=== Starting Envelope 5 Dual-Client Live Camera Qualification (%u frames per client) ===\n",
           ctx->target_iterations);

    size_t in_size = ctx->net_in.in_desc[0].size;
    size_t out_size = ctx->net_out.out_desc[0].size;

    struct client_worker_args c0 = { 0 }, c1 = { 0 };
    c0.ctx = ctx; c0.client_id = 0; c0.client_cid = 5201; c0.iterations = ctx->target_iterations;
    c0.in_size = in_size; c0.out_size = out_size;
    c0.in_buf = malloc(in_size); c0.out_buf = malloc(out_size);
    c0.recorded_seqs = calloc(ctx->target_iterations, sizeof(uint64_t));
    c0.recorded_crcs = calloc(ctx->target_iterations, sizeof(uint32_t));

    c1.ctx = ctx; c1.client_id = 1; c1.client_cid = 5203; c1.iterations = ctx->target_iterations;
    c1.in_size = in_size; c1.out_size = out_size;
    c1.in_buf = malloc(in_size); c1.out_buf = malloc(out_size);
    c1.recorded_seqs = calloc(ctx->target_iterations, sizeof(uint64_t));
    c1.recorded_crcs = calloc(ctx->target_iterations, sizeof(uint32_t));

    for (int i = 0; i < E5_SAMPLE_CHECKS; i++) {
        c0.sample_outputs[i] = malloc(out_size);
        c1.sample_outputs[i] = malloc(out_size);
    }

    uint64_t start_sof = read_sof_interrupts();
    uint64_t start_time = get_time_us();

    pthread_t t0, t1;
    pthread_create(&t0, NULL, e5_client_worker, &c0);
    pthread_create(&t1, NULL, e5_client_worker, &c1);

    pthread_join(t0, NULL);
    pthread_join(t1, NULL);

    uint64_t end_time = get_time_us();
    uint64_t end_sof = read_sof_interrupts();
    double duration_s = (double)(end_time - start_time) / 1000000.0;
    uint32_t final_dsp = read_dsp_state();

    /* Match common sequence numbers between Client 0 and Client 1 */
    uint32_t common_seq_count = 0;
    uint32_t crc_matches = 0;
    uint32_t byte_exact_sample_matches = 0;
    int samples_compared = 0;

    for (uint32_t i = 0; i < c0.completed; i++) {
        uint64_t s0 = c0.recorded_seqs[i];
        for (uint32_t j = 0; j < c1.completed; j++) {
            if (c1.recorded_seqs[j] == s0) {
                common_seq_count++;
                if (c0.recorded_crcs[i] == c1.recorded_crcs[j]) {
                    crc_matches++;
                }
                break;
            }
        }
    }

    for (int i = 0; i < c0.sample_count; i++) {
        for (int j = 0; j < c1.sample_count; j++) {
            if (c0.sample_seqs[i] == c1.sample_seqs[j] && c0.sample_seqs[i] > 0) {
                samples_compared++;
                if (memcmp(c0.sample_outputs[i], c1.sample_outputs[j], out_size) == 0) {
                    byte_exact_sample_matches++;
                }
            }
        }
    }

    printf("\n=== Envelope 5 Qualification Report ===\n");
    printf("Client 0 Inferences    : %u / %u\n", c0.completed, ctx->target_iterations);
    printf("Client 1 Inferences    : %u / %u\n", c1.completed, ctx->target_iterations);
    printf("Common Seqs Evaluated  : %u\n", common_seq_count);
    printf("CRC Parity Matches     : %u / %u (%.1f%%)\n",
           crc_matches, common_seq_count,
           common_seq_count > 0 ? 100.0 * (double)crc_matches / common_seq_count : 0.0);
    printf("Byte-Exact Samples     : %d / %d\n", byte_exact_sample_matches, samples_compared);
    printf("Tap Drop Count         : %lu\n", ctx->ring.drop_count);
    printf("Dual-Client Duration   : %.3f s\n", duration_s);
    printf("SOF Interrupt Delta    : %lu -> %lu (+%lu)\n",
           start_sof, end_sof, end_sof - start_sof);
    printf("DSP Final State Bitmap : 0x%x\n", final_dsp);

    int pass = (c0.completed >= ctx->target_iterations &&
                c1.completed >= ctx->target_iterations &&
                common_seq_count > 0 &&
                crc_matches == common_seq_count &&
                final_dsp == 0x0);

    if (pass) {
        printf("\n>>> ENVELOPE 5 EXIT GATE: PASSED <<<\n");
    } else {
        printf("\n>>> ENVELOPE 5 EXIT GATE: FAILED <<<\n");
    }

    /* Cleanup buffers */
    free(c0.in_buf); free(c0.out_buf);
    free(c1.in_buf); free(c1.out_buf);
    for (int i = 0; i < E5_SAMPLE_CHECKS; i++) {
        free(c0.sample_outputs[i]);
        free(c1.sample_outputs[i]);
    }
    free(c0.recorded_seqs); free(c0.recorded_crcs);
    free(c1.recorded_seqs); free(c1.recorded_crcs);

    return pass ? 0 : 1;
}

int main(int argc, char *argv[])
{
    struct test_ctx *ctx = calloc(1, sizeof(struct test_ctx));
    if (!ctx) return 1;

    ctx->target_iterations = 300;
    ctx->model_path = "/persist/fresh-bringup/vp_clk_cavalry.bin";
    bool dual_client_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            ctx->target_iterations = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            ctx->model_path = argv[++i];
        } else if (strcmp(argv[i], "--dual-client") == 0) {
            dual_client_mode = true;
        }
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    ctx->dual_client_mode = dual_client_mode;

    if (init_pipeline(ctx) < 0) {
        fprintf(stderr, "Pipeline initialization failed\n");
        return 1;
    }

    int ret = 0;
    if (dual_client_mode) {
        ret = run_e5_dual_client_qualification(ctx);
    } else {
        ret = run_e2_qualification(ctx);
    }

    g_running = 0;
    if (ctx->tap_thread_running) {
        pthread_join(ctx->tap_thread, NULL);
    }

    iav_proxy_detach(1, 1);
    iav_proxy_cleanup();

    cavalry_mem_free(ctx->net_m.mem_size, ctx->net_m.phy_addr, ctx->net_m.virt_addr);
    nnctrl_exit_net(ctx->net_id);
    nnctrl_exit();
    cavalry_mem_exit();
    close(ctx->fd_cav);

    munmap(ctx->dsp_base, ctx->dsp_len);
    close(ctx->fd_iav);
    free(ctx);

    return ret;
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
