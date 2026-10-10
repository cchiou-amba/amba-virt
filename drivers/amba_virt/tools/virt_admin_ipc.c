/*
 * virt_admin_ipc.c
 *
 * Local UNIX domain socket control plane for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "cavalry_proxy.h"
#include "virt_acl.h"
#include "virt_admin_ipc.h"
#include "virt_host_ops.h"
#include "virt_driver_matrix.h"
#include "virt_mem_pool.h"
#include "virt_query.h"

static pthread_t g_admin_thread;
static int g_admin_running = 0;
static int g_admin_server_fd = -1;
static char g_admin_sock_path[256];

__attribute__((weak)) int server_get_geometry_info(uint64_t *window_phys, uint64_t *window_bytes, uint32_t *slices, int *matched)
{
	if (window_phys) *window_phys = 0;
	if (window_bytes) *window_bytes = 0;
	if (slices) *slices = 0;
	if (matched) *matched = 0;
	return 0;
}

static void handle_client(int client_fd)
{
	char req_buf[VIRT_ADMIN_MAX_BUF];
	char resp_buf[VIRT_ADMIN_MAX_BUF];
	ssize_t n;

	memset(req_buf, 0, sizeof(req_buf));
	memset(resp_buf, 0, sizeof(resp_buf));

	n = read(client_fd, req_buf, sizeof(req_buf) - 1);
	if (n <= 0) {
		close(client_fd);
		return;
	}

	/* Trim trailing newline / carriage return */
	while (n > 0 && (req_buf[n - 1] == '\n' || req_buf[n - 1] == '\r')) {
		req_buf[n - 1] = '\0';
		n--;
	}

	char cmd[64] = {0};
	uint32_t cid = 0, val1 = 0, val2 = 0, val3 = 0;
	char str_arg[64] = {0};

	if (sscanf(req_buf, "%63s", cmd) != 1) {
		snprintf(resp_buf, sizeof(resp_buf), "ERR INVALID_CMD\n");
	} else if (strcmp(cmd, "STATUS") == 0) {
		uint64_t win_phys = 0, win_bytes = 0;
		uint32_t slices = 0;
		int matched = 0;
		int off = 0;

		server_get_geometry_info(&win_phys, &win_bytes, &slices, &matched);
		off += snprintf(resp_buf + off, sizeof(resp_buf) - off,
				"OK window_phys=0x%llx window_bytes=%llu slices=%u geometry=%s\n",
				(unsigned long long)win_phys,
				(unsigned long long)win_bytes,
				slices,
				matched ? "match" : "reject");

		struct cavalry_tenant_ctx tenants[MAX_TENANTS];
		int n_tenants = cavalry_proxy_get_all_tenants(tenants, MAX_TENANTS);
		for (int i = 0; i < n_tenants; i++) {
			off += snprintf(resp_buf + off, sizeof(resp_buf) - off,
					"BIND cid=%u index=%u offset=0x%llx phys=0x%llx usable=%zu\n",
					tenants[i].cid,
					tenants[i].tenant_idx,
					(unsigned long long)tenants[i].slice_offset,
					(unsigned long long)tenants[i].phys_base,
					tenants[i].shm_size);
			if ((size_t)off >= sizeof(resp_buf) - 128)
				break;
		}
	} else if (strcmp(cmd, "LIST_GUESTS") == 0) {
		struct virt_acl_entry entries[VIRT_ACL_MAX_RULES];
		int count = virt_acl_get_all_entries(entries, VIRT_ACL_MAX_RULES);
		int offset = 0;
		offset += snprintf(resp_buf + offset, sizeof(resp_buf) - offset, "OK COUNT %d\n", count);
		for (int i = 0; i < count; i++) {
			uint32_t q_max = 0, q_alloc = 0;
			virt_mem_pool_get_quota(entries[i].cid, &q_max, &q_alloc);
			offset += snprintf(resp_buf + offset, sizeof(resp_buf) - offset,
					   "CID %u NAME %s QUOTA %u ALLOC %u PRIO %u CAPS 0x%08x\n",
					   entries[i].cid, entries[i].tenant_name,
					   q_max ? (q_max / (1024 * 1024)) : entries[i].quota_mb,
					   q_alloc / (1024 * 1024),
					   entries[i].priority, entries[i].caps);
			if ((size_t)offset >= sizeof(resp_buf) - 128)
				break;
		}
	} else if (strcmp(cmd, "GET_GUEST") == 0) {
		if (sscanf(req_buf, "%*s %u", &cid) == 1) {
			struct virt_acl_entry acl;
			uint32_t q_max = 0, q_alloc = 0;
			uint32_t cav_base = 0, cav_size = 0, cav_arena = 0, cav_arena_sz = 0;
			uint32_t gdma_base = 0, gdma_size = 0;
			virt_acl_get_rule(cid, &acl);
			virt_mem_pool_get_quota(cid, &q_max, &q_alloc);
			virt_mem_pool_get_device_bounds(cid, AMBA_VIRT_DEV_TYPE_CAVALRY,
							&cav_base, &cav_size, &cav_arena, &cav_arena_sz);
			virt_mem_pool_get_device_bounds(cid, AMBA_VIRT_DEV_TYPE_GDMA,
							&gdma_base, &gdma_size, NULL, NULL);

			snprintf(resp_buf, sizeof(resp_buf),
				 "OK CID %u NAME %s QUOTA %u ALLOC %u PRIO %u CAPS 0x%08x "
				 "CAVALRY[base=0x%08x,size=%uMB,arena=0x%08x] GDMA[base=0x%08x,size=%uMB]\n",
				 cid, acl.tenant_name,
				 q_max ? (q_max / (1024 * 1024)) : acl.quota_mb,
				 q_alloc / (1024 * 1024),
				 acl.priority, acl.caps,
				 cav_base, cav_size / (1024 * 1024), cav_arena,
				 gdma_base, gdma_size / (1024 * 1024));
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR MISSING_CID\n");
		}
	} else if (strcmp(cmd, "SET_QUOTA") == 0) {
		if (sscanf(req_buf, "%*s %u %u", &cid, &val1) == 2) {
			int ret = virt_mem_pool_set_quota(cid, val1);
			if (ret == -ERANGE) {
				snprintf(resp_buf, sizeof(resp_buf), "ERR BAR_OVERFLOW (%d)\n", ret);
			} else if (ret < 0) {
				snprintf(resp_buf, sizeof(resp_buf), "ERR FAILED (%d)\n", ret);
			} else {
				struct virt_acl_entry acl;
				virt_acl_get_rule(cid, &acl);
				virt_acl_set_rule(cid, acl.tenant_name, acl.caps, val1, acl.priority);
				virt_acl_save_policies(NULL);
				snprintf(resp_buf, sizeof(resp_buf), "OK\n");
			}
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR USAGE: SET_QUOTA <cid> <mb>\n");
		}
	} else if (strcmp(cmd, "SET_ACL") == 0) {
		if (sscanf(req_buf, "%*s %u %x", &cid, &val1) == 2) {
			struct virt_acl_entry acl;
			virt_acl_get_rule(cid, &acl);
			virt_acl_set_rule(cid, acl.tenant_name, val1, acl.quota_mb, acl.priority);
			virt_acl_save_policies(NULL);
			snprintf(resp_buf, sizeof(resp_buf), "OK\n");
		} else if (sscanf(req_buf, "%*s %u %63s", &cid, str_arg) == 2) {
			uint32_t caps = AMBA_VIRT_ROLE_STANDARD;
			if (strcmp(str_arg, "untrusted") == 0)
				caps = AMBA_VIRT_ROLE_UNTRUSTED;
			else if (strcmp(str_arg, "standard") == 0)
				caps = AMBA_VIRT_ROLE_STANDARD;

			struct virt_acl_entry acl;
			virt_acl_get_rule(cid, &acl);
			virt_acl_set_rule(cid, acl.tenant_name, caps, acl.quota_mb, acl.priority);
			virt_acl_save_policies(NULL);
			snprintf(resp_buf, sizeof(resp_buf), "OK\n");
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR USAGE: SET_ACL <cid> <caps_hex|name>\n");
		}
	} else if (strcmp(cmd, "SET_PRIORITY") == 0) {
		if (sscanf(req_buf, "%*s %u %63s", &cid, str_arg) == 2) {
			uint32_t prio = VIRT_PRIORITY_NORMAL;
			if (strcmp(str_arg, "high") == 0 || strcmp(str_arg, "2") == 0)
				prio = VIRT_PRIORITY_HIGH;
			else if (strcmp(str_arg, "low") == 0 || strcmp(str_arg, "0") == 0)
				prio = VIRT_PRIORITY_LOW;

			struct virt_acl_entry acl;
			virt_acl_get_rule(cid, &acl);
			virt_acl_set_rule(cid, acl.tenant_name, acl.caps, acl.quota_mb, prio);
			virt_acl_save_policies(NULL);
			snprintf(resp_buf, sizeof(resp_buf), "OK\n");
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR USAGE: SET_PRIORITY <cid> <low|normal|high>\n");
		}
	} else if (strcmp(cmd, "SET_BOUNDS") == 0) {
		if (sscanf(req_buf, "%*s %u %u %x %u", &cid, &val1, &val2, &val3) == 4) {
			struct amba_virt_dev_bounds_req req;
			struct amba_virt_dev_bounds_resp resp;
			memset(&req, 0, sizeof(req));
			req.dev_type = val1;
			req.preferred_offset = val2;
			req.requested_size = val3 * 1024 * 1024;
			req.flags = AMBA_VIRT_DEV_F_EXACT | AMBA_VIRT_DEV_F_REPLACE;

			int ret = virt_mem_pool_set_device_bounds(cid, &req, &resp, NULL);
			if (ret == 0 && resp.status == 0) {
				if (req.dev_type == AMBA_VIRT_DEV_TYPE_CAVALRY) {
					cavalry_proxy_set_tenant_bounds(cid, resp.granted_offset,
									resp.granted_size,
									resp.rpc_arena_offset,
									req.rpc_arena_size);
				}
				snprintf(resp_buf, sizeof(resp_buf), "OK granted=0x%08x size=%u\n",
					 resp.granted_offset, resp.granted_size);
			} else {
				snprintf(resp_buf, sizeof(resp_buf), "ERR status=%d err=%u\n",
					 resp.status, resp.err_code);
			}
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR USAGE: SET_BOUNDS <cid> <dev_type> <offset_hex> <size_mb>\n");
		}
	} else if (strcmp(cmd, "EVICT") == 0) {
		if (sscanf(req_buf, "%*s %u", &cid) == 1) {
			/* Immediate quarantine & teardown */
			cavalry_proxy_client_disconnect(cid);
			virt_mem_pool_unregister_tenant(cid);
			virt_acl_remove_rule(cid);
			virt_acl_save_policies(NULL);
			snprintf(resp_buf, sizeof(resp_buf), "OK EVICTED %u\n", cid);
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR MISSING_CID\n");
		}
	} else if (strcmp(cmd, "RELOAD") == 0) {
		virt_acl_load_policies(NULL);
		snprintf(resp_buf, sizeof(resp_buf), "OK\n");
	} else if (strcmp(cmd, "HOST_STATUS") == 0) {
		uint32_t mask = virt_host_module_mask();
		snprintf(resp_buf, sizeof(resp_buf), "OK MASK=0x%08x\n", mask);
	} else if (strcmp(cmd, "MODULE_LOAD") == 0) {
		if (sscanf(req_buf, "%*s %63s", str_arg) == 1) {
			int ret = virt_host_module_load(str_arg, NULL);
			if (ret == 0) {
				snprintf(resp_buf, sizeof(resp_buf), "OK LOADED %s\n", str_arg);
			} else {
				snprintf(resp_buf, sizeof(resp_buf), "ERR LOAD_FAILED (%d)\n", ret);
			}
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR USAGE: MODULE_LOAD <name>\n");
		}
	} else if (strcmp(cmd, "MODULE_UNLOAD") == 0) {
		if (sscanf(req_buf, "%*s %63s", str_arg) == 1) {
			int is_cavalry = strstr(str_arg, "cavalry") != NULL;
			int drained = 0;

			if (is_cavalry) {
				cavalry_proxy_start_drain();
				drained = cavalry_proxy_wait_drained(200);
			}
			int ret = virt_host_module_unload_drained(str_arg, drained);
			if (is_cavalry)
				cavalry_proxy_finish_drain();
			if (ret == 0) {
				snprintf(resp_buf, sizeof(resp_buf), "OK UNLOADED %s\n", str_arg);
			} else {
				snprintf(resp_buf, sizeof(resp_buf), "ERR UNLOAD_FAILED (%d)\n", ret);
			}
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR USAGE: MODULE_UNLOAD <name>\n");
		}
	} else if (strcmp(cmd, "PIPELINE_START") == 0) {
		if (sscanf(req_buf, "%*s %63s", str_arg) == 1) {
			if (strcmp(str_arg, "npu") == 0) {
				virt_host_module_load("ambcma.ko", NULL);
				int ret = virt_host_module_load("cavalry.ko", NULL);
				snprintf(resp_buf, sizeof(resp_buf), ret == 0 ? "OK PIPELINE npu STARTED\n" : "ERR PIPELINE npu FAILED (%d)\n", ret);
			} else if (strcmp(str_arg, "camera") == 0) {
				virt_host_module_load("hw_timer.ko", NULL);
				virt_host_module_load("ambcma.ko", NULL);
				virt_host_module_load("msg.ko", NULL);
				virt_host_module_load("dsp.ko", NULL);
				virt_host_module_load("imgproc.ko", NULL);
				int ret = virt_host_module_load("iav.ko", NULL);
				snprintf(resp_buf, sizeof(resp_buf), ret == 0 ? "OK PIPELINE camera STARTED\n" : "ERR PIPELINE camera FAILED (%d)\n", ret);
			} else {
				snprintf(resp_buf, sizeof(resp_buf), "ERR UNKNOWN_PIPELINE %s\n", str_arg);
			}
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR USAGE: PIPELINE_START <npu|camera>\n");
		}
	} else if (strcmp(cmd, "FIRMWARE") == 0) {
		struct virt_host_firmware_resp fw;
		memset(&fw, 0, sizeof(fw));
		int ret = virt_host_get_firmware(&fw);
		if (ret == 0) {
			int offset = snprintf(resp_buf, sizeof(resp_buf), "OK COUNT %u\n", fw.count);
			for (uint32_t i = 0; i < fw.count; i++) {
				offset += snprintf(resp_buf + offset, sizeof(resp_buf) - offset,
						   "FW NAME %s SIZE %u SHA256 %s PRESENT %u\n",
						   fw.entries[i].name, fw.entries[i].size,
						   fw.entries[i].sha256, fw.entries[i].present);
				if ((size_t)offset >= sizeof(resp_buf) - 128)
					break;
			}
		} else {
			snprintf(resp_buf, sizeof(resp_buf), "ERR FIRMWARE_QUERY_FAILED (%d)\n", ret);
		}
	} else {
		snprintf(resp_buf, sizeof(resp_buf), "ERR UNKNOWN_CMD\n");
	}

	ssize_t written = write(client_fd, resp_buf, strlen(resp_buf));
	(void)written;
	close(client_fd);
}

static void *admin_thread_fn(void *arg)
{
	(void)arg;

	while (g_admin_running && g_admin_server_fd >= 0) {
		struct sockaddr_un client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int client_fd = accept(g_admin_server_fd, (struct sockaddr *)&client_addr, &addr_len);
		if (client_fd < 0) {
			if (!g_admin_running)
				break;
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			usleep(10000);
			continue;
		}
		handle_client(client_fd);
	}

	return NULL;
}

int virt_admin_ipc_start(const char *sock_path)
{
	struct sockaddr_un addr;
	struct timeval tv;
	char dir[256];
	char *slash;

	if (g_admin_running)
		return 0;

	if (!sock_path || strlen(sock_path) == 0)
		sock_path = VIRT_ADMIN_SOCK_PATH;

	snprintf(g_admin_sock_path, sizeof(g_admin_sock_path), "%s", sock_path);

	/* Ensure parent directory exists */
	snprintf(dir, sizeof(dir), "%s", sock_path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0755);
	}

	unlink(sock_path);

	g_admin_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_admin_server_fd < 0) {
		perror("admin socket");
		return -errno;
	}

	tv.tv_sec = 0;
	tv.tv_usec = 200000;
	setsockopt(g_admin_server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	if (bind(g_admin_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("admin bind");
		close(g_admin_server_fd);
		g_admin_server_fd = -1;
		return -errno;
	}

	/* Strictly enforce 0600 file permissions for root Dom0 access */
	chmod(sock_path, 0600);

	if (listen(g_admin_server_fd, 8) < 0) {
		perror("admin listen");
		close(g_admin_server_fd);
		g_admin_server_fd = -1;
		return -errno;
	}

	g_admin_running = 1;
	if (pthread_create(&g_admin_thread, NULL, admin_thread_fn, NULL) != 0) {
		perror("admin pthread_create");
		close(g_admin_server_fd);
		g_admin_server_fd = -1;
		g_admin_running = 0;
		return -errno;
	}

	printf("virt_admin_ipc: listening on %s (mode 0600)\n", sock_path);
	return 0;
}

void virt_admin_ipc_stop(void)
{
	if (!g_admin_running)
		return;

	g_admin_running = 0;
	if (g_admin_server_fd >= 0) {
		shutdown(g_admin_server_fd, SHUT_RDWR);
		close(g_admin_server_fd);
		g_admin_server_fd = -1;
	}
	pthread_join(g_admin_thread, NULL);
	unlink(g_admin_sock_path);
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
