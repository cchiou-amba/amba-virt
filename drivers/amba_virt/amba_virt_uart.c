/*
 * amba_virt_uart.c
 *
 * Ambarella Virtual UART Host Lease & Level-IRQ Bridge Driver.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/eventfd.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#include "include/uapi/amba_virt_uart_uapi.h"

#define DRV_NAME "amba_virt_uart"

struct amba_virt_uart_port {
    uint32_t uart_id;
    const char *name;
    const char *clk_name;
    phys_addr_t phys_base;
    size_t size;
    int irq;
    struct clk *clk;
    struct miscdevice misc;
    
    spinlock_t lock;
    struct eventfd_ctx *efd_ctx;
    atomic_t is_open;
    bool irq_requested;
    bool irq_masked;
    uint64_t irq_count;
    uint64_t ack_count;
};

/* Hardware parameters for Ambarella CV3-AD655 AHB UARTs */
static struct amba_virt_uart_port g_ports[] = {
    {
        .uart_id = 2,
        .name = "amba_virt_uart2",
        .clk_name = "gclk_uart1",
        .phys_base = 0xffe0018000ULL,
        .size = 0x1000,
        .irq = 147, /* GIC SPI 115 (115 + 32 = 147) */
    },
    {
        .uart_id = 3,
        .name = "amba_virt_uart3",
        .clk_name = "gclk_uart2",
        .phys_base = 0xffe0019000ULL,
        .size = 0x1000,
        .irq = 148, /* GIC SPI 116 (116 + 32 = 148) */
    }
};

#define NUM_PORTS ARRAY_SIZE(g_ports)

static irqreturn_t amba_virt_uart_irq_handler(int irq, void *dev_id)
{
    struct amba_virt_uart_port *port = (struct amba_virt_uart_port *)dev_id;
    unsigned long flags;
    struct eventfd_ctx *efd = NULL;

    spin_lock_irqsave(&port->lock, flags);
    if (!port->irq_masked) {
        port->irq_masked = true;
        disable_irq_nosync(port->irq);
        port->irq_count++;
        efd = port->efd_ctx;
    }
    spin_unlock_irqrestore(&port->lock, flags);

    if (efd) {
        eventfd_signal(efd, 1);
    }

    return IRQ_HANDLED;
}

static int amba_virt_uart_open(struct inode *inode, struct file *file)
{
    struct miscdevice *misc = file->private_data;
    struct amba_virt_uart_port *port = container_of(misc, struct amba_virt_uart_port, misc);

    if (atomic_cmpxchg(&port->is_open, 0, 1) != 0) {
        return -EBUSY;
    }

    i_size_write(inode, port->size);
    file->private_data = port;
    pr_info("amba_virt_uart: port %u opened (size=%zu)\n", port->uart_id, port->size);
    return 0;
}

static int amba_virt_uart_release(struct inode *inode, struct file *file)
{
    struct amba_virt_uart_port *port = file->private_data;
    unsigned long flags;
    struct eventfd_ctx *old_efd = NULL;

    spin_lock_irqsave(&port->lock, flags);
    if (port->irq_requested) {
        if (port->irq_masked) {
            enable_irq(port->irq);
            port->irq_masked = false;
        }
        free_irq(port->irq, port);
        port->irq_requested = false;
    }

    old_efd = port->efd_ctx;
    port->efd_ctx = NULL;
    spin_unlock_irqrestore(&port->lock, flags);

    if (old_efd) {
        eventfd_ctx_put(old_efd);
    }

    atomic_set(&port->is_open, 0);
    pr_info("amba_virt_uart: port %u released\n", port->uart_id);
    return 0;
}

static int amba_virt_uart_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct amba_virt_uart_port *port = file->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    if (size > port->size) {
        pr_err("amba_virt_uart: requested mmap size %lu > %zu\n", size, port->size);
        return -EINVAL;
    }

    pfn = PHYS_PFN(port->phys_base);
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
        pr_err("amba_virt_uart: remap_pfn_range failed for pfn 0x%lx\n", pfn);
        return -EAGAIN;
    }

    pr_info("amba_virt_uart: mapped UART%u (pfn 0x%lx, size %lu) to vma 0x%lx\n",
            port->uart_id, pfn, size, vma->vm_start);
    return 0;
}

static long amba_virt_uart_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct amba_virt_uart_port *port = file->private_data;
    unsigned long flags;
    int ret = 0;

    switch (cmd) {
    case AMBA_VIRT_UART_IOC_GET_INFO: {
        struct amba_virt_uart_info info;
        memset(&info, 0, sizeof(info));
        info.uart_id = port->uart_id;
        info.phys_base = port->phys_base;
        info.size = port->size;
        info.irq = port->irq;

        spin_lock_irqsave(&port->lock, flags);
        info.irq_count = port->irq_count;
        info.ack_count = port->ack_count;
        info.irq_masked = port->irq_masked ? 1 : 0;
        spin_unlock_irqrestore(&port->lock, flags);

        if (copy_to_user((void __user *)arg, &info, sizeof(info)))
            return -EFAULT;
        break;
    }

    case AMBA_VIRT_UART_IOC_SET_EVENTFD: {
        int efd_val;
        struct eventfd_ctx *new_efd, *old_efd = NULL;

        if (copy_from_user(&efd_val, (void __user *)arg, sizeof(int)))
            return -EFAULT;

        new_efd = eventfd_ctx_fdget(efd_val);
        if (IS_ERR(new_efd)) {
            pr_err("amba_virt_uart: invalid eventfd %d\n", efd_val);
            return PTR_ERR(new_efd);
        }

        spin_lock_irqsave(&port->lock, flags);
        old_efd = port->efd_ctx;
        port->efd_ctx = new_efd;

        if (!port->irq_requested && port->irq > 0) {
            ret = request_irq(port->irq, amba_virt_uart_irq_handler,
                              IRQF_SHARED | IRQF_TRIGGER_HIGH,
                              port->name, port);
            if (ret) {
                pr_err("amba_virt_uart: request_irq %d failed: %d\n", port->irq, ret);
                port->efd_ctx = old_efd;
                spin_unlock_irqrestore(&port->lock, flags);
                eventfd_ctx_put(new_efd);
                return ret;
            }
            port->irq_requested = true;
            port->irq_masked = false;
        }
        spin_unlock_irqrestore(&port->lock, flags);

        if (old_efd)
            eventfd_ctx_put(old_efd);

        pr_info("amba_virt_uart: port %u bound eventfd (fd=%d, irq=%d)\n",
                port->uart_id, efd_val, port->irq);
        break;
    }

    case AMBA_VIRT_UART_IOC_ACK_IRQ: {
        spin_lock_irqsave(&port->lock, flags);
        if (port->irq_requested && port->irq_masked) {
            port->irq_masked = false;
            port->ack_count++;
            enable_irq(port->irq);
        }
        spin_unlock_irqrestore(&port->lock, flags);
        break;
    }

    case AMBA_VIRT_UART_IOC_RESET: {
        spin_lock_irqsave(&port->lock, flags);
        port->irq_count = 0;
        port->ack_count = 0;
        if (port->irq_requested && port->irq_masked) {
            port->irq_masked = false;
            enable_irq(port->irq);
        }
        spin_unlock_irqrestore(&port->lock, flags);
        pr_info("amba_virt_uart: port %u reset\n", port->uart_id);
        break;
    }

    default:
        return -ENOTTY;
    }

    return ret;
}

static const struct file_operations amba_virt_uart_fops = {
    .owner          = THIS_MODULE,
    .open           = amba_virt_uart_open,
    .release        = amba_virt_uart_release,
    .mmap           = amba_virt_uart_mmap,
    .unlocked_ioctl = amba_virt_uart_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = amba_virt_uart_ioctl,
#endif
};

static int __init amba_virt_uart_init(void)
{
    int i, ret;

    pr_info("amba_virt_uart: initializing Ambarella Virtual UART Lease Driver\n");

    for (i = 0; i < NUM_PORTS; i++) {
        struct amba_virt_uart_port *port = &g_ports[i];
        char dt_path[64];
        struct device_node *np;

        spin_lock_init(&port->lock);
        atomic_set(&port->is_open, 0);

        port->misc.minor = MISC_DYNAMIC_MINOR;
        port->misc.name = port->name;
        port->misc.fops = &amba_virt_uart_fops;

        /* Prepare clock if available */
        if (port->clk_name) {
            port->clk = clk_get(NULL, port->clk_name);
            if (!IS_ERR_OR_NULL(port->clk)) {
                clk_prepare_enable(port->clk);
                pr_info("amba_virt_uart: enabled clock %s for %s\n", port->clk_name, port->name);
            }
        }

        /* 1. Try resolving Linux IRQ from Device Tree */
        snprintf(dt_path, sizeof(dt_path), "/ahb@ffe0000000/uart@%llx",
                 (unsigned long long)(port->phys_base & 0xffffffff));
        np = of_find_node_by_path(dt_path);
        if (np) {
            port->irq = irq_of_parse_and_map(np, 0);
            of_node_put(np);
            pr_info("amba_virt_uart: mapped DT node %s to Linux IRQ %d\n", dt_path, port->irq);
        }

        /* 2. If DT node mapping was not present, create GIC mapping directly */
        if (port->irq <= 0) {
            struct device_node *gic_node = of_find_compatible_node(NULL, NULL, "arm,gic-400");
            if (!gic_node)
                gic_node = of_find_compatible_node(NULL, NULL, "arm,cortex-a15-gic");
            if (gic_node) {
                struct of_phandle_args oirq;
                oirq.np = gic_node;
                oirq.args_count = 3;
                oirq.args[0] = 0; /* GIC_SPI */
                oirq.args[1] = (port->uart_id == 2) ? 115 : 116; /* SPI number */
                oirq.args[2] = 4; /* IRQ_TYPE_LEVEL_HIGH */
                port->irq = irq_create_of_mapping(&oirq);
                of_node_put(gic_node);
                pr_info("amba_virt_uart: created GIC SPI %u -> Linux IRQ %d\n",
                        oirq.args[1], port->irq);
            }
        }

        ret = misc_register(&port->misc);
        if (ret) {
            pr_err("amba_virt_uart: failed to register %s (ret=%d)\n", port->name, ret);
            while (--i >= 0) {
                if (!IS_ERR_OR_NULL(g_ports[i].clk)) {
                    clk_disable_unprepare(g_ports[i].clk);
                    clk_put(g_ports[i].clk);
                }
                misc_deregister(&g_ports[i].misc);
            }
            return ret;
        }

        pr_info("amba_virt_uart: registered %s (phys=0x%llx, irq=%d, minor=%d)\n",
                port->name, (unsigned long long)port->phys_base, port->irq, port->misc.minor);
    }

    return 0;
}

static void __exit amba_virt_uart_exit(void)
{
    int i;
    pr_info("amba_virt_uart: exiting\n");

    for (i = 0; i < NUM_PORTS; i++) {
        struct amba_virt_uart_port *port = &g_ports[i];
        if (!IS_ERR_OR_NULL(port->clk)) {
            clk_disable_unprepare(port->clk);
            clk_put(port->clk);
        }
        misc_deregister(&port->misc);
    }
}

module_init(amba_virt_uart_init);
module_exit(amba_virt_uart_exit);

MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Ambarella Virtual UART Host Lease & Level-IRQ Bridge");
MODULE_LICENSE("GPL v2");

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
