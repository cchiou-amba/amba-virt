# Ambarella UART Virtualization and Physical Console Guide

*Copyright (C) 2026, Ambarella International LLC*

## Status

This document describes the target UART virtualization architecture. It is not
yet a hardware-qualified runbook.

Delivered today:

- EVE emits `ivshmem-plain` for the `amba_shm` `IO_TYPE_OTHER` marker.
- The HVM and NOHYPER applications communicate over vsock and a shared 1 GiB
  ivshmem DRAM window.
- Linux `amba_uart.ko` implements the Ambarella UART programming model.
- Current cloud models still contain `COM2`/`COM3` entries with
  `phyaddrs.Serial=/dev/ttyS*`; Pillar turns those into QEMU `pci-serial`.

Not delivered today:

- UART register MMIO backed by an ivshmem PCI BAR.
- `ivshmem-doorbell` eventfd notification into the guest vGIC.
- The host UART IRQ stub and level-IRQ ACK protocol.
- A QNX UART frontend for the BAR/doorbell interface.
- End-to-end physical Console 2 or Console 3 qualification.

## Architecture

The guest owns the UART programming model, but Dom0 mediates infrastructure
which cannot safely be assigned directly:

```text
ZEDEDA
  IO_TYPE_OTHER UART adapter bundle
  empty phyaddrs + cbattr + assigngrp
                    |
                    v
EVE Pillar -> QEMU ivshmem BAR + ivshmem-doorbell
                    |
          +---------+----------+
          |                    |
          v                    v
HVM guest UART driver     Dom0 UART stub
real register access      physical IRQ 114/115
MSI-X through vGIC        mask/eventfd/unmask
          |                    ^
          +---- doorbell ACK --+

Guest virtual dmaengine
          |
          v
vsock descriptors + shared DRAM offsets
          |
          v
amba-virt-server -> host dmaengine -> physical Generic-DMA
```

The UART is not a bus master and does not need SMMU translation. The physical
Generic-DMA controller is a bus master and remains entirely in Dom0.

This design does not use VFIO, `vfio-platform`, direct physical SPI assignment,
or QEMU `pci-serial`. The planned model uses `IO_TYPE_OTHER` to reuse the
delivered ivshmem predicate; `IO_TYPE_COM` is not inherently prohibited, but
the current `IO_TYPE_COM` entries have populated `Serial` fields and therefore
select the wrong QEMU device.

## Hardware Resources

Values below come from the current
[`n1_655.dts`](../eve-kernel/arch/arm64/boot/dts/ambarella/n1_655.dts):

| Function | Physical address | Physical IRQ | DMA requests | Ownership |
|---|---:|---:|---:|---|
| UART0 | `0xffe4000000` | SPI 192 | — | EVE/U-Boot console |
| UART1 | `0xffe0017000` | SPI 114, level-high | TX 11, RX 12 | Ubuntu HVM target |
| UART2 | `0xffe0018000` | SPI 115, level-high | TX 13, RX 14 | QNX HVM target |
| Generic-DMA1 | `0xffe0021000` | SPI 131 | Shared controller | Dom0 only |

UART1 is currently enabled in the DTS. UART2 is currently disabled and must not
be described as assignable until its clock, pinmux, and host-stub ownership are
implemented.

Physical routing:

| Console | Devkit endpoint | Pro endpoint | Target domain |
|---|---|---|---|
| Dom0 console | `ttyCH9344USB8` | `ttyCH9344USB0` | EVE |
| Console 2 | `ttyCH9344USB9` | `ttyCH9344USB1` | Ubuntu HVM |
| Console 3 | `ttyCH9344USB10` | `ttyCH9344USB2` | QNX HVM |
| MCU | `ttyCH9344USB11` | `ttyCH9344USB3` | Board controller |

## Cloud Adapter Contract

No ZEDEDA API extension is required. UART assignment uses an
`IO_TYPE_OTHER` adapter bundle with:

- empty `PciLong`, `Ifname`, `Serial`, and `UsbAddr`;
- an exclusive `assigngrp`;
- hardware-specific parameters in `cbattr`.

`IO_TYPE_OTHER` is chosen because it reuses the delivered ivshmem recognition
shape. It does not mean “UART.” The UART parser and bulk-window parser must be
mutually exclusive on `cbattr` keys.

The `Serial` field must remain empty. Pillar emits `pci-serial` whenever that
field is non-empty, regardless of adapter type.

The exact UART `cbattr` schema is not delivered yet and must be frozen during
implementation. Do not publish model JSON containing guessed keys.

## MMIO Path

The host UART stub:

1. Claims one UART platform device instead of `ambarella-uart`.
2. Keeps its clock and pinmux active.
3. Exposes exactly the selected 4 KiB register aperture through a restricted
   character-device mmap.
4. Does not read IIR/RBR or otherwise consume guest-visible UART state.

EVE supplies that mapping to QEMU as the backing for the UART ivshmem BAR. The
guest maps the BAR and programs the physical UART registers directly.

Before implementation proceeds, the delivered arm64 KVM must demonstrate that
it accepts this device-PFN-backed memory slot. A successful shared-DRAM
ivshmem test does not prove device-MMIO mapping.

## Interrupt Path

The guest does not receive physical SPI 114 or 115. It receives a virtual
MSI-X interrupt through its vGIC:

1. The physical level-high IRQ enters the Dom0 stub.
2. The stub masks it without reading UART registers.
3. The stub signals the eventfd associated with the owning HVM.
4. `ivshmem-doorbell` raises MSI-X; KVM injects it through the vGIC.
5. The guest ISR services the physical UART registers through the BAR.
6. The guest sends a doorbell ACK.
7. The host verifies ownership and unmasks the physical IRQ.

Guest death, timeout, or stale ACK must leave the physical IRQ masked until
ownership is safely restored.

The doorbell server/socket must exist before QEMU starts. EVE must supervise
that lifecycle; a missing helper or eventfd must fail domain creation rather
than falling back to `pci-serial`.

## DMA Path

FIFO/PIO mode is qualified first.

DMA mode uses [`AmbaVirtDMA.md`](AmbaVirtDMA.md):

- guest `amba_dma.ko` implements the `dmaengine` API;
- requests contain only offsets into the guest's existing shared DRAM BAR;
- `amba-virt-server` validates adapter ownership, channel, direction, length,
  overflow, and window bounds;
- the host submits through its native Ambarella dmaengine driver;
- completion returns to the guest DMA callback;
- a watchdog aborts stalled channels.

Never map Generic-DMA controller MMIO into a guest.

## Guest Drivers

Linux `amba_uart.ko` must be changed from its current direct DT/ACPI fallback:

- discover the assigned UART PCI function;
- map the UART BAR;
- request its MSI-X/vGIC interrupt;
- ACK the host only after servicing the UART condition;
- use FIFO independently of `amba_dma`;
- optionally bind TX/RX to `amba_dma` after DMA qualification.

QNX requires the equivalent PCI BAR discovery, interrupt attachment, UART
register driver integration, ACK, and teardown behavior. Existing README-only
claims are not implementation evidence.

## Verification

Software checks are diagnostics only:

- QEMU contains the intended ivshmem devices.
- The guest enumerates the BAR and MSI-X vector.
- Host eventfd and guest IRQ counters increase.
- The UART driver and login service are active.

Qualification requires physical I/O:

1. Observe the guest login prompt on Console 2 or Console 3.
2. Inject a unique command through that physical endpoint.
3. Match the exact command output and returned prompt.
4. Exercise bidirectional deterministic patterns.
5. Repeat under FIFO saturation, guest restart, server restart, and adapter
   reassignment.
6. Repeat with DMA enabled and verify bit-exact data.

Report MMIO mapping, vGIC delivery, FIFO, external TX, external RX, and DMA as
separate results. Do not infer one from another.

## Related Documents

- [`Architecture.md`](Architecture.md)
- [`AmbaVirtDMA.md`](AmbaVirtDMA.md)
- [`AmbaVirtServer.md`](AmbaVirtServer.md)
- [`EVE-Native-ivshmem-Support.md`](EVE-Native-ivshmem-Support.md)
- [`EVE-Ambarella-Models.md`](EVE-Ambarella-Models.md)
- [`EVE-EdgeApp-Provision.md`](EVE-EdgeApp-Provision.md)
