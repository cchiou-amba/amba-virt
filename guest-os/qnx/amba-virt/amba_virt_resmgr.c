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
    }
    pthread_mutex_unlock(&g_resmgr.sock_lock);
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

    g_resmgr.ctrl_sock = sock;
    if (g_resmgr.verbose) {
        printf("amba_virt_resmgr: connected to host control plane at %s:%u\n",
               g_resmgr.host_ip, g_resmgr.host_port);
    }

    pthread_mutex_unlock(&g_resmgr.sock_lock);
    return 0;
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

static int discover_pci_ivshmem(void)
{
    pci_bdf_t bdf;
    pci_err_t err;
    pci_devhdl_t hdl;
    int_t nba = 6;
    pci_ba_t ba[6];
    int i;
    bool found = false;

    memset(ba, 0, sizeof(ba));

    bdf = pci_device_find(0, IVSHMEM_VENDOR_ID, IVSHMEM_DEVICE_ID, PCI_CCODE_ANY);
    if (bdf == PCI_BDF_NONE) {
        if (g_resmgr.verbose)
            printf("amba_virt_resmgr: PCI ivshmem (0x%04x:0x%04x) not found via pci_device_find()\n",
                   IVSHMEM_VENDOR_ID, IVSHMEM_DEVICE_ID);
        return -ENODEV;
    }

    hdl = pci_device_attach(bdf, pci_attachFlags_DEFAULT, &err);
    if (!hdl) {
        fprintf(stderr, "amba_virt_resmgr: pci_device_attach failed (err=%d)\n", err);
        return -EIO;
    }

    if (pci_device_read_ba(hdl, &nba, ba, pci_reqType_e_MANDATORY) != PCI_ERR_OK) {
        fprintf(stderr, "amba_virt_resmgr: pci_device_read_ba failed\n");
        return -EIO;
    }

    for (i = 0; i < nba; i++) {
        if (ba[i].type == pci_asType_e_MEM && ba[i].size > 0) {
            /* Look specifically for BAR 2 or the largest memory window */
            if (ba[i].bar_num == 2 || ba[i].size > g_resmgr.shm_size) {
                g_resmgr.shm_phys = ba[i].addr;
                g_resmgr.shm_size = (size_t)ba[i].size;
                found = true;
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
        {"foreground", no_argument,       NULL, 'f'},
        {"verbose",    no_argument,       NULL, 'v'},
        {"help",       no_argument,       NULL, '?'},
        {NULL, 0, NULL, 0}
    };

    memset(&g_resmgr, 0, sizeof(g_resmgr));
    strncpy(g_resmgr.host_ip, DEFAULT_HOST_IP, sizeof(g_resmgr.host_ip) - 1);
    g_resmgr.host_port = DEFAULT_PORT;
    g_resmgr.shm_size = DEFAULT_SHM_SIZE;
    g_resmgr.ctrl_sock = -1;
    pthread_mutex_init(&g_resmgr.sock_lock, NULL);
    pthread_mutex_init(&g_resmgr.rpc_lock, NULL);

    while ((opt = getopt_long(argc, argv, "h:p:s:b:fv?", long_opts, NULL)) != -1) {
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
