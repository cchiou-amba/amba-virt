/*
 * amba_uart.c
 *
 * Ambarella HVM UART Serial Passthrough Driver
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/acpi.h>

#include "amba_uart.h"

#define DRIVER_NAME		"amba_uart"
#define DEV_NAME		"ttyAMBA"

static unsigned long mmio = 0xffe0017000;
module_param(mmio, ulong, 0444);
MODULE_PARM_DESC(mmio, "Physical MMIO base address for fallback platform device (default: 0xffe0017000)");

static int gsi = 146; /* INTID 146 / GIC SPI 114 */
module_param(gsi, int, 0444);
MODULE_PARM_DESC(gsi, "ACPI GSI / INTID for fallback platform device (default: 146 for SPI 114)");

static unsigned int clk_hz = AMBA_UART_DEFAULT_CLK;
module_param(clk_hz, uint, 0444);
MODULE_PARM_DESC(clk_hz, "UART reference clock frequency in Hz (default: 24000000)");

static bool auto_instantiate = true;
module_param(auto_instantiate, bool, 0444);
MODULE_PARM_DESC(auto_instantiate, "Auto-instantiate UART port if DT match is absent (default: 1)");

static struct platform_device *amba_fallback_pdev;
static int amba_fallback_virq = -1;


struct amba_uart_port {
	struct uart_port	port;
	struct platform_device	*pdev;
	unsigned int		id;
	bool			tx_fifo_fix;
};

static struct amba_uart_port *amba_ports[AMBA_UART_MAX_PORTS];
static DEFINE_MUTEX(amba_port_mutex);

static inline u32 amba_uart_read(struct uart_port *port, unsigned int offset)
{
	return readl_relaxed(port->membase + offset);
}

static inline void amba_uart_write(struct uart_port *port, u32 val, unsigned int offset)
{
	writel_relaxed(val, port->membase + offset);
}

static void amba_uart_stop_tx(struct uart_port *port)
{
	u32 ier = amba_uart_read(port, UART_IE_OFFSET);
	amba_uart_write(port, ier & ~(UART_IE_ETBEI | UART_IE_PTIME), UART_IE_OFFSET);
}

static void amba_uart_transmit_chars(struct uart_port *port)
{
	struct circ_buf *xmit = &port->state->xmit;
	int count;

	if (port->x_char) {
		amba_uart_write(port, port->x_char, UART_TH_OFFSET);
		port->icount.tx++;
		port->x_char = 0;
		return;
	}

	if (uart_tx_stopped(port) || uart_circ_empty(xmit)) {
		amba_uart_stop_tx(port);
		return;
	}

	count = port->fifosize;
	while (count-- > 0 && !uart_circ_empty(xmit)) {
		u32 us = amba_uart_read(port, UART_US_OFFSET);
		if (us & UART_US_TFNF) {
			amba_uart_write(port, xmit->buf[xmit->tail], UART_TH_OFFSET);
			xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
			port->icount.tx++;
		} else {
			break;
		}
	}

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	if (uart_circ_empty(xmit))
		amba_uart_stop_tx(port);
}

static void amba_uart_start_tx(struct uart_port *port)
{
	u32 ier;

	if (uart_tx_stopped(port) || !port->state->xmit.buf)
		return;

	ier = amba_uart_read(port, UART_IE_OFFSET);
	if (!(ier & UART_IE_ETBEI))
		amba_uart_write(port, ier | UART_IE_ETBEI, UART_IE_OFFSET);

	amba_uart_transmit_chars(port);
}

static void amba_uart_receive_chars(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;
	unsigned int max_count = port->fifosize * 2;
	u32 ls;

	while (max_count-- > 0) {
		ls = amba_uart_read(port, UART_LS_OFFSET);
		if (!(ls & UART_LS_DR))
			break;

		do {
			u32 ch = amba_uart_read(port, UART_RB_OFFSET);
			char flag = TTY_NORMAL;

			port->icount.rx++;

			if (unlikely(ls & (UART_LS_BI | UART_LS_PE | UART_LS_FE | UART_LS_OE))) {
				if (ls & UART_LS_BI) {
					port->icount.brk++;
					if (uart_handle_break(port))
						goto next_char;
					flag = TTY_BREAK;
				} else if (ls & UART_LS_PE) {
					port->icount.parity++;
					flag = TTY_PARITY;
				} else if (ls & UART_LS_FE) {
					port->icount.frame++;
					flag = TTY_FRAME;
				}
				if (ls & UART_LS_OE) {
					port->icount.overrun++;
					flag = TTY_OVERRUN;
				}
			}

			if (uart_handle_sysrq_char(port, ch))
				goto next_char;

			uart_insert_char(port, ls, UART_LS_OE, ch, flag);

next_char:
			ls = amba_uart_read(port, UART_LS_OFFSET);
		} while (ls & UART_LS_DR);
	}

	tty_flip_buffer_push(tport);
}

static irqreturn_t amba_uart_irq(int irq, void *dev_id)
{
	struct uart_port *port = dev_id;
	unsigned long flags;
	u32 iir;
	int handled = 0;

	spin_lock_irqsave(&port->lock, flags);

	for (;;) {
		iir = amba_uart_read(port, UART_II_OFFSET);
		if (iir & UART_II_NO_INT_PENDING)
			break;

		handled = 1;
		switch (iir & 0x0f) {
		case UART_II_RCV_STATUS:
		case UART_II_RCV_DATA_AVAIL:
		case UART_II_CHAR_TIMEOUT:
		case UART_II_CHAR_TIMEOUT_FIFO_EMPTY:
			amba_uart_receive_chars(port);
			break;

		case UART_II_THR_EMPTY:
			amba_uart_transmit_chars(port);
			break;

		case UART_II_MODEM_STATUS_CHANGED:
			amba_uart_read(port, UART_MS_OFFSET);
			break;

		default:
			break;
		}
	}

	spin_unlock_irqrestore(&port->lock, flags);

	return IRQ_RETVAL(handled);
}

static unsigned int amba_uart_tx_empty(struct uart_port *port)
{
	unsigned long flags;
	u32 ls;

	spin_lock_irqsave(&port->lock, flags);
	ls = amba_uart_read(port, UART_LS_OFFSET);
	spin_unlock_irqrestore(&port->lock, flags);

	return ((ls & (UART_LS_TEMT | UART_LS_THRE)) == (UART_LS_TEMT | UART_LS_THRE)) ?
		TIOCSER_TEMT : 0;
}

static unsigned int amba_uart_get_mctrl(struct uart_port *port)
{
	u32 ms = amba_uart_read(port, UART_MS_OFFSET);
	unsigned int mctrl = 0;

	if (ms & UART_MS_CTS)
		mctrl |= TIOCM_CTS;
	if (ms & UART_MS_DSR)
		mctrl |= TIOCM_DSR;
	if (ms & UART_MS_RI)
		mctrl |= TIOCM_RI;
	if (ms & UART_MS_DCD)
		mctrl |= TIOCM_CD;

	return mctrl | TIOCM_CTS | TIOCM_DSR | TIOCM_CD;
}

static void amba_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	u32 mc = amba_uart_read(port, UART_MC_OFFSET) & ~0x1f;

	if (mctrl & TIOCM_RTS)
		mc |= UART_MC_RTS;
	if (mctrl & TIOCM_DTR)
		mc |= UART_MC_DTR;
	if (mctrl & TIOCM_OUT1)
		mc |= UART_MC_OUT1;
	if (mctrl & TIOCM_OUT2)
		mc |= UART_MC_OUT2;
	if (mctrl & TIOCM_LOOP)
		mc |= UART_MC_LB;

	amba_uart_write(port, mc, UART_MC_OFFSET);
}

static void amba_uart_stop_rx(struct uart_port *port)
{
	u32 ier = amba_uart_read(port, UART_IE_OFFSET);
	amba_uart_write(port, ier & ~UART_IE_ERBFI, UART_IE_OFFSET);
}

static void amba_uart_enable_ms(struct uart_port *port)
{
	u32 ier = amba_uart_read(port, UART_IE_OFFSET);
	amba_uart_write(port, ier | UART_IE_EDSSI, UART_IE_OFFSET);
}

static void amba_uart_break_ctl(struct uart_port *port, int break_state)
{
	unsigned long flags;
	u32 lc;

	spin_lock_irqsave(&port->lock, flags);
	lc = amba_uart_read(port, UART_LC_OFFSET);
	if (break_state != 0)
		lc |= UART_LC_BRK;
	else
		lc &= ~UART_LC_BRK;
	amba_uart_write(port, lc, UART_LC_OFFSET);
	spin_unlock_irqrestore(&port->lock, flags);
}

static void amba_uart_hw_init(struct uart_port *port)
{
	/* Reset FIFO and hardware controller */
	amba_uart_write(port, 0x01, UART_SRR_OFFSET);
	udelay(100);
	amba_uart_write(port, 0x00, UART_SRR_OFFSET);

	/* Enable FIFOs with RX 2-to-full threshold and clear FIFOs */
	amba_uart_write(port, UART_FC_FIFOE | UART_FC_RX_2_TO_FULL |
			UART_FC_TX_EMPTY | UART_FC_XMITR | UART_FC_RCVRR,
			UART_FC_OFFSET);

	/* Enable standard interrupt set */
	amba_uart_write(port, AMBA_UART_DEFAULT_IER, UART_IE_OFFSET);
}

static int amba_uart_startup(struct uart_port *port)
{
	int ret;

	amba_uart_hw_init(port);

	ret = request_irq(port->irq, amba_uart_irq, IRQF_SHARED, "amba_uart", port);
	if (ret) {
		dev_err(port->dev, "Failed to request IRQ %d: %d\n", port->irq, ret);
		return ret;
	}

	return 0;
}

static void amba_uart_shutdown(struct uart_port *port)
{
	unsigned long flags;

	/* Disable interrupts */
	amba_uart_write(port, 0, UART_IE_OFFSET);

	free_irq(port->irq, port);

	spin_lock_irqsave(&port->lock, flags);
	/* Clear break bit */
	amba_uart_write(port, amba_uart_read(port, UART_LC_OFFSET) & ~UART_LC_BRK,
			UART_LC_OFFSET);
	spin_unlock_irqrestore(&port->lock, flags);
}

static void amba_uart_set_termios(struct uart_port *port, struct ktermios *termios,
				  const struct ktermios *old)
{
	unsigned int baud, quot;
	unsigned long flags;
	u32 lc = 0;

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		lc |= UART_LC_CLS_5_BITS;
		break;
	case CS6:
		lc |= UART_LC_CLS_6_BITS;
		break;
	case CS7:
		lc |= UART_LC_CLS_7_BITS;
		break;
	case CS8:
	default:
		lc |= UART_LC_CLS_8_BITS;
		break;
	}

	if (termios->c_cflag & CSTOPB)
		lc |= UART_LC_STOP_2BIT;
	else
		lc |= UART_LC_STOP_1BIT;

	if (termios->c_cflag & PARENB) {
		if (termios->c_cflag & PARODD)
			lc |= (UART_LC_PEN | UART_LC_ODD_PARITY);
		else
			lc |= (UART_LC_PEN | UART_LC_EVEN_PARITY);
	}

	baud = uart_get_baud_rate(port, termios, old, 50, 4000000);
	quot = uart_get_divisor(port, baud);

	spin_lock_irqsave(&port->lock, flags);

	uart_update_timeout(port, termios->c_cflag, baud);

	port->read_status_mask = UART_LS_OE | UART_LS_THRE | UART_LS_DR;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UART_LS_FE | UART_LS_PE;
	if (termios->c_iflag & (BRKINT | PARMRK))
		port->read_status_mask |= UART_LS_BI;

	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UART_LS_PE | UART_LS_FE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= UART_LS_BI;
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UART_LS_OE;
	}
	if ((termios->c_cflag & CREAD) == 0)
		port->ignore_status_mask |= UART_LS_DR;

	/* Write divisor with DLAB bit */
	amba_uart_write(port, amba_uart_read(port, UART_LC_OFFSET) | UART_LC_DLAB,
			UART_LC_OFFSET);
	amba_uart_write(port, quot & 0xff, UART_DLL_OFFSET);
	amba_uart_write(port, (quot >> 8) & 0xff, UART_DLH_OFFSET);

	/* Clear DLAB and apply line parameters */
	amba_uart_write(port, lc, UART_LC_OFFSET);

	amba_uart_set_mctrl(port, port->mctrl);

	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *amba_uart_type(struct uart_port *port)
{
	return (port->type == PORT_8250) ? "amba_uart" : NULL;
}

static void amba_uart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_8250;
}

static int amba_uart_request_port(struct uart_port *port)
{
	return 0;
}

static void amba_uart_release_port(struct uart_port *port)
{
}

static int amba_uart_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_8250)
		return -EINVAL;
	if (ser->irq != port->irq)
		return -EINVAL;
	return 0;
}

static const struct uart_ops amba_uart_ops = {
	.tx_empty	= amba_uart_tx_empty,
	.set_mctrl	= amba_uart_set_mctrl,
	.get_mctrl	= amba_uart_get_mctrl,
	.stop_tx	= amba_uart_stop_tx,
	.start_tx	= amba_uart_start_tx,
	.stop_rx	= amba_uart_stop_rx,
	.enable_ms	= amba_uart_enable_ms,
	.break_ctl	= amba_uart_break_ctl,
	.startup	= amba_uart_startup,
	.shutdown	= amba_uart_shutdown,
	.set_termios	= amba_uart_set_termios,
	.type		= amba_uart_type,
	.release_port	= amba_uart_release_port,
	.request_port	= amba_uart_request_port,
	.config_port	= amba_uart_config_port,
	.verify_port	= amba_uart_verify_port,
};

static struct uart_driver amba_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= DRIVER_NAME,
	.dev_name	= DEV_NAME,
	.major		= 0,	/* Dynamic major number */
	.minor		= 0,
	.nr		= AMBA_UART_MAX_PORTS,
};

static int amba_uart_probe(struct platform_device *pdev)
{
	struct amba_uart_port *amb_port;
	struct resource *res;
	void __iomem *base;
	int irq, id, ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "No memory resource found in DTB\n");
		return -ENODEV;
	}

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "No IRQ resource found in DTB: %d\n", irq);
		return irq;
	}

	mutex_lock(&amba_port_mutex);
	for (id = 0; id < AMBA_UART_MAX_PORTS; id++) {
		if (!amba_ports[id])
			break;
	}
	if (id >= AMBA_UART_MAX_PORTS) {
		mutex_unlock(&amba_port_mutex);
		dev_err(&pdev->dev, "Maximum number of UART ports (%d) exceeded\n",
			AMBA_UART_MAX_PORTS);
		return -EBUSY;
	}

	amb_port = devm_kzalloc(&pdev->dev, sizeof(*amb_port), GFP_KERNEL);
	if (!amb_port) {
		mutex_unlock(&amba_port_mutex);
		return -ENOMEM;
	}

	amb_port->pdev = pdev;
	amb_port->id = id;
	amb_port->port.dev = &pdev->dev;
	amb_port->port.type = PORT_8250;
	amb_port->port.iotype = UPIO_MEM32;
	amb_port->port.membase = base;
	amb_port->port.mapbase = res->start;
	amb_port->port.irq = irq;
	amb_port->port.fifosize = AMBA_UART_FIFO_SIZE;
	amb_port->port.ops = &amba_uart_ops;
	amb_port->port.flags = UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ;
	amb_port->port.line = id;
	amb_port->port.uartclk = clk_hz;
	spin_lock_init(&amb_port->port.lock);

	amba_ports[id] = amb_port;
	platform_set_drvdata(pdev, amb_port);

	ret = uart_add_one_port(&amba_uart_driver, &amb_port->port);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add UART port %d: %d\n", id, ret);
		amba_ports[id] = NULL;
		mutex_unlock(&amba_port_mutex);
		return ret;
	}

	mutex_unlock(&amba_port_mutex);

	dev_info(&pdev->dev, "Ambarella HVM UART%d at MMIO 0x%pa (IRQ %d, clk %u Hz) registered as %s%d\n",
		 id, &res->start, irq, clk_hz, DEV_NAME, id);

	return 0;
}

static int amba_uart_remove(struct platform_device *pdev)
{
	struct amba_uart_port *amb_port = platform_get_drvdata(pdev);

	if (amb_port) {
		mutex_lock(&amba_port_mutex);
		uart_remove_one_port(&amba_uart_driver, &amb_port->port);
		amba_ports[amb_port->id] = NULL;
		mutex_unlock(&amba_port_mutex);
	}

	return 0;
}

static const struct of_device_id amba_uart_of_match[] = {
	{ .compatible = "ambarella,uart" },
	{ .compatible = "ambarella,amba-uart" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, amba_uart_of_match);

static struct platform_driver amba_uart_platform_driver = {
	.probe		= amba_uart_probe,
	.remove		= amba_uart_remove,
	.driver		= {
		.name		= DRIVER_NAME,
		.of_match_table	= amba_uart_of_match,
	},
};

static int __init amba_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&amba_uart_driver);
	if (ret) {
		pr_err("amba_uart: Failed to register UART driver: %d\n", ret);
		return ret;
	}

	ret = platform_driver_register(&amba_uart_platform_driver);
	if (ret) {
		pr_err("amba_uart: Failed to register platform driver: %d\n", ret);
		uart_unregister_driver(&amba_uart_driver);
		return ret;
	}

	/* Fallback platform device when OF DT matching is absent (e.g. ACPI guest) */
	if (!amba_ports[0] && auto_instantiate && mmio) {
		struct resource res[2];
		int virq = -1;

		memset(res, 0, sizeof(res));
		res[0].name = "uart_mmio";
		res[0].flags = IORESOURCE_MEM;
		res[0].start = mmio;
		res[0].end = mmio + 0x1000 - 1;

		if (!acpi_disabled && gsi > 0) {
			virq = acpi_register_gsi(NULL, gsi, ACPI_LEVEL_SENSITIVE, ACPI_ACTIVE_HIGH);
			if (virq < 0) {
				pr_warn("amba_uart: Failed to register ACPI GSI %d: %d\n", gsi, virq);
			} else {
				amba_fallback_virq = virq;
			}
		} else if (gsi > 0) {
			virq = gsi;
		}

		res[1].name = "uart_irq";
		res[1].flags = IORESOURCE_IRQ;
		res[1].start = virq > 0 ? virq : 0;
		res[1].end = virq > 0 ? virq : 0;

		amba_fallback_pdev = platform_device_register_resndata(NULL, DRIVER_NAME, 0,
								       res, ARRAY_SIZE(res),
								       NULL, 0);
		if (IS_ERR(amba_fallback_pdev)) {
			pr_warn("amba_uart: Failed to create fallback platform device: %ld\n",
				PTR_ERR(amba_fallback_pdev));
			if (amba_fallback_virq > 0 && !acpi_disabled) {
				acpi_unregister_gsi(gsi);
				amba_fallback_virq = -1;
			}
			amba_fallback_pdev = NULL;
		}
	}

	pr_info("amba_uart: Ambarella HVM UART passthrough driver loaded (default clk: %u Hz)\n",
		clk_hz);
	return 0;
}

static void __exit amba_uart_exit(void)
{
	if (amba_fallback_pdev) {
		platform_device_unregister(amba_fallback_pdev);
		amba_fallback_pdev = NULL;
	}
	if (amba_fallback_virq > 0 && !acpi_disabled) {
		acpi_unregister_gsi(gsi);
		amba_fallback_virq = -1;
	}
	platform_driver_unregister(&amba_uart_platform_driver);
	uart_unregister_driver(&amba_uart_driver);
	pr_info("amba_uart: Ambarella HVM UART passthrough driver unloaded\n");
}

module_init(amba_uart_init);
module_exit(amba_uart_exit);

MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Ambarella HVM UART Passthrough Serial Driver");
MODULE_LICENSE("GPL");

