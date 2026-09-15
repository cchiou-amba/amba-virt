/*
 * cavalry_hvm_test.c
 *
 * Verification test for Ambarella Cavalry HVM guest frontend driver (/dev/cavalry).
 * Tests ABI queries, mmap boundaries, slice allocation, and security denials.
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

int main(int argc, char *argv[])
{
	int fd;
	int ret;
	int failures = 0;

	printf("=== Cavalry HVM Frontend Driver Verification ===\n");

	fd = open("/dev/cavalry", O_RDWR);
	if (fd < 0) {
		perror("open /dev/cavalry");
		return 1;
	}
	printf("[PASS] Opened /dev/cavalry successfully (fd=%d)\n", fd);

	/* 1. CAVALRY_GET_DRIVER_VERSION */
	{
		struct cavalry_driver_version ver = { 0 };
		ret = ioctl(fd, CAVALRY_GET_DRIVER_VERSION, &ver);
		if (ret < 0) {
			perror("CAVALRY_GET_DRIVER_VERSION");
			failures++;
		} else {
			printf("[PASS] GET_DRIVER_VERSION: major=%u, minor=%u\n", ver.major, ver.minor);
			if (ver.major != 3) {
				fprintf(stderr, "FAIL: expected major=3, got %u\n", ver.major);
				failures++;
			}
		}
	}

	/* 2. CAVALRY_GET_CV_CHIP_ID */
	{
		uint32_t chip_id = 0;
		ret = ioctl(fd, CAVALRY_GET_CV_CHIP_ID, &chip_id);
		if (ret < 0) {
			perror("CAVALRY_GET_CV_CHIP_ID");
			failures++;
		} else {
			printf("[PASS] GET_CV_CHIP_ID: chip_id=%u (N1-655=10)\n", chip_id);
			if (chip_id != 10) {
				fprintf(stderr, "FAIL: expected chip_id=10, got %u\n", chip_id);
				failures++;
			}
		}
	}

	/* 3. CAVALRY_GET_CAVALRY_STATUS */
	{
		struct cavalry_status status = { 0 };
		ret = ioctl(fd, CAVALRY_GET_CAVALRY_STATUS, &status);
		if (ret < 0) {
			perror("CAVALRY_GET_CAVALRY_STATUS");
			failures++;
		} else {
			printf("[PASS] GET_CAVALRY_STATUS: is_started=%u\n", status.is_cavalry_started);
			if (!status.is_cavalry_started) {
				fprintf(stderr, "FAIL: expected is_started=1\n");
				failures++;
			}
		}
	}

	/* 4. CAVALRY_QUERY_BUF */
	{
		struct cavalry_querybuf q = { .buf = CAVALRY_MEM_USER };
		ret = ioctl(fd, CAVALRY_QUERY_BUF, &q);
		if (ret < 0) {
			perror("CAVALRY_QUERY_BUF");
			failures++;
		} else {
			printf("[PASS] QUERY_BUF(USER): offset=0x%08lx, length=0x%08lx (%lu MiB)\n",
			       q.offset, q.length, q.length / (1024 * 1024));
			if (q.offset != 0x02000000UL || q.length != 0x3E000000UL) {
				fprintf(stderr, "FAIL: expected [0x02000000, 0x3E000000), got [0x%lx, 0x%lx)\n",
					q.offset, q.length);
				failures++;
			}
		}
	}

	/* 5. mmap Guard: Reject offset < 32 MiB (GDMA zone) */
	{
		void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p != MAP_FAILED) {
			fprintf(stderr, "FAIL: mmap at offset 0 succeeded unexpectedly!\n");
			munmap(p, 4096);
			failures++;
		} else {
			printf("[PASS] mmap at offset 0 rejected with errno=%d (%s)\n", errno, strerror(errno));
		}
	}

	/* 6. mmap Guard: Reject offset 0x01F00000 (RPC Arena) */
	{
		void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01F00000);
		if (p != MAP_FAILED) {
			fprintf(stderr, "FAIL: mmap at offset 0x01F00000 succeeded unexpectedly!\n");
			munmap(p, 4096);
			failures++;
		} else {
			printf("[PASS] mmap at offset 0x01F00000 (RPC arena) rejected with errno=%d (%s)\n",
			       errno, strerror(errno));
		}
	}

	/* 7. ALLOC_MEM, valid mmap, memory touch, and FREE_MEM */
	{
		struct cavalry_mem mem = { 0 };
		mem.length = 65536;
		ret = ioctl(fd, CAVALRY_ALLOC_MEM, &mem);
		if (ret < 0) {
			perror("CAVALRY_ALLOC_MEM");
			failures++;
		} else {
			printf("[PASS] ALLOC_MEM: 64 KiB allocated at offset=0x%08lx\n", mem.offset);
			if (mem.offset < 0x02000000UL || mem.offset >= 0x40000000UL) {
				fprintf(stderr, "FAIL: allocated offset 0x%lx outside [32 MiB, 1 GiB)\n", mem.offset);
				failures++;
			} else {
				/* mmap allocated offset */
				volatile uint32_t *ptr = mmap(NULL, mem.length, PROT_READ | PROT_WRITE,
							      MAP_SHARED, fd, mem.offset);
				if (ptr == MAP_FAILED) {
					perror("mmap allocated slice");
					failures++;
				} else {
					/* Write test pattern */
					ptr[0] = 0xdeadbeef;
					ptr[1] = 0xcafebabe;
					if (ptr[0] == 0xdeadbeef && ptr[1] == 0xcafebabe) {
						printf("[PASS] mmap touch successful (0x%08x, 0x%08x)\n", ptr[0], ptr[1]);
					} else {
						fprintf(stderr, "FAIL: memory readback mismatch\n");
						failures++;
					}
					munmap((void *)ptr, mem.length);
				}

				/* Free slice */
				ret = ioctl(fd, CAVALRY_FREE_MEM, &mem);
				if (ret < 0) {
					perror("CAVALRY_FREE_MEM");
					failures++;
				} else {
					printf("[PASS] FREE_MEM for offset=0x%08lx succeeded\n", mem.offset);
				}
			}
		}
	}

	/* 8. Denial of CAVALRY_DMA_COPY (0x28) */
	{
		struct {
			uint32_t src;
			uint32_t dst;
			uint32_t size;
		} dma_req = { 0 };
		ret = ioctl(fd, CAVALRY_DMA_COPY, &dma_req);
		if (ret == 0) {
			fprintf(stderr, "FAIL: CAVALRY_DMA_COPY was accepted!\n");
			failures++;
		} else {
			printf("[PASS] CAVALRY_DMA_COPY denied with errno=%d (%s)\n", errno, strerror(errno));
			if (errno != EPERM) {
				fprintf(stderr, "FAIL: expected EPERM (1), got %d\n", errno);
				failures++;
			}
		}
	}

	/* 9. mmap Guard: Reject offset >= 1 GiB (beyond BAR) */
	{
		void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x40000000UL);
		if (p != MAP_FAILED) {
			fprintf(stderr, "FAIL: mmap at offset 0x40000000 succeeded unexpectedly!\n");
			munmap(p, 4096);
			failures++;
		} else {
			printf("[PASS] mmap at offset 0x40000000 (>= 1 GiB) rejected with errno=%d (%s)\n",
			       errno, strerror(errno));
		}
	}

	/* 10. FREE_MEM Guard: Reject unallocated offset */
	{
		struct cavalry_mem unalloc = { .offset = 0x03500000UL, .length = 4096 };
		ret = ioctl(fd, CAVALRY_FREE_MEM, &unalloc);
		if (ret == 0) {
			fprintf(stderr, "FAIL: FREE_MEM of unallocated offset succeeded unexpectedly!\n");
			failures++;
		} else {
			printf("[PASS] FREE_MEM for unallocated offset rejected with errno=%d (%s)\n",
			       errno, strerror(errno));
		}
	}

	/* 11. CAVALRY_QUERY_UCODE_CMD_SIZE */
	{
		struct cavalry_ucode_cmd_size ucmd = { .cmd_id = CAVALRY_RUN_DAGS, .dag_cnt = 1 };
		ret = ioctl(fd, CAVALRY_QUERY_UCODE_CMD_SIZE, &ucmd);
		if (ret < 0) {
			perror("CAVALRY_QUERY_UCODE_CMD_SIZE");
			failures++;
		} else {
			printf("[PASS] QUERY_UCODE_CMD_SIZE: cmd_size=%u bytes\n", ucmd.cmd_size);
			if (ucmd.cmd_size == 0) {
				fprintf(stderr, "FAIL: cmd_size returned 0\n");
				failures++;
			}
		}
	}

	/* 12. CAVALRY_GET_AUDIO_CLK */
	{
		uint64_t aclk = 0;
		ret = ioctl(fd, CAVALRY_GET_AUDIO_CLK, &aclk);
		if (ret < 0) {
			perror("CAVALRY_GET_AUDIO_CLK");
			failures++;
		} else {
			printf("[PASS] GET_AUDIO_CLK: clk=%lu Hz\n", aclk);
			if (aclk != 24000000ULL) {
				fprintf(stderr, "FAIL: expected 24000000, got %lu\n", aclk);
				failures++;
			}
		}
	}

	/* 13. Sequential Multi-Slice Lifecycle Stability */
	{
		struct cavalry_mem s1 = { .length = 1048576 }; /* 1 MiB */
		struct cavalry_mem s2 = { .length = 2097152 }; /* 2 MiB */
		ret = ioctl(fd, CAVALRY_ALLOC_MEM, &s1);
		if (ret < 0) {
			perror("ALLOC s1");
			failures++;
		}
		ret = ioctl(fd, CAVALRY_ALLOC_MEM, &s2);
		if (ret < 0) {
			perror("ALLOC s2");
			failures++;
		}
		if (s1.offset != 0 && s2.offset != 0) {
			if (s2.offset != s1.offset + s1.length) {
				fprintf(stderr, "FAIL: expected contiguous allocation: s1=0x%lx, s2=0x%lx\n",
					s1.offset, s2.offset);
				failures++;
			} else {
				printf("[PASS] Multi-slice alloc: s1=0x%08lx (1 MiB), s2=0x%08lx (2 MiB)\n",
				       s1.offset, s2.offset);
			}
			/* Free in reverse order */
			ret = ioctl(fd, CAVALRY_FREE_MEM, &s2);
			if (ret < 0) failures++;
			ret = ioctl(fd, CAVALRY_FREE_MEM, &s1);
			if (ret < 0) failures++;
			printf("[PASS] Multi-slice free: reverse-order deallocation succeeded\n");
		}
	}

	close(fd);
	printf("=== Verification Result: %s (%d failures) ===\n",
	       failures == 0 ? "ALL PASSED" : "FAILED", failures);
	return failures;
}
