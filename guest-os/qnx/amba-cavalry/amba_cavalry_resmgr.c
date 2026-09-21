/*
 * amba_cavalry_resmgr.c
 *
 * QNX Neutrino RTOS 8.0 Resource Manager for Ambarella Cavalry NPU (/dev/cavalry).
 * Exposes /dev/cavalry to userspace runtimes (nnctrl/cavalry_mem) and proxies
 * operations as RPC transactions over amba-virt.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/resmgr.h>
#include <sys/types.h>
#include <unistd.h>
#include <devctl.h>

#include "amba_virt.h"
#include "amba_virt_qnx.h"
#include "cavalry_ioctl.h"
#include "cavalry_ioctl_path_b.h"

#define CAVALRY_POOL_BASE        0x02000000U    /* 32 MiB */
#define CAVALRY_POOL_SIZE        0x3E000000U    /* 992 MiB */
#define CAVALRY_RPC_ARENA_OFFSET 0x01F00000U    /* 31 MiB (1 MiB control arena) */
#define CAVALRY_RPC_ARENA_SIZE   0x00100000U    /* 1 MiB */

typedef struct {
    iofunc_ocb_t ocb;
    uint32_t session_id;
} cav_ocb_t;

typedef struct {
    dispatch_t              *dpp;
    resmgr_io_funcs_t       io_funcs;
    resmgr_connect_funcs_t  connect_funcs;
    iofunc_attr_t           attr;
    int                     resmgr_id;

    /* Shared Memory Window */
    uint64_t                bar_phys;
    uintptr_t               bar_virt;
    size_t                  bar_size;

    /* Cavalry bounds */
    uint32_t                pool_base;
    uint32_t                pool_size;
    uint32_t                rpc_arena_offset;
    uint32_t                rpc_arena_size;

    /* Mutex and state */
    pthread_mutex_t         arena_mutex;
    uint32_t                next_session_id;
    uint32_t                sequence;
    bool                    foreground;
    bool                    verbose;
} amba_cav_resmgr_t;

static amba_cav_resmgr_t g_cav;

static int cav_send_rpc(struct amba_virt_cavalry_rpc *rpc)
{
    struct {
        struct amba_virt_msg msg;
        struct amba_virt_cavalry_rpc payload;
    } req_pkt, resp_pkt;
    uint32_t resp_len = sizeof(resp_pkt);
    int ret;

    memset(&req_pkt, 0, sizeof(req_pkt));
    req_pkt.msg.type = AMBA_VIRT_MSG_CAVALRY_REQ;
    req_pkt.msg.seq = ++g_cav.sequence;
    memcpy(&req_pkt.payload, rpc, sizeof(*rpc));

    memset(&resp_pkt, 0, sizeof(resp_pkt));
    ret = amba_virt_rpc(&req_pkt, sizeof(req_pkt), &resp_pkt, &resp_len, 5000);
    if (ret < 0)
        return ret;

    if (resp_len < sizeof(resp_pkt) ||
        resp_pkt.msg.type != AMBA_VIRT_MSG_CAVALRY_RESP)
        return -EPROTO;

    memcpy(rpc, &resp_pkt.payload, sizeof(*rpc));
    return rpc->status;
}

static int cav_negotiate_bounds(void)
{
    struct {
        struct amba_virt_msg msg;
        struct amba_virt_dev_bounds_req req;
    } req_pkt;
    struct {
        struct amba_virt_msg msg;
        struct amba_virt_dev_bounds_resp resp;
    } resp_pkt;
    uint32_t resp_len = sizeof(resp_pkt);
    int ret;

    memset(&req_pkt, 0, sizeof(req_pkt));
    req_pkt.msg.type = AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ;
    req_pkt.msg.seq = ++g_cav.sequence;
    req_pkt.req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
    req_pkt.req.requested_size = 960 * 1024 * 1024;
    req_pkt.req.preferred_offset = AMBA_VIRT_OFFSET_AUTO;
    req_pkt.req.rpc_arena_size = CAVALRY_RPC_ARENA_SIZE;
    req_pkt.req.flags = AMBA_VIRT_DEV_F_BEST_EFFORT | AMBA_VIRT_DEV_F_REPLACE;

    memset(&resp_pkt, 0, sizeof(resp_pkt));
    ret = amba_virt_rpc(&req_pkt, sizeof(req_pkt), &resp_pkt, &resp_len, 5000);
    if (ret < 0) {
        fprintf(stderr, "amba_cavalry_resmgr: vsock RPC bounds negotiation failed: %d\n", ret);
        return ret;
    }

    if (resp_len < sizeof(resp_pkt) ||
        resp_pkt.msg.type != AMBA_VIRT_MSG_DEV_SET_BOUNDS_RESP)
        return -EPROTO;

    if (resp_pkt.resp.status == 0) {
        g_cav.pool_base = resp_pkt.resp.granted_offset;
        g_cav.pool_size = resp_pkt.resp.granted_size;
        g_cav.rpc_arena_offset = resp_pkt.resp.rpc_arena_offset;
        printf("amba_cavalry_resmgr: registered at BAR offset 0x%08x (%u MB), arena at 0x%08x\n",
               g_cav.pool_base, g_cav.pool_size / (1024 * 1024), g_cav.rpc_arena_offset);
        return 0;
    }

    fprintf(stderr, "amba_cavalry_resmgr: bounds negotiation returned error %d\n",
            resp_pkt.resp.status);
    return resp_pkt.resp.status;
}

static int io_open(resmgr_context_t *ctp, io_open_t *msg,
                   RESMGR_HANDLE_T *handle, void *extra)
{
    printf("amba_cavalry_resmgr: io_open called (ioflag=0x%x)\n", msg->connect.ioflag);
    return iofunc_open_default(ctp, msg, handle, extra);
}

static int io_close(resmgr_context_t *ctp, void *reserved,
                    RESMGR_OCB_T *ocb)
{
    printf("amba_cavalry_resmgr: io_close called\n");
    if (ocb) {
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_CLOSE_SESSION;
        rpc.session_id = (uint32_t)(uintptr_t)ocb;
        cav_send_rpc(&rpc);
    }
    return iofunc_close_ocb_default(ctp, reserved, ocb);
}

static int io_mmap(resmgr_context_t *ctp, io_mmap_t *msg,
                   RESMGR_OCB_T *ocb)
{
    (void)ocb;
    printf("amba_cavalry_resmgr: io_mmap offset=0x%llx\n", (unsigned long long)msg->i.offset);
    if (g_cav.bar_phys != 0) {
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.allowed_prot = PROT_READ | PROT_WRITE | PROT_NOCACHE;
        msg->o.offset = g_cav.bar_phys + msg->i.offset;
        msg->o.coid = -1;
        msg->o.fd = -1;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }
    return ENXIO;
}

static int read_client_process_memory(rcvid_t rcvid, uintptr_t vaddr, void *buf, size_t len)
{
    struct _msg_info info;
    pid_t pid = 0;

    if (MsgInfo(rcvid, &info) == 0 && info.pid > 0) {
        pid = info.pid;
    }

    if (pid <= 0) {
        fprintf(stderr, "amba_cavalry_resmgr: MsgInfo failed to get pid for rcvid %d\n", rcvid);
        return -ESRCH;
    }

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/as", (int)pid);
    int fd = open(proc_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "amba_cavalry_resmgr: failed to open %s: %s\n", proc_path, strerror(errno));
        return -errno;
    }
    ssize_t n = pread(fd, buf, len, (off_t)vaddr);
    int err = errno;
    close(fd);
    if (n < 0) {
        fprintf(stderr, "amba_cavalry_resmgr: pread from pid %d at 0x%lx len %zu failed: %s\n",
                (int)pid, (unsigned long)vaddr, len, strerror(err));
        return -err;
    }
    if ((size_t)n != len) {
        fprintf(stderr, "amba_cavalry_resmgr: short read from pid %d at 0x%lx: got %zd expected %zu\n",
                (int)pid, (unsigned long)vaddr, n, len);
        return -EIO;
    }
    return 0;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                     RESMGR_OCB_T *ocb)
{
    uint32_t session_id = (uint32_t)(uintptr_t)ocb;
    int ret;

    printf("amba_cavalry_resmgr: io_devctl dcmd=0x%08x sess=%u\n", msg->i.dcmd, session_id);

    switch (msg->i.dcmd) {
    case CAVALRY_GET_CV_CHIP_ID: {
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_GET_CHIP_ID;
        rpc.session_id = session_id;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        uint32_t *chip_id = (uint32_t *)_DEVCTL_DATA(msg->i);
        *chip_id = rpc.chip_id;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*chip_id);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*chip_id));
    }

    case CAVALRY_GET_CAVALRY_STATUS: {
        struct cavalry_status *s =
            (struct cavalry_status *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_GET_STATUS;
        rpc.session_id = session_id;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        memset(s, 0, sizeof(*s));
        s->is_cavalry_started = rpc.rval ? 1 : 0;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*s);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*s));
    }

    case CAVALRY_GET_DRAM_CFG: {
        struct cavalry_dram_cfg *cfg =
            (struct cavalry_dram_cfg *)_DEVCTL_DATA(msg->i);
        memset(cfg, 0, sizeof(*cfg));
        cfg->burst_size = 64;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*cfg);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*cfg));
    }

    case CAVALRY_GET_AUDIO_CLK: {
        uint64_t *clk = (uint64_t *)_DEVCTL_DATA(msg->i);
        *clk = 24000000ULL;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*clk);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*clk));
    }

    case CAVALRY_QUERY_MEM_ATTR: {
        struct cavalry_mem_attr *attr =
            (struct cavalry_mem_attr *)_DEVCTL_DATA(msg->i);
        memset(attr, 0, sizeof(*attr));
        attr->cache_en = 0;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*attr);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*attr));
    }

    case CAVALRY_QUERY_BUF: {
        struct cavalry_querybuf *q =
            (struct cavalry_querybuf *)_DEVCTL_DATA(msg->i);
        if (q->buf == CAVALRY_MEM_USER) {
            q->offset = g_cav.pool_base ? g_cav.pool_base : CAVALRY_POOL_BASE;
            q->length = g_cav.pool_size ? g_cav.pool_size : CAVALRY_POOL_SIZE;
        } else {
            q->offset = 0;
            q->length = 0;
        }
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*q);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*q));
    }

    case CAVALRY_QUERY_UCODE_CMD_SIZE: {
        struct cavalry_ucode_cmd_size *ucmd =
            (struct cavalry_ucode_cmd_size *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_QUERY_UCODE_CMD_SIZE;
        rpc.session_id = session_id;
        rpc.bar_offset = ucmd->cmd_id;
        rpc.size = ucmd->dag_cnt;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        ucmd->cmd_size = rpc.rval;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*ucmd);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*ucmd));
    }

    case CAVALRY_ALLOC_MEM: {
        struct cavalry_mem *mem =
            (struct cavalry_mem *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_ALLOC_MEM;
        rpc.session_id = session_id;
        rpc.size = mem->length;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        mem->offset = rpc.bar_offset;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*mem);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*mem));
    }

    case CAVALRY_FREE_MEM: {
        struct cavalry_mem *mem =
            (struct cavalry_mem *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_FREE_MEM;
        rpc.session_id = session_id;
        rpc.bar_offset = mem->offset;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        msg->o.ret_val = 0;
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case CAVALRY_SYNC_CACHE_MEM: {
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_SYNC_CACHE;
        rpc.session_id = session_id;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        msg->o.ret_val = 0;
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case CAVALRY_START_VP: {
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_START_VP;
        rpc.session_id = session_id;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        msg->o.ret_val = 0;
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case CAVALRY_STOP_VP: {
        msg->o.ret_val = 0;
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case CAVALRY_RUN_DAGS: {
        struct cavalry_run_dags *run_hdr =
            (struct cavalry_run_dags *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_rpc rpc;

        uint32_t arena_off = g_cav.rpc_arena_offset;
        uint32_t arena_sz = g_cav.rpc_arena_size ? g_cav.rpc_arena_size : CAVALRY_RPC_ARENA_SIZE;

        if (run_hdr->dag_cnt == 0 || run_hdr->dag_cnt > 128)
            return EINVAL;

        size_t req_size = sizeof(struct cavalry_run_dags) +
            run_hdr->dag_cnt * sizeof(struct cavalry_dag_desc);
        if (req_size > arena_sz)
            return EMSGSIZE;

        pthread_mutex_lock(&g_cav.arena_mutex);

        if (g_cav.bar_virt) {
            memcpy((void *)(g_cav.bar_virt + arena_off), run_hdr, req_size);
        }

        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_RUN_DAGS;
        rpc.session_id = session_id;
        rpc.bar_offset = arena_off;
        rpc.arena_len = (uint32_t)req_size;

        ret = cav_send_rpc(&rpc);
        if (ret == 0) {
            run_hdr->rval = rpc.rval;
            run_hdr->exec_total_ticks = rpc.exec_ticks;
            run_hdr->finish_dags = run_hdr->dag_cnt;
        }

        pthread_mutex_unlock(&g_cav.arena_mutex);

        if (ret < 0)
            return -ret;

        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*run_hdr);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*run_hdr));
    }

    case CAVALRY_IOC_REGISTER_DAG: {
        struct cavalry_reg_dag_user *reg_u =
            (struct cavalry_reg_dag_user *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_reg_dag_desc rdesc;
        struct amba_virt_cavalry_rpc rpc;

        uint32_t arena_off = g_cav.rpc_arena_offset;
        uint32_t arena_sz = g_cav.rpc_arena_size ? g_cav.rpc_arena_size : CAVALRY_RPC_ARENA_SIZE;

        if (reg_u->run_dags_bytes == 0 ||
            reg_u->run_dags_bytes > (arena_sz - sizeof(rdesc)))
            return EINVAL;

        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.staging_bar_offset = reg_u->staging_bar_offset;
        rdesc.staging_size = reg_u->staging_size;
        rdesc.dvi_offset_in_slice = reg_u->dvi_offset_in_slice;
        rdesc.extra_dag_list_offset_in_slice = reg_u->extra_dag_list_offset_in_slice;
        rdesc.extra_dag_common_offset_in_slice = reg_u->extra_dag_common_offset_in_slice;
        rdesc.extra_poke_list_offset_in_slice = reg_u->extra_poke_list_offset_in_slice;
        memcpy(rdesc.sha256, reg_u->sha256, sizeof(rdesc.sha256));
        rdesc.run_dags_bytes = reg_u->run_dags_bytes;

        size_t total_payload = sizeof(rdesc) + reg_u->run_dags_bytes;

        pthread_mutex_lock(&g_cav.arena_mutex);

        if (g_cav.bar_virt) {
            memcpy((void *)(g_cav.bar_virt + arena_off), &rdesc, sizeof(rdesc));
            int rerr = read_client_process_memory(ctp->rcvid,
                                                  (uintptr_t)reg_u->run_dags_ptr,
                                                  (void *)(g_cav.bar_virt + arena_off + sizeof(rdesc)),
                                                  reg_u->run_dags_bytes);
            if (rerr < 0) {
                pthread_mutex_unlock(&g_cav.arena_mutex);
                return -rerr;
            }
        }

        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_REGISTER_DAG;
        rpc.session_id = session_id;
        rpc.bar_offset = arena_off;
        rpc.arena_len = (uint32_t)total_payload;

        ret = cav_send_rpc(&rpc);
        if (ret == 0) {
            reg_u->dag_id = rpc.dag_id;
        }

        pthread_mutex_unlock(&g_cav.arena_mutex);

        if (ret < 0)
            return -ret;

        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*reg_u);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*reg_u));
    }

    case CAVALRY_IOC_UNREGISTER_DAG: {
        struct cavalry_unreg_dag_user *unreg =
            (struct cavalry_unreg_dag_user *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_UNREGISTER_DAG;
        rpc.session_id = session_id;
        rpc.dag_id = unreg->dag_id;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        msg->o.ret_val = 0;
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case CAVALRY_IOC_ALLOC_HANDLE: {
        struct cavalry_alloc_handle_user *h =
            (struct cavalry_alloc_handle_user *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_ALLOC_HANDLE;
        rpc.session_id = session_id;
        rpc.size = h->size;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        h->handle_id = rpc.dag_id;
        h->bar_offset = rpc.bar_offset;
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*h);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*h));
    }

    case CAVALRY_IOC_FREE_HANDLE: {
        struct cavalry_free_handle_user *h =
            (struct cavalry_free_handle_user *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_rpc rpc;
        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_FREE_HANDLE;
        rpc.session_id = session_id;
        rpc.dag_id = h->handle_id;
        ret = cav_send_rpc(&rpc);
        if (ret < 0)
            return -ret;
        msg->o.ret_val = 0;
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    case CAVALRY_IOC_RUN_REGISTERED_DAG: {
        struct cavalry_run_reg_user *run_u =
            (struct cavalry_run_reg_user *)_DEVCTL_DATA(msg->i);
        struct amba_virt_cavalry_run_reg_desc rdesc;
        struct amba_virt_cavalry_rpc rpc;

        uint32_t arena_off = g_cav.rpc_arena_offset;

        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.dag_id = run_u->dag_id;
        rdesc.port_cnt = run_u->port_cnt;
        memcpy(rdesc.ports, run_u->ports, sizeof(run_u->ports));

        pthread_mutex_lock(&g_cav.arena_mutex);

        if (g_cav.bar_virt) {
            memcpy((void *)(g_cav.bar_virt + arena_off), &rdesc, sizeof(rdesc));
        }

        memset(&rpc, 0, sizeof(rpc));
        rpc.opcode = VCAV_OP_RUN_REGISTERED_DAG;
        rpc.session_id = session_id;
        rpc.dag_id = run_u->dag_id;
        rpc.bar_offset = arena_off;
        rpc.arena_len = sizeof(rdesc);

        ret = cav_send_rpc(&rpc);
        if (ret == 0) {
            run_u->rval = rpc.rval;
            run_u->exec_ticks = rpc.exec_ticks;
        }

        pthread_mutex_unlock(&g_cav.arena_mutex);

        if (ret < 0)
            return -ret;

        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*run_u);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*run_u));
    }

    default:
        return iofunc_devctl_default(ctp, msg, ocb);
    }
}

int main(int argc, char **argv)
{
    dispatch_context_t *ctp;
    int opt;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    static struct option long_opts[] = {
        {"foreground", no_argument, NULL, 'f'},
        {"verbose",    no_argument, NULL, 'v'},
        {"help",       no_argument, NULL, '?'},
        {NULL, 0, NULL, 0}
    };

    memset(&g_cav, 0, sizeof(g_cav));
    g_cav.pool_base = CAVALRY_POOL_BASE;
    g_cav.pool_size = CAVALRY_POOL_SIZE;
    g_cav.rpc_arena_offset = CAVALRY_RPC_ARENA_OFFSET;
    g_cav.rpc_arena_size = CAVALRY_RPC_ARENA_SIZE;
    pthread_mutex_init(&g_cav.arena_mutex, NULL);

    while ((opt = getopt_long(argc, argv, "fv?", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            g_cav.foreground = true;
            break;
        case 'v':
            g_cav.verbose = true;
            break;
        case '?':
        default:
            printf("Usage: %s [-f|--foreground] [-v|--verbose]\n", argv[0]);
            return EXIT_SUCCESS;
        }
    }

    /* 1. Connect to amba-virt and query shared memory window */
    uint64_t phys = 0;
    void *virt = NULL;
    size_t size = 0;
    if (amba_virt_get_window(&phys, &virt, &size) < 0) {
        fprintf(stderr, "amba_cavalry_resmgr: failed to connect to /dev/amba_virt: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }

    g_cav.bar_phys = phys;
    g_cav.bar_virt = (uintptr_t)virt;
    g_cav.bar_size = size;

    /* 2. Negotiate Cavalry pool bounds with host amba-virt-server */
    if (cav_negotiate_bounds() < 0) {
        fprintf(stderr, "amba_cavalry_resmgr: warning: using default local pool offsets\n");
    }

    if (!g_cav.foreground) {
        if (procmgr_daemon(EXIT_SUCCESS, PROCMGR_DAEMON_NOCHDIR | PROCMGR_DAEMON_NOCLOSE) < 0) {
            fprintf(stderr, "amba_cavalry_resmgr: failed to daemonize: %s\n", strerror(errno));
        }
    }

    /* 3. Initialize QNX Resource Manager */
    g_cav.dpp = dispatch_create();
    if (!g_cav.dpp) {
        fprintf(stderr, "amba_cavalry_resmgr: dispatch_create failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_cav.connect_funcs,
                     _RESMGR_IO_NFUNCS, &g_cav.io_funcs);

    g_cav.connect_funcs.open = io_open;
    g_cav.io_funcs.close_ocb = io_close;
    g_cav.io_funcs.devctl = io_devctl;
    g_cav.io_funcs.mmap = io_mmap;

    static resmgr_attr_t resmgr_attr;
    memset(&resmgr_attr, 0, sizeof(resmgr_attr));
    resmgr_attr.nparts_max = 1;
    resmgr_attr.msg_max_size = 8192;

    iofunc_attr_init(&g_cav.attr, S_IFCHR | 0666, NULL, NULL);

    g_cav.resmgr_id = resmgr_attach(
        g_cav.dpp,
        &resmgr_attr,
        CAVALRY_DEV_NODE,
        _FTYPE_ANY,
        0,
        &g_cav.connect_funcs,
        &g_cav.io_funcs,
        &g_cav.attr);

    if (g_cav.resmgr_id < 0) {
        fprintf(stderr, "amba_cavalry_resmgr: resmgr_attach failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("amba_cavalry_resmgr: registered %s in QNX pathname space\n", CAVALRY_DEV_NODE);

    /* 4. Message Loop */
    ctp = dispatch_context_alloc(g_cav.dpp);
    while (1) {
        ctp = dispatch_block(ctp);
        if (!ctp)
            break;
        dispatch_handler(ctp);
    }

    return EXIT_SUCCESS;
}
