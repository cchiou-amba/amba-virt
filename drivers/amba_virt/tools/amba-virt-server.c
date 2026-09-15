/*
 * amba-virt-server.c
 *
 * Userspace daemon on NOHYPER host container.
 * Handles vsock control messages, shared memory validation, and benchmark bursts.
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
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include "amba_virt.h"
#include "amba_virt_test.h"
#include "cavalry_proxy.h"

static int ensure_dev_node(const char *path)
{
	struct stat st;
	int major = -1, minor = 0;
	FILE *fp;
	char line[256];

	if (stat(path, &st) == 0) {
		if (S_ISCHR(st.st_mode))
			return 0;
		fprintf(stderr, "%s exists but is not a char device\n", path);
		return -1;
	}

	/* 1. Try reading major:minor from sysfs */
	fp = fopen("/sys/class/amba_virt/amba_virt/dev", "r");
	if (fp) {
		if (fscanf(fp, "%d:%d", &major, &minor) == 2) {
			fclose(fp);
			goto create_node;
		}
		fclose(fp);
	}

	/* 2. Fallback: Parse /proc/devices for "amba_virt" */
	fp = fopen("/proc/devices", "r");
	if (fp) {
		int in_char = 0;
		while (fgets(line, sizeof(line), fp)) {
			if (strstr(line, "Character devices:")) {
				in_char = 1;
				continue;
			}
			if (strstr(line, "Block devices:")) {
				in_char = 0;
				break;
			}
			if (in_char) {
				int num;
				char name[64];
				if (sscanf(line, "%d %63s", &num, name) == 2) {
					if (strcmp(name, AMBA_VIRT_DEV_NAME) == 0) {
						major = num;
						minor = 0;
						break;
					}
				}
			}
		}
		fclose(fp);
	}

	if (major < 0) {
		fprintf(stderr, "Cannot find %s in /sys or /proc/devices (is amba_virt.ko loaded?)\n",
			AMBA_VIRT_DEV_NAME);
		return -1;
	}

create_node:
	if (mknod(path, S_IFCHR | 0600, makedev(major, minor)) < 0) {
		if (errno != EEXIST) {
			fprintf(stderr, "mknod %s (c %d %d) failed: %s\n",
				path, major, minor, strerror(errno));
			return -1;
		}
	} else {
		chmod(path, 0600);
		printf("Auto-created device node %s (c %d %d)\n", path, major, minor);
	}

	return 0;
}

static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	int ret = ioctl(fd, req, arg);

	if (ret < 0)
		fprintf(stderr, "%s: %s\n", what, strerror(errno));
	return ret;
}

int main(void)
{
	struct amba_virt_info info;
	unsigned char *map = MAP_FAILED;
	uint64_t msg_count = 0;
	int fd;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (ensure_dev_node(AMBA_VIRT_DEV_PATH) < 0)
		return 1;

	fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror(AMBA_VIRT_DEV_PATH);
		return 1;
	}
	if (xioctl(fd, AMBA_VIRT_IOC_GET_INFO, &info, "GET_INFO") < 0)
		return 1;
	printf("amba-virt-server proto=%u role=%u shm=%u port=%u (listening)\n",
	       info.proto, info.role, info.shm_size, info.vsock_port);

	if (info.shm_size) {
		map = mmap(NULL, info.shm_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
		if (map == MAP_FAILED)
			perror("mmap (continuing without shm)");
	}

	cavalry_proxy_init(fd, map, info.shm_size);

	for (;;) {
		struct amba_virt_xfer rx, tx;
		struct amba_virt_msg *in;
		struct amba_virt_msg *out;

		memset(&rx, 0, sizeof(rx));
		if (ioctl(fd, AMBA_VIRT_IOC_RECV, &rx) < 0) {
			if (errno == ENOTCONN || errno == ECONNRESET || errno == ECONNABORTED ||
			    errno == ETIMEDOUT || errno == EPIPE) {
				usleep(10000);
				continue;
			}
			fprintf(stderr, "RECV error (%d: %s), retrying...\n", errno, strerror(errno));
			usleep(50000);
			continue;
		}
		if (rx.len < sizeof(*in)) {
			fprintf(stderr, "short msg %u\n", rx.len);
			continue;
		}

		in = (struct amba_virt_msg *)rx.data;
		memset(&tx, 0, sizeof(tx));
		msg_count++;

		if (in->type == AMBA_VIRT_MSG_PING) {
			out = (struct amba_virt_msg *)tx.data;
			out->type = AMBA_VIRT_MSG_PONG;
			out->seq = in->seq;
			out->shm_off = in->shm_off;
			out->shm_len = in->shm_len;
			tx.len = sizeof(*out);
			if (msg_count <= 5 || (msg_count % 1000 == 0))
				printf("PING seq=%u -> PONG\n", in->seq);
		} else if (in->type == AMBA_VIRT_MSG_SHM_NOTIFY) {
			out = (struct amba_virt_msg *)tx.data;
			out->type = AMBA_VIRT_MSG_SHM_ACK;
			out->seq = in->seq;
			out->shm_off = in->shm_off;
			out->shm_len = in->shm_len;
			tx.len = sizeof(*out);

			if (map != MAP_FAILED &&
			    (uint64_t)in->shm_off + in->shm_len <= info.shm_size) {
				// Memory touch/verification
				volatile uint8_t *p = map + in->shm_off;
				uint8_t first_byte = p[0];
				if (msg_count <= 5 || (msg_count % 1000 == 0)) {
					printf("SHM_NOTIFY seq=%u off=%u len=%u first=%u\n",
					       in->seq, in->shm_off, in->shm_len, first_byte);
				}
			}
		} else if (in->type == AMBA_VIRT_MSG_ECHO_REQ) {
			// Variable-length echo: copy exact payload back
			memcpy(tx.data, rx.data, rx.len);
			out = (struct amba_virt_msg *)tx.data;
			out->type = AMBA_VIRT_MSG_ECHO_RESP;
			tx.len = rx.len;
		} else if (in->type == AMBA_VIRT_MSG_BENCH_BURST) {
			// Fast benchmark echo
			memcpy(tx.data, rx.data, rx.len);
			out = (struct amba_virt_msg *)tx.data;
			out->type = AMBA_VIRT_MSG_BENCH_ACK;
			tx.len = rx.len;
		} else if (in->type == AMBA_VIRT_MSG_GDMA_COPY_REQ ||
			   in->type == AMBA_VIRT_MSG_GDMA_PITCH_REQ) {
			struct amba_virt_gdma_copy copy;
			uint32_t response_type =
				in->type == AMBA_VIRT_MSG_GDMA_PITCH_REQ ?
				AMBA_VIRT_MSG_GDMA_PITCH_RESP :
				AMBA_VIRT_MSG_GDMA_COPY_RESP;

			memset(&copy, 0, sizeof(copy));
			if (rx.len != sizeof(*in) + sizeof(copy)) {
				fprintf(stderr, "bad GDMA request length %u\n", rx.len);
				copy.status = -EPROTO;
			} else {
				memcpy(&copy, rx.data + sizeof(*in), sizeof(copy));
				if (ioctl(fd, AMBA_VIRT_IOC_HOST_GDMA_COPY,
					  &copy) < 0)
					copy.status = -errno;
			}

			out = (struct amba_virt_msg *)tx.data;
			memset(out, 0, sizeof(*out));
			out->type = response_type;
			out->seq = in->seq;
			memcpy(tx.data + sizeof(*out), &copy, sizeof(copy));
			tx.len = sizeof(*out) + sizeof(copy);
		} else if (in->type == AMBA_VIRT_MSG_CAVALRY_MOCK_REQ) {
			struct amba_virt_cavalry_mock *job =
				(struct amba_virt_cavalry_mock *)(rx.data + sizeof(struct amba_virt_msg));
			if (job->execution_delay_us > 0)
				usleep(job->execution_delay_us);
			job->status = 0; // success
			memcpy(tx.data, rx.data, rx.len);
			out = (struct amba_virt_msg *)tx.data;
			out->type = AMBA_VIRT_MSG_CAVALRY_MOCK_RESP;
			tx.len = rx.len;
		} else if (in->type == AMBA_VIRT_MSG_CAVALRY_REQ) {
			struct amba_virt_cavalry_rpc *cav_req;
			struct amba_virt_cavalry_rpc *cav_resp;

			if (rx.len < sizeof(*in) + sizeof(*cav_req)) {
				fprintf(stderr, "short cavalry rpc %u (expected >= %zu)\n",
					rx.len, sizeof(*in) + sizeof(*cav_req));
				continue;
			}
			cav_req = (struct amba_virt_cavalry_rpc *)(rx.data + sizeof(*in));
			out = (struct amba_virt_msg *)tx.data;
			memset(out, 0, sizeof(*out));
			out->type = AMBA_VIRT_MSG_CAVALRY_RESP;
			out->seq = in->seq;
			cav_resp = (struct amba_virt_cavalry_rpc *)(tx.data + sizeof(*out));
			cavalry_proxy_handle_rpc(cav_req, cav_resp, 1 /* default client_cid */);
			tx.len = sizeof(*out) + sizeof(*cav_resp);
		} else {
			fprintf(stderr, "unknown type %u\n", in->type);
			continue;
		}

		if (ioctl(fd, AMBA_VIRT_IOC_SEND, &tx) < 0) {
			if (errno != ENOTCONN && errno != ECONNRESET && errno != EPIPE) {
				perror("SEND");
			}
		}
	}
}
