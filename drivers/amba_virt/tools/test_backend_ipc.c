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
				read(client_fd, name, hdr.len);
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_LOAD_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = 0,
				};
				write(client_fd, &resp, sizeof(resp));
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

int main(void)
{
	printf("=== Running Backend IPC Test Suite ===\n");
	pthread_t tid;
	pthread_create(&tid, NULL, mock_backend_thread, NULL);
	usleep(100000);

	test_token_auth_flow();

	g_mock_running = false;
	pthread_cancel(tid);
	pthread_join(tid, NULL);
	printf("=== All Backend IPC Tests Passed ===\n\n");
	return 0;
}
