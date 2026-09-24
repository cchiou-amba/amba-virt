/*
 * amba_virt_uart_uapi.h
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef _UAPI_AMBA_VIRT_UART_H
#define _UAPI_AMBA_VIRT_UART_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define AMBA_VIRT_UART_MAGIC 'U'

struct amba_virt_uart_info {
    __u32 uart_id;          /* 2 or 3 */
    __u64 phys_base;        /* 0xffe0018000 or 0xffe0019000 */
    __u32 size;             /* 4096 */
    __u32 irq;              /* Linux IRQ number */
    __u64 irq_count;        /* Total IRQs received */
    __u64 ack_count;        /* Total ACKs received */
    __u32 irq_masked;       /* Current masked state */
    __u32 reserved;
};

/* IOCTL Commands */
#define AMBA_VIRT_UART_IOC_GET_INFO    _IOR(AMBA_VIRT_UART_MAGIC, 1, struct amba_virt_uart_info)
#define AMBA_VIRT_UART_IOC_SET_EVENTFD _IOW(AMBA_VIRT_UART_MAGIC, 2, int)
#define AMBA_VIRT_UART_IOC_ACK_IRQ     _IO(AMBA_VIRT_UART_MAGIC, 3)
#define AMBA_VIRT_UART_IOC_RESET       _IO(AMBA_VIRT_UART_MAGIC, 4)

#endif /* _UAPI_AMBA_VIRT_UART_H */
