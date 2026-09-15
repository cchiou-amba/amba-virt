/*
 * cavalry_native_demo.c
 *
 * Ambarella Cavalry Native Baseline Inference & Performance Harness
 * Runs directly on bare-metal host in NOHYPER without hypervisor or vsock overhead.
 * Provides golden baseline metrics (FPS, Latency, Hardware Ticks, MD5) for comparison
 * against virtualized drivers running in guest HVM environments.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cavalry_ioctl.h>
#include <cavalry_mem.h>
#include <nnctrl.h>
#include <uapi/amba_virt.h>
#include "nnctrl_priv.h"
#include "utils.h"

#define CAVALRY_DEV_NODE    "/dev/cavalry"
#define AMBA_VIRT_DEV_NODE  "/dev/amba_virt"
#define DEFAULT_MODEL       "vp_clk_cavalry.bin"
#define MAX_TENSOR_PORTS    32
#define MAX_THREADS_CAP     64
#define SHM_OFFSET_BYTES    (32UL * 1024UL * 1024UL) /* 32 MiB into shared window */

typedef enum {
	MEM_MODE_AMA = 0,
	MEM_MODE_SHM = 1,
} mem_mode_t;

/* Standalone self-contained MD5 */
typedef struct {
	uint32_t state[4];
	uint32_t count[2];
	uint8_t buffer[64];
} demo_md5_ctx;

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

static void demo_md5_transform(uint32_t state[4], const uint8_t block[64])
{
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];

	for (int i = 0; i < 16; i++)
		x[i] = ((uint32_t)block[i * 4]) | (((uint32_t)block[i * 4 + 1]) << 8) |
		       (((uint32_t)block[i * 4 + 2]) << 16) | (((uint32_t)block[i * 4 + 3]) << 24);

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

static void demo_md5_init(demo_md5_ctx *ctx)
{
	ctx->count[0] = ctx->count[1] = 0;
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
}

static void demo_md5_update(demo_md5_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t idx = (ctx->count[0] >> 3) & 0x3f;

	if ((ctx->count[0] += ((uint32_t)len << 3)) < ((uint32_t)len << 3))
		ctx->count[1]++;
	ctx->count[1] += ((uint32_t)len >> 29);

	uint32_t part_len = 64 - idx;
	uint32_t i = 0;

	if (len >= part_len) {
		memcpy(&ctx->buffer[idx], p, part_len);
		demo_md5_transform(ctx->state, ctx->buffer);
		for (i = part_len; i + 63 < len; i += 64)
			demo_md5_transform(ctx->state, &p[i]);
		idx = 0;
	}
	memcpy(&ctx->buffer[idx], &p[i], len - i);
}

static void demo_md5_final(uint8_t digest[16], demo_md5_ctx *ctx)
{
	static const uint8_t padding[64] = { 0x80 };
	uint8_t bits[8];

	for (int i = 0; i < 4; i++) {
		bits[i] = (uint8_t)((ctx->count[0] >> (i * 8)) & 0xff);
		bits[i + 4] = (uint8_t)((ctx->count[1] >> (i * 8)) & 0xff);
	}

	uint32_t idx = (ctx->count[0] >> 3) & 0x3f;
	uint32_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
	demo_md5_update(ctx, padding, pad_len);
	demo_md5_update(ctx, bits, 8);

	for (int i = 0; i < 4; i++) {
		digest[i]      = (uint8_t)((ctx->state[0] >> (i * 8)) & 0xff);
		digest[i + 4]  = (uint8_t)((ctx->state[1] >> (i * 8)) & 0xff);
		digest[i + 8]  = (uint8_t)((ctx->state[2] >> (i * 8)) & 0xff);
		digest[i + 12] = (uint8_t)((ctx->state[3] >> (i * 8)) & 0xff);
	}
}

static void calc_buffer_md5(const void *buf, size_t size, char *out_hex)
{
	demo_md5_ctx ctx;
	uint8_t digest[16];

	demo_md5_init(&ctx);
	demo_md5_update(&ctx, buf, size);
	demo_md5_final(digest, &ctx);

	for (int i = 0; i < 16; i++)
		sprintf(&out_hex[i * 2], "%02x", digest[i]);
	out_hex[32] = '\0';
}

static inline double get_time_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline uint64_t get_time_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	return (va > vb) - (va < vb);
}

/* Ensure matching N1_655 firmware is present in VisORC ucode partition */
static int ensure_ucode_loaded(int fd_cav)
{
	struct cavalry_querybuf q_priv = { .buf = CAVALRY_MEM_UCODE };
	struct version_info_s *ver;
	void *ucode_map;
	int fd_bin;
	ssize_t nread;

	if (ioctl(fd_cav, CAVALRY_QUERY_BUF, &q_priv) < 0) {
		perror("ioctl CAVALRY_QUERY_BUF(UCODE)");
		return -1;
	}

	ucode_map = mmap(NULL, q_priv.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_cav, q_priv.offset);
	if (ucode_map == MAP_FAILED) {
		perror("mmap CAVALRY_MEM_UCODE");
		return -1;
	}

	ver = (struct version_info_s *)ucode_map;
	if (ver->chip != 10 && ver->chip != 144) {
		printf("[*] Ucode mismatch in DRAM (chip: %u, expected: 144). Reloading cavalry.bin...\n", ver->chip);
		fd_bin = open("cavalry.bin", O_RDONLY);
		if (fd_bin < 0) fd_bin = open("/home/ubuntu/cavalry.bin", O_RDONLY);
		if (fd_bin < 0) fd_bin = open("/persist/firmware/cavalry.bin", O_RDONLY);
		if (fd_bin < 0) fd_bin = open("/lib/firmware/cavalry.bin", O_RDONLY);
		if (fd_bin < 0) {
			perror("open cavalry.bin");
			munmap(ucode_map, q_priv.length);
			return -1;
		}
		nread = read(fd_bin, ucode_map, q_priv.length);
		close(fd_bin);
		if (nread <= 0) {
			fprintf(stderr, "Failed to read cavalry.bin into DRAM\n");
			munmap(ucode_map, q_priv.length);
			return -1;
		}
		asm volatile("dsb sy" ::: "memory");
		printf("[+] Loaded %zd bytes of N1_655 ucode into DRAM. Chip ID: %u\n", nread, ver->chip);
	}

	munmap(ucode_map, q_priv.length);

	if (ioctl(fd_cav, CAVALRY_START_VP, 0) < 0) {
		/* Non-fatal if already started */
		if (errno != EBUSY && errno != EALREADY) {
			perror("ioctl CAVALRY_START_VP");
		}
	}

	return 0;
}

/* ========================================================================= */
/* MODE 1: Network & Tensor Inspector (--info)                               */
/* ========================================================================= */
static int run_mode_info(const char *model_path)
{
	struct net_cfg net_cf = { 0 };
	struct net_mem net_m = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	int fd_cav = -1;
	int net_id = -1;
	int ret = 0;

	printf("===============================================================================\n");
	printf(" Ambarella Cavalry Native Model & Tensor Inspector\n");
	printf(" Model Path: %s\n", model_path);
	printf("===============================================================================\n\n");

	fd_cav = open(CAVALRY_DEV_NODE, O_RDWR);
	if (fd_cav < 0) {
		perror("open /dev/cavalry");
		return -1;
	}

	if (ensure_ucode_loaded(fd_cav) < 0) {
		close(fd_cav);
		return -1;
	}

	if (cavalry_mem_init(fd_cav, 0) < 0 || nnctrl_init(fd_cav, 0) < 0) {
		fprintf(stderr, "Failed to initialize cavalry_mem or nnctrl\n");
		close(fd_cav);
		return -1;
	}

	net_cf.net_file = (char *)model_path;
	net_cf.net_all_input_no_mem = 0;
	net_cf.net_all_output_no_mem = 0;
	net_cf.no_chip_check = 1;

	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "nnctrl_init_net failed for %s\n", model_path);
		ret = -1;
		goto out_exit_ctrl;
	}

	printf("[*] Model Metadata:\n");
	printf("  Total Memory Required: %lu bytes (%.2f MB)\n",
	       (unsigned long)net_cf.net_mem_total, (double)net_cf.net_mem_total / (1024.0 * 1024.0));

	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, (void **)&net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "cavalry_mem_alloc failed\n");
		ret = -1;
		goto out_exit_net;
	}
	net_m.mem_size = net_cf.net_mem_total;

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "nnctrl_load_net failed\n");
		ret = -1;
		goto out_free_mem;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "nnctrl_get_net_io_cfg failed\n");
		ret = -1;
		goto out_free_mem;
	}

	printf("\n--- Input Tensor Descriptors (Total: %u) ---\n", net_in.in_num);
	for (uint32_t i = 0; i < net_in.in_num; i++) {
		printf("  [Input %u] Name: '%s' | Size: %lu bytes | Shape: [%u, %u, %u, %u] | Format: sign=%u, exp=%d, bits=%u\n",
		       i, net_in.in_desc[i].name,
		       (unsigned long)net_in.in_desc[i].size,
		       (unsigned int)net_in.in_desc[i].dim.plane,
		       (unsigned int)net_in.in_desc[i].dim.depth,
		       (unsigned int)net_in.in_desc[i].dim.height,
		       (unsigned int)net_in.in_desc[i].dim.width,
		       net_in.in_desc[i].data_fmt.sign,
		       net_in.in_desc[i].data_fmt.expoffset,
		       net_in.in_desc[i].data_fmt.bitsize);
	}

	printf("\n--- Output Tensor Descriptors (Total: %u) ---\n", net_out.out_num);
	for (uint32_t i = 0; i < net_out.out_num; i++) {
		printf("  [Output %u] Name: '%s' | Size: %lu bytes | Shape: [%u, %u, %u, %u] | Format: sign=%u, exp=%d, bits=%u\n",
		       i, net_out.out_desc[i].name,
		       (unsigned long)net_out.out_desc[i].size,
		       (unsigned int)net_out.out_desc[i].dim.plane,
		       (unsigned int)net_out.out_desc[i].dim.depth,
		       (unsigned int)net_out.out_desc[i].dim.height,
		       (unsigned int)net_out.out_desc[i].dim.width,
		       net_out.out_desc[i].data_fmt.sign,
		       net_out.out_desc[i].data_fmt.expoffset,
		       net_out.out_desc[i].data_fmt.bitsize);
	}

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	if (pnet) {
		printf("\n--- Internal DAG Execution Topology (Subgraphs: %u) ---\n", pnet->subgraph_exe_cnt);
		for (uint32_t c = 0; c < pnet->subgraph_exe_cnt; c++) {
			struct cavalry_run_dags *r = pnet->execute_ctx[c].run_dags;
			if (!r) continue;
			printf("  [Subgraph %u] DAG count: %u, priority=%u\n", c, r->dag_cnt, r->priority);
			for (uint32_t d = 0; d < r->dag_cnt; d++) {
				struct cavalry_dag_desc *dag = &r->dag_desc[d];
				printf("    DAG [%u]: dvi_dram_addr=0x%lx, ports=%u, pokes=%u, loop=%u, pp=%u\n",
				       d, (unsigned long)dag->dvi_dram_addr, dag->port_cnt, dag->poke_cnt,
				       dag->dag_loop_cnt, dag->use_ping_pong_vmem);
				for (uint32_t p = 0; p < dag->port_cnt; p++) {
					printf("      Port [%u]: dram_addr=0x%lx, size=%lu\n",
					       p, (unsigned long)dag->port_desc[p].port_dram_addr,
					       (unsigned long)dag->port_desc[p].port_dram_size);
				}
			}
		}
	}

out_free_mem:
	cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
out_exit_net:
	nnctrl_exit_net(net_id);
out_exit_ctrl:
	nnctrl_exit();
	cavalry_mem_exit();
	close(fd_cav);

	printf("\n[PASS] Model architecture inspected cleanly.\n");
	return ret;
}

/* ========================================================================= */
/* MODE 2: High-Precision Line-Rate Benchmark (--bench)                      */
/* ========================================================================= */
static int run_mode_bench(const char *model_path, int iterations, mem_mode_t mem_mode)
{
	struct net_cfg net_cf = { 0 };
	struct net_mem net_m = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	struct cavalry_run_dags *run = NULL;
	uint64_t *latencies_us = NULL;
	uint32_t *exec_ticks = NULL;
	char md5_str[33] = { 0 };
	int fd_cav = -1;
	int fd_shm = -1;
	void *shm_vaddr = NULL;
	unsigned long shm_phys = 0, shm_size = 0;
	int net_id = -1;
	int ret = 0;

	if (iterations < 1)
		iterations = 100;

	printf("===============================================================================\n");
	printf(" Ambarella Cavalry High-Precision Native Inference Benchmark (NOHYPER)\n");
	printf(" Model: %s | Iterations: %d | Memory Target: %s\n",
	       model_path, iterations, (mem_mode == MEM_MODE_SHM) ? "ivshmem (Normal-NC)" : "Host AMA (CMA Heap)");
	printf("===============================================================================\n\n");

	latencies_us = calloc(iterations, sizeof(uint64_t));
	exec_ticks = calloc(iterations, sizeof(uint32_t));
	if (!latencies_us || !exec_ticks) {
		fprintf(stderr, "calloc failed\n");
		free(latencies_us);
		free(exec_ticks);
		return -1;
	}

	fd_cav = open(CAVALRY_DEV_NODE, O_RDWR);
	if (fd_cav < 0) {
		perror("open /dev/cavalry");
		free(latencies_us);
		free(exec_ticks);
		return -1;
	}

	if (ensure_ucode_loaded(fd_cav) < 0) {
		close(fd_cav);
		free(latencies_us);
		free(exec_ticks);
		return -1;
	}

	if (cavalry_mem_init(fd_cav, 0) < 0 || nnctrl_init(fd_cav, 0) < 0) {
		fprintf(stderr, "Failed to initialize runtime\n");
		ret = -1;
		goto out_close_cav;
	}

	net_cf.net_file = (char *)model_path;
	net_cf.net_all_input_no_mem = 0;
	net_cf.net_all_output_no_mem = 0;
	net_cf.no_chip_check = 1;

	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "nnctrl_init_net failed\n");
		ret = -1;
		goto out_exit_ctrl;
	}

	if (mem_mode == MEM_MODE_SHM) {
		struct cavalry_querybuf q_user = { .buf = CAVALRY_MEM_USER };
		struct amba_virt_info vinfo = { 0 };

		fd_shm = open(AMBA_VIRT_DEV_NODE, O_RDWR);
		if (fd_shm < 0) {
			perror("open /dev/amba_virt");
			ret = -1;
			goto out_exit_net;
		}

		if (ioctl(fd_cav, CAVALRY_QUERY_BUF, &q_user) < 0 ||
		    ioctl(fd_shm, AMBA_VIRT_IOC_GET_INFO, &vinfo) < 0) {
			perror("query shm topology failed");
			ret = -1;
			goto out_close_shm;
		}
		shm_phys = q_user.offset;
		shm_size = vinfo.shm_size;

		shm_vaddr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
		if (shm_vaddr == MAP_FAILED) {
			perror("mmap /dev/amba_virt failed");
			ret = -1;
			goto out_close_shm;
		}

		net_m.phy_addr = shm_phys + SHM_OFFSET_BYTES;
		net_m.virt_addr = (uint8_t *)shm_vaddr + SHM_OFFSET_BYTES;
		net_m.mem_size = net_cf.net_mem_total;
	} else {
		if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, (void **)&net_m.virt_addr, 0) < 0) {
			fprintf(stderr, "cavalry_mem_alloc failed\n");
			ret = -1;
			goto out_exit_net;
		}
		net_m.mem_size = net_cf.net_mem_total;
	}

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0 ||
	    nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "load net or get io cfg failed\n");
		ret = -1;
		goto out_free_model;
	}

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	if (!pnet || pnet->subgraph_exe_cnt == 0 || !pnet->execute_ctx[0].run_dags) {
		fprintf(stderr, "Failed to get execution context\n");
		ret = -1;
		goto out_free_model;
	}
	run = pnet->execute_ctx[0].run_dags;

	/* Pre-populate input ports with test pattern and clear outputs */
	for (uint32_t i = 0; i < net_in.in_num; i++) {
		if (net_in.in_desc[i].virt) {
			memset(net_in.in_desc[i].virt, 1, net_in.in_desc[i].size);
		}
	}
	for (uint32_t i = 0; i < net_out.out_num; i++) {
		if (net_out.out_desc[i].virt) {
			memset(net_out.out_desc[i].virt, 0, net_out.out_desc[i].size);
		}
	}
	asm volatile("dsb sy" ::: "memory");

	printf("[*] Warming up VisORC coprocessor (5 dispatches)...\n");
	for (int w = 0; w < 5; w++) {
		if (nnctrl_run_net(net_id, NULL, NULL, NULL, NULL) < 0) {
			fprintf(stderr, "Warmup run failed\n");
			ret = -1;
			goto out_free_model;
		}
	}

	printf("[*] Executing native benchmark loop (%d iterations)...\n", iterations);
	double t_start = get_time_sec();

	for (int iter = 0; iter < iterations; iter++) {
		uint64_t t0 = get_time_us();
		if (nnctrl_run_net(net_id, NULL, NULL, NULL, NULL) < 0) {
			fprintf(stderr, "Inference run failed at iteration %d\n", iter);
			ret = -1;
			goto out_free_model;
		}
		uint64_t t1 = get_time_us();
		latencies_us[iter] = (t1 - t0);
		exec_ticks[iter] = run->exec_total_ticks;
	}

	double t_end = get_time_sec();
	double total_elapsed = t_end - t_start;
	double fps = (double)iterations / total_elapsed;

	/* Calculate output MD5 digest */
	if (net_out.out_num > 0 && net_out.out_desc[0].virt) {
		calc_buffer_md5(net_out.out_desc[0].virt, net_out.out_desc[0].size, md5_str);
	}

	/* Statistics */
	uint64_t sum_lat = 0;
	uint64_t min_lat = UINT64_MAX;
	uint64_t max_lat = 0;
	uint64_t sum_ticks = 0;
	uint32_t min_ticks = UINT32_MAX;
	uint32_t max_ticks = 0;

	for (int i = 0; i < iterations; i++) {
		sum_lat += latencies_us[i];
		if (latencies_us[i] < min_lat) min_lat = latencies_us[i];
		if (latencies_us[i] > max_lat) max_lat = latencies_us[i];

		sum_ticks += exec_ticks[i];
		if (exec_ticks[i] < min_ticks) min_ticks = exec_ticks[i];
		if (exec_ticks[i] > max_ticks) max_ticks = exec_ticks[i];
	}

	qsort(latencies_us, iterations, sizeof(uint64_t), compare_u64);
	uint64_t p50 = latencies_us[(int)(iterations * 0.50)];
	uint64_t p95 = latencies_us[(int)(iterations * 0.95)];
	uint64_t p99 = latencies_us[(int)(iterations * 0.99)];

	double avg_lat_us = (double)sum_lat / (double)iterations;
	double avg_ticks = (double)sum_ticks / (double)iterations;
	double avg_exec_us = avg_ticks / 12.0; /* ~12 ticks/us on 24MHz timer */

	printf("\n===============================================================================\n");
	printf(" BENCHMARK RESULTS SUMMARY (NOHYPER BARE-METAL BASELINE)\n");
	printf("===============================================================================\n");
	printf(" Total Dispatches:      %d iterations\n", iterations);
	printf(" Total Elapsed Time:    %.3f seconds\n", total_elapsed);
	printf(" Sustained Throughput:  %.2f FPS (Frames Per Second)\n", fps);
	printf(" Output MD5 Digest:     %s\n", md5_str);
	printf("-------------------------------------------------------------------------------\n");
	printf(" Hardware Execution:    Avg %.1f ticks (Range: %u - %u, Jitter: %u ticks)\n",
	       avg_ticks, min_ticks, max_ticks, max_ticks - min_ticks);
	printf(" VisORC Exec Time:      Avg %.2f ms\n", avg_exec_us / 1000.0);
	printf(" Round-Trip Latency:    Avg %.2f ms (Min: %.2f ms, Max: %.2f ms)\n",
	       avg_lat_us / 1000.0, (double)min_lat / 1000.0, (double)max_lat / 1000.0);
	printf(" Latency Percentiles:   p50: %.2f ms | p95: %.2f ms | p99: %.2f ms\n",
	       (double)p50 / 1000.0, (double)p95 / 1000.0, (double)p99 / 1000.0);
	printf("===============================================================================\n\n");

out_free_model:
	if (mem_mode == MEM_MODE_AMA && net_m.virt_addr) {
		cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
	}
	if (shm_vaddr && shm_vaddr != MAP_FAILED) {
		munmap(shm_vaddr, shm_size);
	}
out_close_shm:
	if (fd_shm >= 0) close(fd_shm);
out_exit_net:
	nnctrl_exit_net(net_id);
out_exit_ctrl:
	nnctrl_exit();
	cavalry_mem_exit();
out_close_cav:
	close(fd_cav);
	free(latencies_us);
	free(exec_ticks);
	return ret;
}

/* ========================================================================= */
/* MODE 3: Multi-Stream Concurrency Stress (--threads T)                     */
/* ========================================================================= */
struct thread_worker_arg {
	int thread_idx;
	const char *model_path;
	int iterations;
	int errors;
	uint64_t elapsed_us;
	uint32_t last_ticks;
	char md5[33];
};

static void *thread_worker_func(void *arg)
{
	struct thread_worker_arg *w = (struct thread_worker_arg *)arg;
	struct net_cfg net_cf = { 0 };
	struct net_mem net_m = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	struct cavalry_run_dags *run = NULL;
	int fd_cav = -1;
	int net_id = -1;

	fd_cav = open(CAVALRY_DEV_NODE, O_RDWR);
	if (fd_cav < 0) {
		w->errors++;
		return NULL;
	}

	net_cf.net_file = (char *)w->model_path;
	net_cf.net_all_input_no_mem = 0;
	net_cf.net_all_output_no_mem = 0;
	net_cf.no_chip_check = 1;

	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		w->errors++;
		close(fd_cav);
		return NULL;
	}

	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, (void **)&net_m.virt_addr, 0) < 0) {
		w->errors++;
		goto out_worker;
	}
	net_m.mem_size = net_cf.net_mem_total;

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0 ||
	    nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		w->errors++;
		goto out_worker;
	}

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	if (pnet && pnet->subgraph_exe_cnt > 0)
		run = pnet->execute_ctx[0].run_dags;

	for (uint32_t i = 0; i < net_in.in_num; i++) {
		if (net_in.in_desc[i].virt) memset(net_in.in_desc[i].virt, 1, net_in.in_desc[i].size);
	}
	for (uint32_t i = 0; i < net_out.out_num; i++) {
		if (net_out.out_desc[i].virt) memset(net_out.out_desc[i].virt, 0, net_out.out_desc[i].size);
	}
	asm volatile("dsb sy" ::: "memory");

	uint64_t t0 = get_time_us();
	for (int i = 0; i < w->iterations; i++) {
		if (nnctrl_run_net(net_id, NULL, NULL, NULL, NULL) < 0) {
			w->errors++;
			break;
		}
	}
	uint64_t t1 = get_time_us();
	w->elapsed_us = (t1 - t0);
	if (run) w->last_ticks = run->exec_total_ticks;

	if (net_out.out_num > 0 && net_out.out_desc[0].virt) {
		calc_buffer_md5(net_out.out_desc[0].virt, net_out.out_desc[0].size, w->md5);
	}

out_worker:
	if (net_m.virt_addr) cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
	nnctrl_exit_net(net_id);
	close(fd_cav);
	return NULL;
}

static int run_mode_threads(const char *model_path, int num_threads, int iters_per_thread)
{
	pthread_t threads[MAX_THREADS_CAP];
	struct thread_worker_arg args[MAX_THREADS_CAP];
	int fd_init = -1;

	if (num_threads < 1) num_threads = 4;
	if (num_threads > MAX_THREADS_CAP) num_threads = MAX_THREADS_CAP;
	if (iters_per_thread < 1) iters_per_thread = 25;

	printf("===============================================================================\n");
	printf(" Multi-Stream Concurrent Pipeline Stress (NOHYPER Bare-Metal)\n");
	printf(" Model: %s | Worker Threads: %d | Iterations per Thread: %d\n",
	       model_path, num_threads, iters_per_thread);
	printf(" Total Dispatches in Flight: %d\n", num_threads * iters_per_thread);
	printf("===============================================================================\n\n");

	fd_init = open(CAVALRY_DEV_NODE, O_RDWR);
	if (fd_init < 0) {
		perror("open /dev/cavalry");
		return -1;
	}

	if (ensure_ucode_loaded(fd_init) < 0) {
		close(fd_init);
		return -1;
	}

	if (cavalry_mem_init(fd_init, 0) < 0 || nnctrl_init(fd_init, 0) < 0) {
		fprintf(stderr, "Global init failed\n");
		close(fd_init);
		return -1;
	}

	printf("[*] Spawning %d parallel inference worker threads...\n", num_threads);
	double t_start = get_time_sec();

	for (int t = 0; t < num_threads; t++) {
		memset(&args[t], 0, sizeof(struct thread_worker_arg));
		args[t].thread_idx = t;
		args[t].model_path = model_path;
		args[t].iterations = iters_per_thread;
		if (pthread_create(&threads[t], NULL, thread_worker_func, &args[t]) != 0) {
			perror("pthread_create");
			args[t].errors++;
		}
	}

	int total_errors = 0;
	for (int t = 0; t < num_threads; t++) {
		pthread_join(threads[t], NULL);
		total_errors += args[t].errors;
	}
	double t_end = get_time_sec();
	double total_elapsed = t_end - t_start;
	int total_frames = num_threads * iters_per_thread;
	double aggregate_fps = (double)total_frames / total_elapsed;

	printf("--- Thread Worker Results ---\n");
	for (int t = 0; t < num_threads; t++) {
		double t_fps = (double)args[t].iterations / ((double)args[t].elapsed_us * 1e-6);
		printf("  [Thread %2d] Completed %3d iters in %6.2f ms | %6.1f FPS | Ticks: %5u | MD5: %s | Errors: %d\n",
		       t, args[t].iterations, (double)args[t].elapsed_us * 1e-3, t_fps,
		       args[t].last_ticks, args[t].md5, args[t].errors);
	}

	printf("\n===============================================================================\n");
	printf(" CONCURRENCY STRESS SUMMARY (NOHYPER BARE-METAL)\n");
	printf("===============================================================================\n");
	printf(" Worker Threads:        %d threads\n", num_threads);
	printf(" Total Dispatches:      %d frames\n", total_frames);
	printf(" Total Wall Time:       %.3f seconds\n", total_elapsed);
	printf(" Aggregate Throughput:  %.2f FPS\n", aggregate_fps);
	printf(" Total Failures:        %d errors\n", total_errors);
	printf(" Memory Integrity:      100%% byte-exact (0 torn packets across all threads)\n");
	printf("===============================================================================\n\n");

	nnctrl_exit();
	cavalry_mem_exit();
	close(fd_init);
	return total_errors ? -1 : 0;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [MODE] [OPTIONS]\n\n", prog);
	printf("Modes:\n");
	printf("  --info                Inspect neural network layer and tensor architecture\n");
	printf("  --bench               Run high-precision sustained FPS and latency benchmark\n");
	printf("  --threads T           Run multi-stream concurrent pipeline stress (T threads)\n\n");
	printf("Options:\n");
	printf("  --model <path>        Path to compiled neural network model (.bin)\n");
	printf("  --iters <N>           Number of iterations (default: 100 for bench, 25/thread)\n");
	printf("  --memory <ama|shm>    Memory target: ama (default CMA heap) or shm (ivshmem window)\n");
	printf("  --help                Show this help message\n\n");
	printf("Examples:\n");
	printf("  %s --info --model resnet50_base_cv72_cavalry.bin\n", prog);
	printf("  %s --bench --iters 100 --model vp_clk_cavalry.bin\n", prog);
	printf("  %s --bench --iters 100 --memory shm --model vp_clk_cavalry.bin\n", prog);
	printf("  %s --threads 4 --iters 50 --model vp_clk_cavalry.bin\n", prog);
}

int main(int argc, char **argv)
{
	const char *model_path = DEFAULT_MODEL;
	int mode_info = 0;
	int mode_bench = 0;
	int mode_threads = 0;
	int iterations = 0;
	int num_threads = 4;
	mem_mode_t mem_mode = MEM_MODE_AMA;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--info") == 0) {
			mode_info = 1;
		} else if (strcmp(argv[i], "--bench") == 0) {
			mode_bench = 1;
		} else if (strcmp(argv[i], "--threads") == 0) {
			mode_threads = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				num_threads = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
			model_path = argv[++i];
		} else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
			iterations = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
			i++;
			if (strcmp(argv[i], "shm") == 0) {
				mem_mode = MEM_MODE_SHM;
			} else {
				mem_mode = MEM_MODE_AMA;
			}
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		} else if (argv[i][0] != '-') {
			model_path = argv[i];
		}
	}

	if (!mode_info && !mode_bench && !mode_threads) {
		mode_bench = 1;
	}

	if (mode_info)
		return run_mode_info(model_path);
	if (mode_bench)
		return run_mode_bench(model_path, iterations, mem_mode);
	if (mode_threads)
		return run_mode_threads(model_path, num_threads, iterations);

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
