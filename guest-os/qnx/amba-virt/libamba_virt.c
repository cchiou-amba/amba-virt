/*
 * libamba_virt.c
 *
 * Ambarella Virtualization QNX Client Library.
 * Provides C ABI compatibility for sibling resource managers and applications.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "amba_virt_qnx.h"

static pthread_mutex_t g_lib_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1;
static struct amba_virt_info g_info;
static void *g_shm_virt = MAP_FAILED;
static size_t g_shm_size = 0;
static uint64_t g_shm_phys = 0;

static int ensure_opened(void)
{
    if (g_fd >= 0)
        return 0;

    g_fd = open(AMBA_VIRT_DEV_PATH, O_RDWR);
    if (g_fd < 0)
        return -errno;

    memset(&g_info, 0, sizeof(g_info));
    if (ioctl(g_fd, AMBA_VIRT_IOC_GET_INFO, &g_info) < 0) {
        int err = -errno;
        close(g_fd);
        g_fd = -1;
        return err;
    }

    g_shm_size = (size_t)g_info.shm_size;
    g_shm_phys = g_info.shm_phys;
    return 0;
}

int amba_virt_get_window(uint64_t *phys, void **virt, size_t *size)
{
    int ret = 0;

    if (!phys || !virt || !size)
        return -EINVAL;

    pthread_mutex_lock(&g_lib_mutex);

    ret = ensure_opened();
    if (ret < 0) {
        pthread_mutex_unlock(&g_lib_mutex);
        return ret;
    }

    if (g_shm_virt == MAP_FAILED && g_shm_size > 0) {
        if (g_shm_phys != 0) {
            g_shm_virt = mmap_device_memory(NULL, g_shm_size,
                                            PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                            0, g_shm_phys);
        }
        if (g_shm_virt == MAP_FAILED) {
            size_t map_sz = g_shm_size;
            if (map_sz > 64 * 1024 * 1024)
                map_sz = 64 * 1024 * 1024;
            g_shm_virt = mmap(NULL, map_sz,
                              PROT_READ | PROT_WRITE,
                              MAP_ANON | MAP_PRIVATE, NOFD, 0);
        }
        if (g_shm_virt == MAP_FAILED) {
            pthread_mutex_unlock(&g_lib_mutex);
            return -ENOMEM;
        }
    }

    *phys = g_shm_phys;
    *virt = g_shm_virt;
    *size = g_shm_size;

    pthread_mutex_unlock(&g_lib_mutex);
    return 0;
}

int amba_virt_rpc(const void *request, uint32_t request_len,
                  void *response, uint32_t *response_len,
                  unsigned int timeout_ms)
{
    struct amba_virt_xfer xfer;
    uint32_t cap;
    int ret;

    if (!request || !response || !response_len)
        return -EINVAL;
    if (request_len == 0 || request_len > AMBA_VIRT_MAX_MSG)
        return -EMSGSIZE;

    cap = *response_len;
    if (cap == 0 || cap > AMBA_VIRT_MAX_MSG)
        return -EMSGSIZE;

    pthread_mutex_lock(&g_lib_mutex);

    ret = ensure_opened();
    if (ret < 0) {
        pthread_mutex_unlock(&g_lib_mutex);
        return ret;
    }

    memset(&xfer, 0, sizeof(xfer));
    xfer.len = request_len;
    xfer.timeout_ms = (int32_t)(timeout_ms ? timeout_ms : 5000);
    memcpy(xfer.data, request, request_len);

    if (ioctl(g_fd, AMBA_VIRT_IOC_RPC, &xfer) < 0) {
        ret = -errno;
        if (ret == -EBADF) {
            close(g_fd);
            g_fd = -1;
        }
        pthread_mutex_unlock(&g_lib_mutex);
        return ret;
    }

    if (xfer.len > cap) {
        pthread_mutex_unlock(&g_lib_mutex);
        return -EMSGSIZE;
    }

    memcpy(response, xfer.data, xfer.len);
    *response_len = xfer.len;

    pthread_mutex_unlock(&g_lib_mutex);
    return 0;
}

void amba_virt_close(void)
{
    pthread_mutex_lock(&g_lib_mutex);
    if (g_shm_virt != MAP_FAILED && g_shm_size > 0) {
        if (g_shm_phys != 0)
            munmap_device_memory(g_shm_virt, g_shm_size);
        else
            munmap(g_shm_virt, g_shm_size);
        g_shm_virt = MAP_FAILED;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    pthread_mutex_unlock(&g_lib_mutex);
}
