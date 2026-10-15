# Ambarella Virtualized Peripheral DMA Architecture (`amba-dma`)

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

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

The guest protocol (`amba_dma.ko`), vsock DMA broker (`drivers/amba_virt/tools/virt_dma_broker.c`),
and Dom0 kernel reference monitor (`drivers/amba_virt/amba_virt_dma.c`) are implemented and
hardware-qualified on `n1-655-devkit` and `n1-655-pro` ([UART-Passthrough.md](UART-Passthrough.md)).

UART register access and UART interrupts are strictly decoupled from this DMA data
path. Under the True MMIO Passthrough architecture, the guest reaches UART registers
directly through a 4 KiB Stage-2 aperture assigned via `vfio-platform` and receives
level interrupts via in-kernel GICv2 `irqfd` resampling. Physical Generic-DMA1 registers
(`0xffe0021000`) never enter the guest.

For DMA data transfers, the architecture uses a **Dual-Window model**:
- **Bulk 1 GiB Window**: High-memory DRAM IVSHMEM window (`/dev/amba_virt_shm0..7`) for Cavalry/GDMA bulk buffers.
- **DMA32 16 MiB Window**: Low-memory 32-bit IVSHMEM window (`/dev/amba_dma_lease0..3`) backed by a dedicated 16 MiB
  slice of the 64 MiB `no-map` pool `/reserved-memory/dma32@17c00000` (`0x17c00000`–`0x1bbfffff`), required for Generic-DMA1 32-bit descriptors.
- **Split Authority & Host-Bound Endpoint (`endpoint_id`)**: At lease allocation (`AMBA_DMA_IOC_LEASE_ALLOC` on `/dev/amba_dma_ctl`),
  EVE Pillar fixes `struct amba_dma_lease_alloc.endpoint_id` from the assigned UART adapter (`2` for `UART2`, `3` for `UART3`).
  Untrusted guest frontends submit offset-only requests (`endpoint_id = 0` or the lease's bound endpoint) over vsock
  RPC to the DMA broker in `amba-virt-server`; the trusted Dom0 kernel authority (`amba_virt_dma.ko`) enforces
  the host-bound `endpoint_id` (rejecting any other endpoint with `-EPERM`), address bounds, 32-bit DMA masks,
  single-flight execution, and synchronous 500 ms timeouts.

---

## 2. Subsystem Architecture & Data Flow

```text
+===================================================================================================+
|                                    GUEST DOMAINS (Multi-Tenant Isolation)                         |
|                                                                                                   |
|   +-------------------------------------------------------+  +--------------------------------+   |
|   | Ubuntu 24.04 LTS HVM (amba_shm0, UART2, console1)     |  | Alpine Linux 3.20 HVM          |   |
|   |   - /dev/ttyAMBA0 (serial-getty@ttyAMBA0.service)     |  |   (amba_shm1, UART3, console2) |   |
|   |   - amba_uart.ko (UART2 BAR + vGIC IRQ 50 / GSI 144)  |  |   - /dev/ttyAMBA0 (/etc/inittab)|   |
|   |   - amba_dma.ko (endpoint_id = 0 -> lease endpoint 2) |  |   - amba_uart.ko + amba_dma.ko |   |
|   +---------------------------|---------------------------+  +----------------|---------------+   |
|                               | (DREQ 13 TX / 14 RX)                          | (DREQ 15/16)      |
+===============================|===============================================|===================+
                                |                                               |
              AF_VSOCK Control  | | ivshmem 16 MiB DMA32      AF_VSOCK Control  | | ivshmem 16 MiB DMA32
              (Port 5555)       | | (/dev/amba_dma_lease0)    (Port 5555)       | | (/dev/amba_dma_lease1)
                                v v                                             v v
+===================================================================================================+
|                                  HOST DOMAIN (EVE OS Dom0)                                        |
|                                                                                                   |
|   +-------------------------------------------------------------------------------------------+   |
|   | amba-virt-server (virt_dma_broker) + amba_virt_dma.ko (Kernel Reference Monitor)          |   |
|   |                                                                                           |   |
|   |   +-----------------------------------------------------------------------------------+   |   |
|   |   | Host-Bound Lease Endpoint Enforcement (AMBA_DMA_IOC_LEASE_ALLOC)                  |   |   |
|   |   |   - Lease 0 (Ubuntu CID): bound to endpoint_id=2 -> UART2 TX (13) & RX (14).      |   |   |
|   |   |   - Lease 1 (Alpine CID): bound to endpoint_id=3 -> UART3 TX (15) & RX (16).      |   |   |
|   |   |   - Cross-endpoint requests rejected with -EPERM before touching DMA channels.    |   |   |
|   |   +-----------------------------------------------------------------------------------+   |   |
|   |   | DMA32 16 MiB Slice Bounds Clamping (dma32@17c00000: 0x17c00000..0x1bbfffff)       |   |   |
|   |   |   - Asserts offset/length within the tenant's 16 MiB DMA32 slice                  |   |   |
|   |   |   - Rejects out-of-bounds descriptors and sanitizes slice on teardown             |   |   |
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
|   | UART2 (0xffe0018000, GIC SPI 115) |       | UART3 (0xffe0019000, GIC SPI 116) |               |
|   +-----------------|-----------------+       +-----------------|-----------------+               |
|                     |                                           |                                 |
|                     v (CH9344 Port 1)                           v (CH9344 Port 2)                 |
|   +-------------------------------------------------------------------------------------------+   |
|   | Onboard CH9344 USB-to-Quad-UART Bridge                                                    |   |
|   |   - Port 1: "console1" (Ubuntu 24.04 HVM shell, ttyCH9344USB9 / 6071, USB1 / 6061)        |   |
|   |   - Port 2: "console2" (Alpine 3.20 HVM shell, ttyCH9344USB10 / 6072, USB2 / 6062)        |   |
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
Guest Driver (amba_uart)     Guest Virt DMA (amba_dma)      Host Broker + amba_virt_dma.ko       Physical Hardware (dma1)
          │                            │                                  │                                  │
          │── dma_request_channel() ──>│                                  │                                  │
          │── dmaengine_submit(desc) ─>│                                  │                                  │
          │── dma_async_issue_pending()│                                  │                                  │
          │                            │── SUBMIT (endpoint_id=0, off) ──>│                                  │
          │                            │                                  │── Verify CID, epoch & endpoint   │
          │                            │                                  │── Verify 16 MiB DMA32 bounds     │
          │                            │                                  │── dmaengine_submit() ───────────>│
          │                            │                                  │                                  │── DMA Burst Execution
          │                            │                                  │<── Physical IRQ (SPI 131) ───────│
          │                            │<── DMA_RESP (status=0, xfer) ────│                                  │
          │<── desc->callback() ───────│                                  │                                  │
```

---

## 4. Security, Multi-Tenant Isolation & Anti-Tampering

| Security Vector | Threat / Attack Description | Mitigation & Enforcement Mechanism |
| :--- | :--- | :--- |
| **Channel Hijacking** | Rogue guest attempts to allocate or inject commands into another VM's UART DMA channel. | Pillar fixes `endpoint_id` at `AMBA_DMA_IOC_LEASE_ALLOC`; `amba_virt_dma.ko` rejects any request whose `endpoint_id` is neither `0` nor the lease's bound endpoint with `-EPERM`. |
| **Memory Boundary Escape** | Guest passes DMA buffer addresses pointing outside its assigned partition into host DRAM. | Strict clamping within the tenant's 16 MiB DMA32 slice (`dma32@17c00000`, above the 4 KiB bootstrap header). Out-of-bounds or misaligned requests are rejected. |
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
