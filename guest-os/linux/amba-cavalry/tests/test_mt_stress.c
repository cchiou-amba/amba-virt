/*
 * cavalry_hvm_mt_stress.c
 *
 * Phase 4 Multi-Threaded Arena Race Contention Harness
 * Tests guest module arena_mutex serialization under concurrent RUN_DAGS ioctls.
 * Eliminates process-global nnctrl from the hot path; computes MD5 post-join.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cavalry_ioctl.h>
#include <cavalry_mem.h>
#include <nnctrl.h>
#include "nnctrl_priv.h"
#include "utils.h"

#define GOLDEN_MD5 "b2d1236c286a3c0704224fe4105eca49"

typedef struct {
	uint32_t state[4];
	uint32_t count[2];
	uint8_t  buffer[64];
} md5_ctx_t;

#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define FF(a, b, c, d, x, s, ac) { \
	(a) += F((b), (c), (d)) + (x) + (uint32_t)(ac); \
	(a) = ROTL((a), (s)); \
	(a) += (b); \
}
#define GG(a, b, c, d, x, s, ac) { \
	(a) += G((b), (c), (d)) + (x) + (uint32_t)(ac); \
	(a) = ROTL((a), (s)); \
	(a) += (b); \
}
#define HH(a, b, c, d, x, s, ac) { \
	(a) += H((b), (c), (d)) + (x) + (uint32_t)(ac); \
	(a) = ROTL((a), (s)); \
	(a) += (b); \
}
#define II(a, b, c, d, x, s, ac) { \
	(a) += I((b), (c), (d)) + (x) + (uint32_t)(ac); \
	(a) = ROTL((a), (s)); \
	(a) += (b); \
}

static void md5_transform(uint32_t state[4], const uint8_t block[64]);

static void md5_init(md5_ctx_t *ctx)
{
	ctx->count[0] = ctx->count[1] = 0;
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
}

static void md5_update(md5_ctx_t *ctx, const uint8_t *input, size_t input_len)
{
	size_t i, index, part_len;

	index = (size_t)((ctx->count[0] >> 3) & 0x3f);
	if ((ctx->count[0] += ((uint32_t)input_len << 3)) < ((uint32_t)input_len << 3))
		ctx->count[1]++;
	ctx->count[1] += ((uint32_t)input_len >> 29);

	part_len = 64 - index;
	if (input_len >= part_len) {
		memcpy(&ctx->buffer[index], input, part_len);
		md5_transform(ctx->state, ctx->buffer);
		for (i = part_len; i + 63 < input_len; i += 64)
			md5_transform(ctx->state, &input[i]);
		index = 0;
	} else {
		i = 0;
	}
	memcpy(&ctx->buffer[index], &input[i], input_len - i);
}

static void md5_final(uint8_t digest[16], md5_ctx_t *ctx)
{
	uint8_t bits[8];
	size_t index, pad_len;
	static const uint8_t padding[64] = { 0x80 };

	for (int i = 0; i < 4; i++) {
		bits[i] = (uint8_t)((ctx->count[0] >> (i * 8)) & 0xff);
		bits[i + 4] = (uint8_t)((ctx->count[1] >> (i * 8)) & 0xff);
	}

	index = (size_t)((ctx->count[0] >> 3) & 0x3f);
	pad_len = (index < 56) ? (56 - index) : (120 - index);
	md5_update(ctx, padding, pad_len);
	md5_update(ctx, bits, 8);

	for (int i = 0; i < 4; i++) {
		digest[i * 4]     = (uint8_t)((ctx->state[i]) & 0xff);
		digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
		digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
		digest[i * 4 + 3] = (uint8_t)((ctx->state[i] >> 24) & 0xff);
	}
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];

	for (int i = 0; i < 16; i++) {
		x[i] = ((uint32_t)block[i * 4]) |
		       (((uint32_t)block[i * 4 + 1]) << 8) |
		       (((uint32_t)block[i * 4 + 2]) << 16) |
		       (((uint32_t)block[i * 4 + 3]) << 24);
	}

	FF(a, b, c, d, x[ 0],  7, 0xd76aa478);
	FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
	FF(c, d, a, b, x[ 2], 17, 0x242070db);
	FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
	FF(a, b, c, d, x[ 4],  7, 0xf57c0faf);
	FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
	FF(c, d, a, b, x[ 6], 17, 0xa8304613);
	FF(b, c, d, a, x[ 7], 22, 0xfd469501);
	FF(a, b, c, d, x[ 8],  7, 0x698098d8);
	FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
	FF(c, d, a, b, x[10], 17, 0xffff5bb1);
	FF(b, c, d, a, x[11], 22, 0x895cd7be);
	FF(a, b, c, d, x[12],  7, 0x6b901122);
	FF(d, a, b, c, x[13], 12, 0xfd987193);
	FF(c, d, a, b, x[14], 17, 0xa679438e);
	FF(b, c, d, a, x[15], 22, 0x49b40821);

	GG(a, b, c, d, x[ 1],  5, 0xf61e2562);
	GG(d, a, b, c, x[ 6],  9, 0xc040b340);
	GG(c, d, a, b, x[11], 14, 0x265e5a51);
	GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
	GG(a, b, c, d, x[ 5],  5, 0xd62f105d);
	GG(d, a, b, c, x[10],  9, 0x02441453);
	GG(c, d, a, b, x[15], 14, 0xd8a1e681);
	GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
	GG(a, b, c, d, x[ 9],  5, 0x21e1cde6);
	GG(d, a, b, c, x[14],  9, 0xc33707d6);
	GG(c, d, a, b, x[ 3], 14, 0xf4d50d87);
	GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
	GG(a, b, c, d, x[13],  5, 0xa9e3e905);
	GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);
	GG(c, d, a, b, x[ 7], 14, 0x676f02d9);
	GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

	HH(a, b, c, d, x[ 5],  4, 0xfffa3942);
	HH(d, a, b, c, x[ 8], 11, 0x8771f681);
	HH(c, d, a, b, x[11], 16, 0x6d9d6122);
	HH(b, c, d, a, x[14], 23, 0xfde5380c);
	HH(a, b, c, d, x[ 1],  4, 0xa4beea44);
	HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
	HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60);
	HH(b, c, d, a, x[10], 23, 0xbebfbc70);
	HH(a, b, c, d, x[13],  4, 0x289b7ec6);
	HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
	HH(c, d, a, b, x[ 3], 16, 0xd4ef3085);
	HH(b, c, d, a, x[ 6], 23, 0x04881d05);
	HH(a, b, c, d, x[ 9],  4, 0xd9d4d039);
	HH(d, a, b, c, x[12], 11, 0xe6db99e5);
	HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
	HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

	II(a, b, c, d, x[ 0],  6, 0xf4292244);
	II(d, a, b, c, x[ 7], 10, 0x432aff97);
	II(c, d, a, b, x[14], 15, 0xab9423a7);
	II(b, c, d, a, x[ 5], 21, 0xfc93a039);
	II(a, b, c, d, x[12],  6, 0x655b59c3);
	II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
	II(c, d, a, b, x[10], 15, 0xffeff47d);
	II(b, c, d, a, x[ 1], 21, 0x85845dd1);
	II(a, b, c, d, x[ 8],  6, 0x6fa87e4f);
	II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
	II(c, d, a, b, x[ 6], 15, 0xa3014314);
	II(b, c, d, a, x[13], 21, 0x4e0811a1);
	II(a, b, c, d, x[ 4],  6, 0xf7537e82);
	II(d, a, b, c, x[11], 10, 0xbd3af235);
	II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb);
	II(b, c, d, a, x[ 9], 21, 0xeb86d391);

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}

static void compute_md5_str(const void *data, size_t len, char *out_str)
{
	md5_ctx_t ctx;
	uint8_t digest[16];

	md5_init(&ctx);
	md5_update(&ctx, (const uint8_t *)data, len);
	md5_final(digest, &ctx);

	for (int i = 0; i < 16; i++)
		sprintf(&out_str[i * 2], "%02x", digest[i]);
	out_str[32] = '\0';
}

struct worker_arg {
	int thread_idx;
	int iterations;
	void *req_buf;
	size_t req_size;
	int success_count;
	int error_count;
};

static void *worker_func(void *arg)
{
	struct worker_arg *w = (struct worker_arg *)arg;
	int fd;
	int i;

	fd = open("/dev/cavalry", O_RDWR);
	if (fd < 0) {
		perror("worker: open /dev/cavalry");
		w->error_count = w->iterations;
		return NULL;
	}

	for (i = 0; i < w->iterations; i++) {
		struct cavalry_run_dags *run = (struct cavalry_run_dags *)w->req_buf;
		int ret;

		ret = ioctl(fd, CAVALRY_RUN_DAGS, run);
		if (ret < 0 || run->rval != 0) {
			fprintf(stderr, "[worker %d] run %d FAILED: ret=%d, errno=%d, rval=%u\n",
				w->thread_idx, i, ret, errno, run->rval);
			w->error_count++;
		} else if (run->exec_total_ticks < 24160 || run->exec_total_ticks > 24190) {
			fprintf(stderr, "[worker %d] run %d tick outlier: %u\n",
				w->thread_idx, i, run->exec_total_ticks);
			w->error_count++;
		} else {
			w->success_count++;
		}
	}

	close(fd);
	return NULL;
}

int main(int argc, char *argv[])
{
	const char *model_path;
	int num_threads = 4;
	int iterations = 25;
	int fd_cav = -1;
	int net_id = -1;
	struct net_cfg net_cf = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct net_mem net_m = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	struct cavalry_run_dags *orig_run = NULL;
	size_t req_size = 0;
	pthread_t *threads = NULL;
	struct worker_arg *args = NULL;
	struct timeval t_start, t_end;
	double elapsed_ms;
	int total_success = 0;
	int total_errors = 0;
	char final_md5[33] = { 0 };
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path_to_vp_clk_cavalry.bin> [threads] [iterations]\n", argv[0]);
		return 1;
	}
	model_path = argv[1];
	if (argc >= 3)
		num_threads = atoi(argv[2]);
	if (argc >= 4)
		iterations = atoi(argv[3]);

	if (num_threads <= 0) num_threads = 4;
	if (iterations <= 0) iterations = 25;

	printf("===============================================================\n");
	printf(" Ambarella Class D Cavalry Phase 4 Multi-Threaded Stress Harness\n");
	printf(" Model: %s\n", model_path);
	printf(" Concurrency: %d threads x %d iterations = %d total runs\n",
	       num_threads, iterations, num_threads * iterations);
	printf("===============================================================\n");

	fd_cav = open("/dev/cavalry", O_RDWR);
	if (fd_cav < 0) {
		perror("open /dev/cavalry");
		return 1;
	}

	if (cavalry_mem_init(fd_cav, 0) < 0 || nnctrl_init(fd_cav, 0) < 0) {
		fprintf(stderr, "Initialization failed\n");
		close(fd_cav);
		return 1;
	}

	net_cf.net_file = (char *)model_path;
	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "nnctrl_init_net failed\n");
		close(fd_cav);
		return 1;
	}

	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, (void **)&net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "cavalry_mem_alloc failed\n");
		close(fd_cav);
		return 1;
	}
	net_m.mem_size = net_cf.net_mem_total;

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "nnctrl_load_net failed\n");
		close(fd_cav);
		return 1;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "nnctrl_get_net_io_cfg failed\n");
		close(fd_cav);
		return 1;
	}

	/* Initialize input with pattern, clear output */
	memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
	memset(net_out.out_desc[0].virt, 0, net_out.out_desc[0].size);
	asm volatile("dsb sy" ::: "memory");

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	if (!pnet || !pnet->execute_ctx[0].run_dags) {
		fprintf(stderr, "Failed to retrieve initialized run_dags descriptor\n");
		close(fd_cav);
		return 1;
	}

	orig_run = pnet->execute_ctx[0].run_dags;
	req_size = sizeof(struct cavalry_run_dags) +
		   orig_run->dag_cnt * sizeof(struct cavalry_dag_desc);

	threads = calloc(num_threads, sizeof(pthread_t));
	args = calloc(num_threads, sizeof(struct worker_arg));

	for (i = 0; i < num_threads; i++) {
		args[i].thread_idx = i;
		args[i].iterations = iterations;
		args[i].req_size = req_size;
		args[i].req_buf = malloc(req_size);
		memcpy(args[i].req_buf, orig_run, req_size);
	}

	printf("[*] Spawning %d worker threads...\n", num_threads);
	gettimeofday(&t_start, NULL);

	for (i = 0; i < num_threads; i++) {
		pthread_create(&threads[i], NULL, worker_func, &args[i]);
	}

	for (i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);
		total_success += args[i].success_count;
		total_errors += args[i].error_count;
		free(args[i].req_buf);
	}

	gettimeofday(&t_end, NULL);
	elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0 +
		     (t_end.tv_usec - t_start.tv_usec) / 1000.0;

	/* Assert post-join MD5 on the shared output tensor */
	compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, final_md5);

	printf("\n================ Concurrency Results ================\n");
	printf(" Total Dispatches: %d\n", num_threads * iterations);
	printf(" Successful:       %d\n", total_success);
	printf(" Errors / Torns:   %d\n", total_errors);
	printf(" Total Time:       %.2f ms (average %.2f ms / dispatch)\n",
	       elapsed_ms, elapsed_ms / (num_threads * iterations));
	printf(" Post-Join MD5:    %s (golden: %s)\n", final_md5, GOLDEN_MD5);

	int md5_match = (strcmp(final_md5, GOLDEN_MD5) == 0);
	if (md5_match && total_errors == 0 && total_success == num_threads * iterations) {
		printf(">>> [PASS] 100%% ARENA CONTENTION PASS - ZERO TORN PACKETS <<<\n");
	} else {
		fprintf(stderr, ">>> [FAIL] CONCURRENCY STRESS FAILED <<<\n");
	}

	/* Clean up */
	cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
	close(fd_cav);
	free(threads);
	free(args);

	return (total_errors == 0 && md5_match) ? 0 : 1;
}
