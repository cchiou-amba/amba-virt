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
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "amba_virt.h"
#include "amba_virt_test.h"
#include "cavalry_proxy.h"
#include "virt_acl.h"
#include "virt_admin_ipc.h"
#include "virt_backend_client.h"
#include "virt_dma_broker.h"
#include "virt_driver_matrix.h"
#include "virt_mem_pool.h"
#include "virt_query.h"

static pthread_mutex_t g_gdma_hw_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t msg_count = 0;

struct server_ctx {
	int fd;
	unsigned char *map;
	struct amba_virt_info info;
	uint16_t tcp_port;
};

static struct server_ctx g_ctx;

int server_broadcast_dev_state(uint32_t dev_id, uint32_t state, uint32_t reason_code, uint32_t host_mod_mask)
{
	if (g_ctx.fd < 0)
		return -1;

	struct amba_virt_push_msg push;
	memset(&push, 0, sizeof(push));
	push.target_cid = 0; /* 0 = broadcast to all active guests */

	struct amba_virt_msg *msg = (struct amba_virt_msg *)push.data;
	msg->type = AMBA_VIRT_MSG_DEV_STATE_EVENT;
	msg->seq = 0;

	struct amba_virt_dev_state_event *evt =
		(struct amba_virt_dev_state_event *)(push.data + sizeof(*msg));
	evt->dev_id = dev_id;
	evt->state = state;
	evt->reason_code = reason_code;
	evt->host_mod_mask = host_mod_mask;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	evt->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

	push.len = sizeof(*msg) + sizeof(*evt);

	int ret = ioctl(g_ctx.fd, AMBA_VIRT_IOC_PUSH, &push);
	return ret;
}

static void on_backend_module_state_change(uint32_t new_mod_mask)
{
	virt_query_set_host_mod_mask(new_mod_mask);

	if (new_mod_mask & 0x00000002) {
		cavalry_proxy_reopen();
	}

	for (size_t i = 0; i < DRIVER_MATRIX_COUNT; i++) {
		if (g_driver_matrix[i].virt_dev_id == 0)
			continue;

		uint32_t needed = g_driver_matrix[i].module_mask | g_driver_matrix[i].prerequisite_mask;
		uint32_t state = ((new_mod_mask & needed) == needed) ?
				 AMBA_VIRT_DEV_STATE_ONLINE : AMBA_VIRT_DEV_STATE_OFFLINE;

		server_broadcast_dev_state(g_driver_matrix[i].virt_dev_id,
					   state,
					   (state == AMBA_VIRT_DEV_STATE_ONLINE) ? 0 : 1,
					   new_mod_mask);
	}
}

static void process_incoming_msg(const struct amba_virt_xfer *rx,
				 struct amba_virt_xfer *tx,
				 int fd,
				 unsigned char *map,
				 const struct amba_virt_info *info)
{
	const struct amba_virt_msg *in;
	struct amba_virt_msg *out;

	if (rx->len < sizeof(*in)) {
		fprintf(stderr, "short msg %u\n", rx->len);
		return;
	}

	in = (const struct amba_virt_msg *)rx->data;
	memset(tx, 0, sizeof(*tx));
	tx->client_cid = rx->client_cid;
	msg_count++;

	if (in->type == AMBA_VIRT_MSG_PING) {
		out = (struct amba_virt_msg *)tx->data;
		out->type = AMBA_VIRT_MSG_PONG;
		out->seq = in->seq;
		out->shm_off = in->shm_off;
		out->shm_len = in->shm_len;
		tx->len = sizeof(*out);
		if (msg_count <= 5 || (msg_count % 1000 == 0))
			printf("PING seq=%u (from cid=%u) -> PONG\n", in->seq, rx->client_cid);
	} else if (in->type == AMBA_VIRT_MSG_SHM_NOTIFY) {
		out = (struct amba_virt_msg *)tx->data;
		out->type = AMBA_VIRT_MSG_SHM_ACK;
		out->seq = in->seq;
		out->shm_off = in->shm_off;
		out->shm_len = in->shm_len;
		tx->len = sizeof(*out);

		if (map != MAP_FAILED &&
		    (uint64_t)in->shm_off + in->shm_len <= info->shm_size) {
			volatile uint8_t *p = map + in->shm_off;
			uint8_t first_byte = p[0];
			if (msg_count <= 5 || (msg_count % 1000 == 0)) {
				printf("SHM_NOTIFY seq=%u cid=%u off=%u len=%u first=%u\n",
				       in->seq, rx->client_cid, in->shm_off, in->shm_len, first_byte);
			}
		}
	} else if (in->type == AMBA_VIRT_MSG_ECHO_REQ) {
		memcpy(tx->data, rx->data, rx->len);
		out = (struct amba_virt_msg *)tx->data;
		out->type = AMBA_VIRT_MSG_ECHO_RESP;
		tx->len = rx->len;
	} else if (in->type == AMBA_VIRT_MSG_BENCH_BURST) {
		memcpy(tx->data, rx->data, rx->len);
		out = (struct amba_virt_msg *)tx->data;
		out->type = AMBA_VIRT_MSG_BENCH_ACK;
		tx->len = rx->len;
	} else if (in->type == AMBA_VIRT_MSG_GDMA_COPY_REQ ||
		   in->type == AMBA_VIRT_MSG_GDMA_PITCH_REQ) {
		struct amba_virt_gdma_copy copy;
		struct cavalry_tenant_ctx *tenant;
		uint32_t response_type =
			in->type == AMBA_VIRT_MSG_GDMA_PITCH_REQ ?
			AMBA_VIRT_MSG_GDMA_PITCH_RESP :
			AMBA_VIRT_MSG_GDMA_COPY_RESP;

		memset(&copy, 0, sizeof(copy));
		if (rx->len != sizeof(*in) + sizeof(copy)) {
			fprintf(stderr, "bad GDMA request length %u\n", rx->len);
			copy.status = -EPROTO;
		} else {
			memcpy(&copy, rx->data + sizeof(*in), sizeof(copy));
			tenant = cavalry_proxy_get_tenant(rx->client_cid);
			if (!tenant) {
				fprintf(stderr, "GDMA: unknown tenant for cid=%u\n", rx->client_cid);
				copy.status = -EACCES;
			} else {
				uint32_t cap = (in->type == AMBA_VIRT_MSG_GDMA_PITCH_REQ) ?
						AMBA_VIRT_CAP_GDMA_PITCH : AMBA_VIRT_CAP_GDMA_COPY;
				if (!virt_acl_has_cap(rx->client_cid, cap)) {
					copy.status = -EPERM;
				} else {
					uint32_t slice_off = tenant->slice_offset;
					int range_err = 0;

					if (copy.flags & AMBA_VIRT_GDMA_F_PITCH) {
						uint64_t src_end = (uint64_t)copy.src_off +
							(uint64_t)(copy.height ? copy.height - 1 : 0) * copy.src_pitch + copy.width;
						uint64_t dst_end = (uint64_t)copy.dst_off +
							(uint64_t)(copy.height ? copy.height - 1 : 0) * copy.dst_pitch + copy.width;
						if (src_end > tenant->shm_size || dst_end > tenant->shm_size)
							range_err = 1;
					} else {
						if ((uint64_t)copy.src_off + copy.len > tenant->shm_size ||
						    (uint64_t)copy.dst_off + copy.len > tenant->shm_size)
							range_err = 1;
					}

					if (range_err) {
						fprintf(stderr, "GDMA bounds violation: cid=%u src=0x%x dst=0x%x len=0x%x > 0x%zx\n",
							rx->client_cid, copy.src_off, copy.dst_off, copy.len, tenant->shm_size);
						copy.status = -ERANGE;
					} else {
						copy.src_off += slice_off;
						copy.dst_off += slice_off;

						pthread_mutex_lock(&g_gdma_hw_mutex);
						if (ioctl(fd, AMBA_VIRT_IOC_HOST_GDMA_COPY, &copy) < 0)
							copy.status = -errno;
						pthread_mutex_unlock(&g_gdma_hw_mutex);

						copy.src_off -= slice_off;
						copy.dst_off -= slice_off;
					}
				}
			}
		}

		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = response_type;
		out->seq = in->seq;
		memcpy(tx->data + sizeof(*out), &copy, sizeof(copy));
		tx->len = sizeof(*out) + sizeof(copy);
	} else if (in->type == AMBA_VIRT_MSG_CAVALRY_MOCK_REQ) {
		struct amba_virt_cavalry_mock *job =
			(struct amba_virt_cavalry_mock *)(rx->data + sizeof(struct amba_virt_msg));
		if (job->execution_delay_us > 0)
			usleep(job->execution_delay_us);
		job->status = 0; // success
		memcpy(tx->data, rx->data, rx->len);
		out = (struct amba_virt_msg *)tx->data;
		out->type = AMBA_VIRT_MSG_CAVALRY_MOCK_RESP;
		tx->len = rx->len;
	} else if (in->type == AMBA_VIRT_MSG_CAVALRY_REQ) {
		struct amba_virt_cavalry_rpc *cav_req;
		struct amba_virt_cavalry_rpc *cav_resp;

		if (rx->len < sizeof(*in) + sizeof(*cav_req)) {
			fprintf(stderr, "short cavalry rpc %u (expected >= %zu)\n",
				rx->len, sizeof(*in) + sizeof(*cav_req));
			return;
		}
		cav_req = (struct amba_virt_cavalry_rpc *)(rx->data + sizeof(*in));
		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = AMBA_VIRT_MSG_CAVALRY_RESP;
		out->seq = in->seq;
		cav_resp = (struct amba_virt_cavalry_rpc *)(tx->data + sizeof(*out));

		if (cav_req->opcode == VCAV_OP_RUN_DAGS &&
		    !virt_acl_has_cap(rx->client_cid, AMBA_VIRT_CAP_CAVALRY_PATH_A)) {
			cav_resp->status = -EPERM;
		} else if (cav_req->opcode == VCAV_OP_REGISTER_DAG &&
			   !virt_acl_has_cap(rx->client_cid, AMBA_VIRT_CAP_CAVALRY_REGISTER)) {
			cav_resp->status = -EPERM;
		} else {
			cavalry_proxy_handle_rpc(cav_req, cav_resp, rx->client_cid);
		}
		tx->len = sizeof(*out) + sizeof(*cav_resp);
	} else if (in->type == AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ) {
		struct amba_virt_dev_bounds_req *b_req;
		struct amba_virt_dev_bounds_resp *b_resp;
		struct cavalry_tenant_ctx *tenant;
		unsigned char *submap = MAP_FAILED;

		if (rx->len < sizeof(*in) + sizeof(*b_req)) {
			fprintf(stderr, "short dev bounds req %u (expected >= %zu)\n",
				rx->len, sizeof(*in) + sizeof(*b_req));
			return;
		}
		b_req = (struct amba_virt_dev_bounds_req *)(rx->data + sizeof(*in));
		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = AMBA_VIRT_MSG_DEV_SET_BOUNDS_RESP;
		out->seq = in->seq;
		b_resp = (struct amba_virt_dev_bounds_resp *)(tx->data + sizeof(*out));

		if (!virt_acl_has_cap(rx->client_cid, AMBA_VIRT_CAP_DEV_CONFIG)) {
			b_resp->status = -EPERM;
			b_resp->err_code = AMBA_VIRT_ERR_PERM_DENIED;
			tx->len = sizeof(*out) + sizeof(*b_resp);
		} else {
			tenant = cavalry_proxy_get_tenant(rx->client_cid);
			if (tenant)
				submap = tenant->shm_map;

			virt_mem_pool_set_device_bounds(rx->client_cid, b_req, b_resp, submap);

			if (b_resp->status == 0 && b_req->dev_type == AMBA_VIRT_DEV_TYPE_CAVALRY) {
				cavalry_proxy_set_tenant_bounds(rx->client_cid,
								b_resp->granted_offset,
								b_resp->granted_size,
								b_resp->rpc_arena_offset,
								b_req->rpc_arena_size);
			}

			tx->len = sizeof(*out) + sizeof(*b_resp);
		}
	} else if (in->type == AMBA_VIRT_MSG_DEV_RELEASE_BOUNDS_REQ) {
		struct amba_virt_dev_bounds_req *b_req;
		struct amba_virt_dev_bounds_resp *b_resp;

		if (rx->len < sizeof(*in) + sizeof(*b_req)) {
			fprintf(stderr, "short dev release req %u\n", rx->len);
			return;
		}
		b_req = (struct amba_virt_dev_bounds_req *)(rx->data + sizeof(*in));
		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = AMBA_VIRT_MSG_DEV_RELEASE_BOUNDS_RESP;
		out->seq = in->seq;
		b_resp = (struct amba_virt_dev_bounds_resp *)(tx->data + sizeof(*out));

		b_resp->status = virt_mem_pool_release_device_bounds(rx->client_cid, b_req->dev_type);
		if (b_req->dev_type == AMBA_VIRT_DEV_TYPE_CAVALRY) {
			cavalry_proxy_set_tenant_bounds(rx->client_cid,
							CAVALRY_POOL_BASE,
							CAVALRY_POOL_SIZE,
							CAVALRY_RPC_ARENA_OFFSET,
							CAVALRY_RPC_ARENA_SIZE);
		}

		tx->len = sizeof(*out) + sizeof(*b_resp);
	} else if (in->type == AMBA_VIRT_MSG_MEM_ALLOC_REQ) {
		struct amba_virt_mem_req *m_req;
		struct amba_virt_mem_resp *m_resp;
		struct cavalry_tenant_ctx *tenant;
		unsigned char *submap = MAP_FAILED;
		uint64_t phys_base = 0;

		if (rx->len < sizeof(*in) + sizeof(*m_req)) {
			fprintf(stderr, "short mem alloc req %u\n", rx->len);
			return;
		}
		m_req = (struct amba_virt_mem_req *)(rx->data + sizeof(*in));
		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = AMBA_VIRT_MSG_MEM_ALLOC_RESP;
		out->seq = in->seq;
		m_resp = (struct amba_virt_mem_resp *)(tx->data + sizeof(*out));

		if (!virt_acl_has_cap(rx->client_cid, AMBA_VIRT_CAP_MEM_ALLOC)) {
			m_resp->status = -EPERM;
			tx->len = sizeof(*out) + sizeof(*m_resp);
		} else {
			tenant = cavalry_proxy_get_tenant(rx->client_cid);
			if (tenant) {
				submap = tenant->shm_map;
				phys_base = tenant->phys_base;
			}

			if (m_req->op == AMBA_VIRT_MEM_OP_ALLOC) {
				virt_mem_pool_alloc_extent(rx->client_cid, m_req, m_resp, submap, phys_base);
			} else if (m_req->op == AMBA_VIRT_MEM_OP_FREE) {
				virt_mem_pool_free_extent(rx->client_cid, m_req->bar_offset, m_resp);
			} else {
				m_resp->status = -EOPNOTSUPP;
			}

			tx->len = sizeof(*out) + sizeof(*m_resp);
		}
	} else if (in->type == AMBA_VIRT_MSG_QUERY_REQ) {
		struct amba_virt_query_req *q_req;
		struct amba_virt_query_resp *q_resp;

		if (rx->len < sizeof(*in) + sizeof(*q_req)) {
			fprintf(stderr, "short query req %u (expected >= %zu)\n",
				rx->len, sizeof(*in) + sizeof(*q_req));
			return;
		}
		q_req = (struct amba_virt_query_req *)(rx->data + sizeof(*in));
		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = AMBA_VIRT_MSG_QUERY_RESP;
		out->seq = in->seq;
		q_resp = (struct amba_virt_query_resp *)(tx->data + sizeof(*out));

		virt_query_handle_req(rx->client_cid, q_req, q_resp);
		tx->len = sizeof(*out) + sizeof(*q_resp);
	} else if (in->type == AMBA_VIRT_MSG_DMA_SLAVE_CFG_REQ) {
		struct amba_virt_dma_slave_cfg *d_req;
		struct amba_virt_dma_slave_cfg *d_resp;

		if (rx->len < sizeof(*in) + sizeof(*d_req)) {
			fprintf(stderr, "short DMA slave cfg req %u\n", rx->len);
			return;
		}
		d_req = (struct amba_virt_dma_slave_cfg *)(rx->data + sizeof(*in));
		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = AMBA_VIRT_MSG_DMA_SLAVE_CFG_RESP;
		out->seq = in->seq;
		d_resp = (struct amba_virt_dma_slave_cfg *)(tx->data + sizeof(*out));

		virt_dma_handle_slave_cfg(rx->client_cid, d_req, d_resp);
		tx->len = sizeof(*out) + sizeof(*d_resp);
	} else if (in->type == AMBA_VIRT_MSG_DMA_SUBMIT_REQ) {
		struct amba_virt_dma_submit *d_req;
		struct amba_virt_dma_submit *d_resp;

		if (rx->len < sizeof(*in) + sizeof(*d_req)) {
			fprintf(stderr, "short DMA submit req %u\n", rx->len);
			return;
		}
		d_req = (struct amba_virt_dma_submit *)(rx->data + sizeof(*in));
		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = AMBA_VIRT_MSG_DMA_SUBMIT_RESP;
		out->seq = in->seq;
		d_resp = (struct amba_virt_dma_submit *)(tx->data + sizeof(*out));

		virt_dma_handle_submit(rx->client_cid, info->shm_size,
				      d_req, d_resp);
		tx->len = sizeof(*out) + sizeof(*d_resp);
	} else if (in->type == AMBA_VIRT_MSG_DMA_TERMINATE_REQ) {
		struct amba_virt_dma_terminate *d_req;
		struct amba_virt_dma_terminate *d_resp;

		if (rx->len < sizeof(*in) + sizeof(*d_req)) {
			fprintf(stderr, "short DMA terminate req %u\n", rx->len);
			return;
		}
		d_req = (struct amba_virt_dma_terminate *)(rx->data + sizeof(*in));
		out = (struct amba_virt_msg *)tx->data;
		memset(out, 0, sizeof(*out));
		out->type = AMBA_VIRT_MSG_DMA_TERMINATE_RESP;
		out->seq = in->seq;
		d_resp = (struct amba_virt_dma_terminate *)(tx->data + sizeof(*out));

		virt_dma_handle_terminate(rx->client_cid, d_req, d_resp);
		tx->len = sizeof(*out) + sizeof(*d_resp);
	} else {
		fprintf(stderr, "unknown type %u\n", in->type);
	}
}

static int send_exact_tcp(int sock, const void *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(sock, (const char *)buf + done, len - done);
		if (n <= 0)
			return -1;
		done += n;
	}
	return 0;
}

static int recv_exact_tcp(int sock, void *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		ssize_t n = read(sock, (char *)buf + done, len - done);
		if (n <= 0)
			return -1;
		done += n;
	}
	return 0;
}

static void *tcp_client_worker(void *arg)
{
	int sock = (int)(intptr_t)arg;
	struct amba_virt_xfer rx, tx;
	uint32_t len = 0;

	for (;;) {
		if (recv_exact_tcp(sock, &len, sizeof(len)) < 0)
			break;
		if (len == 0 || len > AMBA_VIRT_MAX_MSG)
			break;
		memset(&rx, 0, sizeof(rx));
		rx.len = len;
		rx.client_cid = 4; /* Default guest CID */
		if (recv_exact_tcp(sock, rx.data, len) < 0)
			break;

		memset(&tx, 0, sizeof(tx));
		process_incoming_msg(&rx, &tx, g_ctx.fd, g_ctx.map, &g_ctx.info);

		if (tx.len > 0) {
			if (send_exact_tcp(sock, &tx.len, sizeof(tx.len)) < 0)
				break;
			if (send_exact_tcp(sock, tx.data, tx.len) < 0)
				break;
		}
	}

	close(sock);
	return NULL;
}

static void *tcp_listener_thread(void *arg)
{
	uint16_t port = (uint16_t)(uintptr_t)arg;
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("tcp socket");
		return NULL;
	}

	int opt = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(listen_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		perror("tcp bind");
		close(listen_fd);
		return NULL;
	}

	if (listen(listen_fd, 16) < 0) {
		perror("tcp listen");
		close(listen_fd);
		return NULL;
	}

	printf("amba-virt-server: listening for TCP client bridges on port %u\n", port);

	for (;;) {
		struct sockaddr_in peer;
		socklen_t peerlen = sizeof(peer);
		int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peerlen);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		int nodelay = 1;
		setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

		pthread_t tid;
		pthread_create(&tid, NULL, tcp_client_worker, (void *)(intptr_t)client_fd);
		pthread_detach(tid);
	}

	close(listen_fd);
	return NULL;
}

#define MAX_CLI_TENANTS 8
struct cli_tenant {
	uint32_t cid;
	uint32_t tenant_idx;
};

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n", prog);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -b, --enforce-path-b        Reject legacy Path A (VCAV_OP_RUN_DAGS) with -EPERM\n");
	fprintf(stderr, "  -t, --tenant <cid>:<idx>    Pre-register tenant slice for vsock CID to index\n");
	fprintf(stderr, "  -h, --help                  Show this help message\n");
}

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

int main(int argc, char **argv)
{
	struct amba_virt_info info;
	unsigned char *map = MAP_FAILED;
	int fd;
	int enforce_path_b = 0;
	struct cli_tenant cli_tenants[MAX_CLI_TENANTS];
	int num_cli_tenants = 0;
	int opt;

	static struct option long_options[] = {
		{"enforce-path-b", no_argument,       0, 'b'},
		{"tenant",         required_argument, 0, 't'},
		{"help",           no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	while ((opt = getopt_long(argc, argv, "bt:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'b':
			enforce_path_b = 1;
			break;
		case 't': {
			uint32_t cid = 0, idx = 0;
			if (sscanf(optarg, "%u:%u", &cid, &idx) == 2 ||
			    sscanf(optarg, "%u=%u", &idx, &cid) == 2) {
				if (num_cli_tenants < MAX_CLI_TENANTS) {
					cli_tenants[num_cli_tenants].cid = cid;
					cli_tenants[num_cli_tenants].tenant_idx = idx;
					num_cli_tenants++;
				} else {
					fprintf(stderr, "Too many static tenants (max %d)\n", MAX_CLI_TENANTS);
				}
			} else {
				fprintf(stderr, "Invalid tenant format '%s' (expected <cid>:<tenant_idx>)\n", optarg);
				return 1;
			}
			break;
		}
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (ensure_dev_node(AMBA_VIRT_DEV_PATH) < 0)
		return 1;

	fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror(AMBA_VIRT_DEV_PATH);
		return 1;
	}
	if (xioctl(fd, AMBA_VIRT_IOC_GET_INFO, &info, "GET_INFO") < 0)
		return 1;
	printf("amba-virt-server proto=%u role=%u shm=%u phys=0x%lx port=%u (listening)\n",
	       info.proto, info.role, info.shm_size, (unsigned long)info.shm_phys, info.vsock_port);

	if (info.shm_size) {
		map = mmap(NULL, info.shm_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
		if (map == MAP_FAILED)
			perror("mmap (continuing without shm)");
	}

	if (cavalry_proxy_init(fd, map, info.shm_size, info.shm_phys) < 0) {
		fprintf(stderr, "Failed to initialize cavalry proxy\n");
		return 1;
	}

	virt_mem_pool_init(0x40000000U); /* 1 GiB default BAR */
	virt_acl_init();
	virt_dma_broker_init();
	virt_admin_ipc_start(NULL);
	virt_backend_client_init("127.0.0.1", 5556, "/persist/etc/amba-virt-backend.token", on_backend_module_state_change);

	if (enforce_path_b) {
		cavalry_proxy_set_enforce_path_b(1);
		printf("amba-virt-server: Enforcing Path-B-only policy (legacy Path A disabled)\n");
	}

	for (int i = 0; i < num_cli_tenants; i++) {
		uint32_t cid = cli_tenants[i].cid;
		uint32_t idx = cli_tenants[i].tenant_idx;
		uint32_t slice_sz = 0x40000000U; /* 1 GiB */
		uint32_t slice_off = idx * slice_sz;
		unsigned char *submap = (map != MAP_FAILED && slice_off < info.shm_size) ?
					(map + slice_off) : MAP_FAILED;
		cavalry_proxy_register_tenant(cid, idx, -1, submap, slice_sz,
					      info.shm_phys + slice_off, slice_off);
		virt_mem_pool_register_tenant(cid, idx, slice_sz, slice_sz);
	}

	g_ctx.fd = fd;
	g_ctx.map = map;
	g_ctx.info = info;
	g_ctx.tcp_port = info.vsock_port ? (uint16_t)info.vsock_port : 5555;

	pthread_t tcp_tid;
	if (pthread_create(&tcp_tid, NULL, tcp_listener_thread, (void *)(uintptr_t)g_ctx.tcp_port) == 0) {
		pthread_detach(tcp_tid);
	} else {
		perror("pthread_create(tcp_listener_thread)");
	}

	for (;;) {
		struct amba_virt_xfer rx, tx;

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

		memset(&tx, 0, sizeof(tx));
		process_incoming_msg(&rx, &tx, fd, map, &info);

		if (tx.len > 0) {
			if (ioctl(fd, AMBA_VIRT_IOC_SEND, &tx) < 0) {
				if (errno != ENOTCONN && errno != ECONNRESET && errno != EPIPE) {
					perror("SEND");
				}
			}
		}
	}
	return 0;
}
