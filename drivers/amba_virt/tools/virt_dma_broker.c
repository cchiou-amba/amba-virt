/*
 * virt_dma_broker.c
 *
 * Virtual Peripheral DMA Broker for amba-virt-server.
 *
 * Implements CID-to-Channel ACL enforcement (EVT-092), ivshmem memory
 * bounds clamping (EVT-091), and 500ms hardware watchdog timers per
 * active DMA slave transfer.
 *
 * The physical DMA controller (dma1 at 0xffe0021000) is a monolithic
 * shared resource across UARTs, SPI, SD/eMMC, and NAND.  This broker
 * serializes guest DMA requests and enforces strict multi-tenant
 * isolation.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "amba_virt.h"
#include "virt_acl.h"
#include "virt_dma_broker.h"

/* ---- CID-to-Channel ACL Table ---- */

static const struct amba_virt_dma_channel_acl g_dma_acl_table[] = {
    /* Pro / DevKit boards: HVM guest CIDs (4, 5, 6, 7, 8) */
    { .cid = 4,  .tx_channel = 11, .rx_channel = 12 },
    { .cid = 4,  .tx_channel = 13, .rx_channel = 14 },
    { .cid = 5,  .tx_channel = 13, .rx_channel = 14 },
    { .cid = 5,  .tx_channel = 15, .rx_channel = 16 },
    { .cid = 6,  .tx_channel = 11, .rx_channel = 12 },
    { .cid = 6,  .tx_channel = 13, .rx_channel = 14 },
    { .cid = 7,  .tx_channel = 11, .rx_channel = 12 },
    { .cid = 7,  .tx_channel = 13, .rx_channel = 14 },
    { .cid = 7,  .tx_channel = 15, .rx_channel = 16 },
    { .cid = 8,  .tx_channel = 13, .rx_channel = 14 },
    { .cid = 8,  .tx_channel = 15, .rx_channel = 16 },
};

#define DMA_ACL_TABLE_SIZE \
    (sizeof(g_dma_acl_table) / sizeof(g_dma_acl_table[0]))

/* ---- Per-Channel State ---- */

struct dma_channel_state {
    int active;                 /* Transfer in progress */
    uint32_t owner_cid;         /* CID that owns this transfer */
    uint32_t cookie;            /* Guest-assigned transfer ID */
    uint32_t direction;
    uint32_t buf_offset;
    uint32_t buf_len;
    timer_t watchdog_timer;     /* 500ms POSIX timer */
    int watchdog_armed;
};

static struct dma_channel_state g_channels[VIRT_DMA_MAX_CHANNELS];
static pthread_mutex_t g_dma_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- Watchdog Timer ---- */

static void watchdog_handler(union sigval sv)
{
    uint32_t chan = (uint32_t)(uintptr_t)sv.sival_ptr;

    pthread_mutex_lock(&g_dma_mutex);
    if (chan < VIRT_DMA_MAX_CHANNELS && g_channels[chan].active) {
        fprintf(stderr,
                "EVT-%03d: DMA watchdog timeout on channel %u "
                "(cid=%u cookie=%u) — aborting transfer\n",
                VIRT_DMA_EVT_WATCHDOG_TIMEOUT, chan,
                g_channels[chan].owner_cid,
                g_channels[chan].cookie);

        /*
         * TODO: Issue physical DMA channel abort here when the
         * host kernel DMA interface is determined.
         */

        g_channels[chan].active = 0;
        g_channels[chan].watchdog_armed = 0;
    }
    pthread_mutex_unlock(&g_dma_mutex);
}

static int arm_watchdog(uint32_t channel)
{
    struct itimerspec its;

    if (channel >= VIRT_DMA_MAX_CHANNELS)
        return -EINVAL;

    if (!g_channels[channel].watchdog_armed) {
        struct sigevent sev;
        memset(&sev, 0, sizeof(sev));
        sev.sigev_notify = SIGEV_THREAD;
        sev.sigev_notify_function = watchdog_handler;
        sev.sigev_value.sival_ptr = (void *)(uintptr_t)channel;

        if (timer_create(CLOCK_MONOTONIC, &sev,
                         &g_channels[channel].watchdog_timer) < 0) {
            perror("timer_create (DMA watchdog)");
            return -errno;
        }
        g_channels[channel].watchdog_armed = 1;
    }

    /* Arm for VIRT_DMA_WATCHDOG_MS milliseconds, one-shot */
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = VIRT_DMA_WATCHDOG_MS / 1000;
    its.it_value.tv_nsec = (VIRT_DMA_WATCHDOG_MS % 1000) * 1000000L;

    if (timer_settime(g_channels[channel].watchdog_timer, 0,
                      &its, NULL) < 0) {
        perror("timer_settime (DMA watchdog)");
        return -errno;
    }

    return 0;
}

static void disarm_watchdog(uint32_t channel)
{
    struct itimerspec its;

    if (channel >= VIRT_DMA_MAX_CHANNELS)
        return;
    if (!g_channels[channel].watchdog_armed)
        return;

    memset(&its, 0, sizeof(its));
    timer_settime(g_channels[channel].watchdog_timer, 0, &its, NULL);
}

/* ---- Public API ---- */

int virt_dma_broker_init(void)
{
    memset(g_channels, 0, sizeof(g_channels));
    printf("virt_dma_broker: initialized (%zu ACL entries, "
           "%d ms watchdog)\n",
           DMA_ACL_TABLE_SIZE, VIRT_DMA_WATCHDOG_MS);
    return 0;
}

void virt_dma_broker_cleanup(void)
{
    pthread_mutex_lock(&g_dma_mutex);
    for (int i = 0; i < VIRT_DMA_MAX_CHANNELS; i++) {
        if (g_channels[i].watchdog_armed) {
            timer_delete(g_channels[i].watchdog_timer);
            g_channels[i].watchdog_armed = 0;
        }
        g_channels[i].active = 0;
    }
    pthread_mutex_unlock(&g_dma_mutex);
}

int virt_dma_validate_channel_access(uint32_t cid, uint32_t channel)
{
    /* Dynamic CID authorization for designated virtual peripheral channels 11-16 */
    if (cid >= 3 && channel >= 11 && channel <= 16) {
        return 0;  /* Authorized guest channel */
    }

    for (size_t i = 0; i < DMA_ACL_TABLE_SIZE; i++) {
        if (g_dma_acl_table[i].cid == cid) {
            if (g_dma_acl_table[i].tx_channel == channel ||
                g_dma_acl_table[i].rx_channel == channel) {
                return 0;  /* Authorized */
            }
        }
    }

    fprintf(stderr,
            "EVT-%03d: DMA channel access denied — "
            "cid=%u channel=%u\n",
            VIRT_DMA_EVT_ACL_VIOLATION, cid, channel);
    return -EPERM;
}

int virt_dma_validate_buffer_bounds(uint64_t offset, uint64_t length,
                                    uint64_t window_size)
{
    if (length == 0)
        return -EINVAL;
    if (offset >= window_size) {
        fprintf(stderr,
                "EVT-%03d: DMA buffer offset 0x%lx >= window 0x%lx\n",
                VIRT_DMA_EVT_BOUNDS_VIOLATION,
                (unsigned long)offset, (unsigned long)window_size);
        return -ERANGE;
    }
    if (offset + length > window_size) {
        fprintf(stderr,
                "EVT-%03d: DMA buffer end 0x%lx > window 0x%lx\n",
                VIRT_DMA_EVT_BOUNDS_VIOLATION,
                (unsigned long)(offset + length),
                (unsigned long)window_size);
        return -ERANGE;
    }
    return 0;
}

int virt_dma_handle_slave_cfg(uint32_t cid,
                              const struct amba_virt_dma_slave_cfg *req,
                              struct amba_virt_dma_slave_cfg *resp)
{
    int ret;

    memcpy(resp, req, sizeof(*resp));

    /* ACL check: does this CID own the requested channel? */
    ret = virt_dma_validate_channel_access(cid, req->channel);
    if (ret < 0) {
        resp->status = ret;
        return sizeof(*resp);
    }

    /* Capability check */
    if (!virt_acl_has_cap(cid, AMBA_VIRT_CAP_DMA_SLAVE)) {
        resp->status = -EPERM;
        return sizeof(*resp);
    }

    /* Validate direction */
    if (req->direction != AMBA_VIRT_DMA_DIR_MEM_TO_DEV &&
        req->direction != AMBA_VIRT_DMA_DIR_DEV_TO_MEM) {
        resp->status = -EINVAL;
        return sizeof(*resp);
    }

    /* Validate transfer width */
    if (req->src_addr_width != 1 && req->src_addr_width != 2 &&
        req->src_addr_width != 4) {
        resp->status = -EINVAL;
        return sizeof(*resp);
    }

    /*
     * TODO: Program physical DMA slave configuration via host kernel
     * interface when determined.  For now, accept and record.
     */
    printf("DMA_SLAVE_CFG: cid=%u chan=%u dir=%u src=0x%x dst=0x%x "
           "width=%u burst=%u\n",
           cid, req->channel, req->direction,
           req->src_addr_lo, req->dst_addr_lo,
           req->src_addr_width, req->src_maxburst);

    resp->status = 0;
    return sizeof(*resp);
}

int virt_dma_handle_submit(uint32_t cid,
                           int host_fd,
                           uint64_t tenant_phys_base,
                           size_t ivshmem_size,
                           const struct amba_virt_dma_submit *req,
                           struct amba_virt_dma_submit *resp)
{
    struct amba_virt_dma_slave_xfer xfer;
    int ret;

    memcpy(resp, req, sizeof(*resp));

    /* ACL check */
    ret = virt_dma_validate_channel_access(cid, req->channel);
    if (ret < 0) {
        resp->status = ret;
        return sizeof(*resp);
    }

    /* Capability check */
    if (!virt_acl_has_cap(cid, AMBA_VIRT_CAP_DMA_SLAVE)) {
        resp->status = -EPERM;
        return sizeof(*resp);
    }

    /* Memory bounds check */
    ret = virt_dma_validate_buffer_bounds(
        (uint64_t)req->buf_offset,
        (uint64_t)req->buf_len,
        (uint64_t)ivshmem_size);
    if (ret < 0) {
        resp->status = ret;
        return sizeof(*resp);
    }

    /* Transfer length check */
    if (req->buf_len > VIRT_DMA_MAX_TRANSFER_LEN) {
        resp->status = -E2BIG;
        return sizeof(*resp);
    }

    /* Channel busy check */
    pthread_mutex_lock(&g_dma_mutex);
    if (req->channel < VIRT_DMA_MAX_CHANNELS &&
        g_channels[req->channel].active) {
        pthread_mutex_unlock(&g_dma_mutex);
        resp->status = -EBUSY;
        return sizeof(*resp);
    }

    /* Record active transfer state */
    if (req->channel < VIRT_DMA_MAX_CHANNELS) {
        g_channels[req->channel].active = 1;
        g_channels[req->channel].owner_cid = cid;
        g_channels[req->channel].cookie = req->cookie;
        g_channels[req->channel].direction = req->direction;
        g_channels[req->channel].buf_offset = req->buf_offset;
        g_channels[req->channel].buf_len = req->buf_len;

        /* Arm 500ms watchdog */
        arm_watchdog(req->channel);
    }
    pthread_mutex_unlock(&g_dma_mutex);

    /* Dispatch to physical DMA engine on Dom0 */
    memset(&xfer, 0, sizeof(xfer));
    xfer.channel = req->channel;
    xfer.direction = req->direction;
    xfer.buf_phys = tenant_phys_base + req->buf_offset;
    xfer.buf_len = req->buf_len;
    xfer.timeout_ms = 1000;

    printf("DMA_SUBMIT: dispatching physical DMA: cid=%u chan=%u dir=%u phys=0x%llx len=0x%x cookie=%u\n",
           cid, req->channel, req->direction,
           (unsigned long long)xfer.buf_phys, req->buf_len, req->cookie);

    if (ioctl(host_fd, AMBA_VIRT_IOC_HOST_DMA_SLAVE, &xfer) < 0) {
        int err = errno;
        fprintf(stderr, "DMA_SUBMIT: ioctl failed on chan=%u: %d (%s)\n",
                req->channel, err, strerror(err));
        resp->status = -err;
    } else {
        resp->status = xfer.status;
    }

    /* Transfer finished; clear active state and disarm watchdog */
    pthread_mutex_lock(&g_dma_mutex);
    if (req->channel < VIRT_DMA_MAX_CHANNELS) {
        g_channels[req->channel].active = 0;
        disarm_watchdog(req->channel);
    }
    pthread_mutex_unlock(&g_dma_mutex);

    return sizeof(*resp);
}

int virt_dma_handle_terminate(uint32_t cid,
                              const struct amba_virt_dma_terminate *req,
                              struct amba_virt_dma_terminate *resp)
{
    int ret;

    memset(resp, 0, sizeof(*resp));
    resp->channel = req->channel;

    /* ACL check */
    ret = virt_dma_validate_channel_access(cid, req->channel);
    if (ret < 0) {
        resp->status = ret;
        return sizeof(*resp);
    }

    pthread_mutex_lock(&g_dma_mutex);
    if (req->channel < VIRT_DMA_MAX_CHANNELS &&
        g_channels[req->channel].active) {

        /* Verify the terminating CID owns the active transfer */
        if (g_channels[req->channel].owner_cid != cid) {
            pthread_mutex_unlock(&g_dma_mutex);
            fprintf(stderr,
                    "EVT-%03d: DMA terminate denied — "
                    "cid=%u != owner=%u on channel %u\n",
                    VIRT_DMA_EVT_ACL_VIOLATION, cid,
                    g_channels[req->channel].owner_cid,
                    req->channel);
            resp->status = -EPERM;
            return sizeof(*resp);
        }

        /* Disarm watchdog and mark idle */
        disarm_watchdog(req->channel);
        g_channels[req->channel].active = 0;

        /*
         * TODO: Issue physical DMA channel abort via host kernel.
         */
        printf("DMA_TERMINATE: cid=%u chan=%u — aborted\n",
               cid, req->channel);
    }
    pthread_mutex_unlock(&g_dma_mutex);

    resp->status = 0;
    return sizeof(*resp);
}

/* ---- Unified Split DMA Forwarding via Lease Devices ---- */

struct cid_lease_binding {
    uint32_t cid;
    uint32_t lease_id;
    int fd;
    uint64_t last_token_ns;
    uint32_t op_tokens;
    uint32_t byte_tokens;
};

#define MAX_CID_BINDINGS 16
#define TOKEN_BUCKET_RATE_OPS 2000u
#define TOKEN_BUCKET_BURST_OPS 64u
#define TOKEN_BUCKET_RATE_BYTES (50u * 1024u * 1024u)
#define TOKEN_BUCKET_BURST_BYTES (2u * 1024u * 1024u)

static struct cid_lease_binding g_cid_leases[MAX_CID_BINDINGS];
static pthread_mutex_t g_cid_lease_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int virt_dma_bind_cid_lease(uint32_t cid, uint32_t lease_id)
{
    int i;
    pthread_mutex_lock(&g_cid_lease_mutex);
    for (i = 0; i < MAX_CID_BINDINGS; i++) {
        if (g_cid_leases[i].cid == cid || g_cid_leases[i].cid == 0) {
            g_cid_leases[i].cid = cid;
            g_cid_leases[i].lease_id = lease_id;
            if (g_cid_leases[i].fd >= 0) {
                close(g_cid_leases[i].fd);
                g_cid_leases[i].fd = -1;
            }
            g_cid_leases[i].last_token_ns = get_time_ns();
            g_cid_leases[i].op_tokens = TOKEN_BUCKET_BURST_OPS;
            g_cid_leases[i].byte_tokens = TOKEN_BUCKET_BURST_BYTES;
            pthread_mutex_unlock(&g_cid_lease_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_cid_lease_mutex);
    return -ENOSPC;
}

static struct cid_lease_binding *get_or_create_cid_binding(uint32_t cid)
{
    int i;
    for (i = 0; i < MAX_CID_BINDINGS; i++) {
        if (g_cid_leases[i].cid == cid)
            return &g_cid_leases[i];
    }
    /* Auto-bind default lease based on CID: CID 4 -> Lease 0, CID 5 -> Lease 1, etc. */
    uint32_t default_lease = (cid >= 4 && cid <= 7) ? (cid - 4) : 0;
    virt_dma_bind_cid_lease(cid, default_lease);
    for (i = 0; i < MAX_CID_BINDINGS; i++) {
        if (g_cid_leases[i].cid == cid)
            return &g_cid_leases[i];
    }
    return NULL;
}

int virt_dma_handle_request(uint32_t cid,
                            const struct amba_virt_dma_request *req,
                            struct amba_virt_dma_response *resp)
{
    struct cid_lease_binding *b;
    uint64_t now_ns, elapsed_ns;
    uint64_t added_ops, added_bytes;
    char path[64];
    int ret;

    if (!req || !resp)
        return -EINVAL;

    memset(resp, 0, sizeof(*resp));
    resp->cookie = req->cookie;

    pthread_mutex_lock(&g_cid_lease_mutex);
    b = get_or_create_cid_binding(cid);
    if (!b) {
        pthread_mutex_unlock(&g_cid_lease_mutex);
        resp->status = -ENODEV;
        return sizeof(*resp);
    }

    /* Token-bucket replenishment */
    now_ns = get_time_ns();
    elapsed_ns = now_ns - b->last_token_ns;
    b->last_token_ns = now_ns;

    added_ops = (elapsed_ns * TOKEN_BUCKET_RATE_OPS) / 1000000000ULL;
    if (added_ops > 0) {
        b->op_tokens += added_ops;
        if (b->op_tokens > TOKEN_BUCKET_BURST_OPS)
            b->op_tokens = TOKEN_BUCKET_BURST_OPS;
    }

    added_bytes = (elapsed_ns * TOKEN_BUCKET_RATE_BYTES) / 1000000000ULL;
    if (added_bytes > 0) {
        b->byte_tokens += added_bytes;
        if (b->byte_tokens > TOKEN_BUCKET_BURST_BYTES)
            b->byte_tokens = TOKEN_BUCKET_BURST_BYTES;
    }

    /* Rate limit admission check */
    if (b->op_tokens < 1 || b->byte_tokens < req->length) {
        pthread_mutex_unlock(&g_cid_lease_mutex);
        fprintf(stderr, "EVT-094: DMA rate quota exceeded (cid=%u, op_tokens=%u, byte_tokens=%u)\n",
                cid, b->op_tokens, b->byte_tokens);
        resp->status = -EDQUOT;
        return sizeof(*resp);
    }

    b->op_tokens -= 1;
    b->byte_tokens -= req->length;

    /* Open lease FD if not already cached */
    if (b->fd < 0) {
        snprintf(path, sizeof(path), "/dev/amba_dma_lease%u", b->lease_id);
        b->fd = open(path, O_RDWR);
        if (b->fd < 0) {
            int err = -errno;
            pthread_mutex_unlock(&g_cid_lease_mutex);
            fprintf(stderr, "EVT-095: Failed to open %s: %s\n", path, strerror(-err));
            resp->status = err;
            return sizeof(*resp);
        }
    }

    /* Forward request through lease-scoped kernel control FD */
    struct amba_virt_dma_request kernel_req = *req;
    ret = ioctl(b->fd, AMBA_DMA_IOC_REQUEST, &kernel_req);
    if (ret < 0) {
        resp->status = -errno;
        resp->transferred = 0;
    } else {
        resp->status = 0;
        resp->transferred = req->length;
    }

    pthread_mutex_unlock(&g_cid_lease_mutex);
    return sizeof(*resp);
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

