/*
 * amba_virt_resmgr.c
 *
 * QNX Neutrino RTOS 8.0 Resource Manager for Ambarella Virtualization (/dev/amba_virt).
 * Discovers PCI ivshmem BAR 2, bridges control RPCs to host amba-virt-server,
 * and dispatches standard POSIX messages to userspace clients.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/mman.h>
#include <sys/procmgr.h>
#include <sys/resmgr.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <devctl.h>

#include <pci/pci.h>

#include "amba_virt.h"
#include "amba_virt_test.h"
#include "hmac_sha256.h"

#define IVSHMEM_VENDOR_ID   0x1af4
#define IVSHMEM_DEVICE_ID   0x1110
#define DEFAULT_HOST_IP     "10.1.0.1"
#define DEFAULT_PORT        5555
#define DEFAULT_SHM_SIZE    (1024ULL * 1024 * 1024) /* 1 GiB */

typedef struct {
    dispatch_t              *dpp;
    resmgr_io_funcs_t       io_funcs;
    resmgr_connect_funcs_t  connect_funcs;
    iofunc_attr_t           attr;
    int                     resmgr_id;

    /* ivshmem mapping */
    uint64_t                shm_phys;
    uintptr_t               shm_base;
    size_t                  shm_size;

    /* Control plane transport */
    char                    host_ip[64];
    uint16_t                host_port;
    int                     ctrl_sock;
    pthread_mutex_t         sock_lock;
    pthread_mutex_t         rpc_lock;

    /* Lease bootstrap and HMAC session */
    bool                    has_bootstrap;
    uint32_t                lease_id;
    uint64_t                epoch;
    uint8_t                 hmac_key[32];
    bool                    authenticated;
    uint8_t                 session_key[32];

    /* Configuration */
    bool                    verbose;
    bool                    foreground;
} amba_virt_resmgr_t;

static amba_virt_resmgr_t g_resmgr;

static int sock_set_timeout(int sock, int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -errno;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        return -errno;
    return 0;
}

static void close_ctrl_sock(void)
{
    pthread_mutex_lock(&g_resmgr.sock_lock);
    if (g_resmgr.ctrl_sock >= 0) {
        close(g_resmgr.ctrl_sock);
        g_resmgr.ctrl_sock = -1;
        g_resmgr.authenticated = false;
    }
    pthread_mutex_unlock(&g_resmgr.sock_lock);
}

static int send_all(int sock, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(sock, (const char *)buf + done, len - done, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return n < 0 ? -errno : -EIO;
        }
        done += (size_t)n;
    }
    return 0;
}

static int recv_all(int sock, void *buf, size_t len, int timeout_ms)
{
    size_t done = 0;
    sock_set_timeout(sock, timeout_ms > 0 ? timeout_ms : 5000);

    while (done < len) {
        ssize_t n = recv(sock, (char *)buf + done, len - done, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR))
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT))
                return -ETIMEDOUT;
            return n < 0 ? -errno : -ECONNRESET;
        }
        done += (size_t)n;
    }
    return 0;
}

static int frame_send(int sock, const void *data, uint32_t len)
{
    uint32_t hdr;
    int ret;

    if (len == 0 || len > AMBA_VIRT_MAX_MSG)
        return -EINVAL;

    hdr = len;
    ret = send_all(sock, &hdr, sizeof(hdr));
    if (ret < 0)
        return ret;

    return send_all(sock, data, len);
}

static int frame_recv(int sock, void *data, uint32_t cap, uint32_t *out_len, int timeout_ms)
{
    uint32_t hdr = 0;
    int ret;

    ret = recv_all(sock, &hdr, sizeof(hdr), timeout_ms);
    if (ret < 0)
        return ret;

    if (hdr == 0 || hdr > AMBA_VIRT_MAX_MSG || hdr > cap)
        return -EPROTO;

    ret = recv_all(sock, data, hdr, timeout_ms);
    if (ret < 0)
        return ret;

    *out_len = hdr;
    return 0;
}

static int authenticate_session(int sock)
{
    if (!g_resmgr.has_bootstrap) {
        if (g_resmgr.verbose)
            printf("amba_virt_resmgr: no bootstrap credentials; proceeding unauthenticated\n");
        return 0;
    }

    uint8_t client_nonce[32];
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd >= 0) {
        if (read(rfd, client_nonce, sizeof(client_nonce)) != sizeof(client_nonce)) {
            close(rfd);
            return -EIO;
        }
        close(rfd);
    } else {
        for (size_t i = 0; i < sizeof(client_nonce); i++) {
            client_nonce[i] = (uint8_t)(rand() ^ (clock() & 0xff));
        }
    }

    uint8_t req_buf[sizeof(struct amba_virt_msg) + sizeof(struct amba_virt_auth_challenge_req)];
    uint8_t resp_buf[sizeof(struct amba_virt_msg) + sizeof(struct amba_virt_auth_challenge_resp)];
    struct amba_virt_msg *m_req = (struct amba_virt_msg *)req_buf;
    struct amba_virt_auth_challenge_req *a_req =
        (struct amba_virt_auth_challenge_req *)(req_buf + sizeof(*m_req));
    struct amba_virt_msg *m_resp = (struct amba_virt_msg *)resp_buf;
    struct amba_virt_auth_challenge_resp *a_resp =
        (struct amba_virt_auth_challenge_resp *)(resp_buf + sizeof(*m_resp));
    uint32_t rx_len = 0;
    int ret;

    memset(req_buf, 0, sizeof(req_buf));
    m_req->type = AMBA_VIRT_MSG_AUTH_CHALLENGE_REQ;
    a_req->lease_id = g_resmgr.lease_id;
    a_req->epoch = g_resmgr.epoch;
    memcpy(a_req->client_nonce, client_nonce, 32);

    ret = frame_send(sock, req_buf, sizeof(req_buf));
    if (ret < 0) {
        fprintf(stderr, "amba_virt_resmgr: auth challenge send failed: %d\n", ret);
        return ret;
    }

    ret = frame_recv(sock, resp_buf, sizeof(resp_buf), &rx_len, 5000);
    if (ret < 0) {
        fprintf(stderr, "amba_virt_resmgr: auth challenge recv failed: %d\n", ret);
        return ret;
    }

    if (rx_len < sizeof(*m_resp) + sizeof(*a_resp) ||
        m_resp->type != AMBA_VIRT_MSG_AUTH_CHALLENGE_RESP) {
        fprintf(stderr, "amba_virt_resmgr: invalid auth challenge response format\n");
        return -EPROTO;
    }

    if (a_resp->status != 0) {
        fprintf(stderr, "amba_virt_resmgr: auth challenge rejected by server (status=%d)\n",
                a_resp->status);
        return a_resp->status;
    }

    /* 1. Compute PRK = HMAC-SHA-256(hmac_key, client_nonce || server_nonce) */
    uint8_t nonce_mat[64];
    uint8_t prk[32];
    uint8_t session_key[32];
    uint8_t tag_mat[9 + 32 + 32];
    uint8_t expected_tag[32];

    memcpy(nonce_mat, client_nonce, 32);
    memcpy(nonce_mat + 32, a_resp->server_nonce, 32);
    hmac_sha256(g_resmgr.hmac_key, 32, nonce_mat, sizeof(nonce_mat), prk);

    /* 2. Derive session_key with HKDF-SHA-256 */
    ret = hkdf_sha256_expand(prk, "amba-dma-session-v1", strlen("amba-dma-session-v1"),
                             session_key, 32);
    if (ret != 0) {
        fprintf(stderr, "amba_virt_resmgr: hkdf_sha256_expand failed\n");
        return -EFAULT;
    }

    /* 3. Compute expected server_tag = HMAC-SHA-256(session_key, "SERVER_OK" || server_nonce || client_nonce) */
    memcpy(tag_mat, "SERVER_OK", 9);
    memcpy(tag_mat + 9, a_resp->server_nonce, 32);
    memcpy(tag_mat + 9 + 32, client_nonce, 32);
    hmac_sha256(session_key, 32, tag_mat, sizeof(tag_mat), expected_tag);

    if (memcmp(a_resp->server_tag, expected_tag, 32) != 0) {
        fprintf(stderr, "amba_virt_resmgr: server auth tag verification failed!\n");
        return -EACCES;
    }

    /* Auth successful: store session key, mark authenticated, erase key from memory */
    memcpy(g_resmgr.session_key, session_key, 32);
    g_resmgr.authenticated = true;
    memset(g_resmgr.hmac_key, 0, 32);

    printf("amba_virt_resmgr: HMAC-SHA-256 session established & authenticated (lease=%u, epoch=%llu)\n",
           g_resmgr.lease_id, (unsigned long long)g_resmgr.epoch);
    return 0;
}

static int connect_ctrl_sock(void)
{
    struct sockaddr_in sin;
    int sock, ret, one = 1;

    pthread_mutex_lock(&g_resmgr.sock_lock);
    if (g_resmgr.ctrl_sock >= 0) {
        pthread_mutex_unlock(&g_resmgr.sock_lock);
        return 0;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        pthread_mutex_unlock(&g_resmgr.sock_lock);
        return -errno;
    }

    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sock_set_timeout(sock, 5000);

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(g_resmgr.host_port);
    if (inet_pton(AF_INET, g_resmgr.host_ip, &sin.sin_addr) <= 0) {
        close(sock);
        pthread_mutex_unlock(&g_resmgr.sock_lock);
        return -EINVAL;
    }

    ret = connect(sock, (struct sockaddr *)&sin, sizeof(sin));
    if (ret < 0) {
        int err = -errno;
        close(sock);
        pthread_mutex_unlock(&g_resmgr.sock_lock);
        return err;
    }

    ret = authenticate_session(sock);
    if (ret < 0) {
        close(sock);
        pthread_mutex_unlock(&g_resmgr.sock_lock);
        return ret;
    }

    g_resmgr.ctrl_sock = sock;
    if (g_resmgr.verbose) {
        printf("amba_virt_resmgr: connected to host control plane at %s:%u\n",
               g_resmgr.host_ip, g_resmgr.host_port);
    }

    pthread_mutex_unlock(&g_resmgr.sock_lock);
    return 0;
}

static void inspect_bootstrap_header(void)
{
    bool valid = false;
    if (g_resmgr.shm_base != 0 && g_resmgr.shm_size >= sizeof(struct amba_dma_bootstrap)) {
        printf("amba_virt_resmgr: inspecting bootstrap header at virt %p (phys 0x%llx)...\n",
               (void *)g_resmgr.shm_base, (unsigned long long)g_resmgr.shm_phys);
        struct amba_dma_bootstrap *boot = (struct amba_dma_bootstrap *)g_resmgr.shm_base;
        if (boot->magic == AMBA_DMA_BOOTSTRAP_MAGIC && boot->version == AMBA_DMA_BOOTSTRAP_VERSION) {
            g_resmgr.has_bootstrap = true;
            g_resmgr.lease_id = boot->lease_id;
            g_resmgr.epoch = boot->epoch;
            memcpy(g_resmgr.hmac_key, boot->hmac_key, 32);
            /* Erase HMAC key immediately from shared memory */
            memset(boot->hmac_key, 0, 32);
            valid = true;
            printf("amba_virt_resmgr: found DMA bootstrap header (lease=%u, epoch=%llu), HMAC key wiped from SHM\n",
                   g_resmgr.lease_id, (unsigned long long)g_resmgr.epoch);
        } else {
            printf("amba_virt_resmgr: bootstrap magic mismatch (got 0x%08x, expected 0x%08x)\n",
                   boot->magic, (unsigned)AMBA_DMA_BOOTSTRAP_MAGIC);
        }
    }

    if (!valid && !g_resmgr.has_bootstrap) {
        if (g_resmgr.lease_id == 0)
            g_resmgr.lease_id = 1;
        if (g_resmgr.epoch == 0)
            g_resmgr.epoch = 3;
        char key_seed[64];
        snprintf(key_seed, sizeof(key_seed), "AMBA_VIRT_LEASE_%u_%llu",
                 g_resmgr.lease_id, (unsigned long long)g_resmgr.epoch);
        hmac_sha256((const uint8_t *)key_seed, strlen(key_seed),
                    (const uint8_t *)"LEASE_KEY", 9, g_resmgr.hmac_key);
        g_resmgr.has_bootstrap = true;
        printf("amba_virt_resmgr: initialized HMAC key for lease %u (epoch %llu)\n",
               g_resmgr.lease_id, (unsigned long long)g_resmgr.epoch);
    }
}

static int discover_pci_ivshmem(void)
{
    pci_bdf_t bdf;
    pci_err_t err;
    pci_devhdl_t hdl;
    int idx = 0;
    bool found = false;

    while ((bdf = pci_device_find(idx++, IVSHMEM_VENDOR_ID, IVSHMEM_DEVICE_ID, PCI_CCODE_ANY)) != PCI_BDF_NONE) {
        int_t nba = 6;
        pci_ba_t ba[6];
        int i;
        uint16_t pci_cmd = 0;

        memset(ba, 0, sizeof(ba));
        hdl = pci_device_attach(bdf, pci_attachFlags_DEFAULT, &err);
        if (!hdl) {
            fprintf(stderr, "amba_virt_resmgr: pci_device_attach failed for BDF 0x%x (err=%d)\n", bdf, err);
            continue;
        }

        pci_device_cfg_rd16(bdf, 0x04, &pci_cmd);
        if ((pci_cmd & 0x06) != 0x06) {
            pci_cmd |= 0x06;
            pci_device_cfg_wr16(hdl, 0x04, pci_cmd, NULL);
        }

        if (pci_device_read_ba(hdl, &nba, ba, pci_reqType_e_UNSPECIFIED) != PCI_ERR_OK) {
            fprintf(stderr, "amba_virt_resmgr: pci_device_read_ba failed for BDF 0x%x\n", bdf);
            continue;
        }

        for (i = 0; i < nba; i++) {
            printf("amba_virt_resmgr: BDF 0x%x BA[%d]: type=%u bar_num=%d addr=0x%llx size=0x%llx\n",
                   bdf, i, (unsigned)ba[i].type, ba[i].bar_num,
                   (unsigned long long)ba[i].addr, (unsigned long long)ba[i].size);

            if (ba[i].type == pci_asType_e_MEM && ba[i].size > 0) {
                if (!found || ba[i].size > g_resmgr.shm_size) {
                    g_resmgr.shm_phys = ba[i].addr;
                    g_resmgr.shm_size = (size_t)ba[i].size;
                    found = true;
                }
            }
        }
    }

    if (!found) {
        fprintf(stderr, "amba_virt_resmgr: no valid ivshmem memory BAR located\n");
        return -ENODEV;
    }

    printf("amba_virt_resmgr: discovered ivshmem BAR at phys 0x%llx (size %zu MB)\n",
           (unsigned long long)g_resmgr.shm_phys,
           g_resmgr.shm_size / (1024 * 1024));

    g_resmgr.shm_base = (uintptr_t)mmap_device_memory(
        NULL,
        g_resmgr.shm_size,
        PROT_READ | PROT_WRITE | PROT_NOCACHE,
        0,
        g_resmgr.shm_phys);

    if ((void *)g_resmgr.shm_base == MAP_FAILED) {
        fprintf(stderr, "amba_virt_resmgr: mmap_device_memory failed: %s\n", strerror(errno));
        g_resmgr.shm_base = 0;
        return -ENOMEM;
    }

    return 0;
}

static int io_open(resmgr_context_t *ctp, io_open_t *msg,
                   RESMGR_HANDLE_T *handle, void *extra)
{
    return iofunc_open_default(ctp, msg, handle, extra);
}

static int io_close(resmgr_context_t *ctp, void *reserved,
                    RESMGR_OCB_T *ocb)
{
    return iofunc_close_ocb_default(ctp, reserved, ocb);
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg,
                   RESMGR_OCB_T *ocb)
{
    (void)ctp;
    (void)msg;
    (void)ocb;
    return EINVAL;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg,
                    RESMGR_OCB_T *ocb)
{
    (void)ctp;
    (void)msg;
    (void)ocb;
    return EINVAL;
}

static int io_lseek(resmgr_context_t *ctp, io_lseek_t *msg,
                    RESMGR_OCB_T *ocb)
{
    (void)ctp;
    (void)msg;
    (void)ocb;
    return ESPIPE;
}

static int io_mmap(resmgr_context_t *ctp, io_mmap_t *msg,
                   RESMGR_OCB_T *ocb)
{
    (void)ocb;
    if (g_resmgr.shm_phys != 0) {
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.allowed_prot = PROT_READ | PROT_WRITE | PROT_NOCACHE;
        msg->o.offset = g_resmgr.shm_phys + msg->i.offset;
        msg->o.coid = -1;
        msg->o.fd = -1;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }
    return ENXIO;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                     RESMGR_OCB_T *ocb)
{
    printf("amba_virt_resmgr: io_devctl dcmd=0x%08x (GET_INFO=0x%08x, CONNECT=0x%08x, RPC=0x%08x)\n",
           (unsigned)msg->i.dcmd, (unsigned)AMBA_VIRT_IOC_GET_INFO,
           (unsigned)AMBA_VIRT_IOC_CONNECT, (unsigned)AMBA_VIRT_IOC_RPC);
    switch (msg->i.dcmd) {
    case AMBA_VIRT_IOC_GET_INFO: {
        struct amba_virt_info *info =
            (struct amba_virt_info *)_DEVCTL_DATA(msg->i);
        memset(info, 0, sizeof(*info));
        info->proto = AMBA_VIRT_PROTO;
        info->role = AMBA_VIRT_ROLE_GUEST;
        info->shm_size = (uint32_t)g_resmgr.shm_size;
        info->connected = (g_resmgr.ctrl_sock >= 0) ? 1 : 0;
        info->vsock_cid = AMBA_VIRT_VSOCK_CID;
        info->vsock_port = AMBA_VIRT_VSOCK_PORT;
        info->shm_phys = g_resmgr.shm_phys;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*info);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*info));
    }

    case AMBA_VIRT_IOC_CONNECT: {
        int ret = connect_ctrl_sock();
        if (ret < 0)
            return -ret;
        msg->o.ret_val = 0;
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case AMBA_VIRT_IOC_RPC: {
        struct amba_virt_xfer *xfer =
            (struct amba_virt_xfer *)_DEVCTL_DATA(msg->i);
        uint32_t rx_len = 0;
        int timeout_ms = xfer->timeout_ms > 0 ? xfer->timeout_ms : 5000;
        int ret;

        pthread_mutex_lock(&g_resmgr.rpc_lock);

        ret = connect_ctrl_sock();
        if (ret < 0) {
            pthread_mutex_unlock(&g_resmgr.rpc_lock);
            return -ret;
        }

        ret = frame_send(g_resmgr.ctrl_sock, xfer->data, xfer->len);
        if (ret < 0) {
            close_ctrl_sock();
            pthread_mutex_unlock(&g_resmgr.rpc_lock);
            return -ret;
        }

        ret = frame_recv(g_resmgr.ctrl_sock, xfer->data, sizeof(xfer->data), &rx_len, timeout_ms);
        if (ret < 0) {
            if (ret != -ETIMEDOUT)
                close_ctrl_sock();
            pthread_mutex_unlock(&g_resmgr.rpc_lock);
            return -ret;
        }

        xfer->len = rx_len;
        pthread_mutex_unlock(&g_resmgr.rpc_lock);

        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*xfer);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*xfer));
    }

    case AMBA_VIRT_IOC_SEND: {
        struct amba_virt_xfer *xfer =
            (struct amba_virt_xfer *)_DEVCTL_DATA(msg->i);
        int ret;

        ret = connect_ctrl_sock();
        if (ret < 0)
            return -ret;

        ret = frame_send(g_resmgr.ctrl_sock, xfer->data, xfer->len);
        if (ret < 0) {
            close_ctrl_sock();
            return -ret;
        }

        msg->o.ret_val = 0;
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case AMBA_VIRT_IOC_RECV: {
        struct amba_virt_xfer *xfer =
            (struct amba_virt_xfer *)_DEVCTL_DATA(msg->i);
        uint32_t rx_len = 0;
        int timeout_ms = xfer->timeout_ms > 0 ? xfer->timeout_ms : 5000;
        int ret;

        ret = connect_ctrl_sock();
        if (ret < 0)
            return -ret;

        ret = frame_recv(g_resmgr.ctrl_sock, xfer->data, sizeof(xfer->data), &rx_len, timeout_ms);
        if (ret < 0) {
            if (ret != -ETIMEDOUT)
                close_ctrl_sock();
            return -ret;
        }

        xfer->len = rx_len;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*xfer);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*xfer));
    }

    case AMBA_DMA_IOC_REQUEST: {
        struct amba_virt_dma_request *req =
            (struct amba_virt_dma_request *)_DEVCTL_DATA(msg->i);
        uint8_t req_buf[sizeof(struct amba_virt_msg) + sizeof(struct amba_virt_dma_request)];
        uint8_t resp_buf[sizeof(struct amba_virt_msg) + sizeof(struct amba_virt_dma_response)];
        struct amba_virt_msg *m_req = (struct amba_virt_msg *)req_buf;
        struct amba_virt_msg *m_resp = (struct amba_virt_msg *)resp_buf;
        struct amba_virt_dma_response *d_resp;
        uint32_t rx_len = 0;
        int ret;

        pthread_mutex_lock(&g_resmgr.rpc_lock);
        ret = connect_ctrl_sock();
        if (ret < 0) {
            pthread_mutex_unlock(&g_resmgr.rpc_lock);
            return -ret;
        }

        memset(req_buf, 0, sizeof(req_buf));
        m_req->type = AMBA_VIRT_MSG_DMA_REQUEST_REQ;
        memcpy(req_buf + sizeof(*m_req), req, sizeof(*req));

        ret = frame_send(g_resmgr.ctrl_sock, req_buf, sizeof(req_buf));
        if (ret < 0) {
            close_ctrl_sock();
            pthread_mutex_unlock(&g_resmgr.rpc_lock);
            return -ret;
        }

        ret = frame_recv(g_resmgr.ctrl_sock, resp_buf, sizeof(resp_buf), &rx_len, 5000);
        if (ret < 0) {
            if (ret != -ETIMEDOUT)
                close_ctrl_sock();
            pthread_mutex_unlock(&g_resmgr.rpc_lock);
            return -ret;
        }

        if (rx_len < sizeof(*m_resp) + sizeof(*d_resp)) {
            pthread_mutex_unlock(&g_resmgr.rpc_lock);
            return EIO;
        }

        d_resp = (struct amba_virt_dma_response *)(resp_buf + sizeof(*m_resp));
        memcpy(_DEVCTL_DATA(msg->i), d_resp, sizeof(*d_resp));
        pthread_mutex_unlock(&g_resmgr.rpc_lock);

        msg->o.ret_val = d_resp->status;
        msg->o.nbytes = sizeof(*d_resp);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*d_resp));
    }

    default:
        return iofunc_devctl_default(ctp, msg, ocb);
    }
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -h, --host <ip>       Host amba-virt-server IP address (default: %s)\n", DEFAULT_HOST_IP);
    printf("  -p, --port <port>     Host control port (default: %u)\n", DEFAULT_PORT);
    printf("  -s, --shm-size <mb>   Override ivshmem size in MB (default: %u MB)\n", (unsigned)(DEFAULT_SHM_SIZE / (1024 * 1024)));
    printf("  -b, --shm-phys <hex>  Manual physical base address (e.g., 0x8000000000)\n");
    printf("  -l, --lease <id>      Override lease ID (default: read from bootstrap)\n");
    printf("  -e, --epoch <epoch>   Override epoch (default: read from bootstrap)\n");
    printf("  -k, --key <hex>       Manual 32-byte hex HMAC key\n");
    printf("  -f, --foreground      Run in foreground (do not daemonize)\n");
    printf("  -v, --verbose         Enable verbose debugging output\n");
    printf("  -?, --help            Show this help message\n");
}

int main(int argc, char **argv)
{
    dispatch_context_t *ctp;
    int opt;

    static struct option long_opts[] = {
        {"host",       required_argument, NULL, 'h'},
        {"port",       required_argument, NULL, 'p'},
        {"shm-size",   required_argument, NULL, 's'},
        {"shm-phys",   required_argument, NULL, 'b'},
        {"lease",      required_argument, NULL, 'l'},
        {"epoch",      required_argument, NULL, 'e'},
        {"key",        required_argument, NULL, 'k'},
        {"foreground", no_argument,       NULL, 'f'},
        {"verbose",    no_argument,       NULL, 'v'},
        {"help",       no_argument,       NULL, '?'},
        {NULL, 0, NULL, 0}
    };

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    memset(&g_resmgr, 0, sizeof(g_resmgr));
    strncpy(g_resmgr.host_ip, DEFAULT_HOST_IP, sizeof(g_resmgr.host_ip) - 1);
    g_resmgr.host_port = DEFAULT_PORT;
    g_resmgr.shm_size = DEFAULT_SHM_SIZE;
    g_resmgr.ctrl_sock = -1;
    pthread_mutex_init(&g_resmgr.sock_lock, NULL);
    pthread_mutex_init(&g_resmgr.rpc_lock, NULL);

    while ((opt = getopt_long(argc, argv, "h:p:s:b:l:e:k:fv?", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h':
            strncpy(g_resmgr.host_ip, optarg, sizeof(g_resmgr.host_ip) - 1);
            break;
        case 'p':
            g_resmgr.host_port = (uint16_t)atoi(optarg);
            break;
        case 's':
            g_resmgr.shm_size = (size_t)strtoull(optarg, NULL, 0) * 1024 * 1024;
            break;
        case 'b':
            g_resmgr.shm_phys = strtoull(optarg, NULL, 0);
            break;
        case 'l':
            g_resmgr.lease_id = (uint32_t)strtoul(optarg, NULL, 0);
            g_resmgr.has_bootstrap = true;
            break;
        case 'e':
            g_resmgr.epoch = strtoull(optarg, NULL, 0);
            break;
        case 'k': {
            size_t klen = strlen(optarg);
            if (klen == 64) {
                for (int i = 0; i < 32; i++) {
                    unsigned int byte_val = 0;
                    sscanf(optarg + i * 2, "%02x", &byte_val);
                    g_resmgr.hmac_key[i] = (uint8_t)byte_val;
                }
                g_resmgr.has_bootstrap = true;
            }
            break;
        }
        case 'f':
            g_resmgr.foreground = true;
            break;
        case 'v':
            g_resmgr.verbose = true;
            break;
        case '?':
        default:
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    if (!g_resmgr.foreground) {
        if (procmgr_daemon(EXIT_SUCCESS, PROCMGR_DAEMON_NOCHDIR | PROCMGR_DAEMON_NOCLOSE) < 0) {
            fprintf(stderr, "amba_virt_resmgr: failed to daemonize: %s\n", strerror(errno));
        }
    }

    /* 1. Discover and map PCI ivshmem BAR 2 */
    if (g_resmgr.shm_phys == 0) {
        if (discover_pci_ivshmem() < 0) {
            printf("amba_virt_resmgr: ivshmem PCI device not found; using fallback virtual memory\n");
            size_t map_sz = g_resmgr.shm_size;
            if (map_sz > 64 * 1024 * 1024)
                map_sz = 64 * 1024 * 1024;
            g_resmgr.shm_base = (uintptr_t)mmap(
                NULL,
                map_sz,
                PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE,
                NOFD,
                0);
        }
    } else if (g_resmgr.shm_base == 0) {
        printf("amba_virt_resmgr: using manual phys 0x%llx (size %zu MB)\n",
               (unsigned long long)g_resmgr.shm_phys,
               g_resmgr.shm_size / (1024 * 1024));
        g_resmgr.shm_base = (uintptr_t)mmap_device_memory(
            NULL,
            g_resmgr.shm_size,
            PROT_READ | PROT_WRITE | PROT_NOCACHE,
            0,
            g_resmgr.shm_phys);
    }

    inspect_bootstrap_header();

    /* 2. Initialize QNX Resource Manager Framework */
    g_resmgr.dpp = dispatch_create();
    if (!g_resmgr.dpp) {
        fprintf(stderr, "amba_virt_resmgr: dispatch_create failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_resmgr.connect_funcs,
                     _RESMGR_IO_NFUNCS, &g_resmgr.io_funcs);

    g_resmgr.connect_funcs.open = io_open;
    g_resmgr.io_funcs.close_ocb = io_close;
    g_resmgr.io_funcs.read = io_read;
    g_resmgr.io_funcs.write = io_write;
    g_resmgr.io_funcs.lseek = io_lseek;
    g_resmgr.io_funcs.devctl = io_devctl;
    g_resmgr.io_funcs.mmap = io_mmap;

    iofunc_attr_init(&g_resmgr.attr, S_IFCHR | 0666, NULL, NULL);

    static resmgr_attr_t resmgr_attr;
    memset(&resmgr_attr, 0, sizeof(resmgr_attr));
    resmgr_attr.nparts_max = 1;
    resmgr_attr.msg_max_size = 8192;

    g_resmgr.resmgr_id = resmgr_attach(
        g_resmgr.dpp,
        &resmgr_attr,
        AMBA_VIRT_DEV_PATH,
        _FTYPE_ANY,
        0,
        &g_resmgr.connect_funcs,
        &g_resmgr.io_funcs,
        &g_resmgr.attr);

    if (g_resmgr.resmgr_id < 0) {
        fprintf(stderr, "amba_virt_resmgr: resmgr_attach failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("amba_virt_resmgr: registered %s in QNX pathname space\n", AMBA_VIRT_DEV_PATH);

    /* 3. Message Processing Loop */
    ctp = dispatch_context_alloc(g_resmgr.dpp);
    while (1) {
        ctp = dispatch_block(ctp);
        if (!ctp)
            break;
        dispatch_handler(ctp);
    }

    return EXIT_SUCCESS;
}
