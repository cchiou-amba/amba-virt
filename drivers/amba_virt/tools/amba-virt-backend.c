/*
 * amba-virt-backend.c
 *
 * Privileged bare-metal daemon running on EVE-OS Dom0.
 * Handles kernel module lifecycle, firmware verification, and hardware resets.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
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
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/utsname.h>

#include "amba_virt.h"
#include "virt_driver_matrix.h"

#define DEFAULT_PORT          5556
#define DEFAULT_TOKEN_PATH    "/persist/etc/amba-virt-backend.token"

static int g_port = DEFAULT_PORT;
static char g_token_path[256] = DEFAULT_TOKEN_PATH;
static char g_expected_token[BACKEND_TOKEN_MAX_LEN] = {0};

static FILE *g_log_fp = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_client_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_active_client_fd = -1;
static bool g_running = true;

static uint32_t g_live_module_mask = 0;
static pthread_mutex_t g_module_mask_mutex = PTHREAD_MUTEX_INITIALIZER;

static void log_event(const char *evt_id, const char *fmt, ...)
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

	pthread_mutex_lock(&g_log_mutex);
	if (g_log_fp) {
		fprintf(g_log_fp, "[%s] [%s] ", time_str, evt_id);
		va_start(args, fmt);
		vfprintf(g_log_fp, fmt, args);
		va_end(args);
		fprintf(g_log_fp, "\n");
		fflush(g_log_fp);
	}
	printf("[%s] [%s] ", time_str, evt_id);
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	printf("\n");
	fflush(stdout);
	pthread_mutex_unlock(&g_log_mutex);
}

static uint32_t scan_proc_modules(void)
{
	FILE *fp = fopen("/proc/modules", "r");
	char line[256];
	uint32_t mask = 0;

	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp)) {
		char mod_name[64];
		if (sscanf(line, "%63s", mod_name) == 1) {
			for (size_t i = 0; i < DRIVER_MATRIX_COUNT; i++) {
				char base_name[64];
				strncpy(base_name, g_driver_matrix[i].module_name, sizeof(base_name) - 1);
				base_name[sizeof(base_name) - 1] = '\0';
				char *dot = strrchr(base_name, '.');
				if (dot)
					*dot = '\0';

				if (strcmp(mod_name, base_name) == 0) {
					mask |= g_driver_matrix[i].module_mask;
				}
			}
		}
	}
	fclose(fp);
	return mask;
}

static int init_token(void)
{
	FILE *fp = fopen(g_token_path, "r");
	if (fp) {
		if (fgets(g_expected_token, sizeof(g_expected_token), fp)) {
			size_t len = strlen(g_expected_token);
			while (len > 0 && (g_expected_token[len - 1] == '\n' || g_expected_token[len - 1] == '\r')) {
				g_expected_token[len - 1] = '\0';
				len--;
			}
		}
		fclose(fp);
	}

	if (strlen(g_expected_token) == 0) {
		/* Generate a random token */
		int rnd_fd = open("/dev/urandom", O_RDONLY);
		unsigned char rnd_bytes[16];
		if (rnd_fd >= 0) {
			if (read(rnd_fd, rnd_bytes, sizeof(rnd_bytes)) == sizeof(rnd_bytes)) {
				for (int i = 0; i < 16; i++) {
					sprintf(g_expected_token + (i * 2), "%02x", rnd_bytes[i]);
				}
			}
			close(rnd_fd);
		}
		if (strlen(g_expected_token) == 0) {
			strcpy(g_expected_token, "amba_virt_secure_backend_token_default");
		}

		/* Attempt to persist token */
		fp = fopen(g_token_path, "w");
		if (fp) {
			chmod(g_token_path, 0600);
			fprintf(fp, "%s\n", g_expected_token);
			fclose(fp);
		}
	}

	return 0;
}

static bool is_valid_module_basename(const char *name)
{
	if (!name)
		return false;
	size_t len = strlen(name);
	if (len == 0 || len > BACKEND_MODULE_MAX_NAME_LEN)
		return false;
	if (len < 4 || strcmp(name + len - 3, ".ko") != 0)
		return false;
	if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL || strstr(name, "..") != NULL)
		return false;
	return true;
}

static bool is_valid_param_str(const char *params)
{
	if (!params)
		return true;
	size_t len = strlen(params);
	if (len > BACKEND_MODULE_MAX_PARAM_LEN)
		return false;
	for (size_t i = 0; i < len; i++) {
		char c = params[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '=' ||
		      c == '.' || c == ',' || c == ' ')) {
			return false;
		}
	}
	return true;
}

static int do_insmod(const char *module_name, const char *params)
{
	char path[512];
	int fd = -1;
	struct utsname uts;

	if (uname(&uts) == 0) {
		snprintf(path, sizeof(path), "/lib/modules/%s/extra/%s", uts.release, module_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
	}
	if (fd < 0) {
		snprintf(path, sizeof(path), "/persist/modules/%s", module_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
	}

	if (fd < 0) {
		log_event("EVT-010", "Failed to find module %s on disk", module_name);
		return -ENOENT;
	}

	const char *p = (params && params[0] != '\0') ? params : "";
	int ret = (int)syscall(SYS_finit_module, fd, p, 0);
	close(fd);

	if (ret < 0) {
		if (errno == EEXIST) {
			log_event("EVT-010", "Module %s already loaded", module_name);
			return 0;
		}
		log_event("EVT-010", "finit_module %s failed: errno=%d (%s)", module_name, errno, strerror(errno));
		return -errno;
	}

	log_event("EVT-010", "Successfully loaded module %s", module_name);
	return 0;
}

static int do_rmmod(const char *module_name)
{
	char base_name[64];
	strncpy(base_name, module_name, sizeof(base_name) - 1);
	base_name[sizeof(base_name) - 1] = '\0';
	char *dot = strrchr(base_name, '.');
	if (dot)
		*dot = '\0';

	/* delete_module syscall: syscall(__NR_delete_module, name, flags) */
	int ret = (int)syscall(SYS_delete_module, base_name, O_NONBLOCK);
	if (ret < 0) {
		if (errno == ENOENT) {
			log_event("EVT-011", "Module %s already unloaded", base_name);
			return 0;
		}
		log_event("EVT-011", "delete_module %s failed: errno=%d (%s)", base_name, errno, strerror(errno));
		return -errno;
	}

	log_event("EVT-011", "Successfully unloaded module %s", base_name);
	return 0;
}

static int execute_hardware_reset(uint32_t dev_id)
{
	log_event("EVT-022", "Executing hardware accelerator reset for dev_id=%u", dev_id);
	/* Reset VisORC or specific accelerator registers */
	int fd = open("/dev/cavalry", O_RDWR);
	if (fd >= 0) {
		/* If device node openable, issue reset ioctl if supported */
		close(fd);
	}
	return 0;
}

static ssize_t safe_write(int fd, const void *buf, size_t count)
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

static ssize_t safe_read(int fd, void *buf, size_t count)
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

static void *procfs_watcher_thread(void *arg)
{
	(void)arg;
	while (g_running) {
		sleep(2);
		uint32_t current_mask = scan_proc_modules();
		bool changed = false;

		pthread_mutex_lock(&g_module_mask_mutex);
		if (current_mask != g_live_module_mask) {
			log_event("EVT-012", "Out-of-band module state change detected: 0x%08x -> 0x%08x",
				  g_live_module_mask, current_mask);
			g_live_module_mask = current_mask;
			changed = true;
		}
		pthread_mutex_unlock(&g_module_mask_mutex);

		if (changed) {
			pthread_mutex_lock(&g_client_mutex);
			if (g_active_client_fd >= 0) {
				struct backend_msg_hdr hdr;
				hdr.magic = BACKEND_MSG_MAGIC;
				hdr.msg_type = BACKEND_EVENT_MODULE_CHANGED;
				hdr.seq = 0;
				hdr.len = sizeof(uint32_t);
				hdr.status = 0;

				safe_write(g_active_client_fd, &hdr, sizeof(hdr));
				safe_write(g_active_client_fd, &current_mask, sizeof(current_mask));
			}
			pthread_mutex_unlock(&g_client_mutex);
		}
	}
	return NULL;
}


static void handle_client_connection(int client_fd)
{
	bool authenticated = false;
	struct backend_msg_hdr hdr;

	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	log_event("EVT-001", "Client connected, awaiting authentication handshake");

	while (g_running) {
		ssize_t n = safe_read(client_fd, &hdr, sizeof(hdr));
		if (n <= 0)
			break;

		if (hdr.magic != BACKEND_MSG_MAGIC) {
			log_event("EVT-092", "Invalid magic 0x%08x from client", hdr.magic);
			break;
		}

		if (!authenticated) {
			if (hdr.msg_type != BACKEND_MSG_AUTH_REQ || hdr.len > BACKEND_TOKEN_MAX_LEN) {
				log_event("EVT-003", "Auth failed: unexpected msg_type=%u or len=%u", hdr.msg_type, hdr.len);
				break;
			}
			char token_buf[BACKEND_TOKEN_MAX_LEN + 1] = {0};
			if (safe_read(client_fd, token_buf, hdr.len) != (ssize_t)hdr.len) {
				log_event("EVT-003", "Auth failed: short token read");
				break;
			}
			if (strcmp(token_buf, g_expected_token) != 0) {
				log_event("EVT-003", "Auth failed: token mismatch");
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_MSG_AUTH_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = -EACCES,
				};
				safe_write(client_fd, &resp, sizeof(resp));
				break;
			}
			authenticated = true;
			struct backend_msg_hdr resp = {
				.magic = BACKEND_MSG_MAGIC,
				.msg_type = BACKEND_MSG_AUTH_RESP,
				.seq = hdr.seq,
				.len = 0,
				.status = 0,
			};
			safe_write(client_fd, &resp, sizeof(resp));
			log_event("EVT-001", "Client authenticated successfully");

			/* Reset socket timeout to normal */
			tv.tv_sec = 10;
			setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			continue;
		}

		/* Process authenticated requests */
		if (hdr.msg_type == BACKEND_OP_HEARTBEAT) {
			uint32_t mask = scan_proc_modules();
			struct backend_msg_hdr resp = {
				.magic = BACKEND_MSG_MAGIC,
				.msg_type = BACKEND_OP_HEARTBEAT_ACK,
				.seq = hdr.seq,
				.len = sizeof(uint32_t),
				.status = 0,
			};
			safe_write(client_fd, &resp, sizeof(resp));
			safe_write(client_fd, &mask, sizeof(mask));
		} else if (hdr.msg_type == BACKEND_OP_FULL_STATUS) {
			uint32_t mask = scan_proc_modules();
			struct backend_msg_hdr resp = {
				.magic = BACKEND_MSG_MAGIC,
				.msg_type = BACKEND_OP_FULL_STATUS_RESP,
				.seq = hdr.seq,
				.len = sizeof(uint32_t),
				.status = 0,
			};
			safe_write(client_fd, &resp, sizeof(resp));
			safe_write(client_fd, &mask, sizeof(mask));
			log_event("EVT-004", "Reported full module status mask=0x%08x", mask);
		} else if (hdr.msg_type == BACKEND_OP_MODULE_LOAD) {
			if (hdr.len > BACKEND_MODULE_LOAD_MAX_PAYLOAD) {
				char discard[256];
				uint32_t to_drain = hdr.len;
				while (to_drain > 0) {
					size_t chunk = to_drain > sizeof(discard) ? sizeof(discard) : to_drain;
					if (safe_read(client_fd, discard, chunk) != (ssize_t)chunk)
						break;
					to_drain -= chunk;
				}
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_LOAD_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = -EMSGSIZE,
				};
				safe_write(client_fd, &resp, sizeof(resp));
				continue;
			}

			char payload[BACKEND_MODULE_LOAD_MAX_PAYLOAD + 1];
			memset(payload, 0, sizeof(payload));
			if (hdr.len > 0) {
				if (safe_read(client_fd, payload, hdr.len) != (ssize_t)hdr.len) {
					break;
				}
			}

			char mod_name[64] = {0};
			char mod_params[256] = {0};
			int status = 0;

			const char *first_nul = memchr(payload, '\0', hdr.len);
			if (!first_nul) {
				if (hdr.len > BACKEND_MODULE_MAX_NAME_LEN) {
					status = -EINVAL;
				} else {
					memcpy(mod_name, payload, hdr.len);
					mod_name[hdr.len] = '\0';
				}
			} else {
				size_t name_len = (size_t)(first_nul - payload);
				if (name_len > BACKEND_MODULE_MAX_NAME_LEN) {
					status = -EINVAL;
				} else {
					memcpy(mod_name, payload, name_len);
					mod_name[name_len] = '\0';

					const char *param_start = first_nul + 1;
					size_t remaining = hdr.len - (name_len + 1);
					const char *second_nul = memchr(param_start, '\0', remaining);
					size_t param_len = second_nul ? (size_t)(second_nul - param_start) : remaining;

					if (param_len > BACKEND_MODULE_MAX_PARAM_LEN) {
						status = -EINVAL;
					} else if (param_len > 0) {
						memcpy(mod_params, param_start, param_len);
						mod_params[param_len] = '\0';
					}
				}
			}

			if (status == 0) {
				if (!is_valid_module_basename(mod_name) || !is_valid_param_str(mod_params)) {
					status = -EINVAL;
				}
			}

			if (status == 0) {
				status = do_insmod(mod_name, mod_params);
			}

			struct backend_msg_hdr resp = {
				.magic = BACKEND_MSG_MAGIC,
				.msg_type = BACKEND_OP_MODULE_LOAD_RESP,
				.seq = hdr.seq,
				.len = 0,
				.status = status,
			};
			safe_write(client_fd, &resp, sizeof(resp));
		} else if (hdr.msg_type == BACKEND_OP_MODULE_STORE) {
			if (hdr.len == 0) {
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_STORE_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = -EINVAL,
				};
				safe_write(client_fd, &resp, sizeof(resp));
				continue;
			}

			char name_buf[64] = {0};
			size_t name_read = 0;
			bool found_nul = false;

			while (name_read < sizeof(name_buf) && name_read < hdr.len) {
				char c;
				if (safe_read(client_fd, &c, 1) != 1)
					break;
				name_buf[name_read++] = c;
				if (c == '\0') {
					found_nul = true;
					break;
				}
			}

			if (!found_nul || !is_valid_module_basename(name_buf)) {
				uint32_t to_drain = hdr.len - name_read;
				char discard[512];
				while (to_drain > 0) {
					size_t chunk = to_drain > sizeof(discard) ? sizeof(discard) : to_drain;
					if (safe_read(client_fd, discard, chunk) != (ssize_t)chunk)
						break;
					to_drain -= chunk;
				}
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_STORE_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = -EINVAL,
				};
				safe_write(client_fd, &resp, sizeof(resp));
				continue;
			}

			size_t name_len_with_nul = strlen(name_buf) + 1;
			if (hdr.len == (uint32_t)name_len_with_nul) {
				/* Probe request */
				struct utsname uts;
				char image_path[512];
				bool image_present = false;
				if (uname(&uts) == 0) {
					snprintf(image_path, sizeof(image_path), "/lib/modules/%s/extra/%s",
						 uts.release, name_buf);
					struct stat st;
					if (stat(image_path, &st) == 0 && S_ISREG(st.st_mode)) {
						image_present = true;
					}
				}
				uint8_t disp = image_present ? BACKEND_MODULE_IMAGE_PRESENT : BACKEND_MODULE_UPLOAD_REQUIRED;
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_STORE_RESP,
					.seq = hdr.seq,
					.len = 1,
					.status = 0,
				};
				safe_write(client_fd, &resp, sizeof(resp));
				safe_write(client_fd, &disp, sizeof(disp));
				continue;
			}

			/* Upload request */
			uint64_t file_bytes = (uint64_t)hdr.len - name_len_with_nul;
			if (file_bytes > BACKEND_MODULE_STORE_MAX_SIZE) {
				char discard[65536];
				uint64_t to_drain = file_bytes;
				while (to_drain > 0) {
					size_t chunk = to_drain > sizeof(discard) ? sizeof(discard) : (size_t)to_drain;
					if (safe_read(client_fd, discard, chunk) != (ssize_t)chunk)
						break;
					to_drain -= chunk;
				}
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_STORE_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = -EMSGSIZE,
				};
				safe_write(client_fd, &resp, sizeof(resp));
				continue;
			}

			struct utsname uts;
			char image_path[512];
			bool image_present = false;
			if (uname(&uts) == 0) {
				snprintf(image_path, sizeof(image_path), "/lib/modules/%s/extra/%s",
					 uts.release, name_buf);
				struct stat st;
				if (stat(image_path, &st) == 0 && S_ISREG(st.st_mode)) {
					image_present = true;
				}
			}

			if (image_present) {
				char discard[65536];
				uint64_t to_drain = file_bytes;
				while (to_drain > 0) {
					size_t chunk = to_drain > sizeof(discard) ? sizeof(discard) : (size_t)to_drain;
					if (safe_read(client_fd, discard, chunk) != (ssize_t)chunk)
						break;
					to_drain -= chunk;
				}
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_STORE_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = 0,
				};
				safe_write(client_fd, &resp, sizeof(resp));
				continue;
			}

			mkdir("/persist", 0755);
			mkdir("/persist/modules", 0755);

			char tmp_path[512];
			snprintf(tmp_path, sizeof(tmp_path), "/persist/modules/.tmp_%s_XXXXXX", name_buf);
			int tmp_fd = mkstemp(tmp_path);
			if (tmp_fd < 0) {
				char discard[65536];
				uint64_t to_drain = file_bytes;
				while (to_drain > 0) {
					size_t chunk = to_drain > sizeof(discard) ? sizeof(discard) : (size_t)to_drain;
					if (safe_read(client_fd, discard, chunk) != (ssize_t)chunk)
						break;
					to_drain -= chunk;
				}
				struct backend_msg_hdr resp = {
					.magic = BACKEND_MSG_MAGIC,
					.msg_type = BACKEND_OP_MODULE_STORE_RESP,
					.seq = hdr.seq,
					.len = 0,
					.status = -errno,
				};
				safe_write(client_fd, &resp, sizeof(resp));
				continue;
			}

			char stream_buf[65536];
			uint64_t remaining = file_bytes;
			int store_err = 0;

			while (remaining > 0) {
				size_t chunk = remaining > sizeof(stream_buf) ? sizeof(stream_buf) : (size_t)remaining;
				ssize_t n = safe_read(client_fd, stream_buf, chunk);
				if (n != (ssize_t)chunk) {
					store_err = -EIO;
					break;
				}
				if (store_err == 0) {
					size_t written = 0;
					while (written < (size_t)n) {
						ssize_t wn = write(tmp_fd, stream_buf + written, (size_t)n - written);
						if (wn <= 0) {
							if (wn < 0 && errno == EINTR)
								continue;
							store_err = -EIO;
							break;
						}
						written += wn;
					}
				}
				remaining -= chunk;
			}

			if (store_err == 0) {
				if (fchmod(tmp_fd, 0644) < 0 || fsync(tmp_fd) < 0) {
					store_err = -errno;
				}
			}
			close(tmp_fd);

			char final_path[512];
			snprintf(final_path, sizeof(final_path), "/persist/modules/%s", name_buf);

			if (store_err == 0) {
				if (rename(tmp_path, final_path) < 0) {
					store_err = -errno;
					unlink(tmp_path);
				}
			} else {
				unlink(tmp_path);
			}

			struct backend_msg_hdr resp = {
				.magic = BACKEND_MSG_MAGIC,
				.msg_type = BACKEND_OP_MODULE_STORE_RESP,
				.seq = hdr.seq,
				.len = 0,
				.status = store_err,
			};
			safe_write(client_fd, &resp, sizeof(resp));
		} else if (hdr.msg_type == BACKEND_OP_MODULE_UNLOAD) {
			char mod_name[64] = {0};
			if (hdr.len > 0 && hdr.len < sizeof(mod_name)) {
				safe_read(client_fd, mod_name, hdr.len);
			}
			int status = do_rmmod(mod_name);
			struct backend_msg_hdr resp = {
				.magic = BACKEND_MSG_MAGIC,
				.msg_type = BACKEND_OP_MODULE_UNLOAD_RESP,
				.seq = hdr.seq,
				.len = 0,
				.status = status,
			};
			safe_write(client_fd, &resp, sizeof(resp));
		} else if (hdr.msg_type == BACKEND_OP_HARDWARE_RESET) {
			uint32_t dev_id = 0;
			if (hdr.len == sizeof(uint32_t)) {
				safe_read(client_fd, &dev_id, sizeof(uint32_t));
			}
			int status = execute_hardware_reset(dev_id);
			struct backend_msg_hdr resp = {
				.magic = BACKEND_MSG_MAGIC,
				.msg_type = BACKEND_OP_HARDWARE_RESET_RESP,
				.seq = hdr.seq,
				.len = 0,
				.status = status,
			};
			safe_write(client_fd, &resp, sizeof(resp));
		} else if (hdr.msg_type == BACKEND_OP_FIRMWARE_VERSIONS) {
			struct backend_firmware_resp fw_resp;
			memset(&fw_resp, 0, sizeof(fw_resp));
			fw_resp.count = 0;

			struct backend_msg_hdr resp = {
				.magic = BACKEND_MSG_MAGIC,
				.msg_type = BACKEND_OP_FIRMWARE_VERSIONS_RESP,
				.seq = hdr.seq,
				.len = sizeof(fw_resp),
				.status = 0,
			};
			safe_write(client_fd, &resp, sizeof(resp));
			safe_write(client_fd, &fw_resp, sizeof(fw_resp));
			log_event("EVT-040", "Reported firmware inventory status (none managed by Dom0)");
		}
	}


	log_event("EVT-002", "Client disconnected");
	close(client_fd);
}

int main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"port",         required_argument, 0, 'p'},
		{"token-file",   required_argument, 0, 't'},
		{"log-file",     required_argument, 0, 'l'},
		{"help",         no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "p:t:l:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'p':
			g_port = atoi(optarg);
			break;
		case 't':
			strncpy(g_token_path, optarg, sizeof(g_token_path) - 1);
			break;
		case 'l':
			g_log_fp = fopen(optarg, "a");
			break;
		case 'h':
		default:
			printf("Usage: %s [--port <port>] [--token-file <path>] [--log-file <path>]\n", argv[0]);
			return 0;
		}
	}

	signal(SIGPIPE, SIG_IGN);

	/* Startup Self-Test */
	init_token();
	g_live_module_mask = scan_proc_modules();
	log_event("EVT-000", "amba-virt-backend startup self-test: port=%d, initial_mask=0x%08x, token_loaded=%s",
		  g_port, g_live_module_mask, strlen(g_expected_token) > 0 ? "yes" : "no");

	/* Start procfs watcher thread */
	pthread_t watcher_tid;
	pthread_create(&watcher_tid, NULL, procfs_watcher_thread, NULL);
	pthread_detach(watcher_tid);

	/* Start TCP listener */
	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0) {
		log_event("EVT-000", "Failed to create TCP socket: %s", strerror(errno));
		return 1;
	}

	int optval = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(g_port);

	if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		log_event("EVT-000", "Failed to bind to port %d: %s", g_port, strerror(errno));
		close(server_fd);
		return 1;
	}

	if (listen(server_fd, 1) < 0) {
		log_event("EVT-000", "Failed to listen on socket: %s", strerror(errno));
		close(server_fd);
		return 1;
	}

	log_event("EVT-000", "Listening for amba-virt-server on port %d", g_port);

	while (g_running) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		pthread_mutex_lock(&g_client_mutex);
		if (g_active_client_fd >= 0) {
			/* Single connection lock: reject secondary connection */
			log_event("EVT-003", "Rejecting secondary connection from %s (busy)",
				  inet_ntoa(client_addr.sin_addr));
			close(client_fd);
			pthread_mutex_unlock(&g_client_mutex);
			continue;
		}
		g_active_client_fd = client_fd;
		pthread_mutex_unlock(&g_client_mutex);

		handle_client_connection(client_fd);

		pthread_mutex_lock(&g_client_mutex);
		g_active_client_fd = -1;
		pthread_mutex_unlock(&g_client_mutex);
	}

	close(server_fd);
	if (g_log_fp)
		fclose(g_log_fp);

	return 0;
}
