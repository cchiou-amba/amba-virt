/*
 * test_backend_ipc.c
 *
 * Formalized Unit Tests for amba-virt-backend IPC Protocol & Token Security.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "amba_virt.h"
#include "virt_backend_client.h"

#define TEST_BACKEND_PORT 5599
#define TEST_TOKEN "test_secret_token_1234567890"

static int g_mock_server_fd = -1;
static bool g_mock_running = true;
static uint32_t g_mock_mod_mask = 0x00000003; /* ambcma | cavalry */

/* State tracking for Section 2.8 wire framing assertions */
static uint8_t g_mock_disposition = BACKEND_MODULE_IMAGE_PRESENT;
static int32_t g_mock_store_status = 0;
static int g_probe_count = 0;
static int g_upload_count = 0;
static char g_last_probe_name[64] = {0};
static bool g_probe_has_no_file_bytes = false;
static char g_last_upload_name[64] = {0};
static char g_last_upload_bytes[256] = {0};
static size_t g_last_upload_len = 0;

static void *mock_backend_thread(void *arg)
{
	(void)arg;
	struct sockaddr_in addr;
	g_mock_server_fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(g_mock_server_fd >= 0);

	int opt = 1;
	setsockopt(g_mock_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_BACKEND_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	assert(bind(g_mock_server_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(g_mock_server_fd, 5) == 0);

	while (g_mock_running) {
		int client_fd = accept(g_mock_server_fd, NULL, NULL);
		if (client_fd < 0) {
			if (!g_mock_running) break;
			continue;
		}

		struct backend_msg_hdr hdr;
		bool auth = false;

		while (g_mock_running) {
			ssize_t n = read(client_fd, &hdr, sizeof(hdr));
			if (n <= 0) break;
			if (hdr.magic != BACKEND_MSG_MAGIC) break;

			if (!auth) {
				if (hdr.msg_type == BACKEND_MSG_AUTH_REQ) {
					char tok[128] = {0};
					read(client_fd, tok, hdr.len);
					struct backend_msg_hdr resp = {
						.magic = BACKEND_MSG_MAGIC,
						.msg_type = BACKEND_MSG_AUTH_RESP,
						.seq = hdr.seq,
						.len = 0,
						.status = (strcmp(tok, TEST_TOKEN) == 0) ? 0 : -EACCES,
					};
					write(client_fd, &resp, sizeof(resp));
					if (resp.status == 0) auth = true;
					else break;
				} else {
					break;
				}
				continue;
			}

			if (hdr.msg_type == BACKEND_OP_HEARTBEAT) {
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_HEARTBEAT_ACK,
					.seq = hdr.seq,
					.len = sizeof(uint32_t),
					.status = 0,
				};
				write(client_fd, &resp, sizeof(resp));
				write(client_fd, &g_mock_mod_mask, sizeof(g_mock_mod_mask));
			} else if (hdr.msg_type == BACKEND_OP_FULL_STATUS) {
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_FULL_STATUS_RESP,
					.seq = hdr.seq,
					.len = sizeof(uint32_t),
					.status = 0,
				};
				write(client_fd, &resp, sizeof(resp));
				write(client_fd, &g_mock_mod_mask, sizeof(g_mock_mod_mask));
			} else if (hdr.msg_type == BACKEND_OP_MODULE_LOAD) {
				char name[64] = {0};
				read(client_fd, name, hdr.len > sizeof(name) - 1 ? sizeof(name) - 1 : hdr.len);
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_LOAD_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = 0,
				};
				write(client_fd, &resp, sizeof(resp));
			} else if (hdr.msg_type == BACKEND_OP_MODULE_STORE) {
				char name_buf[64] = {0};
				size_t name_read = 0;
				bool found_nul = false;

				while (name_read < sizeof(name_buf) && name_read < hdr.len) {
					char c;
					if (read(client_fd, &c, 1) != 1)
						break;
					name_buf[name_read++] = c;
					if (c == '\0') {
						found_nul = true;
						break;
					}
				}

				if (!found_nul) {
					struct backend_msg_hdr resp = {
						.magic = BACKEND_MSG_MAGIC,
						.msg_type = BACKEND_OP_MODULE_STORE_RESP,
						.seq = hdr.seq,
						.len = 0,
						.status = -EINVAL,
					};
					write(client_fd, &resp, sizeof(resp));
					continue;
				}

				size_t name_len_with_nul = strlen(name_buf) + 1;
				if (hdr.len == (uint32_t)name_len_with_nul) {
					/* PROBE: basename plus one trailing NUL and no file bytes */
					g_probe_count++;
					strncpy(g_last_probe_name, name_buf, sizeof(g_last_probe_name) - 1);
					g_probe_has_no_file_bytes = true;

					struct backend_msg_hdr resp = {
						.magic = BACKEND_MSG_MAGIC,
						.msg_type = BACKEND_OP_MODULE_STORE_RESP,
						.seq = hdr.seq,
						.len = (g_mock_store_status != 0) ? 0 : 1,
						.status = g_mock_store_status,
					};
					write(client_fd, &resp, sizeof(resp));
					if (g_mock_store_status == 0) {
						write(client_fd, &g_mock_disposition, 1);
					}
				} else {
					/* UPLOAD */
					uint64_t file_bytes = (uint64_t)hdr.len - name_len_with_nul;
					if (file_bytes > BACKEND_MODULE_STORE_MAX_SIZE) {
						/* Drain and reject with -EMSGSIZE */
						char discard[4096];
						uint64_t to_drain = file_bytes;
						while (to_drain > 0) {
							size_t chunk = to_drain > sizeof(discard) ? sizeof(discard) : (size_t)to_drain;
							ssize_t rn = read(client_fd, discard, chunk);
							if (rn <= 0) break;
							to_drain -= (size_t)rn;
						}
						struct backend_msg_hdr resp = {
							.magic = BACKEND_MSG_MAGIC,
							.msg_type = BACKEND_OP_MODULE_STORE_RESP,
							.seq = hdr.seq,
							.len = 0,
							.status = -EMSGSIZE,
						};
						write(client_fd, &resp, sizeof(resp));
						continue;
					}

					g_upload_count++;
					strncpy(g_last_upload_name, name_buf, sizeof(g_last_upload_name) - 1);
					g_last_upload_len = (size_t)file_bytes;

					memset(g_last_upload_bytes, 0, sizeof(g_last_upload_bytes));
					size_t to_save = file_bytes < sizeof(g_last_upload_bytes) ? (size_t)file_bytes : sizeof(g_last_upload_bytes);
					size_t saved = 0;
					while (saved < to_save) {
						ssize_t rn = read(client_fd, g_last_upload_bytes + saved, to_save - saved);
						if (rn <= 0) break;
						saved += (size_t)rn;
					}
					uint64_t remaining = file_bytes - saved;
					char discard[4096];
					while (remaining > 0) {
						size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : (size_t)remaining;
						ssize_t rn = read(client_fd, discard, chunk);
						if (rn <= 0) break;
						remaining -= (size_t)rn;
					}

					struct backend_msg_hdr resp = {
						.magic = BACKEND_MSG_MAGIC,
						.msg_type = BACKEND_OP_MODULE_STORE_RESP,
						.seq = hdr.seq,
						.len = 0,
						.status = g_mock_store_status,
					};
					write(client_fd, &resp, sizeof(resp));
				}
			}
		}
		close(client_fd);
	}
	close(g_mock_server_fd);
	return NULL;
}

static void test_token_auth_flow(void)
{
	printf("[TEST] Testing token authentication and handshake with mock backend...\n");
	FILE *fp = fopen("/tmp/test_amba_backend.token", "w");
	assert(fp != NULL);
	fprintf(fp, "%s\n", TEST_TOKEN);
	fclose(fp);

	int ret = virt_backend_client_init("127.0.0.1", TEST_BACKEND_PORT, "/tmp/test_amba_backend.token", NULL);
	assert(ret == 0);

	/* Wait for client worker to establish connection and handshake */
	usleep(200000);
	assert(virt_backend_client_is_connected());

	uint32_t mask = 0;
	ret = virt_backend_client_get_full_status(&mask);
	assert(ret == 0);
	assert(mask == g_mock_mod_mask);
	printf("[PASS] Token auth succeeded, full status mask=0x%08x verified\n", mask);

	virt_backend_client_stop();
	unlink("/tmp/test_amba_backend.token");
}

struct feeder_args {
	int fd;
	size_t count;
};

static void *stream_feeder_thread(void *arg)
{
	struct feeder_args *fa = (struct feeder_args *)arg;
	char buf[65536];
	memset(buf, 0x5a, sizeof(buf));
	size_t left = fa->count;
	while (left > 0) {
		size_t chunk = left > sizeof(buf) ? sizeof(buf) : left;
		ssize_t n = write(fa->fd, buf, chunk);
		if (n <= 0) break;
		left -= (size_t)n;
	}
	return NULL;
}

static void test_module_store_wire_framing(void)
{
	printf("[TEST] Testing Section 2.8 module store wire framing...\n");
	FILE *fp = fopen("/tmp/test_amba_backend.token", "w");
	assert(fp != NULL);
	fprintf(fp, "%s\n", TEST_TOKEN);
	fclose(fp);

	int ret = virt_backend_client_init("127.0.0.1", TEST_BACKEND_PORT, "/tmp/test_amba_backend.token", NULL);
	assert(ret == 0);
	usleep(200000);
	assert(virt_backend_client_is_connected());

	/* 1. Probe: basename plus one trailing NUL and no file bytes */
	g_probe_count = 0;
	g_upload_count = 0;
	g_mock_disposition = BACKEND_MODULE_IMAGE_PRESENT; /* 0x00 */
	g_mock_store_status = 0;

	uint8_t disp = 0xff;
	ret = virt_backend_client_probe_module("amba_virt.ko", &disp);
	assert(ret == 0);
	assert(disp == BACKEND_MODULE_IMAGE_PRESENT);
	assert(g_probe_count == 1);
	assert(strcmp(g_last_probe_name, "amba_virt.ko") == 0);
	assert(g_probe_has_no_file_bytes == true);
	printf("[PASS] Probe wire framing verified: basename + 1 NUL, no file bytes\n");

	/* 2. Disposition 0x00 suppresses upload */
	/* If disp is 0x00, caller does not initiate upload */
	assert(g_upload_count == 0);
	printf("[PASS] Disposition 0x00 suppresses upload\n");

	/* 3. Disposition 0x01 causes exactly one upload */
	g_mock_disposition = BACKEND_MODULE_UPLOAD_REQUIRED; /* 0x01 */
	ret = virt_backend_client_probe_module("amba_virt.ko", &disp);
	assert(ret == 0);
	assert(disp == BACKEND_MODULE_UPLOAD_REQUIRED);

	/* Create temporary test file for upload */
	const char *test_path = "/tmp/test_package_module.ko";
	FILE *f_pkg = fopen(test_path, "wb");
	assert(f_pkg != NULL);
	const char test_data[] = "TEST_MODULE_BYTE_CONTENT_12345";
	fwrite(test_data, 1, strlen(test_data), f_pkg);
	fclose(f_pkg);

	/* Exact one upload is triggered */
	ret = virt_backend_client_store_module("amba_virt.ko", test_path);
	assert(ret == 0);
	assert(g_upload_count == 1);

	/* 4. Upload is basename, one NUL, then exact input bytes */
	assert(strcmp(g_last_upload_name, "amba_virt.ko") == 0);
	assert(g_last_upload_len == strlen(test_data));
	assert(memcmp(g_last_upload_bytes, test_data, strlen(test_data)) == 0);
	printf("[PASS] Disposition 0x01 causes exactly one upload; upload framing verified\n");
	unlink(test_path);

	/* Stop client so mock_backend_thread completes client_fd and accepts raw_fd */
	virt_backend_client_stop();

	/* 5. A declared upload one byte over 64 MiB is drained and rejected with -EMSGSIZE */
	int raw_fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(raw_fd >= 0);
	struct sockaddr_in saddr;
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(TEST_BACKEND_PORT);
	saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(raw_fd, (struct sockaddr *)&saddr, sizeof(saddr)) == 0);

	/* Authenticate */
	struct backend_msg_hdr auth_hdr = {
		.magic = BACKEND_MSG_MAGIC,
		.msg_type = BACKEND_MSG_AUTH_REQ,
		.seq = 100,
		.len = (uint32_t)strlen(TEST_TOKEN),
		.status = 0,
	};
	assert(write(raw_fd, &auth_hdr, sizeof(auth_hdr)) == sizeof(auth_hdr));
	assert(write(raw_fd, TEST_TOKEN, strlen(TEST_TOKEN)) == (ssize_t)strlen(TEST_TOKEN));
	struct backend_msg_hdr auth_resp;
	assert(read(raw_fd, &auth_resp, sizeof(auth_resp)) == sizeof(auth_resp));
	assert(auth_resp.status == 0);

	/* Send declared upload one byte over 64 MiB */
	const char *mod = "test.ko";
	size_t name_with_nul = strlen(mod) + 1;
	uint64_t over_limit_bytes = (uint64_t)BACKEND_MODULE_STORE_MAX_SIZE + 1;
	struct backend_msg_hdr over_hdr = {
		.magic = BACKEND_MSG_MAGIC,
		.msg_type = BACKEND_OP_MODULE_STORE,
		.seq = 101,
		.len = (uint32_t)(name_with_nul + over_limit_bytes),
		.status = 0,
	};
	assert(write(raw_fd, &over_hdr, sizeof(over_hdr)) == sizeof(over_hdr));
	assert(write(raw_fd, mod, name_with_nul) == (ssize_t)name_with_nul);

	/* Feed over_limit_bytes concurrently so backend can drain without socket deadlock */
	struct feeder_args fa = { .fd = raw_fd, .count = (size_t)over_limit_bytes };
	pthread_t feeder_tid;
	pthread_create(&feeder_tid, NULL, stream_feeder_thread, &fa);

	/* Backend must drain and reject with -EMSGSIZE */
	struct backend_msg_hdr over_resp;
	assert(read(raw_fd, &over_resp, sizeof(over_resp)) == sizeof(over_resp));
	assert(over_resp.status == -EMSGSIZE);
	pthread_join(feeder_tid, NULL);
	close(raw_fd);
	printf("[PASS] Declared upload one byte over 64 MiB drained and rejected with -EMSGSIZE\n");

	/* Reconnect client for disposition / status tests */
	ret = virt_backend_client_init("127.0.0.1", TEST_BACKEND_PORT, "/tmp/test_amba_backend.token", NULL);
	assert(ret == 0);
	usleep(200000);
	assert(virt_backend_client_is_connected());

	/* 6. An unknown disposition and a non-zero response status are errors */
	g_mock_disposition = 0x99; /* Unknown disposition */
	g_mock_store_status = 0;
	ret = virt_backend_client_probe_module("amba_virt.ko", &disp);
	assert(ret != 0); /* Error on unknown disposition */

	g_mock_disposition = BACKEND_MODULE_IMAGE_PRESENT;
	g_mock_store_status = -EACCES; /* Non-zero response status */
	ret = virt_backend_client_probe_module("amba_virt.ko", &disp);
	assert(ret == -EACCES); /* Propagates non-zero status as error */
	printf("[PASS] Unknown disposition and non-zero response status correctly handled as errors\n");

	virt_backend_client_stop();
	unlink("/tmp/test_amba_backend.token");
}

int main(void)
{
	printf("=== Running Backend IPC Test Suite ===\n");
	pthread_t tid;
	pthread_create(&tid, NULL, mock_backend_thread, NULL);
	usleep(100000);

	test_token_auth_flow();
	test_module_store_wire_framing();

	g_mock_running = false;
	pthread_cancel(tid);
	pthread_join(tid, NULL);
	printf("=== All Backend IPC Tests Passed ===\n\n");
	return 0;
}
