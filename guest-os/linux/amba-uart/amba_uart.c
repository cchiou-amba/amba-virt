/*
 * amba_uart.c
 *
 * Ambarella HVM UART Serial Passthrough Driver (ivshmem-doorbell PCI & Platform)
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
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
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/circ_buf.h>

#include "amba_uart.h"

#define DRIVER_NAME		"amba_uart"
#define DEV_NAME		"ttyAMBA"

static unsigned int clk_hz = AMBA_UART_DEFAULT_CLK;
static bool use_dma = true;
static bool force_poll = false;

static ssize_t use_dma_show(struct device_driver *driver, char *buf)
{
	return sprintf(buf, "%d\n", use_dma ? 1 : 0);
}

static ssize_t use_dma_store(struct device_driver *driver, const char *buf, size_t count)
{
	bool val;
	if (kstrtobool(buf, &val))
		return -EINVAL;
	use_dma = val;
	return count;
}
static DRIVER_ATTR_RW(use_dma);

static ssize_t force_poll_show(struct device_driver *driver, char *buf)
{
	return sprintf(buf, "%d\n", force_poll ? 1 : 0);
}

static ssize_t force_poll_store(struct device_driver *driver, const char *buf, size_t count)
{
	bool val;
	if (kstrtobool(buf, &val))
		return -EINVAL;
	force_poll = val;
	return count;
}
static DRIVER_ATTR_RW(force_poll);

static ssize_t clk_hz_show(struct device_driver *driver, char *buf)
{
	return sprintf(buf, "%u\n", clk_hz);
}
static DRIVER_ATTR_RO(clk_hz);

static struct attribute *amba_uart_driver_attrs[] = {
	&driver_attr_use_dma.attr,
	&driver_attr_force_poll.attr,
	&driver_attr_clk_hz.attr,
	NULL,
};
ATTRIBUTE_GROUPS(amba_uart_driver);

struct amba_uart_port {
	struct uart_port	port;
	struct pci_dev		*pdev;
	struct platform_device	*plat_dev;
	void __iomem		*doorbell_base;
	struct hrtimer		poll_timer;
	ktime_t			poll_interval;
	unsigned int		id;
	bool			is_pci;
	bool			tx_fifo_fix;
	bool			running;

	/* DMA Engine State (Envelope 15) */
	bool			dma_enabled;
	struct dma_chan		*tx_dma_chan;
	dma_addr_t		tx_dma_addr;
	unsigned char		*tx_dma_buf;
	bool			tx_dma_is_shm;
	size_t			tx_dma_len;
	bool			tx_dma_in_progress;
	dma_cookie_t		tx_cookie;
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

static void amba_uart_doorbell_ack(struct amba_uart_port *amb_port)
{
	if (amb_port && amb_port->doorbell_base) {
		/* Write to Doorbell register: Peer 1 (Host Server), Vector 0 */
		writel((1U << 16) | 0U, amb_port->doorbell_base + 0x0c);
	}
}

static void amba_uart_stop_tx(struct uart_port *port)
{
	(void)port;
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
}

static void amba_uart_start_tx_dma(struct amba_uart_port *amb_port);

static void amba_uart_dma_tx_complete(void *data)
{
	struct amba_uart_port *amb_port = data;
	struct uart_port *port = &amb_port->port;
	struct circ_buf *xmit = &port->state->xmit;
	unsigned long flags;
	unsigned int count;

	spin_lock_irqsave(&port->lock, flags);

	count = amb_port->tx_dma_len;
	xmit->tail = (xmit->tail + count) & (UART_XMIT_SIZE - 1);
	port->icount.tx += count;
	amb_port->tx_dma_in_progress = false;
	amb_port->tx_dma_len = 0;

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	/* If more data is queued, dispatch next burst */
	if (!uart_circ_empty(xmit) && !uart_tx_stopped(port)) {
		if (uart_circ_chars_pending(xmit) >= 16) {
			amba_uart_start_tx_dma(amb_port);
		} else {
			amba_uart_transmit_chars(port);
		}
	}

	spin_unlock_irqrestore(&port->lock, flags);
}

static void amba_uart_start_tx_dma(struct amba_uart_port *amb_port)
{
	struct uart_port *port = &amb_port->port;
	struct circ_buf *xmit = &port->state->xmit;
	struct dma_async_tx_descriptor *desc;
	unsigned int count, c;
	int tail;

	if (amb_port->tx_dma_in_progress)
		return;

	tail = xmit->tail;
	count = uart_circ_chars_pending(xmit);
	if (count == 0)
		return;

	/* Determine contiguous chunk length */
	c = CIRC_CNT_TO_END(xmit->head, tail, UART_XMIT_SIZE);
	if (c > count)
		c = count;
	if (c > UART_XMIT_SIZE)
		c = UART_XMIT_SIZE;

	memcpy(amb_port->tx_dma_buf, &xmit->buf[tail], c);
	wmb();
	amb_port->tx_dma_len = c;
	amb_port->tx_dma_in_progress = true;

	/* Ensure hardware UART DMA mode is asserted */
	amba_uart_write(port, amba_uart_read(port, UART_FC_OFFSET) | UART_FC_DMA_SELECT, UART_FC_OFFSET);
	amba_uart_write(port, 0x02, UART_DMAE_OFFSET);

	desc = dmaengine_prep_slave_single(amb_port->tx_dma_chan,
					   amb_port->tx_dma_addr,
					   c,
					   DMA_MEM_TO_DEV,
					   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		amb_port->tx_dma_in_progress = false;
		amb_port->tx_dma_len = 0;
		amba_uart_transmit_chars(port);
		return;
	}

	desc->callback = amba_uart_dma_tx_complete;
	desc->callback_param = amb_port;

	amb_port->tx_cookie = dmaengine_submit(desc);
	dma_async_issue_pending(amb_port->tx_dma_chan);
}

static void amba_uart_start_tx(struct uart_port *port)
{
	struct amba_uart_port *amb_port = container_of(port, struct amba_uart_port, port);
	struct circ_buf *xmit = &port->state->xmit;

	if (port->x_char) {
		amba_uart_write(port, port->x_char, UART_TH_OFFSET);
		port->icount.tx++;
		port->x_char = 0;
		return;
	}

	if (uart_tx_stopped(port) || uart_circ_empty(xmit))
		return;

	/* Offload bursts >= 16 bytes via DMA when active */
	if (amb_port->dma_enabled && amb_port->tx_dma_chan && !amb_port->tx_dma_in_progress) {
		if (uart_circ_chars_pending(xmit) >= 16) {
			amba_uart_start_tx_dma(amb_port);
			return;
		}
	}

	/* Short burst or DMA busy: use PIO FIFO transfer */
	amba_uart_transmit_chars(port);
}

static void amba_uart_receive_chars(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;
	unsigned int max_count = 256;
	u32 ls;
	u8 ch, flag;
	bool pushed = false;

	ls = amba_uart_read(port, UART_LS_OFFSET);
	while ((ls & UART_LS_DR) && max_count-- > 0) {
		ch = (u8)amba_uart_read(port, UART_RB_OFFSET);
		flag = TTY_NORMAL;
		port->icount.rx++;

		if (unlikely(ls & (UART_LS_BI | UART_LS_PE | UART_LS_FE | UART_LS_OE))) {
			if (ls & UART_LS_BI) {
				ls &= ~(UART_LS_FE | UART_LS_PE);
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
		pushed = true;

next_char:
		ls = amba_uart_read(port, UART_LS_OFFSET);
	}

	if (pushed)
		tty_flip_buffer_push(tport);
}

static enum hrtimer_restart amba_uart_poll_timer(struct hrtimer *timer)
{
	struct amba_uart_port *amb_port = container_of(timer, struct amba_uart_port, poll_timer);
	struct uart_port *port = &amb_port->port;
	unsigned long flags;

	if (!amb_port->running)
		return HRTIMER_NORESTART;

	spin_lock_irqsave(&port->lock, flags);
	amba_uart_receive_chars(port);
	if (!uart_circ_empty(&port->state->xmit) && !uart_tx_stopped(port))
		amba_uart_transmit_chars(port);
	spin_unlock_irqrestore(&port->lock, flags);

	hrtimer_forward_now(timer, amb_port->poll_interval);
	return HRTIMER_RESTART;
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
	(void)port;
}

static void amba_uart_enable_ms(struct uart_port *port)
{
	(void)port;
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

static irqreturn_t amba_uart_interrupt(int irq, void *dev_id)
{
	struct uart_port *port = dev_id;
	struct amba_uart_port *amb_port = container_of(port, struct amba_uart_port, port);
	u32 iir;
	unsigned long flags;

	iir = amba_uart_read(port, UART_II_OFFSET);
	if (iir & UART_II_NO_INT_PENDING) {
		amba_uart_doorbell_ack(amb_port);
		return IRQ_NONE;
	}

	spin_lock_irqsave(&port->lock, flags);
	amba_uart_receive_chars(port);
	if (!uart_circ_empty(&port->state->xmit) && !uart_tx_stopped(port))
		amba_uart_transmit_chars(port);
	spin_unlock_irqrestore(&port->lock, flags);

	/* Ring ivshmem doorbell to unmask host physical IRQ */
	amba_uart_doorbell_ack(amb_port);

	return IRQ_HANDLED;
}

static bool amba_uart_dma_filter(struct dma_chan *chan, void *param)
{
	(void)param;
	return true;
}

static void amba_uart_release_dma(struct amba_uart_port *amb_port)
{
	if (amb_port->tx_dma_chan) {
		dmaengine_terminate_sync(amb_port->tx_dma_chan);
		dma_release_channel(amb_port->tx_dma_chan);
		amb_port->tx_dma_chan = NULL;
	}

	if (amb_port->tx_dma_buf) {
		if (!amb_port->tx_dma_is_shm) {
			dma_free_coherent(amb_port->port.dev, UART_XMIT_SIZE,
					  amb_port->tx_dma_buf, amb_port->tx_dma_addr);
		}
		amb_port->tx_dma_buf = NULL;
		amb_port->tx_dma_is_shm = false;
	}

	amb_port->dma_enabled = false;
	amb_port->tx_dma_in_progress = false;
}

static void amba_uart_init_dma(struct amba_uart_port *amb_port)
{
	struct uart_port *port = &amb_port->port;
	dma_cap_mask_t mask;
	struct dma_slave_config cfg;
	phys_addr_t shm_phys = 0;
	void __iomem *shm_iomem = NULL;
	size_t shm_size = 0;
	int ret;

	if (!use_dma) {
		dev_info(port->dev, "DMA disabled by module parameter (use_dma=0)\n");
		return;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	amb_port->tx_dma_chan = dma_request_channel(mask, amba_uart_dma_filter, NULL);
	if (!amb_port->tx_dma_chan) {
		dev_info(port->dev, "No DMA channel available; continuing in interrupt-driven PIO mode\n");
		return;
	}

	/*
	 * Allocate TX buffer from ivshmem shared window so physical Generic-DMA1
	 * on Dom0 can read from it directly.
	 * Resolve amba_virt_get_window dynamically via __symbol_get to avoid
	 * static module dependency cycles and sysfs mod_sysfs_setup faults.
	 */
	{
		int (*get_win_fn)(phys_addr_t *, void __iomem **, size_t *) =
			__symbol_get("amba_virt_get_window");
		if (get_win_fn) {
			ret = get_win_fn(&shm_phys, &shm_iomem, &shm_size);
			symbol_put_addr((void *)get_win_fn);
		} else {
			ret = -ENODEV;
		}
	}
	if (ret == 0 && shm_iomem && shm_size >= 0x00200000) {
		/* UART staging buffers at 0x00100000 + (port_id * 0x1000) */
		u32 offset = 0x00100000 + (amb_port->id * 0x1000);
		amb_port->tx_dma_buf = (unsigned char *)shm_iomem + offset;
		amb_port->tx_dma_addr = (dma_addr_t)(shm_phys + offset);
		amb_port->tx_dma_is_shm = true;
	} else {
		amb_port->tx_dma_buf = dma_alloc_coherent(port->dev, UART_XMIT_SIZE,
							  &amb_port->tx_dma_addr, GFP_KERNEL);
		amb_port->tx_dma_is_shm = false;
	}

	if (!amb_port->tx_dma_buf) {
		dev_warn(port->dev, "Failed to allocate DMA buffer; falling back to PIO\n");
		dma_release_channel(amb_port->tx_dma_chan);
		amb_port->tx_dma_chan = NULL;
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.direction = DMA_MEM_TO_DEV;
	cfg.dst_addr = port->mapbase + UART_DMAF_OFFSET;
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	cfg.dst_maxburst = 8;

	ret = dmaengine_slave_config(amb_port->tx_dma_chan, &cfg);
	if (ret) {
		dev_warn(port->dev, "dmaengine_slave_config failed: %d; falling back to PIO\n", ret);
		if (!amb_port->tx_dma_is_shm) {
			dma_free_coherent(port->dev, UART_XMIT_SIZE, amb_port->tx_dma_buf, amb_port->tx_dma_addr);
		}
		amb_port->tx_dma_buf = NULL;
		amb_port->tx_dma_is_shm = false;
		dma_release_channel(amb_port->tx_dma_chan);
		amb_port->tx_dma_chan = NULL;
		return;
	}

	amb_port->dma_enabled = true;
	amb_port->tx_dma_in_progress = false;
	dev_info(port->dev, "DMA TX acceleration initialized (channel: %s, %s buf=0x%llx)\n",
		 dma_chan_name(amb_port->tx_dma_chan),
		 amb_port->tx_dma_is_shm ? "ivshmem" : "coherent",
		 (unsigned long long)amb_port->tx_dma_addr);
}

static void amba_uart_hw_init(struct uart_port *port)
{
	struct amba_uart_port *amb_port = container_of(port, struct amba_uart_port, port);

	/* Reset FIFO and hardware controller */
	amba_uart_write(port, 0x01, UART_SRR_OFFSET);
	udelay(100);
	amba_uart_write(port, 0x00, UART_SRR_OFFSET);

	/* Enable FIFOs with 1-character RX threshold and clear FIFOs */
	amba_uart_write(port, UART_FC_FIFOE | UART_FC_RX_ONECHAR |
			UART_FC_TX_EMPTY | UART_FC_XMITR | UART_FC_RCVRR,
			UART_FC_OFFSET);

	/* Assert DTR, RTS, and OUT2 (hardware interrupt gate) */
	amba_uart_write(port, UART_MC_OUT2 | UART_MC_RTS | UART_MC_DTR,
			UART_MC_OFFSET);

	/* Configure DMA registers if DMA is enabled */
	if (amb_port->dma_enabled) {
		amba_uart_write(port, amba_uart_read(port, UART_FC_OFFSET) | UART_FC_DMA_SELECT, UART_FC_OFFSET);
		amba_uart_write(port, 0x02, UART_DMAE_OFFSET);
	}

	/* Enable configured interrupts (RX Data, Line Status, Character Timeout) */
	amba_uart_write(port, AMBA_UART_DEFAULT_IER, UART_IE_OFFSET);
}

static int amba_uart_startup(struct uart_port *port)
{
	struct amba_uart_port *amb_port = container_of(port, struct amba_uart_port, port);
	int ret = 0;

	/* Initialize DMA channel before hardware setup */
	amba_uart_init_dma(amb_port);

	amba_uart_hw_init(port);

	port->mctrl = TIOCM_DTR | TIOCM_RTS | TIOCM_OUT2;
	amba_uart_set_mctrl(port, port->mctrl);
	amba_uart_write(port, AMBA_UART_DEFAULT_IER, UART_IE_OFFSET);

	/* Unmask host physical IRQ upon startup */
	amba_uart_doorbell_ack(amb_port);

	if (port->irq > 0 && !force_poll) {
		ret = request_irq(port->irq, amba_uart_interrupt, IRQF_SHARED, DRIVER_NAME, port);
		if (ret) {
			dev_err(port->dev, "Failed to request IRQ %d: %d\n", port->irq, ret);
			amba_uart_release_dma(amb_port);
			return ret;
		}
		dev_info(port->dev, "Ambarella UART%d running in interrupt-driven mode (IRQ %d, DMA %s)\n",
			 amb_port->id, port->irq, amb_port->dma_enabled ? "enabled" : "disabled");
	} else {
		/* Fallback to hrtimer polling if no IRQ allocated */
		amb_port->running = true;
		amb_port->poll_interval = ms_to_ktime(1);
		hrtimer_init(&amb_port->poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		amb_port->poll_timer.function = amba_uart_poll_timer;
		hrtimer_start(&amb_port->poll_timer, amb_port->poll_interval, HRTIMER_MODE_REL);
		dev_info(port->dev, "Ambarella UART%d running in polling mode (1 ms hrtimer, DMA %s)\n",
			 amb_port->id, amb_port->dma_enabled ? "enabled" : "disabled");
	}

	return 0;
}

static void amba_uart_shutdown(struct uart_port *port)
{
	struct amba_uart_port *amb_port = container_of(port, struct amba_uart_port, port);
	unsigned long flags;

	if (amb_port->running) {
		amb_port->running = false;
		hrtimer_cancel(&amb_port->poll_timer);
	}

	/* Disable interrupts and DMA */
	amba_uart_write(port, 0, UART_IE_OFFSET);
	amba_uart_write(port, 0, UART_DMAE_OFFSET);

	if (port->irq > 0)
		free_irq(port->irq, port);

	amba_uart_release_dma(amb_port);

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

	/* Restore interrupt enables and modem control after clearing DLAB */
	amba_uart_write(port, AMBA_UART_DEFAULT_IER, UART_IE_OFFSET);
	port->mctrl |= TIOCM_OUT2;
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
	(void)port;
	return 0;
}

static void amba_uart_release_port(struct uart_port *port)
{
	(void)port;
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

/* ========================================================================== */
/* PCI Driver for ivshmem-doorbell UART Passthrough                           */
/* ========================================================================== */

static int amba_uart_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct amba_uart_port *amb_port;
	void __iomem *doorbell_base = NULL;
	void __iomem *uart_base = NULL;
	int ret, irq, id;

	(void)ent;

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", ret);
		return ret;
	}

	/* Ensure device is a UART ivshmem-doorbell aperture (BAR2 == 4 KiB), not shared memory */
	if (pci_resource_len(pdev, 2) != 0x1000) {
		pci_disable_device(pdev);
		return -ENODEV;
	}

	ret = pci_request_regions(pdev, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "pci_request_regions failed: %d\n", ret);
		goto err_disable_pci;
	}

	/* Map BAR 0 (Doorbell control register, 256B) */
	doorbell_base = pci_iomap(pdev, 0, 0);
	if (!doorbell_base) {
		dev_warn(&pdev->dev, "Could not map BAR 0 (Doorbell)\n");
	}

	/* Map BAR 2 (Physical UART MMIO aperture, 4 KiB) */
	uart_base = pci_iomap(pdev, 2, 0);
	if (!uart_base) {
		dev_err(&pdev->dev, "Failed to map BAR 2 (UART MMIO)\n");
		ret = -ENOMEM;
		goto err_unmap_doorbell;
	}

	/* Allocate MSI-X / MSI vector */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI | PCI_IRQ_LEGACY);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to allocate IRQ vectors: %d\n", ret);
		goto err_unmap_uart;
	}

	irq = pci_irq_vector(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "Failed to get PCI IRQ vector: %d\n", irq);
		ret = irq;
		goto err_free_irq_vectors;
	}

	pci_set_master(pdev);
	dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

	mutex_lock(&amba_port_mutex);
	for (id = 0; id < AMBA_UART_MAX_PORTS; id++) {
		if (!amba_ports[id])
			break;
	}
	if (id >= AMBA_UART_MAX_PORTS) {
		mutex_unlock(&amba_port_mutex);
		dev_err(&pdev->dev, "Maximum number of UART ports (%d) exceeded\n",
			AMBA_UART_MAX_PORTS);
		ret = -EBUSY;
		goto err_free_irq_vectors;
	}

	amb_port = devm_kzalloc(&pdev->dev, sizeof(*amb_port), GFP_KERNEL);
	if (!amb_port) {
		mutex_unlock(&amba_port_mutex);
		ret = -ENOMEM;
		goto err_free_irq_vectors;
	}

	amb_port->pdev = pdev;
	amb_port->is_pci = true;
	amb_port->doorbell_base = doorbell_base;
	amb_port->id = id;
	amb_port->port.dev = &pdev->dev;
	amb_port->port.type = PORT_8250;
	amb_port->port.iotype = UPIO_MEM32;
	amb_port->port.membase = uart_base;
	amb_port->port.mapbase = pci_resource_start(pdev, 2);
	amb_port->port.irq = irq;
	amb_port->port.fifosize = AMBA_UART_FIFO_SIZE;
	amb_port->port.ops = &amba_uart_ops;
	amb_port->port.flags = UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ;
	amb_port->port.line = id;
	amb_port->port.uartclk = clk_hz;
	amb_port->port.mctrl = TIOCM_DTR | TIOCM_RTS | TIOCM_OUT2;
	spin_lock_init(&amb_port->port.lock);

	amba_ports[id] = amb_port;
	pci_set_drvdata(pdev, amb_port);

	ret = uart_add_one_port(&amba_uart_driver, &amb_port->port);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add UART port %d: %d\n", id, ret);
		amba_ports[id] = NULL;
		mutex_unlock(&amba_port_mutex);
		goto err_free_irq_vectors;
	}

	mutex_unlock(&amba_port_mutex);

	dev_info(&pdev->dev, "Ambarella Virtual UART%d at PCI BAR2 0x%llx (IRQ %d, clk %u Hz) registered as %s%d\n",
		 id, (unsigned long long)pci_resource_start(pdev, 2), irq, clk_hz, DEV_NAME, id);

	return 0;

err_free_irq_vectors:
	pci_free_irq_vectors(pdev);
err_unmap_uart:
	if (uart_base) pci_iounmap(pdev, uart_base);
err_unmap_doorbell:
	if (doorbell_base) pci_iounmap(pdev, doorbell_base);
	pci_release_regions(pdev);
err_disable_pci:
	pci_disable_device(pdev);
	return ret;
}

static void amba_uart_pci_remove(struct pci_dev *pdev)
{
	struct amba_uart_port *amb_port = pci_get_drvdata(pdev);

	if (amb_port) {
		mutex_lock(&amba_port_mutex);
		uart_remove_one_port(&amba_uart_driver, &amb_port->port);
		amba_ports[amb_port->id] = NULL;
		mutex_unlock(&amba_port_mutex);

		pci_free_irq_vectors(pdev);
		if (amb_port->port.membase)
			pci_iounmap(pdev, amb_port->port.membase);
		if (amb_port->doorbell_base)
			pci_iounmap(pdev, amb_port->doorbell_base);
		pci_release_regions(pdev);
		pci_disable_device(pdev);
	}
}

static const struct pci_device_id amba_uart_pci_ids[] = {
	{ PCI_DEVICE(0x1af4, 0x1110) }, /* Red Hat ivshmem device */
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, amba_uart_pci_ids);

static struct pci_driver amba_uart_pci_driver = {
	.name		= DRIVER_NAME,
	.id_table	= amba_uart_pci_ids,
	.probe		= amba_uart_pci_probe,
	.remove		= amba_uart_pci_remove,
	.driver		= {
		.groups	= amba_uart_driver_groups,
	},
};

/* ========================================================================== */
/* Platform Driver for Fallback / Legacy Discovery                           */
/* ========================================================================== */

static int amba_uart_platform_probe(struct platform_device *pdev)
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

	amb_port->plat_dev = pdev;
	amb_port->is_pci = false;
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
	amb_port->port.mctrl = TIOCM_DTR | TIOCM_RTS | TIOCM_OUT2;
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

static int amba_uart_platform_remove(struct platform_device *pdev)
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
	.probe		= amba_uart_platform_probe,
	.remove		= amba_uart_platform_remove,
	.driver		= {
		.name		= DRIVER_NAME,
		.of_match_table	= amba_uart_of_match,
		.groups		= amba_uart_driver_groups,
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

	ret = pci_register_driver(&amba_uart_pci_driver);
	if (ret) {
		pr_warn("amba_uart: Failed to register PCI driver: %d\n", ret);
	}

	ret = platform_driver_register(&amba_uart_platform_driver);
	if (ret) {
		pr_warn("amba_uart: Failed to register platform driver: %d\n", ret);
	}

	pr_info("amba_uart: Ambarella HVM UART passthrough driver loaded (default clk: %u Hz)\n",
		clk_hz);
	return 0;
}

static void __exit amba_uart_exit(void)
{
	platform_driver_unregister(&amba_uart_platform_driver);
	pci_unregister_driver(&amba_uart_pci_driver);
	uart_unregister_driver(&amba_uart_driver);
	pr_info("amba_uart: Ambarella HVM UART passthrough driver unloaded\n");
}

module_init(amba_uart_init);
module_exit(amba_uart_exit);

MODULE_AUTHOR("Ambarella International LLC");
MODULE_DESCRIPTION("Ambarella HVM UART Passthrough Serial Driver");
MODULE_LICENSE("GPL");
