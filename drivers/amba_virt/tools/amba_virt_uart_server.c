/*
 * amba_virt_uart_server.c
 *
 * Ambarella ivshmem-doorbell Host Bridge Daemon for Physical UART Virtualization.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "amba_virt_uart_uapi.h"

#define IVSHMEM_PROTOCOL_VERSION 0LL
#define MAX_EPOLL_EVENTS 16

enum fd_type {
    FD_TYPE_SRV = 1,
    FD_TYPE_CLIENT,
    FD_TYPE_ACK,
};

struct event_data {
    enum fd_type type;
    struct uart_instance *inst;
};

struct uart_instance {
    int uart_id;
    char dev_path[128];
    char sock_path[128];
    int dev_fd;
    int srv_sock_fd;
    int client_fd;
    int irq_efd;
    int ack_efd;
    bool active;
    struct event_data ev_srv;
    struct event_data ev_client;
    struct event_data ev_ack;
};

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static int send_fd_msg(int sock, int64_t num, int fd)
{
    struct msghdr msg = {0};
    struct iovec iov[1];
    char buf[CMSG_SPACE(sizeof(int))];

    iov[0].iov_base = &num;
    iov[0].iov_len = sizeof(num);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    if (fd >= 0) {
        msg.msg_control = buf;
        msg.msg_controllen = sizeof(buf);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }

    ssize_t ret = sendmsg(sock, &msg, MSG_NOSIGNAL);
    if (ret < 0) {
        fprintf(stderr, "send_fd_msg failed (num=%lld, fd=%d): %s\n",
                (long long)num, fd, strerror(errno));
        return -1;
    }
    return 0;
}

static int init_uart_instance(struct uart_instance *inst, int uart_id, const char *dev_override, const char *sock_override)
{
    inst->uart_id = uart_id;
    inst->client_fd = -1;
    inst->irq_efd = -1;
    inst->ack_efd = -1;
    inst->active = false;

    if (dev_override && dev_override[0]) {
        snprintf(inst->dev_path, sizeof(inst->dev_path), "%s", dev_override);
    } else {
        snprintf(inst->dev_path, sizeof(inst->dev_path), "/dev/amba_virt_uart%d", uart_id);
    }

    if (sock_override && sock_override[0]) {
        snprintf(inst->sock_path, sizeof(inst->sock_path), "%s", sock_override);
    } else {
        snprintf(inst->sock_path, sizeof(inst->sock_path), "/run/amba_virt_uart%d.sock", uart_id);
    }

    /* Open hardware UART lease device */
    inst->dev_fd = open(inst->dev_path, O_RDWR | O_CLOEXEC);
    if (inst->dev_fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", inst->dev_path, strerror(errno));
        return -1;
    }

    /* Create IRQ eventfd */
    inst->irq_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (inst->irq_efd < 0) {
        fprintf(stderr, "Failed to create IRQ eventfd: %s\n", strerror(errno));
        close(inst->dev_fd);
        return -1;
    }

    /* Bind IRQ eventfd to host kernel module */
    if (ioctl(inst->dev_fd, AMBA_VIRT_UART_IOC_SET_EVENTFD, &inst->irq_efd) < 0) {
        fprintf(stderr, "ioctl(SET_EVENTFD) failed on %s: %s\n", inst->dev_path, strerror(errno));
        close(inst->irq_efd);
        close(inst->dev_fd);
        return -1;
    }

    /* Create Doorbell ACK eventfd */
    inst->ack_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (inst->ack_efd < 0) {
        fprintf(stderr, "Failed to create ACK eventfd: %s\n", strerror(errno));
        close(inst->irq_efd);
        close(inst->dev_fd);
        return -1;
    }

    inst->ev_srv.type = FD_TYPE_SRV;
    inst->ev_srv.inst = inst;
    inst->ev_client.type = FD_TYPE_CLIENT;
    inst->ev_client.inst = inst;
    inst->ev_ack.type = FD_TYPE_ACK;
    inst->ev_ack.inst = inst;

    /* Create UNIX domain listener socket (non-blocking) */
    unlink(inst->sock_path);
    inst->srv_sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (inst->srv_sock_fd < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        close(inst->ack_efd);
        close(inst->irq_efd);
        close(inst->dev_fd);
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, inst->sock_path, sizeof(addr.sun_path) - 1);

    if (bind(inst->srv_sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to bind socket %s: %s\n", inst->sock_path, strerror(errno));
        close(inst->srv_sock_fd);
        close(inst->ack_efd);
        close(inst->irq_efd);
        close(inst->dev_fd);
        return -1;
    }

    chmod(inst->sock_path, 0660);

    if (listen(inst->srv_sock_fd, 5) < 0) {
        fprintf(stderr, "Failed to listen on %s: %s\n", inst->sock_path, strerror(errno));
        close(inst->srv_sock_fd);
        close(inst->ack_efd);
        close(inst->irq_efd);
        close(inst->dev_fd);
        unlink(inst->sock_path);
        return -1;
    }

    printf("UART %d initialized: dev=%s, sock=%s\n", inst->uart_id, inst->dev_path, inst->sock_path);
    return 0;
}

static int handshake_client(struct uart_instance *inst)
{
    printf("Performing ivshmem handshake for UART %d on client fd %d\n", inst->uart_id, inst->client_fd);

    /* 1. Protocol Version: 0 */
    if (send_fd_msg(inst->client_fd, IVSHMEM_PROTOCOL_VERSION, -1) < 0) return -1;

    /* 2. Client Peer ID: 0 */
    int64_t peer_id = 0;
    if (send_fd_msg(inst->client_fd, peer_id, -1) < 0) return -1;

    /* 3. SHM MMIO FD (with -1LL) */
    if (send_fd_msg(inst->client_fd, -1LL, inst->dev_fd) < 0) return -1;

    /* 4. Vector 0 EventFD for Peer 0 (Guest IRQ) */
    if (send_fd_msg(inst->client_fd, peer_id, inst->irq_efd) < 0) return -1;

    /* 5. Vector 0 EventFD for Peer 1 (Host Doorbell ACK) */
    int64_t host_peer_id = 1;
    if (send_fd_msg(inst->client_fd, host_peer_id, inst->ack_efd) < 0) return -1;

    inst->active = true;
    printf("UART %d ivshmem handshake complete!\n", inst->uart_id);
    return 0;
}

static void close_client(struct uart_instance *inst, int epoll_fd)
{
    if (inst->client_fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, inst->client_fd, NULL);
        close(inst->client_fd);
        inst->client_fd = -1;
        inst->active = false;
        printf("UART %d client disconnected.\n", inst->uart_id);
    }
}

static void cleanup_uart_instance(struct uart_instance *inst)
{
    if (inst->client_fd >= 0) {
        close(inst->client_fd);
        inst->client_fd = -1;
    }
    if (inst->srv_sock_fd >= 0) {
        close(inst->srv_sock_fd);
        inst->srv_sock_fd = -1;
    }
    if (inst->ack_efd >= 0) {
        close(inst->ack_efd);
        inst->ack_efd = -1;
    }
    if (inst->irq_efd >= 0) {
        close(inst->irq_efd);
        inst->irq_efd = -1;
    }
    if (inst->dev_fd >= 0) {
        ioctl(inst->dev_fd, AMBA_VIRT_UART_IOC_RESET);
        close(inst->dev_fd);
        inst->dev_fd = -1;
    }
    unlink(inst->sock_path);
}

int main(int argc, char **argv)
{
    int uart_ids[4] = {2, 3, 0, 0};
    int num_uarts = 2;
    const char *dev_override = NULL;
    const char *sock_override = NULL;
    bool foreground = false;

    int opt;
    while ((opt = getopt(argc, argv, "u:d:s:fh")) != -1) {
        switch (opt) {
        case 'u': {
            if (strcmp(optarg, "2") == 0) {
                uart_ids[0] = 2;
                num_uarts = 1;
            } else if (strcmp(optarg, "3") == 0) {
                uart_ids[0] = 3;
                num_uarts = 1;
            } else if (strcmp(optarg, "all") == 0 || strcmp(optarg, "2,3") == 0) {
                uart_ids[0] = 2;
                uart_ids[1] = 3;
                num_uarts = 2;
            } else {
                fprintf(stderr, "Unknown UART ID: %s (supported: 2, 3, all)\n", optarg);
                return 1;
            }
            break;
        }
        case 'd':
            dev_override = optarg;
            break;
        case 's':
            sock_override = optarg;
            break;
        case 'f':
            foreground = true;
            break;
        case 'h':
        default:
            printf("Usage: %s [-u 2|3|all] [-d /dev/amba_virt_uartN] [-s /run/amba_virt_uartN.sock] [-f]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (!foreground) {
        if (daemon(0, 0) < 0) {
            perror("daemon");
            return 1;
        }
    }

    setlinebuf(stdout);
    setlinebuf(stderr);
    printf("Starting Ambarella Virtual UART Host Server...\n");

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    struct uart_instance instances[4];
    int active_instances = 0;

    for (int i = 0; i < num_uarts; i++) {
        if (init_uart_instance(&instances[i], uart_ids[i], dev_override, sock_override) == 0) {
            struct epoll_event ev = {0};
            ev.events = EPOLLIN;
            ev.data.ptr = &instances[i].ev_srv;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, instances[i].srv_sock_fd, &ev);

            struct epoll_event ack_ev = {0};
            ack_ev.events = EPOLLIN;
            ack_ev.data.ptr = &instances[i].ev_ack;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, instances[i].ack_efd, &ack_ev);

            active_instances++;
        }
    }

    if (active_instances == 0) {
        fprintf(stderr, "No UART instances initialized successfully. Exiting.\n");
        close(epoll_fd);
        return 1;
    }

    printf("Entering event loop with %d active UART instance(s)...\n", active_instances);
    fflush(stdout);

    struct epoll_event events[MAX_EPOLL_EVENTS];
    while (g_running) {
        int n = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            struct event_data *ev_data = (struct event_data *)events[i].data.ptr;
            if (!ev_data || !ev_data->inst) continue;
            struct uart_instance *inst = ev_data->inst;

            switch (ev_data->type) {
            case FD_TYPE_ACK:
                if (events[i].events & EPOLLIN) {
                    uint64_t count = 0;
                    if (read(inst->ack_efd, &count, sizeof(count)) == sizeof(count)) {
                        printf("UART %d: Doorbell ACK received via eventfd, unmasking IRQ\n", inst->uart_id);
                        ioctl(inst->dev_fd, AMBA_VIRT_UART_IOC_ACK_IRQ);
                    }
                }
                break;

            case FD_TYPE_SRV:
                if (events[i].events & EPOLLIN) {
                    struct sockaddr_un client_addr;
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept4(inst->srv_sock_fd, (struct sockaddr *)&client_addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd >= 0) {
                        if (inst->client_fd >= 0) {
                            close_client(inst, epoll_fd);
                        }
                        inst->client_fd = client_fd;
                        if (handshake_client(inst) < 0) {
                            close_client(inst, epoll_fd);
                        } else {
                            struct epoll_event ev = {0};
                            ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                            ev.data.ptr = &inst->ev_client;
                            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, inst->client_fd, &ev);
                        }
                    }
                }
                break;

            case FD_TYPE_CLIENT:
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    printf("UART %d: QEMU client disconnected\n", inst->uart_id);
                    close_client(inst, epoll_fd);
                } else if (events[i].events & EPOLLIN) {
                    uint64_t val = 0;
                    ssize_t r = read(inst->client_fd, &val, sizeof(val));
                    if (r <= 0) {
                        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                            printf("UART %d: QEMU client connection closed\n", inst->uart_id);
                            close_client(inst, epoll_fd);
                        }
                    } else {
                        printf("UART %d: Reverse doorbell ACK received from QEMU (0x%llx), unmasking IRQ\n",
                               inst->uart_id, (unsigned long long)val);
                        ioctl(inst->dev_fd, AMBA_VIRT_UART_IOC_ACK_IRQ);
                    }
                }
                break;
            }
        }
    }

    printf("Shutting down UART server...\n");
    for (int i = 0; i < num_uarts; i++) {
        cleanup_uart_instance(&instances[i]);
    }
    close(epoll_fd);

    printf("UART server exited cleanly.\n");
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
