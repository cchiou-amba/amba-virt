# Ambarella Virtualized Peripheral DMA Architecture (`amba-dma`)

*Document Path: `doc/AmbaVirtDMA.md`*  
*Copyright (C) 2026, Ambarella International LLC*

---

## 1. Overview & Problem Statement

On the Ambarella N1-655 SoC, peripheral devices (such as UART1..4, SPI0..3, I2S0..1, SD/eMMC, and NAND) utilize hardware DMA engines to offload high-throughput FIFO transfers to physical DRAM.

However, the hardware peripheral DMA controller (**`dma1` at MMIO `0xffe0021000`**) is **monolithic and shared** across all UART controllers (channels 11 to 18) with a single shared GIC interrupt (SPI 131). Passing the raw physical DMA controller MMIO directly to a single guest VM would:
1. Starve EVE Dom0 and all other guest VMs of DMA capabilities for their respective peripherals.
2. Create multi-tenant race conditions on channel configuration registers and interrupt handling.
3. Introduce critical security vulnerabilities where a rogue VM could snoop or corrupt other VMs' DMA transfers.

**`amba-dma`** resolves this challenge by virtualizing peripheral DMA channels across the **`amba-virt`** vsock RPC control plane and ivshmem zero-copy data plane, following the proven architectural design of **`amba-gdma`** (2D/Memcpy DMA) and **`amba-cavalry`** (NPU virtualization).

### Delivery Status

The guest protocol, channel ACL, ivshmem bounds checks, and watchdog framework
exist. The host broker does not yet submit or abort real physical peripheral
DMA transfers; those paths remain TODOs in
`drivers/amba_virt/tools/virt_dma_broker.c`. This document specifies the target
architecture and must not be read as hardware qualification.

UART register access and UART interrupts are also separate from this DMA data
path. The guest reaches UART registers through its planned ivshmem-backed MMIO
BAR and receives a virtual MSI-X interrupt through the vGIC. Physical UART SPI
114/115 never enters the guest.

---

## 2. Subsystem Architecture & Data Flow

```text
+===================================================================================================+
|                                    GUEST DOMAINS (Multi-Tenant Isolation)                         |
|                                                                                                   |
|   +-------------------------------------------------------+  +--------------------------------+   |
|   | Ubuntu 24.04 LTS HVM (runtime CID, Console 2)          |  | QNX Neutrino RTOS 8.0          |   |
|   |   - /dev/ttyAMBA0 (serial-getty@ttyAMBA0.service)     |  |   - /dev/ser2 (tinit / ksh)    |   |
|   |   - amba_uart.ko (UART BAR + vGIC IRQ)                |  |   - QNX UART BAR frontend      |   |
|   |   - amba_dma.ko (Virtual dmaengine provider)          |  |   - Optional DMA client        |   |
|   +---------------------------|---------------------------+  +----------------|---------------+   |
|                               | (Chan 11 TX / 12 RX)                          | (Chan 13/14)      |
+===============================|===============================================|===================+
                                |                                               |
              AF_VSOCK Control  | | ivshmem 1 GiB BAR         AF_VSOCK Control  | | ivshmem 1 GiB BAR
              (Port 5555)       | | (Zero-Copy Data Plane)    (Port 5555)       | | (Zero-Copy Data)
                                v v                                             v v
+===================================================================================================+
|                                  HOST DOMAIN (EVE OS Dom0 / NOHYPER Broker)                       |
|                                                                                                   |
|   +-------------------------------------------------------------------------------------------+   |
|   | amba-virt-server (Virt Daemon & Security ACL Gatekeeper)                                  |   |
|   |                                                                                           |   |
|   |   +-----------------------------------------------------------------------------------+   |   |
|   |   | CID Channel Authorization Matrix (ACL Verification)                               |   |   |
|   |   |   - Assigned Ubuntu CID: UART1 TX (11) & RX (12).                                 |   |   |
|   |   |   - Assigned QNX CID:    UART2 TX (13) & RX (14).                                 |   |   |
|   |   |   - Unauthorized channel access rejected with -EPERM (Security Event EVT-092)     |   |   |
|   |   +-----------------------------------------------------------------------------------+   |   |
|   |   | ivshmem 1 GiB Memory Bounds Clamping                                              |   |   |
|   |   |   - Asserts: 0 <= buf_offset + buf_len <= window_size                             |   |   |
|   |   |   - Rejects out-of-bounds descriptors with -ERANGE (Security Log EVT-091)         |   |   |
|   |   +-----------------------------------------------------------------------------------+   |   |
|   |   | 500ms Hardware Watchdog Timer & Channel Recovery Engine                           |   |   |
|   |   |   - Aborts stalled peripheral DREQ transfers without hanging host CPU             |   |   |
|   |   +-----------------------------------------------------------------------------------+   |   |
|   +---------------------------------------------|---------------------------------------------+   |
|                                                 | (Standard Host Kernel dmaengine API)            |
|                                                 v                                                 |
|   +-------------------------------------------------------------------------------------------+   |
|   | Native Ambarella DMA Controller Driver (eve-kernel/drivers/dma/ambarella_dma.c)           |   |
|   |   - Manages physical hardware dma1 controller registers (0xffe0021000)                    |   |
|   |   - Handles physical GIC SPI 131 interrupt & channel status registers                     |   |
|   +---------------------------------------------|---------------------------------------------+   |
|                                                 |                                                 |
+=================================================|-|===============================================+
                                                  | |
                            AHB/APB Bus Mastering | | Physical Peripheral DREQ Handshake
                                                  v v
+===================================================================================================+
|                              PHYSICAL HARDWARE (Ambarella N1-655 SoC & Baseboard)                 |
|                                                                                                   |
|   +-------------------------------------------------------------------------------------------+   |
|   | Monolithic Peripheral DMA Controller (dma1 at 0xffe0021000, GIC SPI 131)                 |   |
|   |   - Chan 11 (UART1 TX)  - Chan 13 (UART2 TX)  - Chan 15 (UART3 TX)  - Chan 17 (UART4 TX)   |   |
|   |   - Chan 12 (UART1 RX)  - Chan 14 (UART2 RX)  - Chan 16 (UART3 RX)  - Chan 18 (UART4 RX)   |   |
|   +-----------------------|-------------------------------------------|-----------------------+   |
|                           |                                           |                           |
|                           v                                           v                           |
|   +-----------------------------------+       +-----------------------------------+               |
|   | UART1 (0xffe0017000, GIC SPI 114) |       | UART2 (0xffe0018000, GIC SPI 115) |               |
|   +-----------------|-----------------+       +-----------------|-----------------+               |
|                     |                                           |                                 |
|                     v (RS-232 Port 1)                           v (RS-232 Port 2)                 |
|   +-------------------------------------------------------------------------------------------+   |
|   | Onboard CH9344 USB-to-Quad-UART Bridge                                                    |   |
|   |   - Channel 1: "Console 2" (Ubuntu 24.04 HVM shell, ttyCH9344USB1/9)                      |   |
|   |   - Channel 2: "Console 3" (QNX Neutrino RTOS 8.0 shell, ttyCH9344USB2/10)                |   |
|   +-------------------------------------------------------------------------------------------+   |
+===================================================================================================+
```

---

## 3. Protocol Specification & Message Exchange

### 3.1 Message Types
Defined in
[`drivers/amba_virt/include/uapi/amba_virt.h`](../drivers/amba_virt/include/uapi/amba_virt.h):
* `AMBA_VIRT_MSG_DMA_REQ` (`50`): Guest-to-Host DMA operation request.
* `AMBA_VIRT_MSG_DMA_RESP` (`51`): Host-to-Guest synchronous operation response.
* `AMBA_VIRT_MSG_DMA_COMPLETE` (`52`): Host-to-Guest asynchronous transfer completion event.

### 3.2 Protocol Sequence Diagram

```text
Guest Driver (amba_uart)     Guest Virt DMA (amba_dma)      Host Server (amba-virt-server)       Physical Hardware (dma1)
          │                            │                                  │                                  │
          │── dma_request_channel() ──>│                                  │                                  │
          │                            │── OP_REQUEST_CHAN (chan 11) ────>│                                  │
          │                            │                                  │── Verify CID ACL (Pass)          │
          │                            │<── RESP (status=0, handle=1) ────│                                  │
          │<── struct dma_chan ────────│                                  │                                  │
          │                            │                                  │                                  │
          │── dmaengine_submit(desc) ─>│                                  │                                  │
          │── dma_async_issue_pending()│                                  │                                  │
          │                            │── OP_SUBMIT_BURST (buf, len) ───>│                                  │
          │                            │                                  │── Verify ivshmem bounds (Pass)   │
          │                            │                                  │── dmaengine_submit() ───────────>│
          │                            │                                  │                                  │── DMA Burst Execution
          │                            │                                  │<── Physical IRQ (SPI 131) ───────│
          │                            │<── MSG_DMA_COMPLETE (residue=0) ─│                                  │
          │<── desc->callback() ───────│                                  │                                  │
```

---

## 4. Security, Multi-Tenant Isolation & Anti-Tampering

| Security Vector | Threat / Attack Description | Mitigation & Enforcement Mechanism |
| :--- | :--- | :--- |
| **Channel Hijacking** | Rogue guest attempts to allocate or inject commands into another VM's UART DMA channel. | `amba-virt-server` checks the kernel-authenticated runtime CID against current adapter ownership; unauthorized requests are rejected with `-EPERM` (`EVT-092`). |
| **Memory Boundary Escape** | Guest passes DMA buffer addresses pointing outside its assigned partition into host DRAM. | Strict clamping: `0 <= offset + len <= window_size` (1 GiB ivshmem extent). Out-of-bounds requests rejected with `-ERANGE` (`EVT-091`). |
| **RPC DoS Flooding** | Compromised guest floods vsock with rapid requests to exhaust host CPU. | Per-CID token bucket rate limiting drops excessive requests without impacting legitimate owner VM. |
| **Hardware DREQ Stall** | Faulty peripheral fails to assert hardware DREQ lines, stalling the DMA controller. | 500 ms watchdog timer aborts stalled transfer, resets channel, and returns `-ETIMEDOUT`. |
| **Malformed Payloads** | Fuzzing packets with invalid lengths, corrupted opcodes, or null pointers. | Strict parameter sanitization and bounds checking discard malformed frames cleanly with zero daemon crashes. |

---

## 5. Linux Kernel `dmaengine` Integration in Guest

`amba_dma.ko` implements standard Linux DMA provider operations:
- `device_alloc_chan_resources`: Maps virtual channel to host broker.
- `device_free_chan_resources`: Releases virtual channel.
- `device_config`: Configures slave FIFO address, burst sizes, and bus widths.
- `device_prep_slave_single`: Formats zero-copy ivshmem transfer descriptors.
- `device_issue_pending`: Dispatches batch descriptors to `amba-virt-server`.
- `device_terminate_all`: Aborts in-flight transfers during error recovery or driver unload.
