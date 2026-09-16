/*
 * cavalry_hvm_dags_denial.c
 *
 * Phase 4 Negative RUN_DAGS Descriptor Denials on Live Silicon
 * Exercises out-of-bounds, GDMA incursion, arena incursion, slice overflow,
 * and Use-After-Free descriptor rejections via direct CAVALRY_RUN_DAGS ioctl.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

int main(int argc, char *argv[])
{
	const char *model_path;
	int fd_cav = -1;
	int net_id = -1;
	struct net_cfg net_cf = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct net_mem net_m = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	struct cavalry_run_dags *orig_run = NULL;
	struct cavalry_run_dags *test_run = NULL;
	size_t req_size = 0;
	uint32_t slice_start = 0;
	size_t slice_size = 0;
	uint64_t slice_end = 0;
	int failures = 0;
	int ret;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path_to_vp_clk_cavalry.bin>\n", argv[0]);
		return 1;
	}
	model_path = argv[1];

	printf("===============================================================\n");
	printf(" Ambarella Class D Cavalry Phase 4 RUN_DAGS Denials Validation\n");
	printf(" Model: %s\n", model_path);
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
	slice_start = (uint32_t)net_m.phy_addr;
	slice_size = net_m.mem_size;
	slice_end = (uint64_t)slice_start + slice_size;

	printf("[*] Discovered Slice Geometry: start=0x%08x, size=%lu B, end=0x%08lx\n",
	       slice_start, slice_size, slice_end);

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

	test_run = malloc(req_size);
	if (!test_run) {
		fprintf(stderr, "Failed to allocate test_run buffer\n");
		close(fd_cav);
		return 1;
	}

	printf("\n>>> Running Negative RUN_DAGS Descriptor Rejections <<<\n");

	/* Case 4.1: Unallocated Offset Inside Pool while slice is live */
	{
		memcpy(test_run, orig_run, req_size);
		test_run->dag_desc[0].port_desc[0].port_dram_addr = 0x03500000UL;
		ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, test_run);
		if (ret < 0 && errno == EINVAL) {
			printf("[PASS] Case 4.1 (Unallocated offset 0x03500000): rejected with errno=22 (EINVAL)\n");
		} else {
			fprintf(stderr, "[FAIL] Case 4.1: ret=%d, errno=%d (expected -1, EINVAL [22])\n", ret, errno);
			failures++;
		}
	}

	/* Case 4.2a: Port in GDMA Staging Zone (< 32 MiB) */
	{
		memcpy(test_run, orig_run, req_size);
		test_run->dag_desc[0].port_desc[0].port_dram_addr = 0x00100000UL;
		ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, test_run);
		if (ret < 0 && errno == EFAULT) {
			printf("[PASS] Case 4.2a (Port in GDMA zone 0x00100000): rejected with errno=14 (EFAULT)\n");
		} else {
			fprintf(stderr, "[FAIL] Case 4.2a: ret=%d, errno=%d (expected -1, EFAULT [14])\n", ret, errno);
			failures++;
		}
	}

	/* Case 4.2b: Port in RPC Control Arena (< 32 MiB) */
	{
		memcpy(test_run, orig_run, req_size);
		test_run->dag_desc[0].port_desc[0].port_dram_addr = 0x01F00000UL;
		ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, test_run);
		if (ret < 0 && errno == EFAULT) {
			printf("[PASS] Case 4.2b (Port in RPC Arena 0x01F00000): rejected with errno=14 (EFAULT)\n");
		} else {
			fprintf(stderr, "[FAIL] Case 4.2b: ret=%d, errno=%d (expected -1, EFAULT [14])\n", ret, errno);
			failures++;
		}
	}

	/* Case 4.2c: DVI Address in GDMA Zone (< 32 MiB) */
	{
		memcpy(test_run, orig_run, req_size);
		test_run->dag_desc[0].dvi_dram_addr = 0x00100000UL;
		ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, test_run);
		if (ret < 0 && errno == EFAULT) {
			printf("[PASS] Case 4.2c (DVI in GDMA zone 0x00100000): rejected with errno=14 (EFAULT)\n");
		} else {
			fprintf(stderr, "[FAIL] Case 4.2c: ret=%d, errno=%d (expected -1, EFAULT [14])\n", ret, errno);
			failures++;
		}
	}

	/* Case 4.3: Port >= 1 GiB Boundary */
	{
		memcpy(test_run, orig_run, req_size);
		test_run->dag_desc[0].port_desc[0].port_dram_addr = 0x40000000UL;
		ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, test_run);
		if (ret < 0 && errno == EFAULT) {
			printf("[PASS] Case 4.3 (Port >= 1 GiB 0x40000000): rejected with errno=14 (EFAULT)\n");
		} else {
			fprintf(stderr, "[FAIL] Case 4.3: ret=%d, errno=%d (expected -1, EFAULT [14])\n", ret, errno);
			failures++;
		}
	}

	/* Case 4.4: Slice Overflow (port starts inside slice but extends beyond slice_end) */
	{
		memcpy(test_run, orig_run, req_size);
		/* Keep original 8 MiB port_dram_size, but set addr near end of slice */
		test_run->dag_desc[0].port_desc[0].port_dram_addr = (uint32_t)(slice_end - 1024);
		ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, test_run);
		if (ret < 0 && errno == EINVAL) {
			printf("[PASS] Case 4.4 (Slice bounds overflow): rejected with errno=22 (EINVAL)\n");
		} else {
			fprintf(stderr, "[FAIL] Case 4.4: ret=%d, errno=%d (expected -1, EINVAL [22])\n", ret, errno);
			failures++;
		}
	}

	/* Case 4.5: True Use-After-Free: Free slice, then dispatch original ports */
	{
		struct cavalry_mem f = { .offset = net_m.phy_addr };
		ret = ioctl(fd_cav, CAVALRY_FREE_MEM, &f);
		if (ret < 0) {
			perror("CAVALRY_FREE_MEM");
			failures++;
		} else {
			memcpy(test_run, orig_run, req_size);
			ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, test_run);
			if (ret < 0 && errno == EINVAL) {
				printf("[PASS] Case 4.5 (True UAF on freed slice 0x%lx): rejected with errno=22 (EINVAL)\n",
				       net_m.phy_addr);
			} else {
				fprintf(stderr, "[FAIL] Case 4.5: ret=%d, errno=%d (expected -1, EINVAL [22])\n", ret, errno);
				failures++;
			}
		}
	}

	/* Case 4.6: Recovery: Re-allocate, reload network, and execute clean golden inference */
	{
		char rec_md5[33] = { 0 };
		struct net_mem rec_mem = { 0 };

		printf("\n>>> Running Case 4.6 Engine Recovery (Reload & Execution) <<<\n");

		if (cavalry_mem_alloc(&net_cf.net_mem_total, &rec_mem.phy_addr, (void **)&rec_mem.virt_addr, 0) < 0) {
			fprintf(stderr, "[FAIL] Case 4.6: cavalry_mem_alloc failed during recovery\n");
			failures++;
		} else {
			rec_mem.mem_size = net_cf.net_mem_total;
			if (nnctrl_load_net(net_id, &rec_mem, NULL, NULL) < 0) {
				fprintf(stderr, "[FAIL] Case 4.6: nnctrl_load_net failed during recovery\n");
				failures++;
			} else {
				nnctrl_get_net_io_cfg(net_id, &net_in, &net_out);
				memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
				memset(net_out.out_desc[0].virt, 0, net_out.out_desc[0].size);
				asm volatile("dsb sy" ::: "memory");

				orig_run = pnet->execute_ctx[0].run_dags;
				ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, orig_run);
				if (ret < 0) {
					fprintf(stderr, "[FAIL] Case 4.6: CAVALRY_RUN_DAGS failed: ret=%d, errno=%d\n", ret, errno);
					failures++;
				} else {
					compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, rec_md5);
					printf("[PASS] Case 4.6: Recovery execution rval=%u, ticks=%u\n",
					       orig_run->rval, orig_run->exec_total_ticks);
					printf("[PASS] Case 4.6: Recovery output MD5=%s (golden: %s)\n",
					       rec_md5, GOLDEN_MD5);

					if (orig_run->rval != 0 || strcmp(rec_md5, GOLDEN_MD5) != 0) {
						fprintf(stderr, "[FAIL] Case 4.6: Recovery MD5 or rval mismatch!\n");
						failures++;
					} else if (orig_run->exec_total_ticks < 24171 || orig_run->exec_total_ticks > 24173) {
						fprintf(stderr, "[FAIL] Case 4.6: Ticks %u outside 24172±1 range\n",
							orig_run->exec_total_ticks);
						failures++;
					}
				}
				/* Clean up recovery slice */
				cavalry_mem_free(rec_mem.mem_size, rec_mem.phy_addr, rec_mem.virt_addr);
			}
		}
	}

	free(test_run);
	close(fd_cav);

	printf("\n===============================================================\n");
	printf(" Phase 4 RUN_DAGS Denial Suite Result: %s (%d failures)\n",
	       failures == 0 ? "ALL PASSED" : "FAILED", failures);
	printf("===============================================================\n");
	return failures;
}
