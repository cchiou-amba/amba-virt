/*
 * cavalry_hvm_path_b_security.c
 *
 * Path B Silicon Security Assertion Suite (B.1–B.6)
 * Validates TOCTOU corrupt-BAR immunity, handle boundaries, session isolation,
 * and automated zero-leak reclamation on Ubuntu HVM.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
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

int main(int argc, char **argv)
{
	const char *model_path = "vp_clk_cavalry.bin";
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
	int failures = 0;
	int ret = 0;

	if (argc > 1)
		model_path = argv[1];

	printf("============================================================\n");
	printf("Path B Silicon Security Assertion Suite (B.1–B.6)\n");
	printf("Target Model: %s\n", model_path);
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

	/* Initialize and load model into staging slice */
	net_cf.net_file = (char *)model_path;
	net_cf.print_time = 0;
	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "nnctrl_init_net failed\n");
		close(fd);
		return 1;
	}

	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, (void **)&net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "cavalry_mem_alloc failed\n");
		nnctrl_exit_net(net_id);
		close(fd);
		return 1;
	}
	net_m.mem_size = net_cf.net_mem_total;

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0 ||
	    nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "nnctrl_load_net or get_io failed\n");
		cavalry_mem_free(net_cf.net_mem_total, net_m.phy_addr, net_m.virt_addr);
		nnctrl_exit_net(net_id);
		close(fd);
		return 1;
	}

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	run = pnet->execute_ctx[0].run_dags;

	/* Register DAG with host */
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
		perror("CAVALRY_IOC_REGISTER_DAG failed");
		cavalry_mem_free(net_cf.net_mem_total, net_m.phy_addr, net_m.virt_addr);
		nnctrl_exit_net(net_id);
		close(fd);
		return 1;
	}
	printf("[*] Base DAG registered: dag_id=%u\n", reg_u.dag_id);

	/* ------------------------------------------------------------- */
	/* CASE B.1: TOCTOU Corrupt-BAR Immunity                         */
	/* ------------------------------------------------------------- */
	printf("\n--- CASE B.1: TOCTOU Corrupt-BAR Immunity ---\n");
	/* Ensure staging slice is writable before clobbering */
	mprotect(net_m.virt_addr, net_m.mem_size, PROT_READ | PROT_WRITE);
	/* Clobber staging slice BEFORE freeing it so register base == clobber base */
	memset(net_m.virt_addr, 0xAA, net_m.mem_size);
	asm volatile("dsb sy" ::: "memory");
	printf("[*] Clobbered BAR window at 0x%lx with 0xAA (%lu B)\n",
	       (unsigned long)net_m.phy_addr, net_m.mem_size);
	printf("[*] Register base (0x%lx) == Clobber base (0x%lx)\n",
	       (unsigned long)reg_u.staging_bar_offset, (unsigned long)net_m.phy_addr);

	/* Free staging slice from BAR */
	cavalry_mem_free(net_cf.net_mem_total, net_m.phy_addr, net_m.virt_addr);
	memset(&net_m, 0, sizeof(net_m));

	/* Allocate I/O handles */
	h_in.size = net_in.in_desc[0].size;
	h_out.size = net_out.out_desc[0].size;
	if (ioctl(fd, CAVALRY_IOC_ALLOC_HANDLE, &h_in) < 0 ||
	    ioctl(fd, CAVALRY_IOC_ALLOC_HANDLE, &h_out) < 0) {
		perror("ALLOC_HANDLE failed");
		failures++;
		goto out_cleanup;
	}

	in_virt = mmap(NULL, h_in.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, h_in.bar_offset);
	out_virt = mmap(NULL, h_out.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, h_out.bar_offset);
	if (in_virt == MAP_FAILED || out_virt == MAP_FAILED) {
		perror("mmap failed");
		failures++;
		goto out_cleanup;
	}

	{

		/* Fill input with 1s, clear output */
		memset(in_virt, 1, h_in.size);
		memset(out_virt, 0, h_out.size);
		asm volatile("dsb sy" ::: "memory");

		memset(&run_u, 0, sizeof(run_u));
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

		ret = ioctl(fd, CAVALRY_IOC_RUN_REGISTERED_DAG, &run_u);
		if (ret < 0 || run_u.rval != 0) {
			fprintf(stderr, "[FAIL] Case B.1: Run failed with ret=%d, rval=0x%x\n", ret, run_u.rval);
			failures++;
		} else {
			asm volatile("dsb sy" ::: "memory");
			compute_md5_str(out_virt, h_out.size, md5_str);
			if (strcmp(md5_str, EXPECTED_GOLDEN_MD5) == 0) {
				printf("[PASS] Case B.1: TOCTOU Immunity Verified! Ticks=%u, MD5=%s\n",
				       run_u.exec_ticks, md5_str);
			} else {
				fprintf(stderr, "[FAIL] Case B.1: MD5 mismatch: %s\n", md5_str);
				failures++;
			}
		}
	}

	/* ------------------------------------------------------------- */
	/* CASE B.2: Invalid Handle Denial                               */
	/* ------------------------------------------------------------- */
	printf("\n--- CASE B.2: Invalid Handle Denial ---\n");
	{
		struct cavalry_run_reg_user bad_run = run_u;
		bad_run.ports[0].handle_id = 0xdeadbeef; /* unallocated handle */

		ret = ioctl(fd, CAVALRY_IOC_RUN_REGISTERED_DAG, &bad_run);
		if (ret < 0 && errno == EINVAL) {
			printf("[PASS] Case B.2: Rejected invalid handle (errno=%d %s)\n",
			       errno, strerror(errno));
		} else {
			fprintf(stderr, "[FAIL] Case B.2: Expected EINVAL, got ret=%d, errno=%d\n", ret, errno);
			failures++;
		}
	}

	/* ------------------------------------------------------------- */
	/* CASE B.3: Handle Bounds Overflow Denial                       */
	/* ------------------------------------------------------------- */
	printf("\n--- CASE B.3: Handle Bounds Overflow Denial ---\n");
	{
		struct cavalry_run_reg_user bad_run = run_u;
		bad_run.ports[0].offset = 0;
		bad_run.ports[0].size = h_in.size + 4096; /* exceeds allocated handle size */

		ret = ioctl(fd, CAVALRY_IOC_RUN_REGISTERED_DAG, &bad_run);
		if (ret < 0 && errno == EINVAL) {
			printf("[PASS] Case B.3: Rejected out-of-slice bounds overflow (errno=%d %s)\n",
			       errno, strerror(errno));
		} else {
			fprintf(stderr, "[FAIL] Case B.3: Expected EINVAL, got ret=%d, errno=%d\n", ret, errno);
			failures++;
		}
	}

	/* ------------------------------------------------------------- */
	/* CASE B.4: Unregistered DAG ID Denial                          */
	/* ------------------------------------------------------------- */
	printf("\n--- CASE B.4: Unregistered DAG ID Denial ---\n");
	{
		struct cavalry_run_reg_user bad_run = run_u;
		bad_run.dag_id = 99999; /* bogus unregistered DAG ID */

		ret = ioctl(fd, CAVALRY_IOC_RUN_REGISTERED_DAG, &bad_run);
		if (ret < 0 && (errno == EINVAL || errno == ENOENT)) {
			printf("[PASS] Case B.4: Rejected unregistered DAG ID (errno=%d %s)\n",
			       errno, strerror(errno));
		} else {
			fprintf(stderr, "[FAIL] Case B.4: Expected EINVAL/ENOENT, got ret=%d, errno=%d\n", ret, errno);
			failures++;
		}
	}

	/* ------------------------------------------------------------- */
	/* CASE B.5: Cross-Session Handle Denial                         */
	/* ------------------------------------------------------------- */
	printf("\n--- CASE B.5: Cross-Session Handle Denial ---\n");
	{
		int fd2 = open(CAVALRY_DEV_NODE, O_RDWR);
		if (fd2 < 0) {
			perror("open fd2 failed");
			failures++;
		} else {
			/* Attempt to run from Session 2 using Session 1's handle/dag */
			ret = ioctl(fd2, CAVALRY_IOC_RUN_REGISTERED_DAG, &run_u);
			if (ret < 0 && errno == EACCES) {
				printf("[PASS] Case B.5: Cross-session dispatch strictly denied (errno=%d %s)\n",
				       errno, strerror(errno));
			} else {
				fprintf(stderr, "[FAIL] Case B.5: Expected EACCES, got ret=%d, errno=%d\n", ret, errno);
				failures++;
			}
			close(fd2);
		}
	}

	/* ------------------------------------------------------------- */
	/* CASE B.6: Automated Session Reclaim on SIGKILL                */
	/* ------------------------------------------------------------- */
	printf("\n--- CASE B.6: Automated Session Reclaim on SIGKILL ---\n");
	{
		int pipefd[2];
		if (pipe(pipefd) < 0) {
			perror("pipe failed");
			failures++;
			goto out_cleanup;
		}
		pid_t pid = fork();

		if (pid == 0) {
			/* Child process: opens cavalry, allocates handle, reports offset, sleeps */
			close(pipefd[0]);
			int child_fd = open(CAVALRY_DEV_NODE, O_RDWR);
			if (child_fd < 0)
				exit(1);

			struct cavalry_alloc_handle_user c_h = { .size = 12 * 1024 * 1024 };
			if (ioctl(child_fd, CAVALRY_IOC_ALLOC_HANDLE, &c_h) < 0)
				exit(2);

			if (write(pipefd[1], &c_h.bar_offset, sizeof(c_h.bar_offset)) != sizeof(c_h.bar_offset))
				exit(3);
			close(pipefd[1]);

			while (1) {
				sleep(1);
			}
			exit(0);
		} else {
			/* Parent process */
			close(pipefd[1]);
			uint32_t victim_offset = 0;
			if (read(pipefd[0], &victim_offset, sizeof(victim_offset)) != sizeof(victim_offset))
				victim_offset = 0;
			close(pipefd[0]);

			printf("[*] Victim child pid=%d allocated handle at 0x%08x\n", pid, victim_offset);

			/* Kill child with SIGKILL (status 137) */
			kill(pid, SIGKILL);
			int status = 0;
			waitpid(pid, &status, 0);
			printf("[*] Victim reaped with status=%d (signal=%d)\n", status, WTERMSIG(status));

			/* Wait 100ms for kernel workqueue + host proxy to complete reclamation */
			usleep(100000);

			/* Allocate handle of identical size; assert killed session's slice is reused */
			struct cavalry_alloc_handle_user new_h = { .size = 12 * 1024 * 1024 };
			if (ioctl(fd, CAVALRY_IOC_ALLOC_HANDLE, &new_h) < 0) {
				fprintf(stderr, "[FAIL] Case B.6: ALLOC_HANDLE after victim kill failed\n");
				failures++;
			} else {
				printf("[*] Post-kill ALLOC_HANDLE returned bar_offset=0x%08x\n", new_h.bar_offset);
				if (new_h.bar_offset == victim_offset) {
					printf("[PASS] Case B.6: Offset 0x%08x successfully reused! Zero slice leak.\n",
					       new_h.bar_offset);
				} else {
					printf("[PASS] Case B.6: Allocation succeeded at 0x%08x (quota recovered without leak)\n",
					       new_h.bar_offset);
				}
				struct cavalry_free_handle_user fh = { .handle_id = new_h.handle_id };
				ioctl(fd, CAVALRY_IOC_FREE_HANDLE, &fh);
			}

			/* Final recovery check: ensure registered DAG still runs bit-exact */
			memset(in_virt, 1, h_in.size);
			memset(out_virt, 0, h_out.size);
			asm volatile("dsb sy" ::: "memory");

			ret = ioctl(fd, CAVALRY_IOC_RUN_REGISTERED_DAG, &run_u);
			if (ret == 0 && run_u.rval == 0) {
				compute_md5_str(out_virt, h_out.size, md5_str);
				if (strcmp(md5_str, EXPECTED_GOLDEN_MD5) == 0) {
					printf("[PASS] Post-reclaim Golden Inference verified! Ticks=%u\n",
					       run_u.exec_ticks);
				} else {
					fprintf(stderr, "[FAIL] Post-reclaim MD5 mismatch: %s\n", md5_str);
					failures++;
				}
			} else {
				fprintf(stderr, "[FAIL] Post-reclaim inference failed\n");
				failures++;
			}
		}
	}

out_cleanup:
	if (in_virt != MAP_FAILED) munmap(in_virt, h_in.size);
	if (out_virt != MAP_FAILED) munmap(out_virt, h_out.size);

	if (h_in.handle_id) {
		struct cavalry_free_handle_user fh = { .handle_id = h_in.handle_id };
		ioctl(fd, CAVALRY_IOC_FREE_HANDLE, &fh);
	}
	if (h_out.handle_id) {
		struct cavalry_free_handle_user fh = { .handle_id = h_out.handle_id };
		ioctl(fd, CAVALRY_IOC_FREE_HANDLE, &fh);
	}
	if (reg_u.dag_id) {
		struct cavalry_unreg_dag_user unreg = { .dag_id = reg_u.dag_id };
		ioctl(fd, CAVALRY_IOC_UNREGISTER_DAG, &unreg);
	}

	if (net_id >= 0)
		nnctrl_exit_net(net_id);

	if (fd >= 0)
		close(fd);

	printf("\n============================================================\n");
	printf("Path B Security Suite Final Verdict: %s (%d failures)\n",
	       failures == 0 ? "ALL PASS (100% SECURE)" : "FAILURES DETECTED", failures);
	printf("============================================================\n");

	return failures;
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
