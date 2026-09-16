/*
 * cavalry_multi_tenant_test.c
 *
 * Multi-tenant concurrent inference test and security verification for amba-virt
 * Runs multiple iterations of Path B inference, computes FPS/tick metrics, verifies
 * bit-exact golden MD5 digests, and performs cross-tenant handle isolation checks.
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
#include "cavalry_ioctl_path_b.h"
#include <nnctrl.h>
#include "nnctrl_priv.h"
#include "utils.h"

#define CAVALRY_DEV_NODE	"/dev/cavalry"
#define EXPECTED_GOLDEN_MD5	"b2d1236c286a3c0704224fe4105eca49"
#define EXPECTED_EXEC_TICKS	24172U

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
		x[i] = ((uint32_t)block[i * 4]) |
		       (((uint32_t)block[i * 4 + 1]) << 8) |
		       (((uint32_t)block[i * 4 + 2]) << 16) |
		       (((uint32_t)block[i * 4 + 3]) << 24);

	FF(a, b, c, d, x[ 0],  7, 0xd76aa478); FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
	FF(c, d, a, b, x[ 2], 17, 0x242070db); FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
	FF(a, b, c, d, x[ 4],  7, 0xf57c0faf); FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
	FF(c, d, a, b, x[ 6], 17, 0xa8304613); FF(b, c, d, a, x[ 7], 22, 0xfd469501);
	FF(a, b, c, d, x[ 8],  7, 0x698098d8); FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
	FF(c, d, a, b, x[10], 17, 0xffff5bb1); FF(b, c, d, a, x[11], 22, 0x895cd7be);
	FF(a, b, c, d, x[12],  7, 0x6b901122); FF(d, a, b, c, x[13], 12, 0xfd987193);
	FF(c, d, a, b, x[14], 17, 0xa679438e); FF(b, c, d, a, x[15], 22, 0x49b40821);

	GG(a, b, c, d, x[ 1],  5, 0xf61e2562); GG(d, a, b, c, x[ 6],  9, 0xc040b340);
	GG(c, d, a, b, x[11], 14, 0x265e5a51); GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
	GG(a, b, c, d, x[ 5],  5, 0xd62f105d); GG(d, a, b, c, x[10],  9, 0x02441453);
	GG(c, d, a, b, x[15], 14, 0xd8a1e681); GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
	GG(a, b, c, d, x[ 9],  5, 0x21e1cde6); GG(d, a, b, c, x[14],  9, 0xc33707d6);
	GG(c, d, a, b, x[ 3], 14, 0xf4d50d87); GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
	GG(a, b, c, d, x[13],  5, 0xa9e3e905); GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);
	GG(c, d, a, b, x[ 7], 14, 0x676f02d9); GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

	HH(a, b, c, d, x[ 5],  4, 0xfffa3942); HH(d, a, b, c, x[ 8], 11, 0x8771f681);
	HH(c, d, a, b, x[11], 16, 0x6d9d6122); HH(b, c, d, a, x[14], 23, 0xfde5380c);
	HH(a, b, c, d, x[ 1],  4, 0xa4beea44); HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
	HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60); HH(b, c, d, a, x[10], 23, 0xbebfbc70);
	HH(a, b, c, d, x[13],  4, 0x289b7ec6); HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
	HH(c, d, a, b, x[ 3], 16, 0xd4ef3085); HH(b, c, d, a, x[ 6], 23, 0x04881d05);
	HH(a, b, c, d, x[ 9],  4, 0xd9d4d039); HH(d, a, b, c, x[12], 11, 0xe6db99e5);
	HH(c, d, a, b, x[15], 16, 0x1fa27cf8); HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

	II(a, b, c, d, x[ 0],  6, 0xf4292244); II(d, a, b, c, x[ 7], 10, 0x432aff97);
	II(c, d, a, b, x[14], 15, 0xab9423a7); II(b, c, d, a, x[ 5], 21, 0xfc93a039);
	II(a, b, c, d, x[12],  6, 0x655b59c3); II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
	II(c, d, a, b, x[10], 15, 0xffeff47d); II(b, c, d, a, x[ 1], 21, 0x85845dd1);
	II(a, b, c, d, x[ 8],  6, 0x6fa87e4f); II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
	II(c, d, a, b, x[ 6], 15, 0xa3014314); II(b, c, d, a, x[13], 21, 0x4e0811a1);
	II(a, b, c, d, x[ 4],  6, 0xf7537e82); II(d, a, b, c, x[11], 10, 0xbd3af235);
	II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb); II(b, c, d, a, x[ 9], 21, 0xeb86d391);

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
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

static double get_time_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

int main(int argc, char **argv)
{
	const char *model_path = "vp_clk_cavalry.bin";
	const char *role = "Client";
	int iterations = 20;
	int attack_dag_id = 0;
	int attack_handle_id = 0;

	struct net_cfg net_cf = { 0 };
	struct net_mem net_m = { 0 };
	struct net_input_cfg net_in = { 0 };
	struct net_output_cfg net_out = { 0 };
	struct cavalry_alloc_handle_user h_in = { 0 };
	struct cavalry_alloc_handle_user h_out = { 0 };
	struct cavalry_reg_dag_user reg_u = { 0 };
	struct cavalry_run_reg_user run_u = { 0 };
	struct nnctrl_info *pctl = NULL;
	struct net_desc *pnet = NULL;
	struct cavalry_run_dags *run = NULL;
	void *in_virt = MAP_FAILED;
	void *out_virt = MAP_FAILED;
	char md5_str[33] = { 0 };
	int net_id = -1;
	int fd = -1;
	int ret = 0;
	int i;

	int hold_seconds = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
			iterations = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
			role = argv[++i];
		} else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
			hold_seconds = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--attack-dag") == 0 && i + 1 < argc) {
			attack_dag_id = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--attack-handle") == 0 && i + 1 < argc) {
			attack_handle_id = atoi(argv[++i]);
		} else if (argv[i][0] != '-') {
			model_path = argv[i];
		}
	}

	printf("============================================================\n");
	printf("[%s] Multi-Tenant Path B Cavalry Virtualization Test\n", role);
	printf("Model: %s | Iterations: %d\n", model_path, iterations);
	printf("============================================================\n\n");

	fd = open(CAVALRY_DEV_NODE, O_RDWR);
	if (fd < 0) {
		perror("open /dev/cavalry");
		return 1;
	}

	if (cavalry_mem_init(fd, 0) < 0) {
		fprintf(stderr, "cavalry_mem_init failed\n");
		close(fd);
		return 1;
	}

	if (nnctrl_init(fd, 0) < 0) {
		fprintf(stderr, "nnctrl_init failed\n");
		close(fd);
		return 1;
	}

	/* Optional Security Attack Verification against Live Foreign Resources */
	if (attack_dag_id > 0) {
		struct cavalry_run_reg_user atk_u = { 0 };
		atk_u.dag_id = (uint32_t)attack_dag_id;
		atk_u.port_cnt = 2;
		atk_u.ports[0].port_idx = 0;
		atk_u.ports[0].handle_id = attack_handle_id > 0 ? (uint32_t)attack_handle_id : 999;
		atk_u.ports[0].offset = 0;
		atk_u.ports[0].size = 8388608;
		atk_u.ports[1].port_idx = 1;
		atk_u.ports[1].handle_id = attack_handle_id > 0 ? (uint32_t)(attack_handle_id + 1) : 1000;
		atk_u.ports[1].offset = 0;
		atk_u.ports[1].size = 2097152;

		printf("[SECURITY] Testing unauthorized execution of LIVE foreign DAG (dag_id=%u)...\n",
		       atk_u.dag_id);
		if (ioctl(fd, CAVALRY_IOC_RUN_REGISTERED_DAG, &atk_u) < 0) {
			printf("[PASS] Host strictly rejected foreign DAG execution: errno=%d (%s)\n",
			       errno, strerror(errno));
		} else {
			fprintf(stderr, "[FATAL FAIL] Cross-tenant DAG execution was NOT denied!\n");
			close(fd);
			return 2;
		}

		printf("[SECURITY] Testing unauthorized destruction of LIVE foreign DAG (dag_id=%u)...\n",
		       attack_dag_id);
		uint32_t bad_dag = (uint32_t)attack_dag_id;
		if (ioctl(fd, CAVALRY_IOC_UNREGISTER_DAG, &bad_dag) < 0) {
			printf("[PASS] Host strictly rejected foreign DAG unregister: errno=%d (%s)\n",
			       errno, strerror(errno));
		} else {
			fprintf(stderr, "[FATAL FAIL] Cross-tenant DAG unregister was NOT denied!\n");
			close(fd);
			return 2;
		}
	}

	if (attack_handle_id > 0) {
		printf("[SECURITY] Testing unauthorized destruction of LIVE foreign handle (hid=%u)...\n",
		       attack_handle_id);
		uint32_t bad_hid = (uint32_t)attack_handle_id;
		if (ioctl(fd, CAVALRY_IOC_FREE_HANDLE, &bad_hid) < 0) {
			printf("[PASS] Host strictly rejected foreign handle free: errno=%d (%s)\n",
			       errno, strerror(errno));
		} else {
			fprintf(stderr, "[FATAL FAIL] Cross-tenant handle free was NOT denied!\n");
			close(fd);
			return 2;
		}
	}

	/* 1. Initialize neural network */
	net_cf.net_file = (char *)model_path;
	net_cf.print_time = 0;
	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "nnctrl_init_net failed\n");
		close(fd);
		return 1;
	}

	/* 2. Allocate staging slice in BAR window */
	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, (void **)&net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "cavalry_mem_alloc for staging slice failed\n");
		ret = 1;
		goto out_exit_net;
	}
	net_m.mem_size = net_cf.net_mem_total;

	/* 3. Load network into staging slice */
	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "nnctrl_load_net failed\n");
		ret = 1;
		goto out_free_staging;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "nnctrl_get_net_io_cfg failed\n");
		ret = 1;
		goto out_free_staging;
	}

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	if (!pnet || pnet->subgraph_exe_cnt == 0 || !pnet->execute_ctx[0].run_dags) {
		fprintf(stderr, "Failed to query loaded execute_ctx\n");
		ret = 1;
		goto out_free_staging;
	}
	run = pnet->execute_ctx[0].run_dags;

	/* 4. Register DAG with Host Proxy via CAVALRY_IOC_REGISTER_DAG */
	reg_u.staging_bar_offset = (uint32_t)net_m.phy_addr;
	reg_u.staging_size = (uint32_t)net_m.mem_size;
	reg_u.dvi_offset_in_slice = (uint32_t)(run->dag_desc[0].dvi_dram_addr - net_m.phy_addr);
	if (run->dag_desc[0].extra_dag_desc_list_daddr)
		reg_u.extra_dag_list_offset_in_slice =
			(uint32_t)(run->dag_desc[0].extra_dag_desc_list_daddr - net_m.phy_addr);
	if (run->dag_desc[0].extra_dag_desc_common_daddr)
		reg_u.extra_dag_common_offset_in_slice =
			(uint32_t)(run->dag_desc[0].extra_dag_desc_common_daddr - net_m.phy_addr);
	if (run->dag_desc[0].extra_poke_list_daddr)
		reg_u.extra_poke_list_offset_in_slice =
			(uint32_t)(run->dag_desc[0].extra_poke_list_daddr - net_m.phy_addr);

	reg_u.run_dags_bytes = sizeof(struct cavalry_run_dags) +
		run->dag_cnt * sizeof(struct cavalry_dag_desc);
	reg_u.run_dags_ptr = (uint64_t)(uintptr_t)run;

	if (ioctl(fd, CAVALRY_IOC_REGISTER_DAG, &reg_u) < 0) {
		perror("ioctl CAVALRY_IOC_REGISTER_DAG failed");
		ret = 1;
		goto out_free_staging;
	}
	printf("[%s] Registered DAG: dag_id=%u (Host AMA isolation active)\n", role, reg_u.dag_id);

	/* 5. Free temporary staging slice completely from BAR */
	cavalry_mem_free(net_cf.net_mem_total, net_m.phy_addr, net_m.virt_addr);
	memset(&net_m, 0, sizeof(net_m));

	/* 6. Allocate input and output tensor handles via CAVALRY_IOC_ALLOC_HANDLE */
	h_in.size = net_in.in_desc[0].size;
	if (ioctl(fd, CAVALRY_IOC_ALLOC_HANDLE, &h_in) < 0) {
		perror("ALLOC_HANDLE in");
		ret = 1;
		goto out_unreg;
	}
	h_out.size = net_out.out_desc[0].size;
	if (ioctl(fd, CAVALRY_IOC_ALLOC_HANDLE, &h_out) < 0) {
		perror("ALLOC_HANDLE out");
		ret = 1;
		goto out_free_hin;
	}
	printf("[%s] Allocated Handles: in_hid=%u (bar_off=0x%08x), out_hid=%u (bar_off=0x%08x)\n",
	       role, h_in.handle_id, h_in.bar_offset, h_out.handle_id, h_out.bar_offset);

	/* 7. Map input and output handles into userspace */
	in_virt = mmap(NULL, h_in.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, h_in.bar_offset);
	out_virt = mmap(NULL, h_out.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, h_out.bar_offset);
	if (in_virt == MAP_FAILED || out_virt == MAP_FAILED) {
		perror("mmap handles failed");
		ret = 1;
		goto out_unmap;
	}

	/* 8. Prepare run request */
	run_u.dag_id = reg_u.dag_id;
	run_u.port_cnt = 2;
	run_u.ports[0].port_idx = 0;
	run_u.ports[0].handle_id = h_in.handle_id;
	run_u.ports[0].offset = 0;
	run_u.ports[0].size = h_in.size;
	run_u.ports[1].port_idx = 1;
	run_u.ports[1].handle_id = h_out.handle_id;
	run_u.ports[1].offset = 0;
	run_u.ports[1].size = h_out.size;

	/* If attack_handle_id provided, test hijacking foreign handle into our legitimate DAG */
	if (attack_handle_id > 0) {
		struct cavalry_run_reg_user atk_h = run_u;
		atk_h.ports[0].handle_id = (uint32_t)attack_handle_id;
		printf("[SECURITY] Testing unauthorized hijack of LIVE foreign handle (hid=%u) in local DAG...\n",
		       atk_h.ports[0].handle_id);
		if (ioctl(fd, CAVALRY_IOC_RUN_REGISTERED_DAG, &atk_h) < 0) {
			printf("[PASS] Host strictly rejected foreign handle hijack: errno=%d (%s)\n",
			       errno, strerror(errno));
		} else {
			fprintf(stderr, "[FATAL FAIL] Foreign handle hijack was NOT denied!\n");
			ret = 2;
			goto out_unmap;
		}
	}

	/* Optional resource hold to permit concurrent attacker testing */
	if (hold_seconds > 0) {
		printf("[%s] LIVE RESOURCE ANCHOR: dag_id=%u in_hid=%u out_hid=%u (Holding open for %d s)...\n",
		       role, reg_u.dag_id, h_in.handle_id, h_out.handle_id, hold_seconds);
		fflush(stdout);
		sleep(hold_seconds);
	}

	/* 9. Run inference loop */
	printf("[%s] Commencing %d inference iterations...\n", role, iterations);
	double t_start = get_time_ms();
	double total_ioctl_ms = 0.0;
	uint32_t total_ticks = 0;
	int pass_count = 0;

	for (i = 0; i < iterations; i++) {
		memset(in_virt, 1, h_in.size);
		memset(out_virt, 0, h_out.size);
		asm volatile("dsb sy" ::: "memory");

		double t_ioc_start = get_time_ms();
		if (ioctl(fd, CAVALRY_IOC_RUN_REGISTERED_DAG, &run_u) < 0) {
			fprintf(stderr, "[%s] Iteration %d failed: errno=%d (%s)\n",
				role, i, errno, strerror(errno));
			ret = 1;
			break;
		}
		double t_ioc_end = get_time_ms();
		total_ioctl_ms += (t_ioc_end - t_ioc_start);

		asm volatile("dsb sy" ::: "memory");
		compute_md5_str(out_virt, h_out.size, md5_str);
		if (strcmp(md5_str, EXPECTED_GOLDEN_MD5) != 0) {
			fprintf(stderr, "[%s] Iteration %d MD5 mismatch: %s\n",
				role, i, md5_str);
			ret = 1;
			break;
		}
		total_ticks += run_u.exec_ticks;
		pass_count++;
	}

	double t_total = get_time_ms() - t_start;
	double wall_fps = (double)pass_count / (t_total / 1000.0);
	double wall_lat = t_total / (double)pass_count;
	double ioctl_fps = total_ioctl_ms > 0 ? (double)pass_count / (total_ioctl_ms / 1000.0) : 0.0;
	double ioctl_lat = pass_count > 0 ? total_ioctl_ms / (double)pass_count : 0.0;
	uint32_t avg_ticks = pass_count > 0 ? total_ticks / pass_count : 0;

	printf("\n============================================================\n");
	printf("[%s] Final Results:\n", role);
	printf("  Completed:      %d / %d iterations\n", pass_count, iterations);
	printf("  VisORC Ticks:   %u avg (golden baseline %u)\n", avg_ticks, EXPECTED_EXEC_TICKS);
	printf("  Pure ioctl:     %.2f ms / frame (%.2f FPS)\n", ioctl_lat, ioctl_fps);
	printf("  Full Harness:   %.2f ms / frame (%.2f FPS) [incl. 10MB uncached BAR memset + 2MB SW MD5]\n",
	       wall_lat, wall_fps);
	printf("  Tensor MD5:     %s\n", md5_str);
	if (pass_count == iterations)
		printf(">>> [%s] VERDICT: PASS (100%% BIT-EXACT INTEGRITY) <<<\n", role);
	else
		printf(">>> [%s] VERDICT: FAIL <<<\n", role);
	printf("============================================================\n");

out_unmap:
	if (in_virt != MAP_FAILED) munmap(in_virt, h_in.size);
	if (out_virt != MAP_FAILED) munmap(out_virt, h_out.size);
	if (h_out.handle_id) ioctl(fd, CAVALRY_IOC_FREE_HANDLE, &h_out.handle_id);
out_free_hin:
	if (h_in.handle_id) ioctl(fd, CAVALRY_IOC_FREE_HANDLE, &h_in.handle_id);
out_unreg:
	if (reg_u.dag_id) ioctl(fd, CAVALRY_IOC_UNREGISTER_DAG, &reg_u.dag_id);
out_free_staging:
	if (net_m.virt_addr) cavalry_mem_free(net_cf.net_mem_total, net_m.phy_addr, net_m.virt_addr);
out_exit_net:
	nnctrl_exit_net(net_id);
	close(fd);
	return ret;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: t
 * End:
 */
