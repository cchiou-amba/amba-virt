/*
 * test_admin_ipc.c
 *
 * Unit test for virt_admin_ipc and Dom0 control plane commands.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cavalry_proxy.h"
#include "virt_acl.h"
#include "virt_admin_ipc.h"
#include "virt_mem_pool.h"

#define TEST_SOCK "/tmp/test_admin.sock"

void cavalry_proxy_client_disconnect(uint32_t cid)
{
	(void)cid;
}

int cavalry_proxy_set_tenant_bounds(uint32_t cid, uint32_t base_offset,
				    uint32_t pool_size, uint32_t rpc_arena_offset,
				    uint32_t rpc_arena_size)
{
	(void)cid;
	(void)base_offset;
	(void)pool_size;
	(void)rpc_arena_offset;
	(void)rpc_arena_size;
	return 0;
}

static void send_cmd(const char *cmd, char *out, size_t out_len)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, TEST_SOCK, sizeof(addr.sun_path) - 1);

	int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	assert(ret == 0);

	ssize_t n = write(fd, cmd, strlen(cmd));
	assert(n == (ssize_t)strlen(cmd));

	memset(out, 0, out_len);
	n = read(fd, out, out_len - 1);
	assert(n > 0);
	close(fd);
}

int main(void)
{
	char resp[1024];

	printf("=== Starting virt_admin_ipc unit test suite ===\n");

	virt_mem_pool_init(1024 * 1024 * 1024);
	virt_acl_init();
	virt_admin_ipc_start(TEST_SOCK);

	/* 1. Test STATUS */
	send_cmd("STATUS\n", resp, sizeof(resp));
	assert(strncmp(resp, "OK proto=3", 10) == 0);
	printf("STATUS command: PASS\n");

	/* 2. Set rule for CID 15 and test LIST_GUESTS */
	virt_acl_set_rule(15, "ubuntu-test", AMBA_VIRT_ROLE_STANDARD, 1024, VIRT_PRIORITY_HIGH);
	send_cmd("LIST_GUESTS\n", resp, sizeof(resp));
	assert(strstr(resp, "CID 15") != NULL);
	assert(strstr(resp, "ubuntu-test") != NULL);
	printf("LIST_GUESTS command: PASS\n");

	/* 3. Test SET_QUOTA (Valid) */
	send_cmd("SET_QUOTA 15 512\n", resp, sizeof(resp));
	assert(strncmp(resp, "OK", 2) == 0);
	uint32_t q_max = 0, q_alloc = 0;
	virt_mem_pool_get_quota(15, &q_max, &q_alloc);
	assert(q_max == 512 * 1024 * 1024);
	printf("SET_QUOTA (512 MB) command: PASS\n");

	/* 4. Test SET_QUOTA exceeding 1 GiB BAR -> must return BAR_OVERFLOW */
	send_cmd("SET_QUOTA 15 1536\n", resp, sizeof(resp));
	assert(strstr(resp, "BAR_OVERFLOW") != NULL);
	printf("SET_QUOTA (1536 MB -> BAR_OVERFLOW) command: PASS\n");

	/* 5. Test SET_ACL */
	send_cmd("SET_ACL 15 untrusted\n", resp, sizeof(resp));
	assert(strncmp(resp, "OK", 2) == 0);
	assert(virt_acl_get_caps(15) == AMBA_VIRT_ROLE_UNTRUSTED);
	printf("SET_ACL (untrusted) command: PASS\n");

	/* 6. Test SET_PRIORITY */
	send_cmd("SET_PRIORITY 15 high\n", resp, sizeof(resp));
	assert(strncmp(resp, "OK", 2) == 0);
	printf("SET_PRIORITY (high) command: PASS\n");

	/* 7. Test EVICT */
	send_cmd("EVICT 15\n", resp, sizeof(resp));
	assert(strstr(resp, "OK EVICTED 15") != NULL);
	printf("EVICT command: PASS\n");

	virt_admin_ipc_stop();
	virt_acl_cleanup();
	virt_mem_pool_cleanup();

	printf("=== All virt_admin_ipc unit tests passed successfully! ===\n");
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
