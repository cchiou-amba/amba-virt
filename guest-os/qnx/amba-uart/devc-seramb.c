/*
 * devc-seramb.c
 *
 * QNX Neutrino RTOS 8.0 Serial Driver for Ambarella UART Passthrough (/dev/ser3).
 * Supports Stage-2 MMIO passthrough, native GIC interrupt handling, and FIFO polling.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/io-char.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <hw/inout.h>

#define DRIVER_NAME		"devc-seramb"
#define DEFAULT_DEV_NAME	"/dev/ser3"
#define DEFAULT_CLK_HZ		24000000	/* 24 MHz */
#define DEFAULT_BAUD		115200
#define DEFAULT_UART_PHYS	0x804020c000ULL	/* QEMU virt IVSHMEM BAR2 GPA */
#define DEFAULT_UART_IRQ	0		/* 0: pure polling mode (QEMU FDT omits virtual IRQ routing) */
#define UART_MAP_SIZE		0x1000		/* 4 KiB */

/* ========================================================================== */
/* Ambarella DesignWare UART Register Offsets (32-bit MMIO)                   */
/* ========================================================================== */
#define UART_RB_OFFSET		0x00	/* Receive Buffer Register (R)       */
#define UART_TH_OFFSET		0x00	/* Transmit Holding Register (W)     */
#define UART_DLL_OFFSET		0x00	/* Divisor Latch Low (when DLAB=1)   */
#define UART_IE_OFFSET		0x04	/* Interrupt Enable Register         */
#define UART_DLH_OFFSET		0x04	/* Divisor Latch High (when DLAB=1)  */
#define UART_II_OFFSET		0x08	/* Interrupt Identification Reg (R)  */
#define UART_FC_OFFSET		0x08	/* FIFO Control Register (W)         */
#define UART_LC_OFFSET		0x0c	/* Line Control Register             */
#define UART_MC_OFFSET		0x10	/* Modem Control Register            */
#define UART_LS_OFFSET		0x14	/* Line Status Register              */
#define UART_MS_OFFSET		0x18	/* Modem Status Register             */
#define UART_DMAE_OFFSET	0x28	/* DMA Enable Register               */
#define UART_DMAF_OFFSET	0x40	/* DMA FIFO Register                 */
#define UART_US_OFFSET		0x7c	/* UART Status Register              */
#define UART_SRR_OFFSET		0x88	/* Software Reset Register           */

/* FIFO Control Register Bits */
#define UART_FC_FIFOE		0x01
#define UART_FC_RCVRR		0x02
#define UART_FC_XMITR		0x04
#define UART_FC_DMA_SELECT	0x08
#define UART_FC_TX_EMPTY	0x00
#define UART_FC_RX_2_TO_FULL	0xc0

/* Line Control Register Bits */
#define UART_LC_CLS_8_BITS	0x03
#define UART_LC_STOP_1BIT	0x00
#define UART_LC_DLAB		0x80

/* Line Status Register Bits */
#define UART_LS_DR		0x01
#define UART_LS_OE		0x02
#define UART_LS_PE		0x04
#define UART_LS_FE		0x08
#define UART_LS_BI		0x10
#define UART_LS_THRE		0x20
#define UART_LS_TEMT		0x40

/* UART Status Register Bits */
#define UART_US_BUSY		0x01
#define UART_US_TFNF		0x02
#define UART_US_TFE		0x04
#define UART_US_RFNE		0x08
#define UART_US_RFF		0x10

typedef struct dev_entry {
	TTYDEV			tty;
	uintptr_t		base;
	uint64_t		phys;
	int			irq;
	int			intr_id;
	unsigned int		clk;
	unsigned int		baud;
	bool			running;
	pthread_t		intr_tid;
	pthread_t		poll_tid;
	char			devname[TTY_NAME_MAX];
	bool			verbose;
	bool			foreground;
	bool			dma_enabled;
	bool			use_interrupts;
} DEV_ENTRY;

static DEV_ENTRY g_amb_dev;
TTYCTRL ttyctrl;

static inline uint32_t uart_read(DEV_ENTRY *amb, unsigned int offset)
{
	return in32(amb->base + offset);
}

static inline void uart_write(DEV_ENTRY *amb, uint32_t val, unsigned int offset)
{
	out32(amb->base + offset, val);
}

static void uart_hw_init(DEV_ENTRY *amb)
{
	unsigned int quot;

	/* 1. Software Reset */
	uart_write(amb, 0x01, UART_SRR_OFFSET);
	delay(1);
	uart_write(amb, 0x00, UART_SRR_OFFSET);

	/* 2. Configure FIFOs */
	uint32_t fcr = UART_FC_FIFOE | UART_FC_RX_2_TO_FULL |
		       UART_FC_TX_EMPTY | UART_FC_XMITR | UART_FC_RCVRR;
	if (amb->dma_enabled) {
		fcr |= UART_FC_DMA_SELECT;
	}
	uart_write(amb, fcr, UART_FC_OFFSET);
	if (amb->dma_enabled) {
		uart_write(amb, 0x02, UART_DMAE_OFFSET);
	}

	/* 3. Configure Interrupts: enable RX available if interrupt mode enabled */
	if (amb->use_interrupts) {
		uart_write(amb, 0x01, UART_IE_OFFSET); /* ERBFI: Enable Received Data Available */
	} else {
		uart_write(amb, 0x00, UART_IE_OFFSET);
	}

	/* 4. Configure Baud Rate & 8N1 */
	quot = amb->clk / (16 * amb->baud);
	uart_write(amb, UART_LC_DLAB | UART_LC_CLS_8_BITS, UART_LC_OFFSET);
	uart_write(amb, quot & 0xff, UART_DLL_OFFSET);
	uart_write(amb, (quot >> 8) & 0xff, UART_DLH_OFFSET);
	uart_write(amb, UART_LC_CLS_8_BITS, UART_LC_OFFSET);

	/* 5. Assert DTR / RTS */
	uart_write(amb, 0x03, UART_MC_OFFSET);

	if (amb->verbose) {
		printf("devc-seramb: HW Init complete (US=0x%08x, LS=0x%08x, divisor=%u, IRQ=%d (%s), DMA=%s)\n",
		       uart_read(amb, UART_US_OFFSET),
		       uart_read(amb, UART_LS_OFFSET),
		       quot,
		       amb->irq,
		       amb->use_interrupts ? "active" : "polling-fallback",
		       amb->dma_enabled ? "enabled" : "disabled");
	}
}

static pthread_mutex_t g_tx_mutex = PTHREAD_MUTEX_INITIALIZER;

int tto(TTYDEV *dev, int action, int arg)
{
	DEV_ENTRY *amb = (DEV_ENTRY *)dev;
	(void)arg;

	switch (action) {
	case TTO_DATA:
	case TTO_EVENT:
		if (dev->flags & (OHW_PAGED | OSW_PAGED))
			return 0;

		pthread_mutex_lock(&g_tx_mutex);
		dev_lock(dev);
		while ((uart_read(amb, UART_US_OFFSET) & UART_US_TFNF) && (dev->obuf.cnt > 0)) {
			unsigned char ch = tto_getchar(dev);
			uart_write(amb, (uint32_t)ch, UART_TH_OFFSET);
		}
		dev_unlock(dev);
		int status = tto_checkclients(dev);
		pthread_mutex_unlock(&g_tx_mutex);
		return status;

	case TTO_STTY:
		if (dev->baud != amb->baud && dev->baud > 0) {
			unsigned int quot = amb->clk / (16 * dev->baud);
			uart_write(amb, uart_read(amb, UART_LC_OFFSET) | UART_LC_DLAB, UART_LC_OFFSET);
			uart_write(amb, quot & 0xff, UART_DLL_OFFSET);
			uart_write(amb, (quot >> 8) & 0xff, UART_DLH_OFFSET);
			uart_write(amb, uart_read(amb, UART_LC_OFFSET) & ~UART_LC_DLAB, UART_LC_OFFSET);
			amb->baud = dev->baud;
		}
		return 0;

	case TTO_CTRL:
	case TTO_LINESTATUS:
	default:
		return 0;
	}
}

static void drain_rx_tx(DEV_ENTRY *amb)
{
	uint32_t ls = uart_read(amb, UART_LS_OFFSET);
	uint32_t us = uart_read(amb, UART_US_OFFSET);
	int events = 0;
	int count = 0;

	/* Bus float / unmapped guard */
	if (ls == 0xffffffff || us == 0xffffffff) {
		return;
	}

	/* 1. Drain RX FIFO */
	dev_lock(&amb->tty);
	while (((ls & UART_LS_DR) || (us & UART_US_RFNE)) && (++count < 256)) {
		uint32_t ch = uart_read(amb, UART_RB_OFFSET);
		events |= tti(&amb->tty, (unsigned char)(ch & 0xff));
		ls = uart_read(amb, UART_LS_OFFSET);
		us = uart_read(amb, UART_US_OFFSET);
		if (ls == 0xffffffff || us == 0xffffffff) {
			break;
		}
	}
	dev_unlock(&amb->tty);

	if (events) {
		iochar_send_event(&amb->tty);
	}

	/* 2. Drain TX Buffer through unified thread-safe tto */
	if (amb->tty.obuf.cnt > 0) {
		if (tto(&amb->tty, TTO_DATA, 0)) {
			iochar_send_event(&amb->tty);
		}
	}
}

static void *intr_thread(void *arg)
{
	DEV_ENTRY *amb = (DEV_ENTRY *)arg;

	while (amb->running) {
		int r = InterruptWait(0, NULL);
		if (r == -1) {
			delay(10);
			continue;
		}
		drain_rx_tx(amb);
		InterruptUnmask(amb->irq, amb->intr_id);
	}

	return NULL;
}

static void *poll_thread(void *arg)
{
	DEV_ENTRY *amb = (DEV_ENTRY *)arg;
	struct timespec ts = { 0, 1000000 }; /* 1 ms poll interval */

	while (amb->running) {
		drain_rx_tx(amb);
		nanosleep(&ts, NULL);
	}

	return NULL;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -p, --port <name>     Device name to register (default: %s)\n", DEFAULT_DEV_NAME);
	printf("  -b, --baud <rate>     Baud rate (default: %u)\n", DEFAULT_BAUD);
	printf("  -c, --clk <hz>        UART reference clock in Hz (default: %u)\n", DEFAULT_CLK_HZ);
	printf("  -a, --phys <hex>      Physical MMIO aperture address (default: 0x%08llx)\n", DEFAULT_UART_PHYS);
	printf("  -i, --irq <num>       Interrupt vector (default: %d, 0 to disable)\n", DEFAULT_UART_IRQ);
	printf("  -D, --no-dma          Disable DMA acceleration (use pure PIO)\n");
	printf("  -f, --foreground      Run in foreground (do not daemonize)\n");
	printf("  -v, --verbose         Enable verbose debugging output\n");
	printf("  -?, --help            Show this help message\n");
}

int main(int argc, char **argv)
{
	int opt;

	static struct option long_opts[] = {
		{"port",       required_argument, NULL, 'p'},
		{"baud",       required_argument, NULL, 'b'},
		{"clk",        required_argument, NULL, 'c'},
		{"phys",       required_argument, NULL, 'a'},
		{"irq",        required_argument, NULL, 'i'},
		{"no-dma",     no_argument,       NULL, 'D'},
		{"foreground", no_argument,       NULL, 'f'},
		{"verbose",    no_argument,       NULL, 'v'},
		{"help",       no_argument,       NULL, '?'},
		{NULL, 0, NULL, 0}
	};

	memset(&g_amb_dev, 0, sizeof(g_amb_dev));
	strncpy(g_amb_dev.devname, DEFAULT_DEV_NAME, sizeof(g_amb_dev.devname) - 1);
	g_amb_dev.clk = DEFAULT_CLK_HZ;
	g_amb_dev.baud = DEFAULT_BAUD;
	g_amb_dev.phys = DEFAULT_UART_PHYS;
	g_amb_dev.irq = DEFAULT_UART_IRQ;
	g_amb_dev.intr_id = -1;
	g_amb_dev.dma_enabled = false; /* Default: PIO polling mode */

	while ((opt = getopt_long(argc, argv, "p:b:c:a:i:Dfv?", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'p':
			strncpy(g_amb_dev.devname, optarg, sizeof(g_amb_dev.devname) - 1);
			break;
		case 'b':
			g_amb_dev.baud = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'c':
			g_amb_dev.clk = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'a':
			g_amb_dev.phys = strtoull(optarg, NULL, 0);
			break;
		case 'i':
			g_amb_dev.irq = (int)strtol(optarg, NULL, 0);
			break;
		case 'D':
			g_amb_dev.dma_enabled = false;
			break;
		case 'f':
			g_amb_dev.foreground = true;
			break;
		case 'v':
			g_amb_dev.verbose = true;
			break;
		case '?':
		default:
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		}
	}

	/* 1. Request I/O privileges for hardware MMIO and interrupt handling */
	if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) {
		fprintf(stderr, "devc-seramb: Failed to obtain I/O privileges: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	/* 2. Map UART aperture into process virtual address space */
	g_amb_dev.base = (uintptr_t)mmap_device_memory(
		NULL,
		UART_MAP_SIZE,
		PROT_READ | PROT_WRITE | PROT_NOCACHE,
		0,
		g_amb_dev.phys);

	if (g_amb_dev.base == (uintptr_t)MAP_FAILED) {
		fprintf(stderr, "devc-seramb: Failed to map MMIO 0x%llx: %s\n",
			(unsigned long long)g_amb_dev.phys, strerror(errno));
		return EXIT_FAILURE;
	}

	/* 3. Attach hardware interrupt if requested */
	g_amb_dev.use_interrupts = false;
	if (g_amb_dev.irq > 0) {
		struct sigevent event;
		SIGEV_INTR_INIT(&event);
		g_amb_dev.intr_id = InterruptAttachEvent(g_amb_dev.irq, &event, _NTO_INTR_FLAGS_TRK_MSK);
		if (g_amb_dev.intr_id == -1) {
			fprintf(stderr, "devc-seramb: Warning: InterruptAttachEvent(irq=%d) failed: %s; using polling fallback\n",
				g_amb_dev.irq, strerror(errno));
		} else {
			g_amb_dev.use_interrupts = true;
			if (g_amb_dev.verbose) {
				printf("devc-seramb: Attached interrupt vector %d (intr_id=%d)\n",
				       g_amb_dev.irq, g_amb_dev.intr_id);
			}
		}
	}

	/* 4. Initialize QNX io-char TTY controller */
	ttyctrl.dpp = dispatch_create();
	if (!ttyctrl.dpp) {
		fprintf(stderr, "devc-seramb: dispatch_create failed: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	ttyctrl.max_devs = 16;
	ttyctrl.perm = 0666;
	if (g_amb_dev.foreground)
		ttyctrl.flags |= NODAEMONIZE;

	ttc(TTC_INIT_PROC, &ttyctrl, 0);

	{
		unsigned int isize = 4096;
		unsigned int osize = 16384;
		unsigned int csize = 1024;

		g_amb_dev.tty.ibuf.buff = malloc(isize);
		g_amb_dev.tty.ibuf.head = g_amb_dev.tty.ibuf.buff;
		g_amb_dev.tty.ibuf.tail = g_amb_dev.tty.ibuf.buff;
		g_amb_dev.tty.ibuf.size = isize;

		g_amb_dev.tty.obuf.buff = malloc(osize);
		g_amb_dev.tty.obuf.head = g_amb_dev.tty.obuf.buff;
		g_amb_dev.tty.obuf.tail = g_amb_dev.tty.obuf.buff;
		g_amb_dev.tty.obuf.size = osize;

		g_amb_dev.tty.cbuf.buff = malloc(csize);
		g_amb_dev.tty.cbuf.head = g_amb_dev.tty.cbuf.buff;
		g_amb_dev.tty.cbuf.tail = g_amb_dev.tty.cbuf.buff;
		g_amb_dev.tty.cbuf.size = csize;

		g_amb_dev.tty.highwater = isize - 32;
		g_amb_dev.tty.baud = g_amb_dev.baud;
		g_amb_dev.tty.c_cflag = CS8 | CREAD | CLOCAL;
		g_amb_dev.tty.c_iflag = ICRNL;
		g_amb_dev.tty.c_lflag = ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN;
		g_amb_dev.tty.c_oflag = OPOST | ONLCR;
		g_amb_dev.tty.verbose = g_amb_dev.verbose ? 1 : 0;
		int unit = 3;
		char *p = strstr(g_amb_dev.devname, "ser");
		if (p && *(p + 3) >= '0' && *(p + 3) <= '9') {
			unit = atoi(p + 3);
		}
		snprintf(g_amb_dev.tty.name, sizeof(g_amb_dev.tty.name), "/dev/ser");

		ttc(TTC_INIT_CC, &g_amb_dev.tty, 0);
		ttc(TTC_INIT_TTYNAME, &g_amb_dev.tty, NUMBER_DEV_FROM_USER | SET_NAME_NUMBER(unit));
		ttc(TTC_INIT_ATTACH, &g_amb_dev.tty, 0);
	}

	/* 5. Initialize hardware UART */
	uart_hw_init(&g_amb_dev);

	/* 6. Start background worker threads */
	g_amb_dev.running = true;
	if (g_amb_dev.use_interrupts) {
		if (pthread_create(&g_amb_dev.intr_tid, NULL, intr_thread, &g_amb_dev) != 0) {
			fprintf(stderr, "devc-seramb: Failed to create interrupt thread: %s\n", strerror(errno));
			return EXIT_FAILURE;
		}
	}
	if (pthread_create(&g_amb_dev.poll_tid, NULL, poll_thread, &g_amb_dev) != 0) {
		fprintf(stderr, "devc-seramb: Failed to create poll thread: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	printf("devc-seramb: Ambarella UART Passthrough registered at %s (MMIO 0x%llx, IRQ %d (%s), %u baud)\n",
	       g_amb_dev.devname, (unsigned long long)g_amb_dev.phys, g_amb_dev.irq,
	       g_amb_dev.use_interrupts ? "interrupt+poll" : "polling", g_amb_dev.baud);
	fflush(stdout);

	/* 7. Start io-char event loop */
	ttc(TTC_INIT_START, &ttyctrl, 0);

	g_amb_dev.running = false;
	if (g_amb_dev.use_interrupts && g_amb_dev.intr_id != -1) {
		InterruptDetach(g_amb_dev.intr_id);
		pthread_join(g_amb_dev.intr_tid, NULL);
	}
	pthread_join(g_amb_dev.poll_tid, NULL);
	munmap_device_memory((void *)g_amb_dev.base, UART_MAP_SIZE);

	return EXIT_SUCCESS;
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
