/*
 * cavalry_shm_dag_test.c
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
#include <sys/time.h>
#include <unistd.h>

#include <cavalry_ioctl.h>
#include <cavalry_mem.h>
#include <nnctrl.h>
#include <uapi/amba_virt.h>
#include "nnctrl_priv.h"
#include "utils.h"

#define WINDOW_OFFSET_BYTES	(32UL * 1024UL * 1024UL) /* 32 MiB into ivshmem prefix */

/* Simple MD5 implementation for self-contained validation */
typedef struct {
	uint32_t state[4];
	uint32_t count[2];
	uint8_t buffer[64];
} md5_ctx_t;

static void md5_transform(uint32_t state[4], const uint8_t block[64]);

static void md5_init(md5_ctx_t *ctx)
{
	ctx->count[0] = ctx->count[1] = 0;
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
}

static void md5_update(md5_ctx_t *ctx, const uint8_t *input, size_t len)
{
	size_t i, index, part_len;

	index = (size_t)((ctx->count[0] >> 3) & 0x3f);
	if ((ctx->count[0] += ((uint32_t)len << 3)) < ((uint32_t)len << 3))
		ctx->count[1]++;
	ctx->count[1] += (uint32_t)(len >> 29);

	part_len = 64 - index;
	if (len >= part_len) {
		memcpy(&ctx->buffer[index], input, part_len);
		md5_transform(ctx->state, ctx->buffer);
		for (i = part_len; i + 63 < len; i += 64)
			md5_transform(ctx->state, &input[i]);
		index = 0;
	} else {
		i = 0;
	}
	memcpy(&ctx->buffer[index], &input[i], len - i);
}

static void md5_final(uint8_t digest[16], md5_ctx_t *ctx)
{
	uint8_t bits[8];
	size_t index, pad_len;
	static const uint8_t padding[64] = { 0x80 };

	for (unsigned int i = 0; i < 8; i++)
		bits[i] = (uint8_t)((ctx->count[(i >= 4 ? 1 : 0)] >> ((i & 3) * 8)) & 0xff);

	index = (size_t)((ctx->count[0] >> 3) & 0x3f);
	pad_len = (index < 56) ? (56 - index) : (120 - index);
	md5_update(ctx, padding, pad_len);
	md5_update(ctx, bits, 8);

	for (unsigned int i = 0; i < 16; i++)
		digest[i] = (uint8_t)((ctx->state[i >> 2] >> ((i & 3) * 8)) & 0xff);
}

#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))
#define ROT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define FF(a, b, c, d, x, s, ac) { (a) += F((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROT((a), (s)); (a) += (b); }
#define GG(a, b, c, d, x, s, ac) { (a) += G((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROT((a), (s)); (a) += (b); }
#define HH(a, b, c, d, x, s, ac) { (a) += H((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROT((a), (s)); (a) += (b); }
#define II(a, b, c, d, x, s, ac) { (a) += I((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROT((a), (s)); (a) += (b); }

static void md5_transform(uint32_t state[4], const uint8_t block[64])
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

static void compute_md5_str(const void *data, size_t len, char *out_str)
{
	md5_ctx_t ctx;
	uint8_t digest[16];

	md5_init(&ctx);
	md5_update(&ctx, (const uint8_t *)data, len);
	md5_final(digest, &ctx);

	for (int i = 0; i < 16; i++) {
		sprintf(&out_str[i * 2], "%02x", digest[i]);
	}
	out_str[32] = '\0';
}

/* ========================================================================= */
/* ORDER 1: PA CAVALRY_RUN_DAGS VALIDATION                                  */
/* ========================================================================= */

static int run_baseline(int fd_cav, const char *model_path, char *out_md5)
{
	struct net_cfg net_cf = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct net_mem net_m = { 0 };
	int net_id = -1;
	int ret = -1;

	net_cf.net_file = (char *)model_path;
	net_cf.print_time = 1;

	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "Case 1: nnctrl_init_net failed\n");
		return -1;
	}

	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, &net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "Case 1: cavalry_mem_alloc failed\n");
		nnctrl_exit_net(net_id);
		return -1;
	}
	net_m.mem_size = net_cf.net_mem_total;

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "Case 1: nnctrl_load_net failed\n");
		goto out_free_mem;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "Case 1: nnctrl_get_net_io_cfg failed\n");
		goto out_free_mem;
	}

	memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
	memset(net_out.out_desc[0].virt, 0, net_out.out_desc[0].size);

	asm volatile("dsb sy" ::: "memory");

	if (nnctrl_run_net(net_id, NULL, NULL, NULL, NULL) < 0) {
		fprintf(stderr, "Case 1: nnctrl_run_net failed\n");
		goto out_free_mem;
	}

	asm volatile("dsb sy" ::: "memory");

	compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, out_md5);
	printf("Case 1 (Baseline Native AMA): PASS (rval=0, MD5=%s, in_phys=0x%lx, out_phys=0x%lx, net_phys=0x%lx)\n",
	       out_md5,
	       (unsigned long)net_in.in_desc[0].addr,
	       (unsigned long)net_out.out_desc[0].addr,
	       net_m.phy_addr);

	ret = 0;

out_free_mem:
	nnctrl_exit_net(net_id);
	cavalry_mem_free(net_cf.net_mem_total, net_m.phy_addr, net_m.virt_addr);
	return ret;
}

static int verify_and_dump_run_dags_phys(int net_id, unsigned long shm_phys,
					 unsigned long shm_size, const char *stage)
{
	struct nnctrl_info *pctl = get_nnctrl_global_context();
	struct net_desc *pnet = get_net_desc(pctl, net_id);
	uint32_t c, d, p;

	if (!pnet) {
		fprintf(stderr, "Case 2 %s: get_net_desc failed for net_id %d\n", stage, net_id);
		return -1;
	}

	for (c = 0; c < pnet->subgraph_exe_cnt; c++) {
		subgraph_exe_t *gexe = &pnet->execute_ctx[c];
		struct cavalry_run_dags *run = gexe->run_dags;

		if (!run)
			continue;

		if (run->ucode_cmd_phys < shm_phys ||
		    run->ucode_cmd_phys >= shm_phys + shm_size) {
			fprintf(stderr, "Case 2 %s: FATAL - ucode_cmd_phys 0x%lx outside ivshmem [0x%lx, 0x%lx)!\n",
				stage, (unsigned long)run->ucode_cmd_phys, shm_phys, shm_phys + shm_size);
			return -EFAULT;
		}

		for (d = 0; d < run->dag_cnt; d++) {
			struct cavalry_dag_desc *dag = &run->dag_desc[d];
			for (p = 0; p < dag->port_cnt; p++) {
				unsigned long port_phys = (unsigned long)dag->port_desc[p].port_dram_addr;
				if (port_phys < shm_phys || port_phys >= shm_phys + shm_size) {
					fprintf(stderr, "Case 2 %s: FATAL - Subgraph %u Dag %u Port %u phys 0x%lx outside ivshmem [0x%lx, 0x%lx)!\n",
						stage, c, d, p, port_phys, shm_phys, shm_phys + shm_size);
					return -EFAULT;
				}
				printf("  [Case 2 %s ioctl check] Subgraph %u Dag %u Port %u: phys=0x%lx (size=%lu B, offset=+0x%lx)\n",
				       stage, c, d, p, port_phys, (unsigned long)dag->port_desc[p].port_dram_size,
				       port_phys - shm_phys);
			}
		}
	}

	return 0;
}

static int run_ivshmem_prefix(int fd_cav, const char *model_path,
			      unsigned long shm_phys, unsigned long shm_size,
			      void *shm_vaddr, const char *expected_md5)
{
	struct net_cfg net_cf = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct net_mem net_m = { 0 };
	char actual_md5[33];
	int net_id = -1;
	int rval = -1;

	net_cf.net_file = (char *)model_path;
	net_cf.print_time = 1;

	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "Case 2: nnctrl_init_net failed\n");
		return -1;
	}

	if (WINDOW_OFFSET_BYTES + net_cf.net_mem_total > shm_size) {
		fprintf(stderr, "Case 2: model memory (%lu B) exceeds ivshmem window (%lu B)\n",
			WINDOW_OFFSET_BYTES + net_cf.net_mem_total, shm_size);
		goto out_exit_net;
	}

	net_m.phy_addr = shm_phys + WINDOW_OFFSET_BYTES;
	net_m.virt_addr = (uint8_t *)shm_vaddr + WINDOW_OFFSET_BYTES;
	net_m.mem_size = net_cf.net_mem_total;

	mprotect(shm_vaddr, shm_size, PROT_READ | PROT_WRITE);

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "Case 2: nnctrl_load_net into ivshmem prefix failed\n");
		goto out_exit_net;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "Case 2: nnctrl_get_net_io_cfg failed\n");
		goto out_exit_net;
	}

	if ((unsigned long)net_in.in_desc[0].addr < shm_phys ||
	    (unsigned long)net_in.in_desc[0].addr >= shm_phys + shm_size ||
	    (unsigned long)net_out.out_desc[0].addr < shm_phys ||
	    (unsigned long)net_out.out_desc[0].addr >= shm_phys + shm_size) {
		fprintf(stderr, "Case 2: FATAL - tensor physical addresses (in=0x%lx, out=0x%lx) outside ivshmem [0x%lx, 0x%lx)!\n",
			(unsigned long)net_in.in_desc[0].addr,
			(unsigned long)net_out.out_desc[0].addr,
			shm_phys, shm_phys + shm_size);
		rval = -EFAULT;
		goto out_exit_net;
	}

	if (verify_and_dump_run_dags_phys(net_id, shm_phys, shm_size, "Pre-Run") < 0) {
		rval = -EFAULT;
		goto out_exit_net;
	}

	memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
	memset(net_out.out_desc[0].virt, 0, net_out.out_desc[0].size);

	asm volatile("dsb sy" ::: "memory");

	if (nnctrl_run_net(net_id, NULL, NULL, NULL, NULL) < 0) {
		fprintf(stderr, "Case 2: nnctrl_run_net on ivshmem prefix failed\n");
		goto out_exit_net;
	}

	asm volatile("dsb sy" ::: "memory");

	if (verify_and_dump_run_dags_phys(net_id, shm_phys, shm_size, "Post-Run") < 0) {
		rval = -EFAULT;
		goto out_exit_net;
	}

	compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, actual_md5);
	if (strcmp(actual_md5, expected_md5) != 0) {
		fprintf(stderr, "Case 2: MD5 mismatch! Got %s, expected %s\n", actual_md5, expected_md5);
		rval = -EIO;
		goto out_exit_net;
	}

	printf("Case 2 (ivshmem Prefix): PASS (rval=0, MD5=%s matches Case 1, in_phys=0x%lx, out_phys=0x%lx, net_phys=0x%lx)\n",
	       actual_md5,
	       (unsigned long)net_in.in_desc[0].addr,
	       (unsigned long)net_out.out_desc[0].addr,
	       net_m.phy_addr);
	rval = 0;

out_exit_net:
	nnctrl_exit_net(net_id);
	return rval;
}

static int test_driver_rejections(int fd_cav, unsigned long priv_start)
{
	struct cavalry_run_dags *run_dags = NULL;
	unsigned long ucmd_size = 4096;
	unsigned long ucmd_phys = 0;
	void *ucmd_virt = NULL;
	size_t run_size;
	int ret;

	if (cavalry_mem_alloc(&ucmd_size, &ucmd_phys, &ucmd_virt, 0) < 0) {
		fprintf(stderr, "Case 3: cavalry_mem_alloc for ucmd failed\n");
		return -1;
	}

	run_size = RUN_DAG_SIZE(1);
	run_dags = (struct cavalry_run_dags *)calloc(1, run_size);
	if (!run_dags) {
		cavalry_mem_free(ucmd_size, ucmd_phys, ucmd_virt);
		return -ENOMEM;
	}

	run_dags->dag_cnt = 1;
	run_dags->ucode_cmd_phys = ucmd_phys;
	run_dags->dag_desc[0].dag_type = CAVALRY_DAG_TYPE_NORMAL;
	run_dags->dag_desc[0].dag_loop_cnt = 1;
	run_dags->dag_desc[0].port_cnt = 1;
	run_dags->dag_desc[0].dvi_dram_addr = ucmd_phys;
	run_dags->dag_desc[0].dvi_img_vaddr = 0x1000;
	run_dags->dag_desc[0].dvi_img_size = 0x1000;
	run_dags->dag_desc[0].dvi_dag_vaddr = 0x2000;

	/* 3.1: Null Address rejection */
	run_dags->dag_desc[0].port_desc[0].port_dram_addr = 0;
	run_dags->dag_desc[0].port_desc[0].port_dram_size = 4096;
	ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, run_dags);
	if (ret >= 0) {
		fprintf(stderr, "Case 3.1: FAILED - driver accepted null address!\n");
		free(run_dags);
		cavalry_mem_free(ucmd_size, ucmd_phys, ucmd_virt);
		return -EINVAL;
	}
	if (run_dags->rval == MSG_RVAL_VP_HANG) {
		fprintf(stderr, "Case 3.1: FAILED - VP hang reported\n");
		free(run_dags);
		cavalry_mem_free(ucmd_size, ucmd_phys, ucmd_virt);
		return -EIO;
	}
	printf("Case 3.1 (Null Address): PASS (driver rejected with ret=%d, errno=%d)\n", ret, errno);

	/* 3.2: Private CMA overlap rejection */
	run_dags->dag_desc[0].port_desc[0].port_dram_addr = priv_start;
	run_dags->dag_desc[0].port_desc[0].port_dram_size = 4096;
	ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, run_dags);
	if (ret >= 0) {
		fprintf(stderr, "Case 3.2: FAILED - driver accepted cma_private overlap!\n");
		free(run_dags);
		cavalry_mem_free(ucmd_size, ucmd_phys, ucmd_virt);
		return -EINVAL;
	}
	if (run_dags->rval == MSG_RVAL_VP_HANG) {
		fprintf(stderr, "Case 3.2: FAILED - VP hang reported\n");
		free(run_dags);
		cavalry_mem_free(ucmd_size, ucmd_phys, ucmd_virt);
		return -EIO;
	}
	printf("Case 3.2 (Private CMA):  PASS (driver rejected with ret=%d, errno=%d)\n", ret, errno);

	free(run_dags);
	cavalry_mem_free(ucmd_size, ucmd_phys, ucmd_virt);
	return 0;
}

/* ========================================================================= */
/* ORDER 2: CAVALRY_RUN_DAGS_MEMFD (dma-buf) VALIDATION                      */
/* ========================================================================= */

static struct cavalry_run_dags_mfd *convert_to_run_dags_mfd(
	const struct cavalry_run_dags *run,
	int target_fd,
	unsigned long base_phys)
{
	uint32_t d, p;
	size_t sz = sizeof(struct cavalry_run_dags_mfd) +
		run->dag_cnt * sizeof(struct cavalry_dag_desc_mfd);
	struct cavalry_run_dags_mfd *mfd = (struct cavalry_run_dags_mfd *)calloc(1, sz);
	if (!mfd)
		return NULL;

	mfd->dag_cnt = run->dag_cnt;
	mfd->nid = run->nid;
	mfd->affinity = run->affinity;
	mfd->hw_type = run->hw_type;
	mfd->priority = run->priority;
	mfd->is_encrypt = run->is_encrypt;
	mfd->is_resume = run->is_resume;
	mfd->session_id = run->session_id;
	mfd->sub_session_id = run->sub_session_id;
	mfd->no_auto_resume = run->no_auto_resume;

	mfd->dvi_dram_addr_fd = target_fd;
	mfd->ucode_cmd_addr_fd = -1; /* Dynamic host-private CMA allocation by kernel */
	mfd->ucode_cmd_addr_offset = 0;

	for (d = 0; d < run->dag_cnt; d++) {
		const struct cavalry_dag_desc *src = &run->dag_desc[d];
		struct cavalry_dag_desc_mfd *dst = &mfd->dag_desc[d];

		dst->dag_loop_cnt = src->dag_loop_cnt;
		dst->use_ping_pong_vmem = src->use_ping_pong_vmem;
		dst->run_with_checksum = src->run_with_checksum;
		dst->is_orc_pdxs_set = src->is_orc_pdxs_set;
		dst->orc_pdxs = src->orc_pdxs;
		dst->dag_type = src->dag_type;
		dst->dep_cnt = src->dep_cnt;
		dst->dvi_dag_size = src->dvi_dag_size;
		dst->dvi_dram_addr_offset = (target_fd == CAVALRY_DMABUF_FD_REPRESENT_PHYS) ?
			src->dvi_dram_addr : (src->dvi_dram_addr - base_phys);
		dst->dvi_img_vaddr = src->dvi_img_vaddr;
		dst->dvi_img_size = src->dvi_img_size;
		dst->dvi_dag_vaddr = src->dvi_dag_vaddr;
		dst->private_scratchpad_offset = src->private_scratchpad_offset;
		dst->reverse_dep_dag_cnt = src->reverse_dep_dag_cnt;
		dst->port_cnt = src->port_cnt;
		dst->poke_cnt = src->poke_cnt;
		dst->extra_poke_list_cnt = src->extra_poke_list_cnt;

		if (src->extra_poke_list_daddr) {
			dst->extra_poke_list_dram_offset = (target_fd == CAVALRY_DMABUF_FD_REPRESENT_PHYS) ?
				src->extra_poke_list_daddr : (src->extra_poke_list_daddr - base_phys);
		}
		if (src->extra_dag_desc_common_daddr) {
			dst->extra_dag_desc_common_offset = (target_fd == CAVALRY_DMABUF_FD_REPRESENT_PHYS) ?
				src->extra_dag_desc_common_daddr : (src->extra_dag_desc_common_daddr - base_phys);
		}
		if (src->extra_dag_desc_list_daddr) {
			dst->extra_dag_desc_list_offset = (target_fd == CAVALRY_DMABUF_FD_REPRESENT_PHYS) ?
				src->extra_dag_desc_list_daddr : (src->extra_dag_desc_list_daddr - base_phys);
		}

		memcpy(dst->reverse_dep_dag_id, src->reverse_dep_dag_id, sizeof(dst->reverse_dep_dag_id));
		memcpy(dst->port_desc, src->port_desc, sizeof(dst->port_desc));
		memcpy(dst->poke_desc, src->poke_desc, sizeof(dst->poke_desc));

		for (p = 0; p < src->port_cnt; p++) {
			dst->port_dram_addr_fd[p] = target_fd;
			dst->port_desc[p].port_dram_addr = (target_fd == CAVALRY_DMABUF_FD_REPRESENT_PHYS) ?
				src->port_desc[p].port_dram_addr : (src->port_desc[p].port_dram_addr - base_phys);
		}
	}

	return mfd;
}

static int run_case1_baseline_memfd(int fd_cav, const char *model_path, char *out_md5)
{
	struct net_cfg net_cf = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct net_mem net_m = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	int net_id = -1;
	int ret = -1;
	uint32_t c;

	net_cf.net_file = (char *)model_path;
	net_cf.print_time = 1;

	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "Case 1 MEMFD: nnctrl_init_net failed\n");
		return -1;
	}

	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, &net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "Case 1 MEMFD: cavalry_mem_alloc failed\n");
		nnctrl_exit_net(net_id);
		return -1;
	}
	net_m.mem_size = net_cf.net_mem_total;

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "Case 1 MEMFD: nnctrl_load_net failed\n");
		goto out_free_mem;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "Case 1 MEMFD: nnctrl_get_net_io_cfg failed\n");
		goto out_free_mem;
	}

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	if (!pnet) {
		fprintf(stderr, "Case 1 MEMFD: get_net_desc failed\n");
		goto out_free_mem;
	}

	memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
	memset(net_out.out_desc[0].virt, 0, net_out.out_desc[0].size);

	asm volatile("dsb sy" ::: "memory");

	/* Execute via CAVALRY_RUN_DAGS_MEMFD using CAVALRY_DMABUF_FD_REPRESENT_PHYS */
	for (c = 0; c < pnet->subgraph_exe_cnt; c++) {
		struct cavalry_run_dags *run = pnet->execute_ctx[c].run_dags;
		struct cavalry_run_dags_mfd *run_mfd;

		if (!run)
			continue;

		run_mfd = convert_to_run_dags_mfd(run, CAVALRY_DMABUF_FD_REPRESENT_PHYS, 0);
		if (!run_mfd) {
			fprintf(stderr, "Case 1 MEMFD: convert_to_run_dags_mfd failed\n");
			goto out_free_mem;
		}

		if (ioctl(fd_cav, CAVALRY_RUN_DAGS_MEMFD, run_mfd) < 0) {
			perror("Case 1 MEMFD: ioctl CAVALRY_RUN_DAGS_MEMFD");
			free(run_mfd);
			goto out_free_mem;
		}
		if (run_mfd->rval != MSG_RVAL_NONE) {
			fprintf(stderr, "Case 1 MEMFD: run_mfd->rval error 0x%x\n", run_mfd->rval);
			free(run_mfd);
			goto out_free_mem;
		}
		printf("  [Case 1 MEMFD Ticks] Subgraph %u: start=%u, end=%u, exec=%u, load=%u\n",
		       c, run_mfd->start_tick, run_mfd->end_tick, run_mfd->exec_total_ticks, run_mfd->load_total_ticks);
		free(run_mfd);
	}

	asm volatile("dsb sy" ::: "memory");

	compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, out_md5);
	printf("Case 1 (Baseline AMA MEMFD): PASS (rval=0, MD5=%s, size=%lu B, net_phys=0x%lx)\n",
	       out_md5, net_out.out_desc[0].size, net_m.phy_addr);

	ret = 0;

out_free_mem:
	nnctrl_exit_net(net_id);
	cavalry_mem_free(net_cf.net_mem_total, net_m.phy_addr, net_m.virt_addr);
	return ret;
}

static int run_case2_ivshmem_memfd(int fd_cav, int fd_shm, const char *model_path,
				   unsigned long shm_phys, unsigned long shm_size,
				   void *shm_vaddr, const char *expected_md5)
{
	struct net_cfg net_cf = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct net_mem net_m = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	char actual_md5[33];
	int window_fd = -1;
	int net_id = -1;
	int ret = -1;
	uint32_t c, d, p;

	if (ioctl(fd_shm, AMBA_VIRT_IOC_EXPORT_DMABUF, &window_fd) < 0) {
		perror("Case 2 MEMFD: ioctl AMBA_VIRT_IOC_EXPORT_DMABUF");
		return -1;
	}
	if (window_fd < 0) {
		fprintf(stderr, "Case 2 MEMFD: invalid window_fd %d\n", window_fd);
		return -1;
	}
	printf("Case 2 MEMFD: Exported ivshmem USER window as dma-buf fd: %d\n", window_fd);

	net_cf.net_file = (char *)model_path;
	net_cf.print_time = 1;

	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "Case 2 MEMFD: nnctrl_init_net failed\n");
		close(window_fd);
		return -1;
	}

	if (WINDOW_OFFSET_BYTES + net_cf.net_mem_total > shm_size) {
		fprintf(stderr, "Case 2 MEMFD: model size exceeds ivshmem window\n");
		goto out_exit;
	}

	net_m.phy_addr = shm_phys + WINDOW_OFFSET_BYTES;
	net_m.virt_addr = (uint8_t *)shm_vaddr + WINDOW_OFFSET_BYTES;
	net_m.mem_size = net_cf.net_mem_total;

	mprotect(shm_vaddr, shm_size, PROT_READ | PROT_WRITE);

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "Case 2 MEMFD: nnctrl_load_net failed\n");
		goto out_exit;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "Case 2 MEMFD: nnctrl_get_net_io_cfg failed\n");
		goto out_exit;
	}

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	if (!pnet) {
		fprintf(stderr, "Case 2 MEMFD: get_net_desc failed\n");
		goto out_exit;
	}

	/* Populate input tensor in ivshmem Normal-NC window */
	memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
	memset(net_out.out_desc[0].virt, 0, net_out.out_desc[0].size);

	asm volatile("dsb sy" ::: "memory");

	/* Execute via CAVALRY_RUN_DAGS_MEMFD using window_fd and window-relative offsets */
	for (c = 0; c < pnet->subgraph_exe_cnt; c++) {
		struct cavalry_run_dags *run = pnet->execute_ctx[c].run_dags;
		struct cavalry_run_dags_mfd *run_mfd;

		if (!run)
			continue;

		run_mfd = convert_to_run_dags_mfd(run, window_fd, shm_phys);
		if (!run_mfd) {
			fprintf(stderr, "Case 2 MEMFD: convert_to_run_dags_mfd failed\n");
			goto out_exit;
		}

		for (d = 0; d < run_mfd->dag_cnt; d++) {
			for (p = 0; p < run_mfd->dag_desc[d].port_cnt; p++) {
				printf("  [Case 2 MEMFD Pre-Run] Subgraph %u Dag %u Port %u: fd=%d, offset=0x%lx (size=%lu B)\n",
				       c, d, p, run_mfd->dag_desc[d].port_dram_addr_fd[p],
				       (unsigned long)run_mfd->dag_desc[d].port_desc[p].port_dram_addr,
				       (unsigned long)run_mfd->dag_desc[d].port_desc[p].port_dram_size);
			}
		}

		if (ioctl(fd_cav, CAVALRY_RUN_DAGS_MEMFD, run_mfd) < 0) {
			perror("Case 2 MEMFD: ioctl CAVALRY_RUN_DAGS_MEMFD");
			free(run_mfd);
			goto out_exit;
		}
		if (run_mfd->rval != MSG_RVAL_NONE) {
			fprintf(stderr, "Case 2 MEMFD: run_mfd->rval error 0x%x\n", run_mfd->rval);
			free(run_mfd);
			goto out_exit;
		}
		printf("  [Case 2 MEMFD Ticks] Subgraph %u: start=%u, end=%u, exec=%u, load=%u\n",
		       c, run_mfd->start_tick, run_mfd->end_tick, run_mfd->exec_total_ticks, run_mfd->load_total_ticks);
		free(run_mfd);
	}

	asm volatile("dsb sy" ::: "memory");

	compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, actual_md5);
	if (strcmp(actual_md5, expected_md5) != 0) {
		fprintf(stderr, "Case 2 MEMFD: MD5 mismatch! Got %s, expected %s\n", actual_md5, expected_md5);
		goto out_exit;
	}

	printf("Case 2 (ivshmem Prefix MEMFD): PASS (rval=0, MD5=%s matches Case 1, window_fd=%d, offset=+0x%lx)\n",
	       actual_md5, window_fd, WINDOW_OFFSET_BYTES);

	ret = 0;

out_exit:
	nnctrl_exit_net(net_id);
	if (window_fd >= 0)
		close(window_fd);
	return ret;
}

static int test_driver_rejections_memfd(int fd_cav, int fd_shm, unsigned long priv_start, unsigned long shm_phys)
{
	struct cavalry_run_dags_mfd *run_mfd = NULL;
	size_t run_size;
	int window_fd = -1;
	int ret;

	if (ioctl(fd_shm, AMBA_VIRT_IOC_EXPORT_DMABUF, &window_fd) < 0) {
		perror("Case 3 MEMFD: ioctl AMBA_VIRT_IOC_EXPORT_DMABUF");
		return -1;
	}

	run_size = RUN_DAG_MFD_SIZE(1);
	run_mfd = (struct cavalry_run_dags_mfd *)calloc(1, run_size);
	if (!run_mfd) {
		close(window_fd);
		return -ENOMEM;
	}

	run_mfd->dag_cnt = 1;
	run_mfd->ucode_cmd_addr_fd = -1;
	run_mfd->dvi_dram_addr_fd = window_fd;
	run_mfd->dag_desc[0].dag_type = CAVALRY_DAG_TYPE_NORMAL;
	run_mfd->dag_desc[0].dag_loop_cnt = 1;
	run_mfd->dag_desc[0].port_cnt = 1;
	run_mfd->dag_desc[0].dvi_dram_addr_offset = 0x02000000;
	run_mfd->dag_desc[0].dvi_img_vaddr = 0x1000;
	run_mfd->dag_desc[0].dvi_img_size = 0x1000;
	run_mfd->dag_desc[0].dvi_dag_vaddr = 0x2000;

	/* 3.1: Invalid dma-buf fd rejection */
	run_mfd->dag_desc[0].port_dram_addr_fd[0] = 9999;
	run_mfd->dag_desc[0].port_desc[0].port_dram_addr = 0;
	run_mfd->dag_desc[0].port_desc[0].port_dram_size = 4096;
	ret = ioctl(fd_cav, CAVALRY_RUN_DAGS_MEMFD, run_mfd);
	if (ret >= 0) {
		fprintf(stderr, "Case 3.1 MEMFD: FAILED - driver accepted invalid fd!\n");
		free(run_mfd);
		close(window_fd);
		return -EINVAL;
	}
	if (run_mfd->rval == MSG_RVAL_VP_HANG) {
		fprintf(stderr, "Case 3.1 MEMFD: FAILED - VP hang reported\n");
		free(run_mfd);
		close(window_fd);
		return -EIO;
	}
	printf("Case 3.1 (Invalid FD MEMFD): PASS (driver rejected with ret=%d, errno=%d)\n", ret, errno);

	/* 3.2: CMA Private overlap rejection via offset */
	run_mfd->dag_desc[0].port_dram_addr_fd[0] = window_fd;
	run_mfd->dag_desc[0].port_desc[0].port_dram_addr = (cv_doffset_t)(priv_start - shm_phys);
	run_mfd->dag_desc[0].port_desc[0].port_dram_size = 4096;
	ret = ioctl(fd_cav, CAVALRY_RUN_DAGS_MEMFD, run_mfd);
	if (ret >= 0) {
		fprintf(stderr, "Case 3.2 MEMFD: FAILED - driver accepted cma_private overlap!\n");
		free(run_mfd);
		close(window_fd);
		return -EINVAL;
	}
	if (run_mfd->rval == MSG_RVAL_VP_HANG) {
		fprintf(stderr, "Case 3.2 MEMFD: FAILED - VP hang reported\n");
		free(run_mfd);
		close(window_fd);
		return -EIO;
	}
	printf("Case 3.2 (Private CMA MEMFD): PASS (driver rejected with ret=%d, errno=%d)\n", ret, errno);

	free(run_mfd);
	close(window_fd);
	return 0;
}

static int ensure_ucode_loaded(int fd_cav, unsigned long priv_phys, unsigned long priv_size)
{
	struct version_info_s *ver;
	void *ucode_map;
	int fd_bin;
	ssize_t nread;

	ucode_map = mmap(NULL, priv_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_cav, priv_phys);
	if (ucode_map == MAP_FAILED) {
		perror("mmap CAVALRY_MEM_UCODE");
		return -1;
	}

	ver = (struct version_info_s *)ucode_map;
	if (ver->chip != 10) {
		printf("Ucode mismatch in DRAM (chip: %u, expected: 10 for N1_655). Reloading from /persist/firmware/cavalry.bin...\n",
		       ver->chip);
		fd_bin = open("cavalry.bin", O_RDONLY);
		if (fd_bin < 0)
			fd_bin = open("/persist/firmware/cavalry.bin", O_RDONLY);
		if (fd_bin < 0)
			fd_bin = open("/lib/firmware/cavalry.bin", O_RDONLY);
		if (fd_bin < 0) {
			perror("open cavalry.bin");
			munmap(ucode_map, priv_size);
			return -1;
		}
		nread = read(fd_bin, ucode_map, priv_size);
		close(fd_bin);
		if (nread <= 0) {
			fprintf(stderr, "Failed to read cavalry.bin into DRAM\n");
			munmap(ucode_map, priv_size);
			return -1;
		}
		asm volatile("dsb sy" ::: "memory");
		printf("Loaded %zd bytes of N1_655 ucode into DRAM. New chip ID: %u\n", nread, ver->chip);
	}

	munmap(ucode_map, priv_size);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *model_path = NULL;
	struct cavalry_querybuf q_user = { .buf = CAVALRY_MEM_USER };
	struct cavalry_querybuf q_priv = { .buf = CAVALRY_MEM_UCODE };
	struct amba_virt_info vinfo = { 0 };
	char baseline_md5[33] = { 0 };
	char recovery_md5[33] = { 0 };
	unsigned long shm_phys, shm_size, priv_start, ucode_size;
	void *shm_vaddr = NULL;
	int fd_cav = -1, fd_shm = -1;
	int run_order1 = 0;
	int run_order2 = 1; /* Default to Order 2 */
	int ret = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
			i++;
			if (strcmp(argv[i], "pa") == 0) {
				run_order1 = 1;
				run_order2 = 0;
			} else if (strcmp(argv[i], "memfd") == 0) {
				run_order1 = 0;
				run_order2 = 1;
			} else if (strcmp(argv[i], "all") == 0) {
				run_order1 = 1;
				run_order2 = 1;
			}
		} else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
			model_path = argv[++i];
		} else if (argv[i][0] != '-') {
			model_path = argv[i];
		}
	}

	if (!model_path) {
		fprintf(stderr, "Usage: %s [--mode pa|memfd|all] [--model] <path/to/vp_clk_cavalry.bin>\n", argv[0]);
		return 1;
	}

	fd_cav = open(CAVALRY_DEV_NODE, O_RDWR);
	if (fd_cav < 0) {
		perror("open /dev/cavalry");
		return 1;
	}

	fd_shm = open("/dev/amba_virt", O_RDWR);
	if (fd_shm < 0) {
		perror("open /dev/amba_virt");
		close(fd_cav);
		return 1;
	}

	/* Dynamic Discovery: CAVALRY_MEM_UCODE partition (at base of cma_private) */
	if (ioctl(fd_cav, CAVALRY_QUERY_BUF, &q_priv) < 0) {
		perror("ioctl CAVALRY_QUERY_BUF(UCODE)");
		ret = 1;
		goto out_close;
	}
	priv_start = q_priv.offset;
	ucode_size = q_priv.length;

	/* Verify and ensure matching N1_655 ucode in DRAM */
	if (ensure_ucode_loaded(fd_cav, priv_start, ucode_size) < 0) {
		ret = 1;
		goto out_close;
	}

	if (ioctl(fd_cav, CAVALRY_START_VP, 0) < 0) {
		perror("ioctl CAVALRY_START_VP");
		ret = 1;
		goto out_close;
	}

	/* Dynamic Discovery: USER partition base */
	if (ioctl(fd_cav, CAVALRY_QUERY_BUF, &q_user) < 0) {
		perror("ioctl CAVALRY_QUERY_BUF(USER)");
		ret = 1;
		goto out_close;
	}
	shm_phys = q_user.offset;

	/* Dynamic Discovery: ivshmem window extent */
	if (ioctl(fd_shm, AMBA_VIRT_IOC_GET_INFO, &vinfo) < 0) {
		perror("ioctl AMBA_VIRT_IOC_GET_INFO");
		ret = 1;
		goto out_close;
	}
	shm_size = vinfo.shm_size;

	shm_vaddr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
	if (shm_vaddr == MAP_FAILED) {
		perror("mmap /dev/amba_virt");
		ret = 1;
		goto out_close;
	}

	printf("============================================================\n");
	printf("Cavalry ivshmem USER Window Validation\n");
	printf("Target Network: %s\n", model_path);
	printf("Execution Mode: %s\n", (run_order1 && run_order2) ? "ALL (Order 1 + Order 2)" :
				       run_order2 ? "Order 2 (CAVALRY_RUN_DAGS_MEMFD)" :
				       "Order 1 (PA CAVALRY_RUN_DAGS)");
	printf("Discovered Topology:\n");
	printf("  ucode (base): 0x%010lx - 0x%010lx (1 MiB CAVALRY_MEM_UCODE at cma_private base)\n", priv_start, priv_start + ucode_size);
	printf("  USER prefix:  0x%010lx - 0x%010lx (%lu MB window)\n", shm_phys, shm_phys + shm_size, shm_size >> 20);
	printf("  Test offset:  +0x%lx (32 MiB into prefix)\n", WINDOW_OFFSET_BYTES);
	printf("============================================================\n\n");

	if (cavalry_mem_init(fd_cav, 0) < 0) {
		fprintf(stderr, "cavalry_mem_init failed\n");
		ret = 1;
		goto out_unmap;
	}
	if (nnctrl_init(fd_cav, 0) < 0) {
		fprintf(stderr, "nnctrl_init failed\n");
		ret = 1;
		goto out_unmap;
	}

	/* ----------------------------------------------------------------- */
	/* ORDER 1 EXECUTION                                                 */
	/* ----------------------------------------------------------------- */
	if (run_order1) {
		printf(">>> RUNNING ORDER 1 (PA CAVALRY_RUN_DAGS) <<<\n");
		ret = run_baseline(fd_cav, model_path, baseline_md5);
		if (ret)
			goto out_unmap;

		ret = run_ivshmem_prefix(fd_cav, model_path, shm_phys, shm_size, shm_vaddr, baseline_md5);
		if (ret)
			goto out_unmap;

		ret = test_driver_rejections(fd_cav, priv_start);
		if (ret)
			goto out_unmap;

		ret = run_baseline(fd_cav, model_path, recovery_md5);
		if (ret || strcmp(baseline_md5, recovery_md5) != 0) {
			fprintf(stderr, "Order 1 Recovery run failed!\n");
			ret = 1;
			goto out_unmap;
		}
		printf("Order 1 Recovery Check: PASS (engine healthy, MD5 matches)\n\n");
	}

	/* ----------------------------------------------------------------- */
	/* ORDER 2 EXECUTION                                                 */
	/* ----------------------------------------------------------------- */
	if (run_order2) {
		printf(">>> RUNNING ORDER 2 (CAVALRY_RUN_DAGS_MEMFD) <<<\n");

		/* Case 1: Baseline Native AMA MEMFD */
		ret = run_case1_baseline_memfd(fd_cav, model_path, baseline_md5);
		if (ret)
			goto out_unmap;

		/* Case 2: ivshmem Shared Window MEMFD */
		ret = run_case2_ivshmem_memfd(fd_cav, fd_shm, model_path, shm_phys, shm_size, shm_vaddr, baseline_md5);
		if (ret)
			goto out_unmap;

		/* Case 3: Driver Rejections under MEMFD */
		ret = test_driver_rejections_memfd(fd_cav, fd_shm, priv_start, shm_phys);
		if (ret)
			goto out_unmap;

		/* Recovery: Post-Rejection Baseline MEMFD */
		ret = run_case1_baseline_memfd(fd_cav, model_path, recovery_md5);
		if (ret || strcmp(baseline_md5, recovery_md5) != 0) {
			fprintf(stderr, "Order 2 Recovery run failed!\n");
			ret = 1;
			goto out_unmap;
		}
		printf("Order 2 Recovery Check: PASS (engine healthy, MD5 matches)\n\n");
	}

	printf("============================================================\n");
	printf("VALIDATION RESULT: ALL REQUESTED GATES PASSED (100%% byte match)\n");
	printf("============================================================\n");

out_unmap:
	if (shm_vaddr && shm_vaddr != MAP_FAILED)
		munmap(shm_vaddr, shm_size);
out_close:
	if (fd_shm >= 0)
		close(fd_shm);
	if (fd_cav >= 0)
		close(fd_cav);

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
