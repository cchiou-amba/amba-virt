/*
 * amba_dma.c
 *
 * Ambarella Virtual Peripheral DMA Guest Driver
 *
 * Registers as a Linux dmaengine provider in the guest VM.
 * Translates standard dmaengine API calls into vsock RPC messages
 * to the host amba-virt-server DMA broker, which owns the physical
 * DMA controller (dma1 at 0xffe0021000).
 *
 * Depends on amba_virt.ko for vsock RPC transport
 * (amba_virt_rpc() exported symbol).
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

// SPDX-License-Identifier: GPL-2.0

#include <linux/dmaengine.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <amba_virt.h>
#include "amba_virt_kernel.h"

#define AMBA_DMA_MAX_CHANNELS   4
#define AMBA_DMA_RPC_TIMEOUT_MS 1000

static unsigned int base_channel = 13;
module_param(base_channel, uint, 0644);
MODULE_PARM_DESC(base_channel, "Base Generic-DMA1 hardware channel (default 13 for UART2 TX)");

struct amba_dma_chan {
    struct dma_chan          chan;
    u32                     hw_channel;     /* Physical DMA channel */
    u32                     direction;
    bool                    configured;
    spinlock_t              lock;

    /* Active transfer tracking */
    dma_cookie_t            last_cookie;
    struct dma_async_tx_descriptor tx_desc;

    /* Pending transfer info and work item */
    phys_addr_t             pending_buf_addr;
    u32                     pending_buf_len;
    u32                     pending_dir;
    struct work_struct      work;
};

/* Driver-level device */
struct amba_dma_device {
    struct dma_device       dma_dev;
    struct platform_device  *pdev;
    struct amba_dma_chan     channels[AMBA_DMA_MAX_CHANNELS];
    int                     nr_channels;
};

static struct amba_dma_device *g_adev;

/* ---- RPC Helpers ---- */

static int amba_dma_rpc_slave_cfg(u32 channel, u32 direction,
                                  u32 src_addr, u32 dst_addr,
                                  u32 width, u32 maxburst)
{
    u8 req_buf[sizeof(struct amba_virt_msg) +
               sizeof(struct amba_virt_dma_slave_cfg)];
    u8 resp_buf[sizeof(struct amba_virt_msg) +
                sizeof(struct amba_virt_dma_slave_cfg)];
    struct amba_virt_msg *msg;
    struct amba_virt_dma_slave_cfg *cfg;
    struct amba_virt_dma_slave_cfg *resp_cfg;
    u32 resp_len = sizeof(resp_buf);
    int ret;

    memset(req_buf, 0, sizeof(req_buf));
    msg = (struct amba_virt_msg *)req_buf;
    msg->type = AMBA_VIRT_MSG_DMA_SLAVE_CFG_REQ;

    cfg = (struct amba_virt_dma_slave_cfg *)(req_buf + sizeof(*msg));
    cfg->channel = channel;
    cfg->direction = direction;
    cfg->src_addr_lo = src_addr;
    cfg->dst_addr_lo = dst_addr;
    cfg->src_addr_width = width;
    cfg->dst_addr_width = width;
    cfg->src_maxburst = maxburst;
    cfg->dst_maxburst = maxburst;

    ret = amba_virt_rpc(req_buf, sizeof(req_buf),
                        resp_buf, &resp_len, AMBA_DMA_RPC_TIMEOUT_MS);
    if (ret)
        return ret;

    resp_cfg = (struct amba_virt_dma_slave_cfg *)
               (resp_buf + sizeof(struct amba_virt_msg));
    return resp_cfg->status;
}

static int amba_dma_rpc_submit(u32 channel, u32 direction,
                               u32 buf_offset, u32 buf_len,
                               u32 cookie)
{
    u8 req_buf[sizeof(struct amba_virt_msg) +
               sizeof(struct amba_virt_dma_submit)];
    u8 resp_buf[sizeof(struct amba_virt_msg) +
                sizeof(struct amba_virt_dma_submit)];
    struct amba_virt_msg *msg;
    struct amba_virt_dma_submit *sub;
    struct amba_virt_dma_submit *resp_sub;
    u32 resp_len = sizeof(resp_buf);
    int ret;

    memset(req_buf, 0, sizeof(req_buf));
    msg = (struct amba_virt_msg *)req_buf;
    msg->type = AMBA_VIRT_MSG_DMA_SUBMIT_REQ;

    sub = (struct amba_virt_dma_submit *)(req_buf + sizeof(*msg));
    sub->channel = channel;
    sub->direction = direction;
    sub->buf_offset = buf_offset;
    sub->buf_len = buf_len;
    sub->cookie = cookie;

    ret = amba_virt_rpc(req_buf, sizeof(req_buf),
                        resp_buf, &resp_len, AMBA_DMA_RPC_TIMEOUT_MS);
    if (ret)
        return ret;

    resp_sub = (struct amba_virt_dma_submit *)
               (resp_buf + sizeof(struct amba_virt_msg));
    return resp_sub->status;
}

static int amba_dma_rpc_terminate(u32 channel)
{
    u8 req_buf[sizeof(struct amba_virt_msg) +
               sizeof(struct amba_virt_dma_terminate)];
    u8 resp_buf[sizeof(struct amba_virt_msg) +
                sizeof(struct amba_virt_dma_terminate)];
    struct amba_virt_msg *msg;
    struct amba_virt_dma_terminate *term;
    struct amba_virt_dma_terminate *resp_term;
    u32 resp_len = sizeof(resp_buf);
    int ret;

    memset(req_buf, 0, sizeof(req_buf));
    msg = (struct amba_virt_msg *)req_buf;
    msg->type = AMBA_VIRT_MSG_DMA_TERMINATE_REQ;

    term = (struct amba_virt_dma_terminate *)(req_buf + sizeof(*msg));
    term->channel = channel;

    ret = amba_virt_rpc(req_buf, sizeof(req_buf),
                        resp_buf, &resp_len, AMBA_DMA_RPC_TIMEOUT_MS);
    if (ret)
        return ret;

    resp_term = (struct amba_virt_dma_terminate *)
                (resp_buf + sizeof(struct amba_virt_msg));
    return resp_term->status;
}

/* ---- dmaengine Callbacks ---- */

static int amba_dma_alloc_chan_resources(struct dma_chan *chan)
{
    struct amba_dma_chan *achan =
        container_of(chan, struct amba_dma_chan, chan);

    achan->configured = false;
    achan->last_cookie = 1;
    return 0;
}

static void amba_dma_free_chan_resources(struct dma_chan *chan)
{
    struct amba_dma_chan *achan =
        container_of(chan, struct amba_dma_chan, chan);

    cancel_work_sync(&achan->work);

    /* Terminate any active transfer */
    if (achan->hw_channel)
        amba_dma_rpc_terminate(achan->hw_channel);
    achan->configured = false;
}

static int amba_dma_slave_config(struct dma_chan *chan,
                                 struct dma_slave_config *config)
{
    struct amba_dma_chan *achan =
        container_of(chan, struct amba_dma_chan, chan);
    u32 addr, width, burst, direction;
    int ret;

    if (config->direction == DMA_MEM_TO_DEV) {
        direction = AMBA_VIRT_DMA_DIR_MEM_TO_DEV;
        addr = lower_32_bits(config->dst_addr);
        width = config->dst_addr_width;
        burst = config->dst_maxburst;
    } else if (config->direction == DMA_DEV_TO_MEM) {
        direction = AMBA_VIRT_DMA_DIR_DEV_TO_MEM;
        addr = lower_32_bits(config->src_addr);
        width = config->src_addr_width;
        burst = config->src_maxburst;
    } else {
        return -EINVAL;
    }

    ret = amba_dma_rpc_slave_cfg(achan->hw_channel, direction,
                                 addr, addr, width, burst);
    if (ret)
        return ret;

    achan->direction = direction;
    achan->configured = true;
    return 0;
}

static dma_cookie_t amba_dma_tx_submit(
    struct dma_async_tx_descriptor *tx)
{
    struct dma_chan *chan = tx->chan;
    struct amba_dma_chan *achan =
        container_of(chan, struct amba_dma_chan, chan);
    dma_cookie_t cookie;
    unsigned long flags;

    spin_lock_irqsave(&achan->lock, flags);
    cookie = chan->cookie + 1;
    if (cookie < 0)
        cookie = 1;
    chan->cookie = cookie;
    tx->cookie = cookie;
    achan->last_cookie = cookie;
    spin_unlock_irqrestore(&achan->lock, flags);

    return cookie;
}

static void amba_dma_chan_work(struct work_struct *work)
{
    struct amba_dma_chan *achan =
        container_of(work, struct amba_dma_chan, work);
    phys_addr_t shm_phys = 0;
    void __iomem *shm_iomem = NULL;
    size_t shm_size = 0;
    u32 buf_offset;
    dma_async_tx_callback cb;
    void *cb_param;
    unsigned long flags;
    int ret;

    /* Determine ivshmem offset (prefer DMA32 lease slice, fallback to bulk) */
    ret = amba_virt_get_dma32_window(&shm_phys, &shm_iomem, &shm_size);
    if (ret == 0 && achan->pending_buf_addr >= shm_phys &&
        achan->pending_buf_addr < shm_phys + shm_size) {
        buf_offset = (u32)(achan->pending_buf_addr - shm_phys);
    } else {
        ret = amba_virt_get_window(&shm_phys, &shm_iomem, &shm_size);
        if (ret == 0 && achan->pending_buf_addr >= shm_phys &&
            achan->pending_buf_addr < shm_phys + shm_size) {
            buf_offset = (u32)(achan->pending_buf_addr - shm_phys);
        } else {
            buf_offset = (u32)achan->pending_buf_addr;
            pr_warn_ratelimited("amba_dma: chan %u buffer 0x%llx not in ivshmem window [0x%llx..0x%llx]\n",
                                achan->hw_channel,
                                (unsigned long long)achan->pending_buf_addr,
                                (unsigned long long)shm_phys,
                                (unsigned long long)(shm_phys + shm_size));
        }
    }

    ret = amba_dma_rpc_submit(achan->hw_channel, achan->pending_dir,
                              buf_offset, achan->pending_buf_len,
                              achan->last_cookie);

    spin_lock_irqsave(&achan->lock, flags);
    if (ret == 0) {
        achan->chan.completed_cookie = achan->last_cookie;
    } else {
        pr_err("amba_dma: chan %u submit RPC failed: %d\n",
               achan->hw_channel, ret);
    }
    cb = achan->tx_desc.callback;
    cb_param = achan->tx_desc.callback_param;
    spin_unlock_irqrestore(&achan->lock, flags);

    if (cb)
        cb(cb_param);
}

static struct dma_async_tx_descriptor *
amba_dma_prep_slave_sg(struct dma_chan *chan,
                       struct scatterlist *sgl, unsigned int sg_len,
                       enum dma_transfer_direction direction,
                       unsigned long flags, void *context)
{
    struct amba_dma_chan *achan =
        container_of(chan, struct amba_dma_chan, chan);

    if (!achan->configured) {
        pr_err("amba_dma: channel %u not configured\n",
               achan->hw_channel);
        return NULL;
    }

    if (sg_len != 1) {
        pr_err("amba_dma: multi-entry SG not supported (sg_len=%u)\n",
               sg_len);
        return NULL;
    }

    achan->pending_buf_addr = sg_dma_address(sgl);
    achan->pending_buf_len = sg_dma_len(sgl);
    if (!achan->pending_buf_addr)
        achan->pending_buf_addr = sg_phys(sgl);
    if (!achan->pending_buf_len)
        achan->pending_buf_len = sgl->length;
    achan->pending_dir = achan->direction;

    dma_async_tx_descriptor_init(&achan->tx_desc, chan);
    achan->tx_desc.tx_submit = amba_dma_tx_submit;
    return &achan->tx_desc;
}

static void amba_dma_issue_pending(struct dma_chan *chan)
{
    struct amba_dma_chan *achan =
        container_of(chan, struct amba_dma_chan, chan);

    schedule_work(&achan->work);
}

static enum dma_status amba_dma_tx_status(struct dma_chan *chan,
                                           dma_cookie_t cookie,
                                           struct dma_tx_state *txstate)
{
    dma_set_tx_state(txstate, chan->completed_cookie, chan->cookie, 0);
    if (cookie == chan->completed_cookie)
        return DMA_COMPLETE;
    return DMA_IN_PROGRESS;
}

static int amba_dma_terminate_all(struct dma_chan *chan)
{
    struct amba_dma_chan *achan =
        container_of(chan, struct amba_dma_chan, chan);

    cancel_work_sync(&achan->work);
    return amba_dma_rpc_terminate(achan->hw_channel);
}

static int amba_dma_config(struct dma_chan *chan,
                            struct dma_slave_config *config)
{
    return amba_dma_slave_config(chan, config);
}

/* ---- Platform Device ---- */

static int amba_dma_probe(struct platform_device *pdev)
{
    struct amba_dma_device *adev;
    struct dma_device *dma;
    int i, ret;

    adev = devm_kzalloc(&pdev->dev, sizeof(*adev), GFP_KERNEL);
    if (!adev)
        return -ENOMEM;

    adev->pdev = pdev;
    adev->nr_channels = AMBA_DMA_MAX_CHANNELS;
    dma = &adev->dma_dev;

    dma_cap_set(DMA_SLAVE, dma->cap_mask);
    dma_cap_set(DMA_PRIVATE, dma->cap_mask);

    dma->dev = &pdev->dev;
    dma->device_alloc_chan_resources = amba_dma_alloc_chan_resources;
    dma->device_free_chan_resources = amba_dma_free_chan_resources;
    dma->device_prep_slave_sg = amba_dma_prep_slave_sg;
    dma->device_issue_pending = amba_dma_issue_pending;
    dma->device_tx_status = amba_dma_tx_status;
    dma->device_terminate_all = amba_dma_terminate_all;
    dma->device_config = amba_dma_config;

    INIT_LIST_HEAD(&dma->channels);

    for (i = 0; i < adev->nr_channels; i++) {
        struct amba_dma_chan *achan = &adev->channels[i];

        achan->hw_channel = base_channel + i;  /* Channels base_channel .. +3 */
        spin_lock_init(&achan->lock);
        INIT_WORK(&achan->work, amba_dma_chan_work);
        achan->chan.device = dma;
        list_add_tail(&achan->chan.device_node, &dma->channels);
    }

    ret = dma_async_device_register(dma);
    if (ret) {
        dev_err(&pdev->dev, "dmaengine registration failed: %d\n", ret);
        return ret;
    }

    platform_set_drvdata(pdev, adev);
    g_adev = adev;

    dev_info(&pdev->dev,
             "amba_dma: virtual peripheral DMA registered "
             "(%d channels, base %u)\n", adev->nr_channels, base_channel);
    return 0;
}

static int amba_dma_remove(struct platform_device *pdev)
{
    struct amba_dma_device *adev = platform_get_drvdata(pdev);

    if (adev) {
        dma_async_device_unregister(&adev->dma_dev);
        g_adev = NULL;
    }
    return 0;
}

/* Self-instantiating platform device for out-of-tree module */
static struct platform_device *amba_dma_pdev;

static struct platform_driver amba_dma_driver = {
    .driver = {
        .name = "amba-virt-dma",
    },
    .probe = amba_dma_probe,
    .remove = amba_dma_remove,
};

static int __init amba_dma_init(void)
{
    int ret;

    ret = platform_driver_register(&amba_dma_driver);
    if (ret)
        return ret;

    amba_dma_pdev = platform_device_register_simple(
        "amba-virt-dma", -1, NULL, 0);
    if (IS_ERR(amba_dma_pdev)) {
        platform_driver_unregister(&amba_dma_driver);
        return PTR_ERR(amba_dma_pdev);
    }

    return 0;
}

static void __exit amba_dma_exit(void)
{
    if (amba_dma_pdev)
        platform_device_unregister(amba_dma_pdev);
    platform_driver_unregister(&amba_dma_driver);
}

module_init(amba_dma_init);
module_exit(amba_dma_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ambarella Virtual Peripheral DMA Guest Driver");
MODULE_AUTHOR("Ambarella International LLC");

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
