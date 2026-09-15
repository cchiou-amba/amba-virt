/*
 * cavalry_hvm_victim_loop.c
 *
 * Phase 4 Test 4.7 Looping Victim for Mid-Run Kill-9 Test
 * Enters an infinite RUN_DAGS loop so a killer process can signal SIGKILL
 * during active hardware execution.
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
#include <unistd.h>

#include <cavalry_ioctl.h>
#include <cavalry_mem.h>
#include <nnctrl.h>
#include "nnctrl_priv.h"
#include "utils.h"

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
	struct cavalry_run_dags *run = NULL;
	uint32_t iter = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path_to_vp_clk_cavalry.bin>\n", argv[0]);
		return 1;
	}
	model_path = argv[1];

	fd_cav = open("/dev/cavalry", O_RDWR);
	if (fd_cav < 0) {
		perror("open /dev/cavalry");
		return 1;
	}

	if (cavalry_mem_init(fd_cav, 0) < 0 || nnctrl_init(fd_cav, 0) < 0) {
		fprintf(stderr, "init failed\n");
		return 1;
	}

	net_cf.net_file = (char *)model_path;
	net_id = nnctrl_init_net(&net_cf, NULL, NULL);
	if (net_id < 0) {
		fprintf(stderr, "init_net failed\n");
		return 1;
	}

	if (cavalry_mem_alloc(&net_cf.net_mem_total, &net_m.phy_addr, (void **)&net_m.virt_addr, 0) < 0) {
		fprintf(stderr, "cavalry_mem_alloc failed\n");
		return 1;
	}
	net_m.mem_size = net_cf.net_mem_total;

	if (nnctrl_load_net(net_id, &net_m, NULL, NULL) < 0) {
		fprintf(stderr, "nnctrl_load_net failed\n");
		return 1;
	}

	if (nnctrl_get_net_io_cfg(net_id, &net_in, &net_out) < 0) {
		fprintf(stderr, "nnctrl_get_net_io_cfg failed\n");
		return 1;
	}

	memset(net_in.in_desc[0].virt, 1, net_in.in_desc[0].size);
	asm volatile("dsb sy" ::: "memory");

	pctl = get_nnctrl_global_context();
	pnet = get_net_desc(pctl, net_id);
	run = pnet->execute_ctx[0].run_dags;

	printf("[VICTIM READY] PID=%d slice=0x%lx, entering infinite RUN_DAGS loop...\n",
	       getpid(), net_m.phy_addr);
	fflush(stdout);

	while (1) {
		int ret = ioctl(fd_cav, CAVALRY_RUN_DAGS, run);
		if (ret < 0) {
			fprintf(stderr, "victim ioctl failed: ret=%d, errno=%d\n", ret, errno);
			break;
		}
		iter++;
	}

	close(fd_cav);
	return 0;
}
