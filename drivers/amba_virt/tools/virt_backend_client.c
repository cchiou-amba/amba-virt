/*
 * virt_backend_client.c
 *
 * Outbound client connection manager from amba-virt-server to amba-virt-backend.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "amba_virt.h"
#include "virt_backend_client.h"

static char g_host_ip[64] = "127.0.0.1";
static uint16_t g_port = 5556;
static char g_token_file[256] = "/persist/etc/amba-virt-backend.token";
static backend_state_change_cb g_state_cb = NULL;

static bool g_running = false;
static pthread_t g_client_thread;
static pthread_mutex_t g_rpc_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_sock_fd = -1;
static bool g_connected = false;
static uint32_t g_cached_mod_mask = 0;
static uint32_t g_seq = 1;

static void client_log(const char *evt_id, const char *fmt, ...)
{
	char time_str[64];
	struct timeval tv;
	struct tm tm_buf;
	va_list args;

	gettimeofday(&tv, NULL);
	gmtime_r(&tv.tv_sec, &tm_buf);
	snprintf(time_str, sizeof(time_str), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
		 tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
		 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, tv.tv_usec / 1000);

	printf("[%s] [%s] ", time_str, evt_id);
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	printf("\n");
	fflush(stdout);
}

static ssize_t client_safe_write(int fd, const void *buf, size_t count)
{
	size_t written = 0;
	while (written < count) {
		ssize_t n = send(fd, (const char *)buf + written, count - written, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		written += n;
	}
	return (ssize_t)written;
}

static ssize_t client_safe_read(int fd, void *buf, size_t count)
{
	size_t total = 0;
	while (total < count) {
		ssize_t n = read(fd, (char *)buf + total, count - total);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return total > 0 ? (ssize_t)total : -1;
		}
		total += n;
	}
	return (ssize_t)total;
}

static int read_token(char *token, size_t max_len)
{
	FILE *fp = fopen(g_token_file, "r");
	if (!fp) {
		/* Fallback token */
		strncpy(token, "amba_virt_secure_backend_token_default", max_len - 1);
		token[max_len - 1] = '\0';
		return 0;
	}

	if (fgets(token, max_len, fp)) {
		size_t len = strlen(token);
		while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r')) {
			token[len - 1] = '\0';
			len--;
		}
	}
	fclose(fp);
	return 0;
}

static int connect_and_auth(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	int nodelay = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_port);
	if (inet_pton(AF_INET, g_host_ip, &addr.sin_addr) <= 0) {
		close(fd);
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	char token[BACKEND_TOKEN_MAX_LEN + 1] = {0};
	read_token(token, sizeof(token));

	struct backend_msg_hdr auth_req = {
		.magic = BACKEND_MSG_MAGIC,
		.msg_type = BACKEND_MSG_AUTH_REQ,
		.seq = g_seq++,
		.len = (uint32_t)strlen(token),
		.status = 0,
	};

	if (client_safe_write(fd, &auth_req, sizeof(auth_req)) < 0 ||
	    client_safe_write(fd, token, strlen(token)) < 0) {
		close(fd);
		return -1;
	}

	struct backend_msg_hdr auth_resp;
	if (client_safe_read(fd, &auth_resp, sizeof(auth_resp)) < 0 ||
	    auth_resp.magic != BACKEND_MSG_MAGIC ||
	    auth_resp.msg_type != BACKEND_MSG_AUTH_RESP ||
	    auth_resp.status != 0) {
		client_log("EVT-003", "Backend authentication failed (status=%d)", auth_resp.status);
		close(fd);
		return -1;
	}

	client_log("EVT-001", "Backend connected and authenticated successfully (%s:%u)", g_host_ip, g_port);
	return fd;
}

static void *client_worker(void *arg)
{
	(void)arg;
	while (g_running) {
		pthread_mutex_lock(&g_rpc_mutex);
		int fd = g_sock_fd;
		pthread_mutex_unlock(&g_rpc_mutex);

		if (fd < 0) {
			fd = connect_and_auth();
			if (fd >= 0) {
				pthread_mutex_lock(&g_rpc_mutex);
				g_sock_fd = fd;
				g_connected = true;
				pthread_mutex_unlock(&g_rpc_mutex);

				/* Query full status immediately upon connect */
				uint32_t mask = 0;
				if (virt_backend_client_get_full_status(&mask) == 0) {
					g_cached_mod_mask = mask;
					client_log("EVT-004", "Reconciled host modules: mask=0x%08x", mask);
					if (g_state_cb)
						g_state_cb(mask);
				}
			} else {
				sleep(2);
				continue;
			}
		}

		/* Send periodic heartbeat */
		struct backend_msg_hdr hb_req = {
			.magic = BACKEND_MSG_MAGIC,
			.msg_type = BACKEND_OP_HEARTBEAT,
			.seq = g_seq++,
			.len = 0,
			.status = 0,
		};

		pthread_mutex_lock(&g_rpc_mutex);
		if (g_sock_fd >= 0) {
			struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
			setsockopt(g_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(g_sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

			if (client_safe_write(g_sock_fd, &hb_req, sizeof(hb_req)) < 0) {
				client_log("EVT-002", "Backend heartbeat send failed, disconnecting");
				close(g_sock_fd);
				g_sock_fd = -1;
				g_connected = false;
			} else {
				struct backend_msg_hdr hb_resp;
				if (client_safe_read(g_sock_fd, &hb_resp, sizeof(hb_resp)) == sizeof(hb_resp) &&
				    hb_resp.magic == BACKEND_MSG_MAGIC) {
					if (hb_resp.msg_type == BACKEND_OP_HEARTBEAT_ACK && hb_resp.len == sizeof(uint32_t)) {
						uint32_t mask = 0;
						client_safe_read(g_sock_fd, &mask, sizeof(mask));
						if (mask != g_cached_mod_mask) {
							client_log("EVT-012", "Module mask updated via heartbeat: 0x%08x -> 0x%08x",
								   g_cached_mod_mask, mask);
							g_cached_mod_mask = mask;
							if (g_state_cb)
								g_state_cb(mask);
						}
					} else if (hb_resp.msg_type == BACKEND_EVENT_MODULE_CHANGED && hb_resp.len == sizeof(uint32_t)) {
						uint32_t mask = 0;
						client_safe_read(g_sock_fd, &mask, sizeof(mask));
						client_log("EVT-012", "Asynchronous module state change event: mask=0x%08x", mask);
						g_cached_mod_mask = mask;
						if (g_state_cb)
							g_state_cb(mask);
					}
				} else {
					client_log("EVT-002", "Backend heartbeat response timeout/failed");
					close(g_sock_fd);
					g_sock_fd = -1;
					g_connected = false;
				}
			}
		}
		pthread_mutex_unlock(&g_rpc_mutex);

		sleep(5);
	}
	return NULL;
}

int virt_backend_client_init(const char *host_ip, uint16_t port, const char *token_file, backend_state_change_cb cb)
{
	if (host_ip && strlen(host_ip) > 0)
		strncpy(g_host_ip, host_ip, sizeof(g_host_ip) - 1);
	if (port)
		g_port = port;
	if (token_file && strlen(token_file) > 0)
		strncpy(g_token_file, token_file, sizeof(g_token_file) - 1);
	g_state_cb = cb;

	signal(SIGPIPE, SIG_IGN);
	g_running = true;
	if (pthread_create(&g_client_thread, NULL, client_worker, NULL) != 0) {
		g_running = false;
		return -1;
	}
	return 0;
}

void virt_backend_client_stop(void)
{
	g_running = false;
	pthread_mutex_lock(&g_rpc_mutex);
	if (g_sock_fd >= 0) {
		close(g_sock_fd);
		g_sock_fd = -1;
		g_connected = false;
	}
	pthread_mutex_unlock(&g_rpc_mutex);
	pthread_join(g_client_thread, NULL);
}

bool virt_backend_client_is_connected(void)
{
	return g_connected;
}

uint32_t virt_backend_client_get_mod_mask(void)
{
	return g_cached_mod_mask;
}

int virt_backend_client_get_full_status(uint32_t *mod_mask)
{
	pthread_mutex_lock(&g_rpc_mutex);
	if (g_sock_fd < 0) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -ENOTCONN;
	}

	struct backend_msg_hdr req = {
		.magic = BACKEND_MSG_MAGIC,
		.msg_type = BACKEND_OP_FULL_STATUS,
		.seq = g_seq++,
		.len = 0,
		.status = 0,
	};

	if (client_safe_write(g_sock_fd, &req, sizeof(req)) < 0) {
		close(g_sock_fd);
		g_sock_fd = -1;
		g_connected = false;
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	struct backend_msg_hdr resp;
	if (client_safe_read(g_sock_fd, &resp, sizeof(resp)) != sizeof(resp) ||
	    resp.magic != BACKEND_MSG_MAGIC || resp.status != 0) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	uint32_t mask = 0;
	if (resp.len == sizeof(uint32_t)) {
		client_safe_read(g_sock_fd, &mask, sizeof(mask));
	}
	if (mod_mask)
		*mod_mask = mask;

	pthread_mutex_unlock(&g_rpc_mutex);
	return 0;
}

int virt_backend_client_load_module(const char *module_name)
{
	if (!module_name)
		return -EINVAL;

	pthread_mutex_lock(&g_rpc_mutex);
	if (g_sock_fd < 0) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -ENOTCONN;
	}

	struct backend_msg_hdr req = {
		.magic = BACKEND_MSG_MAGIC,
		.msg_type = BACKEND_OP_MODULE_LOAD,
		.seq = g_seq++,
		.len = (uint32_t)strlen(module_name),
		.status = 0,
	};

	if (client_safe_write(g_sock_fd, &req, sizeof(req)) < 0 ||
	    client_safe_write(g_sock_fd, module_name, strlen(module_name)) < 0) {
		close(g_sock_fd);
		g_sock_fd = -1;
		g_connected = false;
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	struct backend_msg_hdr resp;
	if (client_safe_read(g_sock_fd, &resp, sizeof(resp)) != sizeof(resp) ||
	    resp.magic != BACKEND_MSG_MAGIC) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	pthread_mutex_unlock(&g_rpc_mutex);
	return resp.status;
}

int virt_backend_client_unload_module(const char *module_name)
{
	if (!module_name)
		return -EINVAL;

	pthread_mutex_lock(&g_rpc_mutex);
	if (g_sock_fd < 0) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -ENOTCONN;
	}

	struct backend_msg_hdr req = {
		.magic = BACKEND_MSG_MAGIC,
		.msg_type = BACKEND_OP_MODULE_UNLOAD,
		.seq = g_seq++,
		.len = (uint32_t)strlen(module_name),
		.status = 0,
	};

	if (client_safe_write(g_sock_fd, &req, sizeof(req)) < 0 ||
	    client_safe_write(g_sock_fd, module_name, strlen(module_name)) < 0) {
		close(g_sock_fd);
		g_sock_fd = -1;
		g_connected = false;
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	struct backend_msg_hdr resp;
	if (client_safe_read(g_sock_fd, &resp, sizeof(resp)) != sizeof(resp) ||
	    resp.magic != BACKEND_MSG_MAGIC) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	pthread_mutex_unlock(&g_rpc_mutex);
	return resp.status;
}

int virt_backend_client_hardware_reset(uint32_t dev_id)
{
	pthread_mutex_lock(&g_rpc_mutex);
	if (g_sock_fd < 0) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -ENOTCONN;
	}

	struct backend_msg_hdr req = {
		.magic = BACKEND_MSG_MAGIC,
		.msg_type = BACKEND_OP_HARDWARE_RESET,
		.seq = g_seq++,
		.len = sizeof(uint32_t),
		.status = 0,
	};

	if (client_safe_write(g_sock_fd, &req, sizeof(req)) < 0 ||
	    client_safe_write(g_sock_fd, &dev_id, sizeof(uint32_t)) < 0) {
		close(g_sock_fd);
		g_sock_fd = -1;
		g_connected = false;
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	struct backend_msg_hdr resp;
	if (client_safe_read(g_sock_fd, &resp, sizeof(resp)) != sizeof(resp) ||
	    resp.magic != BACKEND_MSG_MAGIC) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	pthread_mutex_unlock(&g_rpc_mutex);
	return resp.status;
}

int virt_backend_client_get_firmware(struct backend_firmware_resp *resp)
{
	if (!resp)
		return -EINVAL;

	pthread_mutex_lock(&g_rpc_mutex);
	if (g_sock_fd < 0) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -ENOTCONN;
	}

	struct backend_msg_hdr req = {
		.magic = BACKEND_MSG_MAGIC,
		.msg_type = BACKEND_OP_FIRMWARE_VERSIONS,
		.seq = g_seq++,
		.len = 0,
		.status = 0,
	};

	if (client_safe_write(g_sock_fd, &req, sizeof(req)) < 0) {
		close(g_sock_fd);
		g_sock_fd = -1;
		g_connected = false;
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	struct backend_msg_hdr r_hdr;
	if (client_safe_read(g_sock_fd, &r_hdr, sizeof(r_hdr)) != sizeof(r_hdr) ||
	    r_hdr.magic != BACKEND_MSG_MAGIC || r_hdr.status != 0) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	if (client_safe_read(g_sock_fd, resp, sizeof(*resp)) != sizeof(*resp)) {
		pthread_mutex_unlock(&g_rpc_mutex);
		return -EIO;
	}

	pthread_mutex_unlock(&g_rpc_mutex);
	return 0;
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
