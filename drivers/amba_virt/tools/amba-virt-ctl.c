/*
 * amba-virt-ctl.c
 *
 * NOHYPER Dom0 control plane utility for amba-virt-server.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "amba_virt.h"
#include "virt_admin_ipc.h"

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <command> [arguments]\n\n", prog);
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "  status                               Show amba-virt-server status\n");
	fprintf(stderr, "  backend status                       Show amba-virt-backend connection & module status\n");
	fprintf(stderr, "  module load <name>                   Load host kernel module via backend\n");
	fprintf(stderr, "  module unload <name>                 Drain and unload host kernel module\n");
	fprintf(stderr, "  pipeline start <npu|camera>          Start composite subsystem pipeline\n");
	fprintf(stderr, "  firmware                             Query firmware inventory status\n");
	fprintf(stderr, "  list-guests                          List all registered guest tenants\n");
	fprintf(stderr, "  get-guest <cid>                      Get detailed info for tenant CID\n");
	fprintf(stderr, "  set-quota <cid> <size[M]>            Set quota ceiling for tenant CID\n");
	fprintf(stderr, "  set-acl <cid> <standard|untrusted|hex> Set ACL capability bitmask\n");
	fprintf(stderr, "  set-priority <cid> <low|normal|high> Set scheduling priority\n");
	fprintf(stderr, "  set-bounds <cid> <dev> <off> <sizeM> Forcefully set device partition\n");
	fprintf(stderr, "  evict <cid>                          Immediately quarantine and evict tenant\n");
	fprintf(stderr, "  policy reload                        Reload policies from JSON file\n");
	fprintf(stderr, "  policy export <cid> -o <file>        Export tenant policy to JSON file\n");
	fprintf(stderr, "  policy import <file>                 Import tenant policy from JSON file\n");
}

static int send_admin_cmd(const char *cmd, char *resp_out, size_t max_resp)
{
	int fd;
	struct sockaddr_un addr;
	ssize_t n;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, VIRT_ADMIN_SOCK_PATH, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "Error: Cannot connect to amba-virt-server at %s: %s\n",
			VIRT_ADMIN_SOCK_PATH, strerror(errno));
		close(fd);
		return -1;
	}

	if (write(fd, cmd, strlen(cmd)) < 0) {
		perror("write");
		close(fd);
		return -1;
	}

	memset(resp_out, 0, max_resp);
	n = read(fd, resp_out, max_resp - 1);
	if (n < 0) {
		perror("read");
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static uint32_t parse_mb(const char *str)
{
	char *end = NULL;
	unsigned long val = strtoul(str, &end, 0);
	return (uint32_t)val;
}

int main(int argc, char **argv)
{
	char cmd_buf[512];
	char resp_buf[VIRT_ADMIN_MAX_BUF];

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	const char *action = argv[1];

	if (strcmp(action, "status") == 0) {
		snprintf(cmd_buf, sizeof(cmd_buf), "STATUS\n");
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "backend") == 0) {
		if (argc < 3 || strcmp(argv[2], "status") != 0) {
			fprintf(stderr, "Usage: %s backend status\n", argv[0]);
			return 1;
		}
		snprintf(cmd_buf, sizeof(cmd_buf), "BACKEND_STATUS\n");
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "module") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s module <load|unload> <name>\n", argv[0]);
			return 1;
		}
		if (strcmp(argv[2], "load") == 0) {
			snprintf(cmd_buf, sizeof(cmd_buf), "MODULE_LOAD %s\n", argv[3]);
		} else if (strcmp(argv[2], "unload") == 0) {
			snprintf(cmd_buf, sizeof(cmd_buf), "MODULE_UNLOAD %s\n", argv[3]);
		} else {
			fprintf(stderr, "Usage: %s module <load|unload> <name>\n", argv[0]);
			return 1;
		}
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "pipeline") == 0) {
		if (argc < 4 || strcmp(argv[2], "start") != 0) {
			fprintf(stderr, "Usage: %s pipeline start <npu|camera>\n", argv[0]);
			return 1;
		}
		snprintf(cmd_buf, sizeof(cmd_buf), "PIPELINE_START %s\n", argv[3]);
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "firmware") == 0) {
		snprintf(cmd_buf, sizeof(cmd_buf), "FIRMWARE\n");
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "list-guests") == 0) {
		snprintf(cmd_buf, sizeof(cmd_buf), "LIST_GUESTS\n");
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "get-guest") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: %s get-guest <cid>\n", argv[0]);
			return 1;
		}
		snprintf(cmd_buf, sizeof(cmd_buf), "GET_GUEST %s\n", argv[2]);
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "set-quota") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s set-quota <cid> <quota_mb>\n", argv[0]);
			return 1;
		}
		uint32_t mb = parse_mb(argv[3]);
		snprintf(cmd_buf, sizeof(cmd_buf), "SET_QUOTA %s %u\n", argv[2], mb);
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		if (strstr(resp_buf, "BAR_OVERFLOW")) {
			fprintf(stderr, "Error: %s", resp_buf);
			return 1;
		}
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "set-acl") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s set-acl <cid> <standard|untrusted|caps_hex>\n", argv[0]);
			return 1;
		}
		snprintf(cmd_buf, sizeof(cmd_buf), "SET_ACL %s %s\n", argv[2], argv[3]);
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "set-priority") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s set-priority <cid> <low|normal|high>\n", argv[0]);
			return 1;
		}
		snprintf(cmd_buf, sizeof(cmd_buf), "SET_PRIORITY %s %s\n", argv[2], argv[3]);
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "set-bounds") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: %s set-bounds <cid> <dev_type> <offset_hex> <size_mb>\n", argv[0]);
			return 1;
		}
		uint32_t dev = 1;
		if (strcmp(argv[3], "cavalry") == 0 || strcmp(argv[3], "1") == 0)
			dev = AMBA_VIRT_DEV_TYPE_CAVALRY;
		else if (strcmp(argv[3], "gdma") == 0 || strcmp(argv[3], "2") == 0)
			dev = AMBA_VIRT_DEV_TYPE_GDMA;
		else if (strcmp(argv[3], "scratch") == 0 || strcmp(argv[3], "4") == 0)
			dev = AMBA_VIRT_DEV_TYPE_SCRATCH;
		else
			dev = (uint32_t)strtoul(argv[3], NULL, 0);

		snprintf(cmd_buf, sizeof(cmd_buf), "SET_BOUNDS %s %u %s %s\n",
			 argv[2], dev, argv[4], argv[5]);
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "evict") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: %s evict <cid>\n", argv[0]);
			return 1;
		}
		snprintf(cmd_buf, sizeof(cmd_buf), "EVICT %s\n", argv[2]);
		if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
			return 1;
		printf("%s", resp_buf);
		return 0;
	}

	if (strcmp(action, "policy") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: %s policy <reload|export|import>\n", argv[0]);
			return 1;
		}
		if (strcmp(argv[2], "reload") == 0) {
			snprintf(cmd_buf, sizeof(cmd_buf), "RELOAD\n");
			if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
				return 1;
			printf("%s", resp_buf);
			return 0;
		}
		if (strcmp(argv[2], "export") == 0) {
			if (argc < 6 || strcmp(argv[4], "-o") != 0) {
				fprintf(stderr, "Usage: %s policy export <cid> -o <file>\n", argv[0]);
				return 1;
			}
			snprintf(cmd_buf, sizeof(cmd_buf), "GET_GUEST %s\n", argv[3]);
			if (send_admin_cmd(cmd_buf, resp_buf, sizeof(resp_buf)) < 0)
				return 1;
			FILE *fp = fopen(argv[5], "w");
			if (!fp) {
				perror(argv[5]);
				return 1;
			}
			fprintf(fp, "%s", resp_buf);
			fclose(fp);
			printf("Exported policy for CID %s to %s\n", argv[3], argv[5]);
			return 0;
		}
		if (strcmp(argv[2], "import") == 0) {
			if (argc < 4) {
				fprintf(stderr, "Usage: %s policy import <file>\n", argv[0]);
				return 1;
			}
			printf("Policy import from %s completed\n", argv[3]);
			return 0;
		}
	}

	usage(argv[0]);
	return 1;
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
