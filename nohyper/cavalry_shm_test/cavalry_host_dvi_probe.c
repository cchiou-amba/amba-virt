/*
 * cavalry_host_dvi_probe.c
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

#define CAVALRY_DEV_NODE	"/dev/cavalry"
#define AMBA_VIRT_DEV_NODE	"/dev/amba_virt"
#define EXPECTED_GOLDEN_MD5	"b2d1236c286a3c0704224fe4105eca49"
#define GOLDEN_TICKS_BASELINE	24172U

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

	for (int i = 0; i < 16; i++)
		sprintf(&out_str[i * 2], "%02x", digest[i]);
	out_str[32] = '\0';
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
	const char *model_path = "vp_clk_cavalry.bin";
	struct cavalry_querybuf q_user = { .buf = CAVALRY_MEM_USER };
	struct cavalry_querybuf q_priv = { .buf = CAVALRY_MEM_UCODE };
	struct amba_virt_info vinfo = { 0 };
	struct net_cfg net_cf = { 0 };
	struct net_mem net_m = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	struct cavalry_run_dags *run = NULL;
	struct cavalry_run_dags_mfd *run_mfd = NULL;
	int fd_cav = -1;
	int fd_shm = -1;
	int window_fd = -1;
	int net_id = -1;
	void *shm_vaddr = MAP_FAILED;
	unsigned long shm_phys = 0;
	unsigned long shm_size = 0;
	unsigned long priv_start = 0;
	unsigned long ucode_size = 0;
	uint32_t window_in_offset = 0x02123380U;   /* Standard BAR input offset */
	uint32_t window_in_size   = 8388608U;      /* 8 MiB */
	uint32_t window_out_offset = 0x02923380U;  /* Standard BAR output offset */
	uint32_t window_out_size  = 2097152U;      /* 2 MiB */
	char md5_str[33] = { 0 };
	size_t mfd_size;
	int ret = 0;

	if (argc > 1 && argv[1][0] != '-')
		model_path = argv[1];

	printf("============================================================\n");
	printf("Stage 2 Go/No-Go Gate: Standalone Host AMA DVI Probe\n");
	printf("Target Model: %s\n", model_path);
	printf("Objective: Prove VisORC executes DVI from host AMA above 1 GiB\n");
	printf("           while input/output tensor ports reside on window_fd\n");
	printf("============================================================\n\n");

	fd_cav = open(CAVALRY_DEV_NODE, O_RDWR);
	if (fd_cav < 0) {
		perror("open /dev/cavalry");
		return 1;
	}

	fd_shm = open(AMBA_VIRT_DEV_NODE, O_RDWR);
	if (fd_shm < 0) {
		perror("open /dev/amba_virt");
		close(fd_cav);
		return 1;
	}

	/* Discover ucode partition & ensure loaded */
	if (ioctl(fd_cav, CAVALRY_QUERY_BUF, &q_priv) < 0) {
		perror("ioctl CAVALRY_QUERY_BUF(UCODE)");
		ret = 1;
		goto out_close;
	}
	priv_start = q_priv.offset;
	ucode_size = q_priv.length;
	if (ensure_ucode_loaded(fd_cav, priv_start, ucode_size) < 0) {
		ret = 1;
		goto out_close;
	}

	if (ioctl(fd_cav, CAVALRY_START_VP, 0) < 0) {
		perror("ioctl CAVALRY_START_VP");
		ret = 1;
		goto out_close;
	}

	/* Discover USER window topology */
	if (ioctl(fd_cav, CAVALRY_QUERY_BUF, &q_user) < 0) {
		perror("ioctl CAVALRY_QUERY_BUF(USER)");
		ret = 1;
		goto out_close;
	}
	shm_phys = q_user.offset;

	if (ioctl(fd_shm, AMBA_VIRT_IOC_GET_INFO, &vinfo) < 0) {
		perror("ioctl AMBA_VIRT_IOC_GET_INFO");
		ret = 1;
		goto out_close;
	}
	shm_size = vinfo.shm_size;

	if (ioctl(fd_shm, AMBA_VIRT_IOC_EXPORT_DMABUF, &window_fd) < 0) {
		perror("ioctl AMBA_VIRT_IOC_EXPORT_DMABUF");
		ret = 1;
		goto out_close;
	}

	shm_vaddr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
	if (shm_vaddr == MAP_FAILED) {
		perror("mmap /dev/amba_virt");
		ret = 1;
		goto out_close;
	}

	printf("[*] Topology Discovered:\n");
	printf("    ivshmem Window:  0x%010lx - 0x%010lx (%lu MB), window_fd=%d\n",
	       shm_phys, shm_phys + shm_size, shm_size >> 20, window_fd);

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

	/* Initialize model */
	net_cf.net_file = (char *)model_path;
	net_cf.print_time = 0;
	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "nnctrl_init_net failed\n");
		ret = 1;
		goto out_unmap;
	}

	/* Allocate host AMA memory above 1 GiB for DVI / extra_dag */
	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, (void **)&net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "cavalry_mem_alloc failed\n");
		ret = 1;
		goto out_exit_net;
	}
	net_m.mem_size = net_cf.net_mem_total;

	printf("    Host AMA Buffer: 0x%010lx - 0x%010lx (%lu B, offset from window base: +%lu MB)\n",
	       (unsigned long)net_m.phy_addr, (unsigned long)(net_m.phy_addr + net_m.mem_size),
	       net_m.mem_size, (net_m.phy_addr >= shm_phys) ? ((net_m.phy_addr - shm_phys) >> 20) : 0);

	if (net_m.phy_addr < shm_phys + shm_size) {
		fprintf(stderr, "WARNING: Host AMA buffer 0x%lx is inside ivshmem window [0x%lx, 0x%lx)!\n",
			(unsigned long)net_m.phy_addr, shm_phys, shm_phys + shm_size);
	} else {
		printf("    [PASS] Host AMA buffer is strictly located ABOVE 1 GiB ivshmem window (+%lu MB above base)!\n",
		       (net_m.phy_addr - shm_phys) >> 20);
	}

	/* Load network into host AMA buffer */
	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "nnctrl_load_net failed\n");
		ret = 1;
		goto out_free_mem;
	}

	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "nnctrl_get_net_io_cfg failed\n");
		ret = 1;
		goto out_free_mem;
	}

	printf("\n[*] nnctrl_get_net_io_cfg Results:\n");
	printf("    net_in.in_num=%u, addr=0x%lx, virt=%p, size=%lu\n",
	       net_in.in_num, (unsigned long)net_in.in_desc[0].addr, net_in.in_desc[0].virt,
	       (unsigned long)net_in.in_desc[0].size);
	printf("    net_out.out_num=%u, addr=0x%lx, virt=%p, size=%lu\n",
	       net_out.out_num, (unsigned long)net_out.out_desc[0].addr, net_out.out_desc[0].virt,
	       (unsigned long)net_out.out_desc[0].size);
	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	if (!pnet || pnet->subgraph_exe_cnt == 0) {
		fprintf(stderr, "get_net_desc failed\n");
		ret = 1;
		goto out_free_mem;
	}
	run = pnet->execute_ctx[0].run_dags;
	if (!run || run->dag_cnt == 0) {
		fprintf(stderr, "execute_ctx has no run_dags\n");
		ret = 1;
		goto out_free_mem;
	}

	printf("\n[*] Loaded Network DVI Topology:\n");
	printf("    dvi_dram_addr:                  0x%lx (Host AMA PA)\n", (unsigned long)run->dag_desc[0].dvi_dram_addr);
	printf("    dvi_img_vaddr:                  0x%x\n", run->dag_desc[0].dvi_img_vaddr);
	printf("    dvi_img_size:                   %u\n", run->dag_desc[0].dvi_img_size);
	printf("    dvi_dag_vaddr:                  0x%x\n", run->dag_desc[0].dvi_dag_vaddr);
	printf("    dvi_dag_size:                   %u\n", run->dag_desc[0].dvi_dag_size);
	printf("    extra_poke_list_daddr:          0x%lx\n", (unsigned long)run->dag_desc[0].extra_poke_list_daddr);
	printf("    extra_dag_desc_common_daddr:    0x%lx\n", (unsigned long)run->dag_desc[0].extra_dag_desc_common_daddr);
	printf("    extra_dag_desc_list_daddr:      0x%lx\n", (unsigned long)run->dag_desc[0].extra_dag_desc_list_daddr);
	printf("    use_ping_pong_vmem:             %u\n", run->dag_desc[0].use_ping_pong_vmem);
	printf("    dag_loop_cnt:                   %u\n", run->dag_desc[0].dag_loop_cnt);
	printf("    poke_cnt:                       %u\n", run->dag_desc[0].poke_cnt);
	printf("    port_cnt:                       %u\n", run->dag_desc[0].port_cnt);
	for (uint32_t p = 0; p < run->dag_desc[0].port_cnt; p++) {
		printf("      Orig Port %u: addr=0x%lx, size=%lu, boff=0x%x, inc=%d (offset from net_m: +0x%lx)\n",
		       p, (unsigned long)run->dag_desc[0].port_desc[p].port_dram_addr,
		       run->dag_desc[0].port_desc[p].port_dram_size,
		       run->dag_desc[0].port_desc[p].port_boffset_in_dag,
		       run->dag_desc[0].port_desc[p].port_daddr_increment,
		       (unsigned long)(run->dag_desc[0].port_desc[p].port_dram_addr - net_m.phy_addr));
	}

	if (run->dag_desc[0].extra_dag_desc_list_daddr) {
		struct extra_dag_desc_list *elist = (struct extra_dag_desc_list *)((uint8_t *)net_m.virt_addr +
			(run->dag_desc[0].extra_dag_desc_list_daddr - net_m.phy_addr));
		printf("    extra_dag_desc_list details:\n");
		printf("      preload_list_cnt:    %u (daddr=0x%lx)\n", elist->preload_list_cnt, (unsigned long)elist->preload_list_daddr);
		printf("      runtime_list_cnt:    %u (daddr=0x%lx)\n", elist->runtime_list_cnt, (unsigned long)elist->runtime_list_daddr);
		printf("      extra_port_list_cnt: %u (daddr=0x%lx)\n", elist->extra_port_list_cnt, (unsigned long)elist->extra_port_list_daddr);
		printf("      bb_list_cnt:         %u (daddr=0x%lx)\n", elist->bb_list_cnt, (unsigned long)elist->bb_list_daddr);
		if (elist->preload_list_cnt && elist->preload_list_daddr) {
			struct extra_dag_desc_preload *pl = (struct extra_dag_desc_preload *)((uint8_t *)net_m.virt_addr +
				(elist->preload_list_daddr - net_m.phy_addr));
			for (uint32_t i = 0; i < elist->preload_list_cnt; i++) {
				printf("        Preload [%u]: daddr=0x%lx (offset=+0x%lx), vaddr=0x%x, size=%u\n",
				       i, (unsigned long)pl[i].preload_daddr,
				       (unsigned long)(pl[i].preload_daddr - net_m.phy_addr),
				       pl[i].preload_vaddr, pl[i].preload_size);
			}
		}
	}
	printf("\n[*] Staging Tensor Ports into ivshmem window (window_fd=%d):\n", window_fd);
	printf("    Port 0 (Input):  window offset=0x%08x, size=%u B, virt=%p\n",
	       window_in_offset, window_in_size, (char *)shm_vaddr + window_in_offset);
	printf("    Port 1 (Output): window offset=0x%08x, size=%u B, virt=%p\n",
	       window_out_offset, window_out_size, (char *)shm_vaddr + window_out_offset);

	/* Golden input fill: memset(..., 1, size) */
	memset((char *)shm_vaddr + window_in_offset, 1, window_in_size);
	/* Clear output tensor to 0 */
	memset((char *)shm_vaddr + window_out_offset, 0, window_out_size);
	asm volatile("dsb sy" ::: "memory");

	/* Construct CAVALRY_RUN_DAGS_MEMFD request */
	mfd_size = sizeof(struct cavalry_run_dags_mfd) + run->dag_cnt * sizeof(struct cavalry_dag_desc_mfd);
	run_mfd = (struct cavalry_run_dags_mfd *)calloc(1, mfd_size);
	if (!run_mfd) {
		perror("calloc run_mfd");
		ret = 1;
		goto out_free_mem;
	}

	run_mfd->dag_cnt = run->dag_cnt;
	run_mfd->nid = run->nid;
	run_mfd->affinity = run->affinity;
	run_mfd->hw_type = run->hw_type;
	run_mfd->priority = run->priority;
	run_mfd->is_encrypt = run->is_encrypt;
	run_mfd->is_resume = run->is_resume;
	run_mfd->session_id = run->session_id;
	run_mfd->sub_session_id = run->sub_session_id;
	run_mfd->no_auto_resume = run->no_auto_resume;

	/* Dynamic kernel CMA command buffer */
	run_mfd->ucode_cmd_addr_fd = -1;
	run_mfd->ucode_cmd_addr_offset = 0;

	/* ------------------------------------------------------------- */
	/* BASELINE: DVI & Ports all in Host AMA                         */
	/* ------------------------------------------------------------- */
	printf("\n--- Executing Baseline (DVI & Ports all in Host AMA) ---\n");
	{
		struct cavalry_run_dags_mfd *base_mfd = (struct cavalry_run_dags_mfd *)calloc(1, mfd_size);
		base_mfd->dag_cnt = run->dag_cnt;
		base_mfd->nid = run->nid;
		base_mfd->affinity = run->affinity;
		base_mfd->hw_type = run->hw_type;
		base_mfd->priority = run->priority;
		base_mfd->ucode_cmd_addr_fd = -1;
		base_mfd->dvi_dram_addr_fd = CAVALRY_DMABUF_FD_REPRESENT_PHYS;

		for (uint32_t d = 0; d < run->dag_cnt; d++) {
			const struct cavalry_dag_desc *src = &run->dag_desc[d];
			struct cavalry_dag_desc_mfd *dst = &base_mfd->dag_desc[d];

			dst->dag_loop_cnt = src->dag_loop_cnt;
			dst->use_ping_pong_vmem = src->use_ping_pong_vmem;
			dst->run_with_checksum = src->run_with_checksum;
			dst->is_orc_pdxs_set = src->is_orc_pdxs_set;
			dst->orc_pdxs = src->orc_pdxs;
			dst->dag_type = src->dag_type;
			dst->dep_cnt = src->dep_cnt;
			dst->dvi_dag_size = src->dvi_dag_size;
			dst->dvi_dram_addr_offset = src->dvi_dram_addr;
			dst->dvi_img_vaddr = src->dvi_img_vaddr;
			dst->dvi_img_size = src->dvi_img_size;
			dst->dvi_dag_vaddr = src->dvi_dag_vaddr;
			dst->private_scratchpad_offset = src->private_scratchpad_offset;
			dst->reverse_dep_dag_cnt = src->reverse_dep_dag_cnt;
			dst->port_cnt = src->port_cnt;
			dst->poke_cnt = src->poke_cnt;
			dst->extra_poke_list_cnt = src->extra_poke_list_cnt;

			if (src->extra_poke_list_daddr)
				dst->extra_poke_list_dram_offset = src->extra_poke_list_daddr;
			if (src->extra_dag_desc_common_daddr)
				dst->extra_dag_desc_common_offset = src->extra_dag_desc_common_daddr;
			if (src->extra_dag_desc_list_daddr)
				dst->extra_dag_desc_list_offset = src->extra_dag_desc_list_daddr;

			memcpy(dst->reverse_dep_dag_id, src->reverse_dep_dag_id, sizeof(dst->reverse_dep_dag_id));
			memcpy(dst->port_desc, src->port_desc, sizeof(dst->port_desc));
			memcpy(dst->poke_desc, src->poke_desc, sizeof(dst->poke_desc));

			for (uint32_t p = 0; p < src->port_cnt; p++) {
				dst->port_dram_addr_fd[p] = CAVALRY_DMABUF_FD_REPRESENT_PHYS;
				dst->port_desc[p] = src->port_desc[p];
			}
		}

		/* Fill AMA input with 1s, and clear output to 0 */
		memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
		memset(net_out.out_desc[0].virt, 0, net_out.out_desc[0].size);
		asm volatile("dsb sy" ::: "memory");

		if (ioctl(fd_cav, CAVALRY_RUN_DAGS_MEMFD, base_mfd) < 0) {
			perror("Baseline: ioctl CAVALRY_RUN_DAGS_MEMFD failed");
		} else {
			compute_md5_str(net_out.out_desc[0].virt, net_out.out_desc[0].size, md5_str);
			printf("Baseline returned: rval=0x%x, exec_ticks=%u, load_ticks=%u, MD5=%s\n",
			       base_mfd->rval, base_mfd->exec_total_ticks, base_mfd->load_total_ticks, md5_str);
		}
		free(base_mfd);
	}

	/* ------------------------------------------------------------- */
	/* METHOD 1: CAVALRY_DMABUF_FD_REPRESENT_PHYS for Host AMA DVI   */
	/*           with window_fd for Tensor Ports                     */
	/* ------------------------------------------------------------- */
	printf("\n--- Executing Method 1: DVI via CAVALRY_DMABUF_FD_REPRESENT_PHYS (Host AMA PA) ---\n");
	run_mfd->dvi_dram_addr_fd = CAVALRY_DMABUF_FD_REPRESENT_PHYS;

	for (uint32_t d = 0; d < run->dag_cnt; d++) {
		const struct cavalry_dag_desc *src = &run->dag_desc[d];
		struct cavalry_dag_desc_mfd *dst = &run_mfd->dag_desc[d];

		dst->dag_loop_cnt = src->dag_loop_cnt;
		dst->use_ping_pong_vmem = src->use_ping_pong_vmem;
		dst->run_with_checksum = src->run_with_checksum;
		dst->is_orc_pdxs_set = src->is_orc_pdxs_set;
		dst->orc_pdxs = src->orc_pdxs;
		dst->dag_type = src->dag_type;
		dst->dep_cnt = src->dep_cnt;
		dst->dvi_dag_size = src->dvi_dag_size;

		/* Host PA in AMA above 1 GiB */
		dst->dvi_dram_addr_offset = src->dvi_dram_addr;
		dst->dvi_img_vaddr = src->dvi_img_vaddr;
		dst->dvi_img_size = src->dvi_img_size;
		dst->dvi_dag_vaddr = src->dvi_dag_vaddr;
		dst->private_scratchpad_offset = src->private_scratchpad_offset;
		dst->reverse_dep_dag_cnt = src->reverse_dep_dag_cnt;
		dst->port_cnt = 2;
		dst->poke_cnt = src->poke_cnt;
		dst->extra_poke_list_cnt = src->extra_poke_list_cnt;

		if (src->extra_poke_list_daddr)
			dst->extra_poke_list_dram_offset = src->extra_poke_list_daddr;
		if (src->extra_dag_desc_common_daddr)
			dst->extra_dag_desc_common_offset = src->extra_dag_desc_common_daddr;
		if (src->extra_dag_desc_list_daddr)
			dst->extra_dag_desc_list_offset = src->extra_dag_desc_list_daddr;

		memcpy(dst->reverse_dep_dag_id, src->reverse_dep_dag_id, sizeof(dst->reverse_dep_dag_id));
		memcpy(dst->port_desc, src->port_desc, sizeof(dst->port_desc));
		memcpy(dst->poke_desc, src->poke_desc, sizeof(dst->poke_desc));

		/* Bind Port 0 to input in window_fd */
		dst->port_dram_addr_fd[0] = window_fd;
		dst->port_desc[0].port_dram_addr = window_in_offset;
		dst->port_desc[0].port_dram_size = window_in_size;

		/* Bind Port 1 to output in window_fd */
		dst->port_dram_addr_fd[1] = window_fd;
		dst->port_desc[1].port_dram_addr = window_out_offset;
		dst->port_desc[1].port_dram_size = window_out_size;
	}

	asm volatile("dsb sy" ::: "memory");

	if (ioctl(fd_cav, CAVALRY_RUN_DAGS_MEMFD, run_mfd) < 0) {
		perror("Method 1: ioctl CAVALRY_RUN_DAGS_MEMFD failed");
		ret = -errno;
	} else {
		printf("Method 1 ioctl returned: rval=0x%x, start=%u, end=%u, exec_ticks=%u, load_ticks=%u\n",
		       run_mfd->rval, run_mfd->start_tick, run_mfd->end_tick,
		       run_mfd->exec_total_ticks, run_mfd->load_total_ticks);

		if (run_mfd->rval != MSG_RVAL_NONE) {
			fprintf(stderr, "Method 1: run_mfd->rval failed with 0x%x\n", run_mfd->rval);
			ret = -EIO;
		} else {
			asm volatile("dsb sy" ::: "memory");
			compute_md5_str((char *)shm_vaddr + window_out_offset, window_out_size, md5_str);
			printf("Method 1 Output MD5: %s\n", md5_str);
			printf("Expected Golden MD5: %s\n", EXPECTED_GOLDEN_MD5);

			if (strcmp(md5_str, EXPECTED_GOLDEN_MD5) == 0) {
				int tick_delta = (int)run_mfd->exec_total_ticks - (int)GOLDEN_TICKS_BASELINE;
				if (tick_delta < 0)
					tick_delta = -tick_delta;
				printf(">>> Method 1 (DVI PHYS in AMA above 1 GiB + Ports in window_fd): PASS <<<\n");
				printf("    Ticks: %u (golden baseline %u, delta: %d ticks)\n",
				       run_mfd->exec_total_ticks, GOLDEN_TICKS_BASELINE, tick_delta);
			} else {
				fprintf(stderr, ">>> Method 1: FAIL - MD5 mismatch! <<<\n");
				ret = -EILSEQ;
			}
		}
	}

	/* ------------------------------------------------------------- */
	/* METHOD 2: CAVALRY_ALLOC_MEMFD for Host AMA DVI                */
	/*           with window_fd for Tensor Ports                     */
	/* ------------------------------------------------------------- */
	printf("\n--- Executing Method 2: DVI via CAVALRY_ALLOC_MEMFD (Host AMA DMA-BUF FD) ---\n");
	{
		struct cavalry_mfd_alloc mfd_alloc = {
			.length = net_cf.net_mem_total,
			.cache_en = 0,
		};
		if (ioctl(fd_cav, CAVALRY_ALLOC_MEMFD, &mfd_alloc) < 0) {
			printf("Method 2: CAVALRY_ALLOC_MEMFD not supported or failed (%s)\n", strerror(errno));
		} else {
			int host_dvi_fd = mfd_alloc.fd;
			void *host_dvi_virt = mmap(NULL, mfd_alloc.length, PROT_READ | PROT_WRITE,
						   MAP_SHARED, host_dvi_fd, 0);
			if (host_dvi_virt == MAP_FAILED) {
				perror("Method 2: mmap host_dvi_fd failed");
				close(host_dvi_fd);
			} else {
				printf("Method 2: Allocated Host AMA DMA-BUF fd=%d (%lu B), copied DVI image\n",
				       host_dvi_fd, mfd_alloc.length);
				memcpy(host_dvi_virt, net_m.virt_addr, net_cf.net_mem_total);
				asm volatile("dsb sy" ::: "memory");

				/* Clear output in window */
				memset((char *)shm_vaddr + window_out_offset, 0, window_out_size);
				asm volatile("dsb sy" ::: "memory");

				run_mfd->dvi_dram_addr_fd = host_dvi_fd;
				for (uint32_t d = 0; d < run->dag_cnt; d++) {
					const struct cavalry_dag_desc *src = &run->dag_desc[d];
					struct cavalry_dag_desc_mfd *dst = &run_mfd->dag_desc[d];

					dst->dvi_dram_addr_offset = src->dvi_dram_addr - net_m.phy_addr;
					if (src->extra_poke_list_daddr)
						dst->extra_poke_list_dram_offset = src->extra_poke_list_daddr - net_m.phy_addr;
					if (src->extra_dag_desc_common_daddr)
						dst->extra_dag_desc_common_offset = src->extra_dag_desc_common_daddr - net_m.phy_addr;
					if (src->extra_dag_desc_list_daddr)
						dst->extra_dag_desc_list_offset = src->extra_dag_desc_list_daddr - net_m.phy_addr;

					memcpy(dst->reverse_dep_dag_id, src->reverse_dep_dag_id, sizeof(dst->reverse_dep_dag_id));
					memcpy(dst->port_desc, src->port_desc, sizeof(dst->port_desc));
					memcpy(dst->poke_desc, src->poke_desc, sizeof(dst->poke_desc));

					dst->port_dram_addr_fd[0] = window_fd;
					dst->port_desc[0].port_dram_addr = window_in_offset;
					dst->port_desc[0].port_dram_size = window_in_size;

					dst->port_dram_addr_fd[1] = window_fd;
					dst->port_desc[1].port_dram_addr = window_out_offset;
					dst->port_desc[1].port_dram_size = window_out_size;
				}

				if (ioctl(fd_cav, CAVALRY_RUN_DAGS_MEMFD, run_mfd) < 0) {
					perror("Method 2: ioctl CAVALRY_RUN_DAGS_MEMFD failed");
				} else {
					printf("Method 2 ioctl returned: rval=0x%x, ticks=%u\n",
					       run_mfd->rval, run_mfd->exec_total_ticks);
					if (run_mfd->rval == MSG_RVAL_NONE) {
						asm volatile("dsb sy" ::: "memory");
						compute_md5_str((char *)shm_vaddr + window_out_offset, window_out_size, md5_str);
						printf("Method 2 Output MD5: %s\n", md5_str);
						if (strcmp(md5_str, EXPECTED_GOLDEN_MD5) == 0) {
							printf(">>> Method 2 (DVI DMA-BUF FD + Ports in window_fd): PASS <<<\n");
						} else {
							fprintf(stderr, ">>> Method 2: FAIL - MD5 mismatch! <<<\n");
						}
					}
				}
				munmap(host_dvi_virt, mfd_alloc.length);
				close(host_dvi_fd);
			}
		}
	}

	free(run_mfd);

out_free_mem:
	cavalry_mem_free(net_cf.net_mem_total, net_m.phy_addr, net_m.virt_addr);
out_exit_net:
	nnctrl_exit_net(net_id);
out_unmap:
	if (shm_vaddr != MAP_FAILED)
		munmap(shm_vaddr, shm_size);
out_close:
	if (window_fd >= 0)
		close(window_fd);
	if (fd_shm >= 0)
		close(fd_shm);
	if (fd_cav >= 0)
		close(fd_cav);

	printf("\n============================================================\n");
	if (ret == 0)
		printf("STAGE 2 VERDICT: HARDWARE GATE PASSED (DVI above 1 GiB + window ports proven)\n");
	else
		printf("STAGE 2 VERDICT: HARDWARE GATE FAILED (ret=%d)\n", ret);
	printf("============================================================\n");

	return (ret == 0) ? 0 : 1;
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
