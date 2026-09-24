/*
 * amba_uart.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef __AMBA_UART_H__
#define __AMBA_UART_H__

/* ========================================================================== */
/* Ambarella DesignWare UART Register Offsets (32-bit MMIO)                   */
/* ========================================================================== */

#define UART_RB_OFFSET			0x00	/* Receive Buffer Register (R)       */
#define UART_TH_OFFSET			0x00	/* Transmit Holding Register (W)     */
#define UART_DLL_OFFSET			0x00	/* Divisor Latch Low (when DLAB=1)   */
#define UART_IE_OFFSET			0x04	/* Interrupt Enable Register         */
#define UART_DLH_OFFSET			0x04	/* Divisor Latch High (when DLAB=1)  */
#define UART_II_OFFSET			0x08	/* Interrupt Identification Reg (R)  */
#define UART_FC_OFFSET			0x08	/* FIFO Control Register (W)         */
#define UART_LC_OFFSET			0x0c	/* Line Control Register             */
#define UART_MC_OFFSET			0x10	/* Modem Control Register            */
#define UART_LS_OFFSET			0x14	/* Line Status Register              */
#define UART_MS_OFFSET			0x18	/* Modem Status Register             */
#define UART_SC_OFFSET			0x1c	/* Scratch Register                  */
#define UART_DMAE_OFFSET		0x28	/* DMA Enable Register               */
#define UART_DMAF_OFFSET		0x40	/* DMA FIFO Register                 */
#define UART_US_OFFSET			0x7c	/* UART Status Register              */
#define UART_TFL_OFFSET			0x80	/* Transmit FIFO Level               */
#define UART_RFL_OFFSET			0x84	/* Receive FIFO Level                */
#define UART_SRR_OFFSET			0x88	/* Software Reset Register           */
#define UART_RTR_OFFSET			0xac	/* RX Trigger Level                  */
#define UART_TTR_OFFSET			0xb0	/* TX Trigger Level                  */

/* ========================================================================== */
/* Interrupt Enable Register (UART_IE) Bits                                   */
/* ========================================================================== */
#define UART_IE_PTIME			0x80	/* Programmable THRE Interrupt Mode  */
#define UART_IE_ERETOI			0x40	/* Enable Early Receiver Timeout IRQ */
#define UART_IE_ETOI			0x20	/* Enable Receiver Timeout Interrupt */
#define UART_IE_EBDI			0x10	/* Enable Busy Detect Interrupt      */
#define UART_IE_EDSSI			0x08	/* Enable Modem Status Interrupt     */
#define UART_IE_ELSI			0x04	/* Enable Receiver Line Status IRQ   */
#define UART_IE_ETBEI			0x02	/* Enable Transmit Holding Reg Empty */
#define UART_IE_ERBFI			0x01	/* Enable Received Data Available    */

/* ========================================================================== */
/* Interrupt Identification Register (UART_II) Bits & IDs                     */
/* ========================================================================== */
#define UART_II_MODEM_STATUS_CHANGED	0x00
#define UART_II_NO_INT_PENDING		0x01
#define UART_II_THR_EMPTY		0x02
#define UART_II_RCV_DATA_AVAIL		0x04
#define UART_II_RCV_STATUS		0x06
#define UART_II_CHAR_TIMEOUT		0x0c
#define UART_II_CHAR_TIMEOUT_FIFO_EMPTY	0x0d

/* ========================================================================== */
/* FIFO Control Register (UART_FC) Bits                                       */
/* ========================================================================== */
#define UART_FC_RX_ONECHAR		0x00
#define UART_FC_RX_QUARTER_FULL		0x40
#define UART_FC_RX_HALF_FULL		0x80
#define UART_FC_RX_2_TO_FULL		0xc0
#define UART_FC_TX_EMPTY		0x00
#define UART_FC_TX_2_IN_FIFO		0x10
#define UART_FC_TX_QUATER_IN_FIFO	0x20
#define UART_FC_TX_HALF_IN_FIFO		0x30
#define UART_FC_DMA_SELECT		0x08
#define UART_FC_XMITR			0x04	/* Clear TX FIFO                     */
#define UART_FC_RCVRR			0x02	/* Clear RX FIFO                     */
#define UART_FC_FIFOE			0x01	/* FIFO Enable                       */

/* ========================================================================== */
/* Line Control Register (UART_LC) Bits                                       */
/* ========================================================================== */
#define UART_LC_DLAB			0x80	/* Divisor Latch Access Bit          */
#define UART_LC_BRK			0x40	/* Break Control                     */
#define UART_LC_EVEN_PARITY		0x10	/* Even Parity Select                */
#define UART_LC_ODD_PARITY		0x00	/* Odd Parity Select                 */
#define UART_LC_PEN			0x08	/* Parity Enable                     */
#define UART_LC_STOP_2BIT		0x04	/* 2 Stop Bits (1.5 for 5-bit)       */
#define UART_LC_STOP_1BIT		0x00	/* 1 Stop Bit                        */
#define UART_LC_CLS_8_BITS		0x03	/* 8 Data Bits                       */
#define UART_LC_CLS_7_BITS		0x02	/* 7 Data Bits                       */
#define UART_LC_CLS_6_BITS		0x01	/* 6 Data Bits                       */
#define UART_LC_CLS_5_BITS		0x00	/* 5 Data Bits                       */

/* ========================================================================== */
/* Modem Control Register (UART_MC) Bits                                      */
/* ========================================================================== */
#define UART_MC_SIRE			0x40	/* SIR Mode Enable                   */
#define UART_MC_AFCE			0x20	/* Auto Flow Control Enable          */
#define UART_MC_LB			0x10	/* Loopback Mode                     */
#define UART_MC_OUT2			0x08	/* User Output 2                     */
#define UART_MC_OUT1			0x04	/* User Output 1                     */
#define UART_MC_RTS			0x02	/* Request To Send                   */
#define UART_MC_DTR			0x01	/* Data Terminal Ready               */

/* ========================================================================== */
/* Line Status Register (UART_LS) Bits                                        */
/* ========================================================================== */
#define UART_LS_FERR			0x80	/* FIFO Error Status                 */
#define UART_LS_TEMT			0x40	/* Transmitter Empty                 */
#define UART_LS_THRE			0x20	/* Transmit Holding Register Empty   */
#define UART_LS_BI			0x10	/* Break Interrupt                   */
#define UART_LS_FE			0x08	/* Framing Error                     */
#define UART_LS_PE			0x04	/* Parity Error                      */
#define UART_LS_OE			0x02	/* Overrun Error                     */
#define UART_LS_DR			0x01	/* Data Ready                        */

/* ========================================================================== */
/* Modem Status Register (UART_MS) Bits                                       */
/* ========================================================================== */
#define UART_MS_DCD			0x80	/* Data Carrier Detect               */
#define UART_MS_RI			0x40	/* Ring Indicator                    */
#define UART_MS_DSR			0x20	/* Data Set Ready                    */
#define UART_MS_CTS			0x10	/* Clear To Send                     */
#define UART_MS_DDCD			0x08	/* Delta DCD                         */
#define UART_MS_TERI			0x04	/* Trailing Edge RI                  */
#define UART_MS_DDSR			0x02	/* Delta DSR                         */
#define UART_MS_DCTS			0x01	/* Delta CTS                         */

/* ========================================================================== */
/* UART Status Register (UART_US) Bits                                        */
/* ========================================================================== */
#define UART_US_RFF			0x10	/* Receive FIFO Full                 */
#define UART_US_RFNE			0x08	/* Receive FIFO Not Empty            */
#define UART_US_TFE			0x04	/* Transmit FIFO Empty               */
#define UART_US_TFNF			0x02	/* Transmit FIFO Not Full            */
#define UART_US_BUSY			0x01	/* UART Busy                         */

/* ========================================================================== */
/* Driver Constants                                                           */
/* ========================================================================== */
#define AMBA_UART_FIFO_SIZE		64
#define AMBA_UART_DEFAULT_CLK		24000000	/* 24 MHz */
#define AMBA_UART_MAX_PORTS		4
#define AMBA_UART_DEFAULT_IER		0

#endif /* __AMBA_UART_H__ */
