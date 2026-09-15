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

static void compute_md5_str(const uint8_t *data, size_t len, char *out_str)
{
	md5_ctx_t ctx;
	uint8_t digest[16];

	md5_init(&ctx);
	md5_update(&ctx, data, len);
	md5_final(digest, &ctx);
	for (int i = 0; i < 16; i++)
		sprintf(&out_str[i * 2], "%02x", digest[i]);
	out_str[32] = '\0';
}

static int run_baseline(int fd_cav, const char *model_path, char *out_md5)
{
	struct net_cfg net_cf = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct net_mem net_m = { 0 };
	unsigned long net_phys = 0, net_size = 0;
	int net_id = -1;
	int rval = -1;

	net_cf.net_file = (char *)model_path;
	net_cf.print_time = 1;

	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "Case 1: nnctrl_init_net failed\n");
		return -1;
	}

	net_size = net_cf.net_mem_total;
	if (cavalry_mem_alloc(&net_size, &net_phys, (void *)&net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "Case 1: cavalry_mem_alloc failed (size=%lu)\n", net_size);
		goto out_exit_net;
	}
	net_m.phy_addr = net_phys;
	net_m.mem_size = net_size;

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

	if (nnctrl_run_net(net_id, NULL, NULL, NULL, NULL) < 0) {
		fprintf(stderr, "Case 1: nnctrl_run_net failed\n");
		goto out_free_mem;
	}

	compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, out_md5);
	printf("Case 1 (Baseline AMA):  PASS (rval=0, MD5=%s, size=%lu B, phys=0x%lx)\n",
	       out_md5, net_out.out_desc[0].size, net_phys);
	rval = 0;

out_free_mem:
	cavalry_mem_free(net_m.mem_size, net_m.phy_addr, net_m.virt_addr);
out_exit_net:
	nnctrl_exit_net(net_id);
	return rval;
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

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "Case 2: nnctrl_load_net into ivshmem prefix failed\n");
		goto out_exit_net;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "Case 2: nnctrl_get_net_io_cfg failed\n");
		goto out_exit_net;
	}

	/* Populate input tensor in ivshmem Normal-NC window */
	memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
	memset(net_out.out_desc[0].virt, 0, net_out.out_desc[0].size);

	/* Data Synchronization Barrier for Normal-NC writes */
	asm volatile("dsb sy" ::: "memory");

	if (nnctrl_run_net(net_id, NULL, NULL, NULL, NULL) < 0) {
		fprintf(stderr, "Case 2: nnctrl_run_net on ivshmem prefix failed\n");
		goto out_exit_net;
	}

	asm volatile("dsb sy" ::: "memory");

	compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, actual_md5);
	if (strcmp(actual_md5, expected_md5) != 0) {
		fprintf(stderr, "Case 2: MD5 mismatch! Got %s, expected %s\n", actual_md5, expected_md5);
		rval = -EIO;
		goto out_exit_net;
	}

	printf("Case 2 (ivshmem Prefix): PASS (rval=0, MD5=%s matches Case 1, phys=0x%lx)\n",
	       actual_md5, net_m.phy_addr);
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

	ver = (struct version_info_s *)((char *)ucode_map + VERSION_INFO_OFFSET);
	printf("Current Ucode in DRAM: chip=%u, ver=%u, date=0x%08x\n",
	       ver->chip, ver->ucode_version, ver->build_date);

	if (ver->chip != CHIP_TYPE_N1_655) {
		printf("Reloading N1_655 ucode firmware into DRAM...\n");
		fd_bin = open("cavalry.bin", O_RDONLY);
		if (fd_bin < 0)
			fd_bin = open("/home/ubuntu/cavalry.bin", O_RDONLY);
		if (fd_bin < 0) {
			perror("open cavalry.bin");
			munmap(ucode_map, priv_size);
			return -1;
		}
		nread = read(fd_bin, ucode_map, priv_size);
		close(fd_bin);
		if (nread <= 0) {
			fprintf(stderr, "Failed to read cavalry.bin\n");
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
	char baseline_md5[33];
	char recovery_md5[33];
	unsigned long shm_phys, shm_size, priv_start, priv_end;
	void *shm_vaddr = NULL;
	int fd_cav = -1, fd_shm = -1;
	int ret = 0;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <path/to/vp_clk_cavalry.bin>\n", argv[0]);
		return 1;
	}
	model_path = argv[1];

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

	/* Dynamic Discovery: cma_private region */
	if (ioctl(fd_cav, CAVALRY_QUERY_BUF, &q_priv) < 0) {
		perror("ioctl CAVALRY_QUERY_BUF(UCODE)");
		ret = 1;
		goto out_close;
	}
	priv_start = q_priv.offset;
	priv_end = q_priv.offset + q_priv.length;

	/* Verify and ensure matching N1_655 ucode in DRAM */
	if (ensure_ucode_loaded(fd_cav, priv_start, q_priv.length) < 0) {
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
	printf("Cavalry ivshmem USER Window Validation (Order 1: PA RUN_DAGS)\n");
	printf("Target Network: %s\n", model_path);
	printf("Discovered Topology:\n");
	printf("  cma_private:  0x%010lx - 0x%010lx (%lu MB)\n", priv_start, priv_end, (priv_end - priv_start) >> 20);
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

	/* Execute Case 1: Baseline Native AMA */
	ret = run_baseline(fd_cav, model_path, baseline_md5);
	if (ret)
		goto out_unmap;

	/* Execute Case 2: ivshmem Normal-NC Window */
	ret = run_ivshmem_prefix(fd_cav, model_path, shm_phys, shm_size, shm_vaddr, baseline_md5);
	if (ret)
		goto out_unmap;

	/* Execute Case 3: Driver Rejections */
	ret = test_driver_rejections(fd_cav, priv_start);
	if (ret)
		goto out_unmap;

	/* Recovery: Post-Rejection Baseline Run */
	ret = run_baseline(fd_cav, model_path, recovery_md5);
	if (ret) {
		fprintf(stderr, "Recovery run failed!\n");
		goto out_unmap;
	}
	if (strcmp(baseline_md5, recovery_md5) != 0) {
		fprintf(stderr, "Recovery run MD5 mismatch!\n");
		ret = 1;
		goto out_unmap;
	}
	printf("Recovery Check:         PASS (engine healthy, MD5 matches)\n");
	printf("\n============================================================\n");
	printf("Order 1 Validation Result: ALL CASES PASSED (100%% byte identity)\n");
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
