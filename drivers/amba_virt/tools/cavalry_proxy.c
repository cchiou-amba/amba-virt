/*
 * cavalry_proxy.c
 *
 * Class D Cavalry Host Virtualization Proxy
 *
 * Handles client session slices, AMA command graph rebuilding, and
 * CAVALRY_RUN_DAGS_MEMFD dispatch over the exported ivshmem DMA-BUF window.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "amba_virt.h"
#include "cavalry_proxy.h"
#include "cavalry_ioctl.h"

#define MAX_SLICES               512
#define MAX_DAG_CNT_CAP          128
#define CAVALRY_DEV_NODE         "/dev/cavalry"

struct cavalry_slice_entry {
	uint32_t bar_offset;
	uint32_t size;
	uint32_t client_cid;
	uint32_t session_id;
	int in_use;
};

#define MAX_HANDLES              1024
#define MAX_REGISTERED_DAGS      128

struct cavalry_handle_entry {
	uint32_t handle_id;
	uint32_t bar_offset;
	uint32_t size;
	uint32_t client_cid;
	uint32_t session_id;
	int in_use;
};

struct cavalry_registered_dag {
	uint32_t dag_id;
	uint32_t client_cid;
	uint32_t session_id;
	uint32_t host_mem_size;
	unsigned long host_mem_phys;
	void *host_mem_virt;
	uint32_t dag_cnt;
	int32_t nid;
	uint32_t affinity;
	uint8_t priority;
	uint8_t hw_type;
	uint32_t is_encrypt : 1;
	uint32_t is_resume : 1;
	uint32_t no_auto_resume : 1;
	uint32_t sub_session_id : 8;
	uint32_t enc_session_id;
	struct cavalry_dag_desc_mfd template_dag[MAX_DAG_CNT_CAP];
	uint8_t sha256[CAVALRY_SHA256_LEN];
	int in_use;
};

static int g_fd_cav = -1;
static int g_fd_shm = -1;
static int g_window_fd = -1;
static unsigned char *g_shm_map = MAP_FAILED;
static size_t g_shm_size = 0;
static uint64_t g_phys_base = 0;
static uint32_t g_chip_id = 0;

static pthread_mutex_t g_visorc_hw_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tenant_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct cavalry_tenant_ctx g_tenants[MAX_TENANTS];
static int g_enforce_path_b = 0;

static struct cavalry_slice_entry g_slices[MAX_SLICES];
static pthread_mutex_t g_slice_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct cavalry_handle_entry g_handles[MAX_HANDLES];
static pthread_mutex_t g_handle_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_handle_id = 1;

static struct cavalry_registered_dag g_registered_dags[MAX_REGISTERED_DAGS];
static pthread_mutex_t g_dag_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_dag_id = 1;

static pthread_mutex_t g_drain_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_drain_cond = PTHREAD_COND_INITIALIZER;
static int g_draining = 0;
static int g_active_requests = 0;

void cavalry_proxy_set_enforce_path_b(int enforce)

{
	g_enforce_path_b = enforce;
}

int cavalry_proxy_get_enforce_path_b(void)
{
	return g_enforce_path_b;
}

int cavalry_proxy_register_tenant(uint32_t cid, uint32_t tenant_idx,
				  int window_fd, unsigned char *shm_map,
				  size_t shm_size, uint64_t phys_base,
				  uint32_t slice_offset)
{
	if (tenant_idx >= MAX_TENANTS)
		return -EINVAL;

	pthread_mutex_lock(&g_tenant_mutex);
	g_tenants[tenant_idx].cid = cid;
	g_tenants[tenant_idx].tenant_idx = tenant_idx;
	g_tenants[tenant_idx].window_fd = window_fd >= 0 ? window_fd : g_window_fd;
	g_tenants[tenant_idx].shm_map = shm_map;
	g_tenants[tenant_idx].shm_size = shm_size;
	g_tenants[tenant_idx].phys_base = phys_base;
	g_tenants[tenant_idx].slice_offset = slice_offset;
	g_tenants[tenant_idx].cavalry_pool_base = CAVALRY_POOL_BASE;
	g_tenants[tenant_idx].cavalry_pool_size = CAVALRY_POOL_SIZE;
	g_tenants[tenant_idx].rpc_arena_offset = CAVALRY_RPC_ARENA_OFFSET;
	g_tenants[tenant_idx].rpc_arena_size = CAVALRY_RPC_ARENA_SIZE;
	g_tenants[tenant_idx].in_use = 1;
	pthread_mutex_unlock(&g_tenant_mutex);

	printf("cavalry_proxy: registered tenant idx=%u cid=%u (phys=0x%lx, off=0x%x, size=%zu)\n",
	       tenant_idx, cid, (unsigned long)phys_base, slice_offset, shm_size);
	return 0;
}

int cavalry_proxy_unregister_tenant(uint32_t cid)
{
	int i;
	pthread_mutex_lock(&g_tenant_mutex);
	for (i = 0; i < MAX_TENANTS; i++) {
		if (g_tenants[i].in_use && g_tenants[i].cid == cid) {
			g_tenants[i].in_use = 0;
			pthread_mutex_unlock(&g_tenant_mutex);
			cavalry_proxy_client_disconnect(cid);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_tenant_mutex);
	return -ENOENT;
}

struct cavalry_tenant_ctx *cavalry_proxy_get_tenant(uint32_t cid)
{
	int i;
	pthread_mutex_lock(&g_tenant_mutex);
	for (i = 0; i < MAX_TENANTS; i++) {
		if (g_tenants[i].in_use && g_tenants[i].cid == cid) {
			pthread_mutex_unlock(&g_tenant_mutex);
			return &g_tenants[i];
		}
	}

	/* Dynamic auto-registration for connecting guest */
	for (i = 0; i < MAX_TENANTS; i++) {
		if (!g_tenants[i].in_use) {
			uint32_t slice_sz = 0x40000000U; /* 1 GiB */
			uint32_t slice_off = i * slice_sz;
			g_tenants[i].cid = cid;
			g_tenants[i].tenant_idx = i;
			g_tenants[i].window_fd = g_window_fd;
			g_tenants[i].shm_map = (g_shm_map != MAP_FAILED && slice_off < g_shm_size) ?
						(g_shm_map + slice_off) : g_shm_map;
			g_tenants[i].shm_size = slice_sz;
			g_tenants[i].phys_base = g_phys_base + slice_off;
			g_tenants[i].slice_offset = slice_off;
			g_tenants[i].cavalry_pool_base = CAVALRY_POOL_BASE;
			g_tenants[i].cavalry_pool_size = CAVALRY_POOL_SIZE;
			g_tenants[i].rpc_arena_offset = CAVALRY_RPC_ARENA_OFFSET;
			g_tenants[i].rpc_arena_size = CAVALRY_RPC_ARENA_SIZE;
			g_tenants[i].in_use = 1;
			printf("cavalry_proxy: auto-registered tenant slot %d for cid=%u (slice_off=0x%08x, phys=0x%lx)\n",
			       i, cid, slice_off, (unsigned long)g_tenants[i].phys_base);
			pthread_mutex_unlock(&g_tenant_mutex);
			return &g_tenants[i];
		}
	}

	pthread_mutex_unlock(&g_tenant_mutex);
	return NULL;
}

int cavalry_proxy_set_tenant_bounds(uint32_t cid, uint32_t pool_base,
				    uint32_t pool_size, uint32_t rpc_arena_offset,
				    uint32_t rpc_arena_size)
{
	struct cavalry_tenant_ctx *tenant = cavalry_proxy_get_tenant(cid);
	if (!tenant)
		return -ENOENT;

	pthread_mutex_lock(&g_tenant_mutex);
	tenant->cavalry_pool_base = pool_base;
	tenant->cavalry_pool_size = pool_size;
	tenant->rpc_arena_offset = rpc_arena_offset;
	tenant->rpc_arena_size = rpc_arena_size;
	pthread_mutex_unlock(&g_tenant_mutex);
	return 0;
}

static int cavalry_proxy_alloc(uint32_t size, uint32_t client_cid, uint32_t session_id, uint32_t *out_offset)
{
	uint32_t aligned_size;
	uint32_t cand;
	int slot = -1;
	int i;
	struct cavalry_tenant_ctx *tenant = client_cid ? cavalry_proxy_get_tenant(client_cid) : NULL;
	uint32_t pool_base = (tenant && tenant->cavalry_pool_size) ? tenant->cavalry_pool_base : CAVALRY_POOL_BASE;
	uint32_t pool_size = (tenant && tenant->cavalry_pool_size) ? tenant->cavalry_pool_size : CAVALRY_POOL_SIZE;

	if (!size || !out_offset)
		return -EINVAL;

	/* Page-align size (4096 bytes) */
	aligned_size = (size + 4095U) & ~4095U;
	if (aligned_size > pool_size)
		return -ENOMEM;

	pthread_mutex_lock(&g_slice_mutex);

	/* Find an empty slot in tracking table */
	for (i = 0; i < MAX_SLICES; i++) {
		if (!g_slices[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_slice_mutex);
		return -ENOMEM;
	}

	/* First-fit bump allocation inside [pool_base, pool_base + pool_size) */
	cand = pool_base;
	while (cand + aligned_size <= pool_base + pool_size) {
		int collision = 0;
		for (i = 0; i < MAX_SLICES; i++) {
			if (g_slices[i].in_use && (!client_cid || g_slices[i].client_cid == client_cid)) {
				uint32_t s_start = g_slices[i].bar_offset;
				uint32_t s_end = s_start + g_slices[i].size;
				if (cand < s_end && (cand + aligned_size) > s_start) {
					cand = (s_end + 4095U) & ~4095U;
					collision = 1;
					break;
				}
			}
		}
		if (!collision) {
			/* Found non-overlapping slice */
			g_slices[slot].bar_offset = cand;
			g_slices[slot].size = aligned_size;
			g_slices[slot].client_cid = client_cid;
			g_slices[slot].session_id = session_id;
			g_slices[slot].in_use = 1;
			*out_offset = cand;
			pthread_mutex_unlock(&g_slice_mutex);
			printf("cavalry_proxy: alloc slice cid=%u sess=%u off=0x%08x size=%u B (aligned %u B)\n",
			       client_cid, session_id, cand, size, aligned_size);
			return 0;
		}
	}

	pthread_mutex_unlock(&g_slice_mutex);
	return -ENOMEM;
}

static int cavalry_proxy_free(uint32_t bar_offset, uint32_t client_cid)
{
	int i;
	int found = 0;

	pthread_mutex_lock(&g_slice_mutex);
	for (i = 0; i < MAX_SLICES; i++) {
		if (g_slices[i].in_use && g_slices[i].bar_offset == bar_offset) {
			if (client_cid && g_slices[i].client_cid != client_cid) {
				pthread_mutex_unlock(&g_slice_mutex);
				return -EPERM;
			}
			g_slices[i].in_use = 0;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_slice_mutex);

	if (!found)
		return -EINVAL;

	printf("cavalry_proxy: freed slice cid=%u off=0x%08x\n", client_cid, bar_offset);
	return 0;
}

static void cavalry_proxy_free_dag_slot(int slot)
{
	if (!g_registered_dags[slot].in_use)
		return;

	if (g_registered_dags[slot].host_mem_virt &&
	    g_registered_dags[slot].host_mem_virt != MAP_FAILED) {
		munmap(g_registered_dags[slot].host_mem_virt, g_registered_dags[slot].host_mem_size);
		g_registered_dags[slot].host_mem_virt = MAP_FAILED;
	}

	if (g_registered_dags[slot].host_mem_phys && g_fd_cav >= 0) {
		struct cavalry_mem mem = { 0 };
		mem.offset = g_registered_dags[slot].host_mem_phys;
		mem.length = g_registered_dags[slot].host_mem_size;
		ioctl(g_fd_cav, CAVALRY_FREE_MEM, &mem);
		g_registered_dags[slot].host_mem_phys = 0;
	}

	g_registered_dags[slot].in_use = 0;
}

static int cavalry_proxy_alloc_handle(uint32_t size, uint32_t client_cid, uint32_t session_id,
				      uint32_t *out_handle_id, uint32_t *out_offset)
{
	uint32_t bar_offset = 0;
	int ret, slot = -1, i;

	if (!size || !out_handle_id || !out_offset)
		return -EINVAL;

	ret = cavalry_proxy_alloc(size, client_cid, session_id, &bar_offset);
	if (ret < 0)
		return ret;

	pthread_mutex_lock(&g_handle_mutex);
	for (i = 0; i < MAX_HANDLES; i++) {
		if (!g_handles[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_handle_mutex);
		cavalry_proxy_free(bar_offset, client_cid);
		return -ENOMEM;
	}

	uint32_t hid = g_next_handle_id++;
	if (hid == 0)
		hid = g_next_handle_id++;

	g_handles[slot].handle_id = hid;
	g_handles[slot].bar_offset = bar_offset;
	g_handles[slot].size = size;
	g_handles[slot].client_cid = client_cid;
	g_handles[slot].session_id = session_id;
	g_handles[slot].in_use = 1;

	*out_handle_id = hid;
	*out_offset = bar_offset;
	pthread_mutex_unlock(&g_handle_mutex);

	printf("cavalry_proxy: alloc handle hid=%u cid=%u sess=%u off=0x%08x size=%u B\n",
	       hid, client_cid, session_id, bar_offset, size);
	return 0;
}

static int cavalry_proxy_free_handle(uint32_t handle_id, uint32_t client_cid, uint32_t session_id)
{
	uint32_t bar_offset = 0;
	int i, found = 0;

	pthread_mutex_lock(&g_handle_mutex);
	for (i = 0; i < MAX_HANDLES; i++) {
		if (g_handles[i].in_use && g_handles[i].handle_id == handle_id) {
			if (g_handles[i].session_id != session_id ||
			    (client_cid && g_handles[i].client_cid != client_cid)) {
				pthread_mutex_unlock(&g_handle_mutex);
				return -EACCES;
			}
			bar_offset = g_handles[i].bar_offset;
			g_handles[i].in_use = 0;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_handle_mutex);

	if (!found)
		return -EINVAL;

	cavalry_proxy_free(bar_offset, client_cid);
	printf("cavalry_proxy: freed handle hid=%u cid=%u off=0x%08x\n", handle_id, client_cid, bar_offset);
	return 0;
}

static int cavalry_proxy_lookup_handle(uint32_t handle_id,
				       uint32_t client_cid,
				       uint32_t session_id,
				       struct cavalry_handle_entry *out_entry)
{
	int i;

	pthread_mutex_lock(&g_handle_mutex);
	for (i = 0; i < MAX_HANDLES; i++) {
		if (g_handles[i].in_use && g_handles[i].handle_id == handle_id) {
			if (g_handles[i].session_id != session_id ||
			    (client_cid && g_handles[i].client_cid != client_cid)) {
				pthread_mutex_unlock(&g_handle_mutex);
				return -EACCES;
			}
			if (out_entry)
				*out_entry = g_handles[i];
			pthread_mutex_unlock(&g_handle_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_handle_mutex);
	return -EINVAL;
}

int cavalry_proxy_close_session(uint32_t client_cid, uint32_t session_id)
{
	int i, count = 0, h_count = 0, d_count = 0;
	uint32_t bytes_reclaimed = 0;

	if (!session_id)
		return 0;

	pthread_mutex_lock(&g_handle_mutex);
	for (i = 0; i < MAX_HANDLES; i++) {
		if (g_handles[i].in_use && g_handles[i].session_id == session_id &&
		    (!client_cid || g_handles[i].client_cid == client_cid)) {
			g_handles[i].in_use = 0;
			h_count++;
		}
	}
	pthread_mutex_unlock(&g_handle_mutex);

	pthread_mutex_lock(&g_dag_mutex);
	for (i = 0; i < MAX_REGISTERED_DAGS; i++) {
		if (g_registered_dags[i].in_use && g_registered_dags[i].session_id == session_id &&
		    (!client_cid || g_registered_dags[i].client_cid == client_cid)) {
			cavalry_proxy_free_dag_slot(i);
			d_count++;
		}
	}
	pthread_mutex_unlock(&g_dag_mutex);

	pthread_mutex_lock(&g_slice_mutex);
	for (i = 0; i < MAX_SLICES; i++) {
		if (g_slices[i].in_use && g_slices[i].session_id == session_id &&
		    (!client_cid || g_slices[i].client_cid == client_cid)) {
			bytes_reclaimed += g_slices[i].size;
			g_slices[i].in_use = 0;
			count++;
		}
	}
	pthread_mutex_unlock(&g_slice_mutex);

	if (count > 0 || h_count > 0 || d_count > 0) {
		printf("cavalry_proxy: closed session cid=%u session_id=%u (slices=%d, handles=%d, dags=%d, reclaimed %u B)\n",
		       client_cid, session_id, count, h_count, d_count, bytes_reclaimed);
	}
	return count;
}

void cavalry_proxy_client_disconnect(uint32_t client_cid)
{
	int i, count = 0, h_count = 0, d_count = 0;

	pthread_mutex_lock(&g_handle_mutex);
	for (i = 0; i < MAX_HANDLES; i++) {
		if (g_handles[i].in_use && g_handles[i].client_cid == client_cid) {
			g_handles[i].in_use = 0;
			h_count++;
		}
	}
	pthread_mutex_unlock(&g_handle_mutex);

	pthread_mutex_lock(&g_dag_mutex);
	for (i = 0; i < MAX_REGISTERED_DAGS; i++) {
		if (g_registered_dags[i].in_use && g_registered_dags[i].client_cid == client_cid) {
			cavalry_proxy_free_dag_slot(i);
			d_count++;
		}
	}
	pthread_mutex_unlock(&g_dag_mutex);

	pthread_mutex_lock(&g_slice_mutex);
	for (i = 0; i < MAX_SLICES; i++) {
		if (g_slices[i].in_use && g_slices[i].client_cid == client_cid) {
			g_slices[i].in_use = 0;
			count++;
		}
	}
	pthread_mutex_unlock(&g_slice_mutex);

	if (count > 0 || h_count > 0 || d_count > 0) {
		printf("cavalry_proxy: reclaimed (slices=%d, handles=%d, dags=%d) for disconnected client cid=%u\n",
		       count, h_count, d_count, client_cid);
	}
}


static int cavalry_proxy_validate_range(uint32_t bar_offset, uint32_t size, uint32_t client_cid)
{
	int i;
	uint64_t req_end;
	struct cavalry_tenant_ctx *tenant = client_cid ? cavalry_proxy_get_tenant(client_cid) : NULL;
	uint32_t pool_base = (tenant && tenant->cavalry_pool_size) ? tenant->cavalry_pool_base : CAVALRY_POOL_BASE;
	uint32_t pool_size = (tenant && tenant->cavalry_pool_size) ? tenant->cavalry_pool_size : CAVALRY_POOL_SIZE;

	/* Must be inside Cavalry pool */
	if (bar_offset < pool_base)
		return -EFAULT;

	req_end = (uint64_t)bar_offset + size;
	if (req_end > (uint64_t)pool_base + pool_size)
		return -EFAULT;

	/* Must be strictly contained within an allocated slice of this client */
	pthread_mutex_lock(&g_slice_mutex);
	for (i = 0; i < MAX_SLICES; i++) {
		if (g_slices[i].in_use &&
		    (!client_cid || g_slices[i].client_cid == client_cid)) {
			uint32_t s_start = g_slices[i].bar_offset;
			uint64_t s_end = (uint64_t)s_start + g_slices[i].size;
			if (bar_offset >= s_start && req_end <= s_end) {
				pthread_mutex_unlock(&g_slice_mutex);
				return 0;
			}
		}
	}
	pthread_mutex_unlock(&g_slice_mutex);

	return -EINVAL;
}

int cavalry_proxy_init(int fd_amba_virt, unsigned char *shm_map, size_t shm_size, uint64_t phys_base)
{
	struct cavalry_status status = { 0 };
	int ret;

	g_fd_shm = fd_amba_virt;
	g_shm_map = shm_map;
	g_shm_size = shm_size;
	g_phys_base = phys_base;

	memset(g_slices, 0, sizeof(g_slices));
	memset(g_tenants, 0, sizeof(g_tenants));

	g_fd_cav = open(CAVALRY_DEV_NODE, O_RDWR);
	if (g_fd_cav < 0) {
		printf("cavalry_proxy: /dev/cavalry not available yet (starting in OFFLINE mode)\n");
		g_fd_cav = -1;
		return 0;
	}

	/* Export ivshmem window as DMA-BUF for MEMFD run */
	ret = ioctl(g_fd_shm, AMBA_VIRT_IOC_EXPORT_DMABUF, &g_window_fd);
	if (ret < 0 || g_window_fd < 0) {
		perror("cavalry_proxy: export dmabuf failed");
		close(g_fd_cav);
		g_fd_cav = -1;
		return -1;
	}

	/* Query host chip ID */
	if (ioctl(g_fd_cav, CAVALRY_GET_CV_CHIP_ID, &g_chip_id) < 0) {
		perror("cavalry_proxy: get chip id");
		g_chip_id = 10; /* Fallback for N1_655 */
	}

	/* Query engine status and auto-start VisORC VP if needed */
	if (ioctl(g_fd_cav, CAVALRY_GET_CAVALRY_STATUS, &status) < 0 ||
	    status.is_cavalry_started == 0) {
		if (ioctl(g_fd_cav, CAVALRY_START_VP, 0) < 0) {
			perror("cavalry_proxy: start vp");
		} else {
			status.is_cavalry_started = 1;
			printf("cavalry_proxy: VisORC ucode started successfully\n");
		}
	}

	printf("cavalry_proxy: initialized (cav_fd=%d, window_fd=%d, chip=%u, started=%u, phys=0x%lx)\n",
	       g_fd_cav, g_window_fd, g_chip_id, status.is_cavalry_started, (unsigned long)g_phys_base);
	return 0;
}

int cavalry_proxy_reopen(void)
{
	pthread_mutex_lock(&g_visorc_hw_mutex);
	if (g_fd_cav >= 0) {
		pthread_mutex_unlock(&g_visorc_hw_mutex);
		return 0;
	}

	int fd = open(CAVALRY_DEV_NODE, O_RDWR);
	if (fd < 0) {
		pthread_mutex_unlock(&g_visorc_hw_mutex);
		return -1;
	}
	g_fd_cav = fd;

	struct cavalry_status status = { 0 };
	if (ioctl(g_fd_cav, CAVALRY_GET_CV_CHIP_ID, &g_chip_id) < 0) {
		g_chip_id = 10;
	}
	if (ioctl(g_fd_cav, CAVALRY_GET_CAVALRY_STATUS, &status) < 0 ||
	    status.is_cavalry_started == 0) {
		if (ioctl(g_fd_cav, CAVALRY_START_VP, 0) == 0) {
			status.is_cavalry_started = 1;
			printf("cavalry_proxy: VisORC ucode started successfully\n");
		}
	}
	printf("cavalry_proxy: reopened (cav_fd=%d, window_fd=%d, chip=%u, started=%u)\n",
	       g_fd_cav, g_window_fd, g_chip_id, status.is_cavalry_started);
	pthread_mutex_unlock(&g_visorc_hw_mutex);
	return 0;
}

void cavalry_proxy_cleanup(void)
{
	if (g_window_fd >= 0) {
		close(g_window_fd);
		g_window_fd = -1;
	}
	if (g_fd_cav >= 0) {
		close(g_fd_cav);
		g_fd_cav = -1;
	}
}

static int handle_run_dags(const struct amba_virt_cavalry_rpc *req,
			   struct amba_virt_cavalry_rpc *resp,
			   uint32_t client_cid)
{
	const struct cavalry_run_dags *run_req;
	struct cavalry_run_dags_mfd *mfd = NULL;
	struct cavalry_tenant_ctx *tenant = NULL;
	uint8_t *host_arena_copy = NULL;
	size_t mfd_size;
	uint32_t d, p;
	int ret = 0;

	if (g_enforce_path_b) {
		fprintf(stderr, "cavalry_proxy: Path A (RUN_DAGS) rejected for CID %u under enforce-path-b policy\n",
			client_cid);
		resp->status = -EPERM;
		return 0;
	}

	tenant = cavalry_proxy_get_tenant(client_cid);
	if (!tenant || tenant->shm_map == MAP_FAILED || !tenant->shm_map) {
		resp->status = -ENODEV;
		return 0;
	}

	uint32_t arena_off = tenant->rpc_arena_offset;
	uint32_t arena_sz = tenant->rpc_arena_size ? tenant->rpc_arena_size : CAVALRY_RPC_ARENA_SIZE;

	/* Cap arena_len against bounds */
	if (req->arena_len == 0 || req->arena_len > arena_sz) {
		fprintf(stderr, "cavalry_proxy: invalid arena_len %u\n", req->arena_len);
		resp->status = -EMSGSIZE;
		return 0;
	}

	if (req->bar_offset != arena_off) {
		fprintf(stderr, "cavalry_proxy: invalid arena offset 0x%08x (expected 0x%08x)\n",
			req->bar_offset, arena_off);
		resp->status = -EINVAL;
		return 0;
	}

	/* Deep-copy the request out of the tenant's shared BAR into host heap */
	host_arena_copy = malloc(req->arena_len);
	if (!host_arena_copy) {
		resp->status = -ENOMEM;
		return 0;
	}
	memcpy(host_arena_copy, tenant->shm_map + arena_off, req->arena_len);

	run_req = (const struct cavalry_run_dags *)host_arena_copy;
	if (run_req->dag_cnt == 0 || run_req->dag_cnt > MAX_DAG_CNT_CAP) {
		fprintf(stderr, "cavalry_proxy: invalid dag_cnt %u\n", run_req->dag_cnt);
		resp->status = -EINVAL;
		goto out_free_arena;
	}

	/* Validate all DVI and port descriptors against the client's allocated slices */
	for (d = 0; d < run_req->dag_cnt; d++) {
		const struct cavalry_dag_desc *dag = &run_req->dag_desc[d];

		ret = cavalry_proxy_validate_range(dag->dvi_dram_addr, dag->dvi_img_size, client_cid);
		if (ret < 0) {
			fprintf(stderr, "cavalry_proxy: invalid DVI address 0x%lx size %u for cid=%u\n",
				(unsigned long)dag->dvi_dram_addr, dag->dvi_img_size, client_cid);
			resp->status = ret;
			goto out_free_arena;
		}

		for (p = 0; p < dag->port_cnt; p++) {
			ret = cavalry_proxy_validate_range(dag->port_desc[p].port_dram_addr,
							   dag->port_desc[p].port_dram_size,
							   client_cid);
			if (ret < 0) {
				fprintf(stderr, "cavalry_proxy: invalid port [%u-%u] addr 0x%lx size %lu for cid=%u\n",
					d, p, (unsigned long)dag->port_desc[p].port_dram_addr,
					(unsigned long)dag->port_desc[p].port_dram_size, client_cid);
				resp->status = ret;
				goto out_free_arena;
			}
		}
	}

	/* Rebuild descriptors into struct cavalry_run_dags_mfd */
	mfd_size = sizeof(struct cavalry_run_dags_mfd) +
		run_req->dag_cnt * sizeof(struct cavalry_dag_desc_mfd);
	mfd = calloc(1, mfd_size);
	if (!mfd) {
		resp->status = -ENOMEM;
		goto out_free_arena;
	}

	mfd->dag_cnt = run_req->dag_cnt;
	mfd->priority = run_req->priority;
	mfd->affinity = run_req->affinity;
	mfd->hw_type = run_req->hw_type;
	mfd->sub_session_id = run_req->sub_session_id;
	mfd->session_id = run_req->session_id;

	mfd->ucode_cmd_addr_fd = -1;
	mfd->ucode_cmd_addr_offset = 0;
	mfd->dvi_dram_addr_fd = tenant->window_fd;

	for (d = 0; d < run_req->dag_cnt; d++) {
		const struct cavalry_dag_desc *src = &run_req->dag_desc[d];
		struct cavalry_dag_desc_mfd *dst = &mfd->dag_desc[d];

		dst->dag_loop_cnt = src->dag_loop_cnt;
		dst->use_ping_pong_vmem = src->use_ping_pong_vmem;
		dst->run_with_checksum = src->run_with_checksum;
		dst->is_orc_pdxs_set = src->is_orc_pdxs_set;
		dst->orc_pdxs = src->orc_pdxs;
		dst->dag_type = src->dag_type;
		dst->dep_cnt = src->dep_cnt;
		dst->dvi_dag_size = src->dvi_dag_size;

		dst->dvi_dram_addr_offset = tenant->slice_offset + src->dvi_dram_addr;

		dst->dvi_img_vaddr = src->dvi_img_vaddr;
		dst->dvi_img_size = src->dvi_img_size;
		dst->dvi_dag_vaddr = src->dvi_dag_vaddr;
		dst->private_scratchpad_offset = src->private_scratchpad_offset;
		dst->reverse_dep_dag_cnt = src->reverse_dep_dag_cnt;
		dst->port_cnt = src->port_cnt;
		dst->poke_cnt = src->poke_cnt;
		dst->extra_poke_list_cnt = src->extra_poke_list_cnt;

		if (src->extra_poke_list_daddr)
			dst->extra_poke_list_dram_offset = tenant->slice_offset + src->extra_poke_list_daddr;
		if (src->extra_dag_desc_common_daddr)
			dst->extra_dag_desc_common_offset = tenant->slice_offset + src->extra_dag_desc_common_daddr;
		if (src->extra_dag_desc_list_daddr)
			dst->extra_dag_desc_list_offset = tenant->slice_offset + src->extra_dag_desc_list_daddr;

		memcpy(dst->reverse_dep_dag_id, src->reverse_dep_dag_id, sizeof(dst->reverse_dep_dag_id));
		memcpy(dst->port_desc, src->port_desc, sizeof(dst->port_desc));
		memcpy(dst->poke_desc, src->poke_desc, sizeof(dst->poke_desc));

		for (p = 0; p < src->port_cnt; p++) {
			dst->port_dram_addr_fd[p] = tenant->window_fd;
			dst->port_desc[p].port_dram_addr = tenant->slice_offset + src->port_desc[p].port_dram_addr;
		}
	}

	/* Dispatch ioctl to real host /dev/cavalry under visorc_hw_mutex */
	pthread_mutex_lock(&g_visorc_hw_mutex);
	ret = ioctl(g_fd_cav, CAVALRY_RUN_DAGS_MEMFD, mfd);
	pthread_mutex_unlock(&g_visorc_hw_mutex);

	if (ret < 0) {
		fprintf(stderr, "cavalry_proxy: CAVALRY_RUN_DAGS_MEMFD ioctl failed: %s\n", strerror(errno));
		resp->status = -errno;
		resp->rval = mfd->rval;
	} else {
		resp->status = 0;
		resp->rval = mfd->rval;
		resp->exec_ticks = mfd->exec_total_ticks;
	}

	free(mfd);

out_free_arena:
	free(host_arena_copy);
	return 0;
}

static int handle_register_dag(const struct amba_virt_cavalry_rpc *req,
			       struct amba_virt_cavalry_rpc *resp,
			       uint32_t client_cid)
{
	const struct amba_virt_cavalry_reg_dag_desc *reg_desc;
	const struct cavalry_run_dags *run_req;
	struct cavalry_tenant_ctx *tenant = NULL;
	uint8_t *host_arena_copy = NULL;
	struct cavalry_mem mem = { 0 };
	void *host_virt = MAP_FAILED;
	unsigned long host_phys = 0;
	uint32_t src_base = 0;
	int slot = -1, i;
	uint32_t d, p;
	int ret = 0;

	tenant = cavalry_proxy_get_tenant(client_cid);
	if (!tenant || tenant->shm_map == MAP_FAILED || !tenant->shm_map) {
		resp->status = -ENODEV;
		return 0;
	}

	uint32_t arena_off = tenant->rpc_arena_offset;
	uint32_t arena_sz = tenant->rpc_arena_size ? tenant->rpc_arena_size : CAVALRY_RPC_ARENA_SIZE;

	if (req->arena_len < sizeof(struct amba_virt_cavalry_reg_dag_desc) + sizeof(struct cavalry_run_dags) ||
	    req->arena_len > arena_sz) {
		fprintf(stderr, "cavalry_proxy: invalid register arena_len %u\n", req->arena_len);
		resp->status = -EMSGSIZE;
		return 0;
	}

	if (req->bar_offset != arena_off) {
		fprintf(stderr, "cavalry_proxy: invalid arena offset 0x%08x (expected 0x%08x)\n",
			req->bar_offset, arena_off);
		resp->status = -EINVAL;
		return 0;
	}

	host_arena_copy = malloc(req->arena_len);
	if (!host_arena_copy) {
		resp->status = -ENOMEM;
		return 0;
	}
	memcpy(host_arena_copy, tenant->shm_map + arena_off, req->arena_len);

	reg_desc = (const struct amba_virt_cavalry_reg_dag_desc *)host_arena_copy;
	run_req = (const struct cavalry_run_dags *)(host_arena_copy + sizeof(struct amba_virt_cavalry_reg_dag_desc));

	src_base = reg_desc->staging_bar_offset;

	/* Validate staging slice range */
	ret = cavalry_proxy_validate_range(reg_desc->staging_bar_offset, reg_desc->staging_size, client_cid);
	if (ret < 0) {
		fprintf(stderr, "cavalry_proxy: register invalid staging range off=0x%x size=%u for cid=%u\n",
			reg_desc->staging_bar_offset, reg_desc->staging_size, client_cid);
		resp->status = ret;
		goto out_free_arena;
	}

	if (run_req->dag_cnt == 0 || run_req->dag_cnt > MAX_DAG_CNT_CAP) {
		fprintf(stderr, "cavalry_proxy: register invalid dag_cnt %u\n", run_req->dag_cnt);
		resp->status = -EINVAL;
		goto out_free_arena;
	}

	/* Find empty slot in g_registered_dags */
	pthread_mutex_lock(&g_dag_mutex);
	for (i = 0; i < MAX_REGISTERED_DAGS; i++) {
		if (!g_registered_dags[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_dag_mutex);
		resp->status = -ENOMEM;
		goto out_free_arena;
	}

	/* Allocate host AMA memory above 1 GiB for DVI / extra_dag */
	memset(&mem, 0, sizeof(mem));
	mem.length = reg_desc->staging_size;
	mem.cache_en = 0;
	pthread_mutex_lock(&g_visorc_hw_mutex);
	ret = ioctl(g_fd_cav, CAVALRY_ALLOC_MEM, &mem);
	pthread_mutex_unlock(&g_visorc_hw_mutex);
	if (ret < 0) {
		perror("cavalry_proxy: register CAVALRY_ALLOC_MEM failed");
		pthread_mutex_unlock(&g_dag_mutex);
		resp->status = -ENOMEM;
		goto out_free_arena;
	}

	host_phys = mem.offset;
	host_virt = mmap(NULL, mem.length, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd_cav, host_phys);
	if (host_virt == MAP_FAILED) {
		perror("cavalry_proxy: register mmap host AMA buffer failed");
		pthread_mutex_lock(&g_visorc_hw_mutex);
		ioctl(g_fd_cav, CAVALRY_FREE_MEM, &mem);
		pthread_mutex_unlock(&g_visorc_hw_mutex);
		pthread_mutex_unlock(&g_dag_mutex);
		resp->status = -ENOMEM;
		goto out_free_arena;
	}

	/* Deep-copy staging slice from tenant BAR into host-private AMA */
	memcpy(host_virt, tenant->shm_map + src_base, reg_desc->staging_size);

	/* Relocate sub-scheduler pointers inside host_virt */
	for (d = 0; d < run_req->dag_cnt; d++) {
		const struct cavalry_dag_desc *src = &run_req->dag_desc[d];

		if (src->extra_dag_desc_list_daddr &&
		    src->extra_dag_desc_list_daddr >= src_base) {
			uint32_t elist_off = src->extra_dag_desc_list_daddr - src_base;
			if (elist_off + sizeof(struct extra_dag_desc_list) <= reg_desc->staging_size) {
				struct extra_dag_desc_list *elist =
					(struct extra_dag_desc_list *)((uint8_t *)host_virt + elist_off);

				if (elist->preload_list_cnt && elist->preload_list_daddr &&
				    elist->preload_list_daddr >= src_base) {
					uint32_t pl_off = elist->preload_list_daddr - src_base;
					elist->preload_list_daddr = host_phys + pl_off;
					if (pl_off + elist->preload_list_cnt * sizeof(struct extra_dag_desc_preload) <= reg_desc->staging_size) {
						struct extra_dag_desc_preload *pl =
							(struct extra_dag_desc_preload *)((uint8_t *)host_virt + pl_off);
						for (i = 0; i < (int)elist->preload_list_cnt; i++) {
							if (pl[i].preload_daddr >= src_base)
								pl[i].preload_daddr = host_phys + (pl[i].preload_daddr - src_base);
						}
					}
				}
				if (elist->runtime_list_cnt && elist->runtime_list_daddr &&
				    elist->runtime_list_daddr >= src_base) {
					elist->runtime_list_daddr = host_phys + (elist->runtime_list_daddr - src_base);
				}
				if (elist->extra_port_list_cnt && elist->extra_port_list_daddr &&
				    elist->extra_port_list_daddr >= src_base) {
					elist->extra_port_list_daddr = host_phys + (elist->extra_port_list_daddr - src_base);
				}
				if (elist->bb_list_cnt && elist->bb_list_daddr &&
				    elist->bb_list_daddr >= src_base) {
					elist->bb_list_daddr = host_phys + (elist->bb_list_daddr - src_base);
				}
			}
		}
	}

	/* Populate template_dag in the registry entry */
	memset(&g_registered_dags[slot], 0, sizeof(g_registered_dags[slot]));
	g_registered_dags[slot].dag_cnt = run_req->dag_cnt;
	g_registered_dags[slot].host_mem_size = reg_desc->staging_size;
	g_registered_dags[slot].host_mem_phys = host_phys;
	g_registered_dags[slot].host_mem_virt = host_virt;
	g_registered_dags[slot].client_cid = client_cid;
	g_registered_dags[slot].session_id = req->session_id;
	g_registered_dags[slot].nid = run_req->nid;
	g_registered_dags[slot].affinity = run_req->affinity;
	g_registered_dags[slot].priority = run_req->priority;
	g_registered_dags[slot].hw_type = run_req->hw_type;
	g_registered_dags[slot].is_encrypt = run_req->is_encrypt;
	g_registered_dags[slot].is_resume = run_req->is_resume;
	g_registered_dags[slot].no_auto_resume = run_req->no_auto_resume;
	g_registered_dags[slot].sub_session_id = run_req->sub_session_id;
	g_registered_dags[slot].enc_session_id = run_req->session_id;

	for (d = 0; d < run_req->dag_cnt; d++) {
		const struct cavalry_dag_desc *src = &run_req->dag_desc[d];
		struct cavalry_dag_desc_mfd *dst = &g_registered_dags[slot].template_dag[d];

		dst->dag_loop_cnt = src->dag_loop_cnt;
		dst->use_ping_pong_vmem = src->use_ping_pong_vmem;
		dst->run_with_checksum = src->run_with_checksum;
		dst->is_orc_pdxs_set = src->is_orc_pdxs_set;
		dst->orc_pdxs = src->orc_pdxs;
		dst->dag_type = src->dag_type;
		dst->dep_cnt = src->dep_cnt;
		dst->dvi_dag_size = src->dvi_dag_size;

		if (src->dvi_dram_addr >= src_base)
			dst->dvi_dram_addr_offset = host_phys + (src->dvi_dram_addr - src_base);
		else
			dst->dvi_dram_addr_offset = src->dvi_dram_addr;

		dst->dvi_img_vaddr = src->dvi_img_vaddr;
		dst->dvi_img_size = src->dvi_img_size;
		dst->dvi_dag_vaddr = src->dvi_dag_vaddr;
		dst->private_scratchpad_offset = src->private_scratchpad_offset;
		dst->reverse_dep_dag_cnt = src->reverse_dep_dag_cnt;
		dst->port_cnt = src->port_cnt;
		dst->poke_cnt = src->poke_cnt;
		dst->extra_poke_list_cnt = src->extra_poke_list_cnt;

		if (src->extra_poke_list_daddr && src->extra_poke_list_daddr >= src_base)
			dst->extra_poke_list_dram_offset = host_phys + (src->extra_poke_list_daddr - src_base);
		if (src->extra_dag_desc_common_daddr && src->extra_dag_desc_common_daddr >= src_base)
			dst->extra_dag_desc_common_offset = host_phys + (src->extra_dag_desc_common_daddr - src_base);
		if (src->extra_dag_desc_list_daddr && src->extra_dag_desc_list_daddr >= src_base)
			dst->extra_dag_desc_list_offset = host_phys + (src->extra_dag_desc_list_daddr - src_base);

		memcpy(dst->reverse_dep_dag_id, src->reverse_dep_dag_id, sizeof(dst->reverse_dep_dag_id));
		memcpy(dst->port_desc, src->port_desc, sizeof(dst->port_desc));
		memcpy(dst->poke_desc, src->poke_desc, sizeof(dst->poke_desc));

		for (p = 0; p < dst->port_cnt && p < CAVALRY_MAX_PORTS; p++) {
			dst->port_dram_addr_fd[p] = CAVALRY_DMABUF_FD_REPRESENT_PHYS;
			if (dst->port_desc[p].port_dram_addr >= src_base &&
			    dst->port_desc[p].port_dram_addr < src_base + reg_desc->staging_size) {
				dst->port_desc[p].port_dram_addr =
					host_phys + (dst->port_desc[p].port_dram_addr - src_base);
			}
		}
	}

	/* Flush host ARM cache for newly copied DVI / extra_dag */
	struct cavalry_cache_mem cmem = {
		.clean = 1,
		.invalid = 0,
		.offset = host_phys,
		.length = mem.length,
	};
	pthread_mutex_lock(&g_visorc_hw_mutex);
	ioctl(g_fd_cav, CAVALRY_SYNC_CACHE_MEM, &cmem);
	pthread_mutex_unlock(&g_visorc_hw_mutex);
#ifdef __aarch64__
	asm volatile("dsb sy" ::: "memory");
#endif

	uint32_t dag_id = g_next_dag_id++;
	if (dag_id == 0)
		dag_id = g_next_dag_id++;
	g_registered_dags[slot].dag_id = dag_id;
	g_registered_dags[slot].in_use = 1;

	resp->dag_id = dag_id;
	resp->status = 0;
	pthread_mutex_unlock(&g_dag_mutex);

	printf("cavalry_proxy: registered DAG id=%u cid=%u sess=%u (host AMA PA=0x%lx, size=%u B)\n",
	       dag_id, client_cid, req->session_id, host_phys, reg_desc->staging_size);

out_free_arena:
	free(host_arena_copy);
	return 0;
}

static int handle_unregister_dag(const struct amba_virt_cavalry_rpc *req,
				 struct amba_virt_cavalry_rpc *resp,
				 uint32_t client_cid)
{
	int i, found = 0;

	pthread_mutex_lock(&g_dag_mutex);
	for (i = 0; i < MAX_REGISTERED_DAGS; i++) {
		if (g_registered_dags[i].in_use && g_registered_dags[i].dag_id == req->dag_id) {
			if (g_registered_dags[i].session_id != req->session_id ||
			    (client_cid && g_registered_dags[i].client_cid != client_cid)) {
				pthread_mutex_unlock(&g_dag_mutex);
				resp->status = -EACCES;
				return 0;
			}
			cavalry_proxy_free_dag_slot(i);
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_dag_mutex);

	if (!found) {
		resp->status = -EINVAL;
		return 0;
	}

	printf("cavalry_proxy: unregistered DAG id=%u cid=%u sess=%u\n", req->dag_id, client_cid, req->session_id);
	resp->status = 0;
	return 0;
}

static int handle_run_registered_dag(const struct amba_virt_cavalry_rpc *req,
				     struct amba_virt_cavalry_rpc *resp,
				     uint32_t client_cid)
{
	const struct amba_virt_cavalry_run_reg_desc *run_reg;
	struct cavalry_run_dags_mfd *mfd = NULL;
	struct cavalry_registered_dag *dag = NULL;
	struct cavalry_tenant_ctx *tenant = NULL;
	uint8_t *host_arena_copy = NULL;
	size_t mfd_size;
	uint32_t d, p;
	int ret = 0, i;

	tenant = cavalry_proxy_get_tenant(client_cid);
	if (!tenant || tenant->shm_map == MAP_FAILED || !tenant->shm_map) {
		resp->status = -ENODEV;
		return 0;
	}

	uint32_t arena_off = tenant->rpc_arena_offset;
	uint32_t arena_sz = tenant->rpc_arena_size ? tenant->rpc_arena_size : CAVALRY_RPC_ARENA_SIZE;

	if (req->arena_len < sizeof(struct amba_virt_cavalry_run_reg_desc) ||
	    req->arena_len > arena_sz) {
		fprintf(stderr, "cavalry_proxy: invalid run_reg arena_len %u\n", req->arena_len);
		resp->status = -EMSGSIZE;
		return 0;
	}

	if (req->bar_offset != arena_off) {
		fprintf(stderr, "cavalry_proxy: invalid arena offset 0x%08x (expected 0x%08x)\n",
			req->bar_offset, arena_off);
		resp->status = -EINVAL;
		return 0;
	}

	/* Lookup registered DAG - enforce client_cid ownership */
	pthread_mutex_lock(&g_dag_mutex);
	for (i = 0; i < MAX_REGISTERED_DAGS; i++) {
		if (g_registered_dags[i].in_use && g_registered_dags[i].dag_id == req->dag_id) {
			if (g_registered_dags[i].session_id != req->session_id ||
			    (client_cid && g_registered_dags[i].client_cid != client_cid)) {
				pthread_mutex_unlock(&g_dag_mutex);
				resp->status = -EACCES;
				return 0;
			}
			dag = &g_registered_dags[i];
			break;
		}
	}

	if (!dag) {
		pthread_mutex_unlock(&g_dag_mutex);
		resp->status = -EINVAL; /* B.4 assertion expects EINVAL or ENOENT */
		return 0;
	}

	host_arena_copy = malloc(req->arena_len);
	if (!host_arena_copy) {
		pthread_mutex_unlock(&g_dag_mutex);
		resp->status = -ENOMEM;
		return 0;
	}
	memcpy(host_arena_copy, tenant->shm_map + arena_off, req->arena_len);

	run_reg = (const struct amba_virt_cavalry_run_reg_desc *)host_arena_copy;

	/* Rebuild descriptors into struct cavalry_run_dags_mfd */
	mfd_size = sizeof(struct cavalry_run_dags_mfd) +
		dag->dag_cnt * sizeof(struct cavalry_dag_desc_mfd);
	mfd = calloc(1, mfd_size);
	if (!mfd) {
		pthread_mutex_unlock(&g_dag_mutex);
		free(host_arena_copy);
		resp->status = -ENOMEM;
		return 0;
	}

	mfd->dag_cnt = dag->dag_cnt;
	mfd->nid = dag->nid;
	mfd->affinity = dag->affinity;
	mfd->priority = dag->priority;
	mfd->hw_type = dag->hw_type;
	mfd->is_encrypt = dag->is_encrypt;
	mfd->is_resume = dag->is_resume;
	mfd->no_auto_resume = dag->no_auto_resume;
	mfd->sub_session_id = dag->sub_session_id;
	mfd->session_id = dag->enc_session_id;
	mfd->ucode_cmd_addr_fd = -1;
	mfd->ucode_cmd_addr_offset = 0;
	mfd->dvi_dram_addr_fd = CAVALRY_DMABUF_FD_REPRESENT_PHYS;

	/* Copy cached templates */
	memcpy(mfd->dag_desc, dag->template_dag, dag->dag_cnt * sizeof(struct cavalry_dag_desc_mfd));
	pthread_mutex_unlock(&g_dag_mutex);

	/* Bind port handles - enforce client_cid ownership */
	for (p = 0; p < run_reg->port_cnt && p < CAVALRY_MAX_PORTS; p++) {
		const struct amba_virt_cavalry_port_bind *bind = &run_reg->ports[p];
		struct cavalry_handle_entry hend;
		int h_ret;

		h_ret = cavalry_proxy_lookup_handle(bind->handle_id, client_cid, req->session_id, &hend);
		if (h_ret < 0) {
			fprintf(stderr, "cavalry_proxy: handle lookup failed for hid=%u cid=%u (ret=%d)\n",
				bind->handle_id, client_cid, h_ret);
			resp->status = h_ret;
			goto out_free;
		}

		if ((uint64_t)bind->offset + bind->size > hend.size) {
			fprintf(stderr, "cavalry_proxy: handle bounds overflow hid=%u off=%u size=%u > max=%u\n",
				bind->handle_id, bind->offset, bind->size, hend.size);
			resp->status = -EINVAL;
			goto out_free;
		}

		uint32_t d_idx = bind->dag_idx;
		uint32_t p_idx = bind->port_idx;

		if (d_idx >= mfd->dag_cnt || p_idx >= mfd->dag_desc[d_idx].port_cnt) {
			fprintf(stderr, "cavalry_proxy: invalid dag/port index [%u][%u] (dag_cnt=%u)\n",
				d_idx, p_idx, mfd->dag_cnt);
			resp->status = -EINVAL;
			goto out_free;
		}

		mfd->dag_desc[d_idx].port_dram_addr_fd[p_idx] = tenant->window_fd;
		mfd->dag_desc[d_idx].port_desc[p_idx].port_dram_addr =
			tenant->slice_offset + hend.bar_offset + bind->offset;
		mfd->dag_desc[d_idx].port_desc[p_idx].port_dram_size = bind->size;
	}

	/* Verify no unclassified port still points to zero fd or untranslated memory */
	for (d = 0; d < mfd->dag_cnt; d++) {
		for (p = 0; p < mfd->dag_desc[d].port_cnt; p++) {
			if (mfd->dag_desc[d].port_dram_addr_fd[p] == 0) {
				fprintf(stderr, "cavalry_proxy: uninitialized port_dram_addr_fd at DAG %u port %u\n", d, p);
				resp->status = -EINVAL;
				goto out_free;
			}
		}
	}

	/* Dispatch ioctl to real host /dev/cavalry under visorc_hw_mutex */
	pthread_mutex_lock(&g_visorc_hw_mutex);
	ret = ioctl(g_fd_cav, CAVALRY_RUN_DAGS_MEMFD, mfd);
	pthread_mutex_unlock(&g_visorc_hw_mutex);

	if (ret < 0) {
		fprintf(stderr, "cavalry_proxy: RUN_REGISTERED_DAG failed: %s\n", strerror(errno));
		resp->status = -errno;
		resp->rval = mfd->rval;
	} else {
		resp->status = 0;
		resp->rval = mfd->rval;
		resp->exec_ticks = mfd->exec_total_ticks;
	}

out_free:
	free(mfd);
	free(host_arena_copy);
	return 0;
}

int cavalry_proxy_handle_rpc(const struct amba_virt_cavalry_rpc *req,
			     struct amba_virt_cavalry_rpc *resp,
			     uint32_t client_cid)
{
	int ret = 0;

	if (!req || !resp)
		return -EINVAL;

	memset(resp, 0, sizeof(*resp));
	resp->opcode = req->opcode;

	pthread_mutex_lock(&g_drain_mutex);
	if (g_draining) {
		pthread_mutex_unlock(&g_drain_mutex);
		resp->status = -EHOSTDOWN;
		return 0;
	}
	g_active_requests++;
	pthread_mutex_unlock(&g_drain_mutex);

	if (g_fd_cav < 0) {
		cavalry_proxy_reopen();
	}

	if (g_fd_cav < 0) {
		resp->status = -ENODEV;
		pthread_mutex_lock(&g_drain_mutex);
		g_active_requests--;
		if (g_active_requests == 0 && g_draining)
			pthread_cond_broadcast(&g_drain_cond);
		pthread_mutex_unlock(&g_drain_mutex);
		return 0;
	}

	switch (req->opcode) {
	case VCAV_OP_GET_VERSION: {
		struct cavalry_driver_version ver = { 0 };
		pthread_mutex_lock(&g_visorc_hw_mutex);
		ret = ioctl(g_fd_cav, CAVALRY_GET_DRIVER_VERSION, &ver);
		pthread_mutex_unlock(&g_visorc_hw_mutex);
		if (ret < 0)
			resp->status = -errno;
		else
			resp->status = 0;
		break;
	}

	case VCAV_OP_GET_CHIP_ID: {
		uint32_t chip_id = 0;
		pthread_mutex_lock(&g_visorc_hw_mutex);
		ret = ioctl(g_fd_cav, CAVALRY_GET_CV_CHIP_ID, &chip_id);
		pthread_mutex_unlock(&g_visorc_hw_mutex);
		if (ret < 0)
			resp->status = -errno;
		else {
			resp->chip_id = chip_id;
			resp->status = 0;
		}
		break;
	}

	case VCAV_OP_GET_STATUS: {
		struct cavalry_status status = { 0 };
		pthread_mutex_lock(&g_visorc_hw_mutex);
		ret = ioctl(g_fd_cav, CAVALRY_GET_CAVALRY_STATUS, &status);
		pthread_mutex_unlock(&g_visorc_hw_mutex);
		if (ret < 0) {
			resp->status = -errno;
			printf("cavalry_proxy: GET_STATUS ioctl failed: %s\n", strerror(errno));
		} else {
			resp->status = 0;
			resp->rval = status.is_cavalry_started;
		}
		break;
	}

	case VCAV_OP_QUERY_BUF: {
		struct cavalry_tenant_ctx *tenant = cavalry_proxy_get_tenant(client_cid);
		resp->bar_offset = (tenant && tenant->cavalry_pool_size) ? tenant->cavalry_pool_base : CAVALRY_POOL_BASE;
		resp->size = (tenant && tenant->cavalry_pool_size) ? tenant->cavalry_pool_size : CAVALRY_POOL_SIZE;
		resp->status = 0;
		break;
	}

	case VCAV_OP_QUERY_UCODE_CMD_SIZE: {
		struct cavalry_ucode_cmd_size ucmd = { 0 };
		ucmd.cmd_id = req->bar_offset;
		ucmd.dag_cnt = req->size;
		pthread_mutex_lock(&g_visorc_hw_mutex);
		ret = ioctl(g_fd_cav, CAVALRY_QUERY_UCODE_CMD_SIZE, &ucmd);
		pthread_mutex_unlock(&g_visorc_hw_mutex);
		if (ret < 0) {
			resp->status = -errno;
		} else {
			resp->status = 0;
			resp->rval = ucmd.cmd_size;
		}
		break;
	}

	case VCAV_OP_ALLOC_MEM:
		resp->status = cavalry_proxy_alloc(req->size, client_cid, req->session_id, &resp->bar_offset);
		resp->size = req->size;
		break;

	case VCAV_OP_CLOSE_SESSION:
		cavalry_proxy_close_session(client_cid, req->session_id);
		resp->status = 0;
		break;

	case VCAV_OP_FREE_MEM:
		resp->status = cavalry_proxy_free(req->bar_offset, client_cid);
		break;

	case VCAV_OP_REGISTER_DAG:
		ret = handle_register_dag(req, resp, client_cid);
		break;

	case VCAV_OP_UNREGISTER_DAG:
		ret = handle_unregister_dag(req, resp, client_cid);
		break;

	case VCAV_OP_ALLOC_HANDLE: {
		uint32_t hid = 0, off = 0;
		ret = cavalry_proxy_alloc_handle(req->size, client_cid, req->session_id, &hid, &off);
		resp->status = ret;
		if (ret == 0) {
			resp->dag_id = hid;
			resp->bar_offset = off;
			resp->size = req->size;
		}
		break;
	}

	case VCAV_OP_FREE_HANDLE:
		resp->status = cavalry_proxy_free_handle(req->dag_id, client_cid, req->session_id);
		break;

	case VCAV_OP_RUN_REGISTERED_DAG:
		ret = handle_run_registered_dag(req, resp, client_cid);
		break;

	case VCAV_OP_SYNC_CACHE:
		/* Normal-NC CPU stores; no-op on host */
		resp->status = 0;
		break;

	case VCAV_OP_START_VP: {
		struct cavalry_status status = { 0 };
		pthread_mutex_lock(&g_visorc_hw_mutex);
		if (ioctl(g_fd_cav, CAVALRY_GET_CAVALRY_STATUS, &status) < 0 ||
		    status.is_cavalry_started == 0) {
			if (ioctl(g_fd_cav, CAVALRY_START_VP, 0) < 0)
				resp->status = -errno;
			else
				resp->status = 0;
		} else {
			resp->status = 0;
		}
		pthread_mutex_unlock(&g_visorc_hw_mutex);
		break;
	}

	case VCAV_OP_STOP_VP:
		/* Host singleton; guest stop is ignored */
		resp->status = 0;
		break;

	case VCAV_OP_RUN_DAGS:
		ret = handle_run_dags(req, resp, client_cid);
		break;

	default:
		fprintf(stderr, "cavalry_proxy: unknown opcode %u\n", req->opcode);
		resp->status = -ENOSYS;
		break;
	}

	pthread_mutex_lock(&g_drain_mutex);
	g_active_requests--;
	if (g_active_requests == 0 && g_draining) {
		pthread_cond_broadcast(&g_drain_cond);
	}
	pthread_mutex_unlock(&g_drain_mutex);

	return ret;
}

int cavalry_proxy_start_drain(void)
{
	pthread_mutex_lock(&g_drain_mutex);
	g_draining = 1;
	pthread_mutex_unlock(&g_drain_mutex);
	return 0;
}

int cavalry_proxy_wait_drained(unsigned int timeout_ms)
{
	struct timespec ts;
	struct timeval tv;
	int ret = 0;

	gettimeofday(&tv, NULL);
	ts.tv_sec = tv.tv_sec + (timeout_ms / 1000);
	ts.tv_nsec = (tv.tv_usec + (timeout_ms % 1000) * 1000) * 1000;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&g_drain_mutex);
	while (g_active_requests > 0 && ret == 0) {
		ret = pthread_cond_timedwait(&g_drain_cond, &g_drain_mutex, &ts);
	}
	int remaining = g_active_requests;
	pthread_mutex_unlock(&g_drain_mutex);

	return remaining == 0 ? 0 : -ETIMEDOUT;
}

void cavalry_proxy_finish_drain(void)
{
	pthread_mutex_lock(&g_drain_mutex);
	g_draining = 0;
	pthread_mutex_unlock(&g_drain_mutex);

	pthread_mutex_lock(&g_visorc_hw_mutex);
	if (g_fd_cav >= 0) {
		close(g_fd_cav);
		g_fd_cav = -1;
	}
	pthread_mutex_unlock(&g_visorc_hw_mutex);
}

int cavalry_proxy_is_draining(void)
{
	pthread_mutex_lock(&g_drain_mutex);
	int d = g_draining;
	pthread_mutex_unlock(&g_drain_mutex);
	return d;
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
