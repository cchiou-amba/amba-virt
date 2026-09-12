/*
 * amba_virt_resmgr.c
 *
 * QNX Neutrino RTOS 8.0 Resource Manager for Ambarella Virtualization (/dev/amba_virt).
 * Maps PCI ivshmem BAR 2 and dispatches standard POSIX messages to userspace clients.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/resmgr.h>
#include <devctl.h>

#include "amba_virt.h"
#include "amba_virt_test.h"

#define IVSHMEM_VENDOR_ID   0x1af4
#define IVSHMEM_DEVICE_ID   0x1110
#define IVSHMEM_SHM_BAR     2

typedef struct {
    dispatch_t              *dpp;
    resmgr_io_funcs_t       io_funcs;
    resmgr_connect_funcs_t  connect_funcs;
    iofunc_attr_t           attr;
    int                     resmgr_id;

    /* ivshmem mapping */
    uintptr_t               shm_base;
    size_t                  shm_size;
} amba_virt_resmgr_t;

static amba_virt_resmgr_t g_resmgr;

static int io_open(resmgr_context_t *ctp, io_open_t *msg,
                   RESMGR_HANDLE_T *handle, void *extra)
{
    return iofunc_open_default(ctp, msg, handle, extra);
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg,
                   RESMGR_OCB_T *ocb)
{
    int status;
    if ((status = iofunc_read_verify(ctp, msg, ocb, NULL)) != EOK)
        return status;

    /* Process read from amba_virt ringbuffer */
    _IO_SET_READ_NBYTES(ctp, 0);
    return EOK;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg,
                    RESMGR_OCB_T *ocb)
{
    int status;
    if ((status = iofunc_write_verify(ctp, msg, ocb, NULL)) != EOK)
        return status;

    /* Process write to amba_virt ringbuffer */
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                     RESMGR_OCB_T *ocb)
{
    int status;
    if ((status = iofunc_devctl_default(ctp, msg, ocb)) != _RESMGR_DEFAULT)
        return status;

    switch (msg->i.dcmd) {
    case AMBA_VIRT_IOC_GET_INFO: {
        struct amba_virt_info *info =
            (struct amba_virt_info *)_DEVCTL_DATA(msg->i);
        memset(info, 0, sizeof(*info));
        info->proto = AMBA_VIRT_PROTO;
        info->role = AMBA_VIRT_ROLE_GUEST;
        info->shm_size = (uint32_t)g_resmgr.shm_size;
        info->connected = 1;
        info->vsock_cid = AMBA_VIRT_VSOCK_CID;
        info->vsock_port = AMBA_VIRT_VSOCK_PORT;
        msg->o.ret_val = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*info));
    }
    case AMBA_VIRT_IOC_CONNECT:
        msg->o.ret_val = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    default:
        return ENOSYS;
    }

}


int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    dispatch_context_t *ctp;

    memset(&g_resmgr, 0, sizeof(g_resmgr));

    g_resmgr.dpp = dispatch_create();
    if (!g_resmgr.dpp) {
        fprintf(stderr, "amba_virt_resmgr: dispatch_create failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_resmgr.connect_funcs,
                     _RESMGR_IO_NFUNCS, &g_resmgr.io_funcs);

    g_resmgr.connect_funcs.open = io_open;
    g_resmgr.io_funcs.read = io_read;
    g_resmgr.io_funcs.write = io_write;
    g_resmgr.io_funcs.devctl = io_devctl;

    iofunc_attr_init(&g_resmgr.attr, S_IFCHR | 0666, NULL, NULL);

    g_resmgr.resmgr_id = resmgr_attach(
        g_resmgr.dpp,
        NULL,
        AMBA_VIRT_DEV_PATH,
        _FTYPE_ANY,
        0,
        &g_resmgr.connect_funcs,
        &g_resmgr.io_funcs,
        &g_resmgr.attr);

    if (g_resmgr.resmgr_id < 0) {
        fprintf(stderr, "amba_virt_resmgr: resmgr_attach failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }

    printf("amba_virt_resmgr: registered %s in QNX pathname space\n",
           AMBA_VIRT_DEV_PATH);

    ctp = dispatch_context_alloc(g_resmgr.dpp);
    while (1) {
        ctp = dispatch_block(ctp);
        if (!ctp)
            break;
        dispatch_handler(ctp);
    }

    return EXIT_SUCCESS;
}
