# Ambarella UART Virtualization and Physical Serial Console Guide

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

*Copyright (C) 2026, Ambarella International LLC*

---

## 1. Overview & Architecture

This guide describes the hardware-qualified UART virtualization architecture and operational runbook for Ambarella CV3-AD655 edge platforms (`n1-655-devkit` and `n1-655-pro`) running EVE-OS with multi-tenant HVM guest virtual machines: **Ubuntu 24.04 LTS** (`ubuntu_24_04`, `amba_shm0`, **UART2** on `console1`) and **Alpine Linux 3.20** (`alpine_3_20`, `amba_shm1`, **UART3** on `console2`). Under the pinned `0..5` board topology ([EVE-EdgeApp-Instances.md](EVE-EdgeApp-Instances.md)), **BlackBerry QNX Neutrino 8.0** (`qnx_8_0`, `amba_shm4`) is provisioned with `rng-mmio` and **no UART passthrough**.

The architecture enforces true zero-copy hardware virtualization:
1. **In-Tree VFIO & Automatic Host Binding (`vfio-platform`)**:
   - The EVE Dom0 kernel (`eve-kernel/arch/arm64/configs/eve_defconfig`) builds VFIO in-tree with `CONFIG_VFIO=y`, `CONFIG_VFIO_PLATFORM=y`, `CONFIG_VFIO_VIRQFD=y`, and `CONFIG_VFIO_IOMMU_TYPE1=y`. (`CONFIG_VFIO_NOIOMMU` is not enabled; platform devices without a hardware IOMMU group use the in-tree `VFIO_EMULATED_IOMMU` path via `vfio_register_emulated_iommu_dev` from commit `eb5e281e5e4d`).
   - EVE Pillar (`eve/pkg/pillar/hypervisor/kvm.go` and `hypervisor.go`) automatically unbinds the host `ambarella-uart` driver, sets `driver_override` to `vfio-platform`, and binds the physical UART device (`ffe0018000.uart` for `UART2`, `ffe0019000.uart` for `UART3`) before QEMU domain start. When the domain stops—or if domain creation fails (`BootFailed`)—Pillar clears `driver_override`, rebinds `ambarella-uart`, and releases the adapter reservation.
2. **True MMIO Passthrough with Split Host-Bound DMA**:
   - Direct guest Stage-2 register access via `vfio-platform` (`0xffe0018000` for UART2 / GIC SPI 115, `0xffe0019000` for UART3 / GIC SPI 116; 4 KiB aperture mapped at guest GPA `0x0c000000`) paired with in-kernel GICv2 level IRQ resampling (`x-irqfd` + `resamplefd`, guest vIRQ 50 / GSI 144).
   - Dual `ivshmem-plain` windows: a 1 GiB shared high-DRAM slice (`/dev/amba_virt_shm0..7`) and a 16 MiB low-DMA32 lease window (`/dev/amba_dma_lease0..3`) carved from `/reserved-memory/dma32@17c00000` (`0x17c00000`–`0x1bbfffff`, 64 MiB).
   - **Host-Bound DMA Endpoint**: At lease allocation (`AMBA_DMA_IOC_LEASE_ALLOC` on `/dev/amba_dma_ctl`), Pillar fixes `struct amba_dma_lease_alloc.endpoint_id` from the assigned UART adapter (`2` for `UART2`, `3` for `UART3`). Guest `amba_uart.ko` submits `endpoint_id = 0` ("my lease's bound endpoint"); `amba_virt_dma.ko` rejects any request naming another UART's endpoint with `-EPERM`.
3. **ACPI Dynamic Enumeration (`AMBA0001:00`)**:
   - QEMU dynamically emits `Device (URTx)` under `\_SB` in the DSDT AML with `_HID ("AMBA0001")`, `_UID`, `_CRS` (`Memory32Fixed` at `0x0c000000` + `Interrupt(144)`), and `_DSD` (`reg-io-width = 4`, `clock-frequency = 24000000`).
   - Both Ubuntu 24.04 (`6.8.0-101-generic`) and Alpine 3.20 (`6.6.142-0-virt`) match `AMBA0001` via `.acpi_match_table` with zero hardcoded physical addresses.

```text
ZEDEDA Cloud
  IO_TYPE_OTHER adapter bundles (UART1, UART2, UART3, UART4)
  empty phyaddrs + cbattr {"uart": "N"} + exclusive assigngrp
          |
          v
EVE Pillar (kvm.go / hypervisor.go) & QEMU Lifecycle
  Host Bind Path:   Unbinds ambarella-uart -> binds vfio-platform on start; restores on stop
  DMA Lease Alloc:  AMBA_DMA_IOC_LEASE_ALLOC binds lease endpoint_id (2=UART2, 3=UART3)
  vfio-platform:    True MMIO 4 KiB Stage-2 aperture + irqfd/resamplefd (SPI 115 / SPI 116)
  virt-acpi-build:  Dynamic DSDT AML generator emits Device (URTx) with AMBA0001 (GSI 144 / vIRQ 50)
  ivshmem-plain #1: 1 GiB shared high DRAM window (/dev/amba_virt_shm0..7, amba_shm0..7)
  ivshmem-plain #2: 16 MiB low DMA32 window (/dev/amba_dma_lease0..3, dma32@17c00000)
          |
          +-----------------------------------+-----------------------------------+
          |                                   |                                   |
          v                                   v                                   v
Ubuntu 24.04 HVM (amba_shm0)        Alpine 3.20 HVM (amba_shm1)         Dom0 Host Reference Monitor
  amba_uart.ko + amba_dma.ko          amba_uart.ko + amba_dma.ko          amba_virt_dma.ko
  ACPI match table (AMBA0001)         ACPI match table (AMBA0001)         exclusive Generic-DMA1 authority
  direct readl/writel on UART2        direct readl/writel on UART3        enforces host-bound endpoint_id
  Level vIRQ 50 (GSI 144)             Level vIRQ 50 (GSI 144)             leases 16 MiB slices from 0x17c00000
  systemd serial-getty@ttyAMBA0       OpenRC /etc/inittab getty ttyAMBA0          |
          |                                   |                                   |
          v                                   v                                   v
CH9344 Port 1 (console1)            CH9344 Port 2 (console2)            CH9344 Port 0 (console0)
ttyCH9344USB9 (Devkit, Port 6071)   ttyCH9344USB10 (Devkit, Port 6072)  ttyCH9344USB8 (Devkit, Port 6070)
ttyCH9344USB1 (Pro, Port 6061)      ttyCH9344USB2 (Pro, Port 6062)      ttyCH9344USB0 (Pro, Port 6060)
```

---

## 2. Authoritative Silicon & PCB Hardware Topology

Cross-referenced against the CV3-AD655 Hardware Programming Reference Manual, Datasheet, and Carrier PCB Schematics:

| Controller | Register Base | Physical IRQ | DMA Reqs | PCB Hardware Routing | Assigned Tenant / Endpoint |
|---|---|---|---|---|---|
| **UART0** (`uart0_apb`) | `0xffe4000000` | GIC SPI 192 | — | Carrier Sheet 8 $\rightarrow$ CH9344 Port 0 | **Dom0 / EVE / U-Boot Console** (`console0`: `6070` Devkit / `6060` Pro) |
| **UART1** (`uart1_ahb`) | `0xffe0017000` | GIC SPI 114 | TX 11, RX 12 | Carrier Sheet 17 $\rightarrow$ AzureWave AW-XM612-SUR Bluetooth (U31) | **Bluetooth 5.4 HCI Subsystem** (Dom0 `ambarella-uart`) |
| **UART2** (`uart2_ahb`) | `0xffe0018000` | GIC SPI 115 (guest vIRQ 50 / GSI 144) | TX 13, RX 14 | Carrier Sheet 8 $\rightarrow$ CH9344 Port 1 | **Ubuntu 24.04 HVM** (`ubuntu_24_04.<node>`, `amba_shm0`, `console1`: `6071` Devkit / `6061` Pro) |
| **UART3** (`uart3_ahb`) | `0xffe0019000` | GIC SPI 116 (guest vIRQ 50 / GSI 144) | TX 15, RX 16 | Carrier Sheet 8 $\rightarrow$ CH9344 Port 2 (SMIO 5..8 mux) | **Alpine 3.20 HVM** (`alpine_3_20.<node>`, `amba_shm1`, `console2`: `6072` Devkit / `6062` Pro) |
| **UART4** (`uart4_ahb`) | `0xffe001a000` | GIC SPI 117 | TX 17, RX 18 | Pin conflicts with Wi-Fi Power Enable & Ethernet 0 | *Reserved / Do Not Assign* |
| **Generic-DMA1** | `0xffe0021000` | GIC SPI 131 | Multi-channel | Monolithic bus master | **Dom0 Only** (Mediated via `amba_virt_dma.ko` & vsock broker) |

---

## 3. Host Low-DMA32 Reference Monitor Subsystem (`amba_virt_dma`)

### 3.1 Kernel Driver (`drivers/amba_virt/amba_virt_dma.c`)
- Loaded automatically at boot by `amba-virt-server` via `/etc/amba-virt/modules.conf` ([EVE-BaseOS-AmbarellaDrivers.md](EVE-BaseOS-AmbarellaDrivers.md)).
- Slices the 64 MiB DT carveout `/reserved-memory/dma32@17c00000` (`0x17c00000`–`0x1bbfffff`, DTS label `dma32_reserved`, `compatible = "ambarella,dma32-pool"`, `no-map`) into four isolated 16 MiB slices (`0x17c00000`, `0x18c00000`, `0x19c00000`, `0x1ac00000`).
- Exposes control device `/dev/amba_dma_ctl` and per-slice character devices `/dev/amba_dma_lease0`..`/dev/amba_dma_lease3`.
- **Host-Bound Endpoint Enforcement**: `struct amba_dma_lease_alloc` carries `endpoint_id` (`2` for `UART2`, `3` for `UART3`), set by Pillar from the adapter's UART number when allocating the lease. A guest DMA request must specify `endpoint_id == 0` (use the lease's bound endpoint) or equal the lease's `endpoint_id`; any other value is rejected with `-EPERM` before touching a DMA channel. A second lease requesting an already-bound UART endpoint is rejected at allocation time with `-EBUSY`.
- Restricts hardware peripheral DMA channel programming (`dma1chan0` `dreq=13` for UART2 TX, `dma1chan2` `dreq=15` for UART3 TX) to trusted kernel code, preventing tenant authority over physical bus masters.
- Enforces a 500 ms hardware watchdog timer and guarantees teardown zero-fill memory sanitization (`memset_io(0)`) upon lease release or tenant crash.

### 3.2 DMA Broker in `amba-virt-server` (`drivers/amba_virt/tools/virt_dma_broker.c`)
- Runs inside `amba-virt-server` in EVE Dom0 and holds no address, FIFO, or channel policy of its own.
- Authenticates the guest caller by `AF_VSOCK` peer CID and forwards guest DMA requests to the matching `/dev/amba_dma_lease<N>` with token-bucket rate limiting (2,000 ops/sec, 50 MiB/sec).

---

## 4. Linux Guest Driver (`amba_uart.ko` & `amba_dma.ko`)

- **Locations:** [`guest-os/linux/amba-uart/`](../guest-os/linux/amba-uart/) and [`guest-os/linux/amba-dma/`](../guest-os/linux/amba-dma/)
- **Supported Linux Guests:**
  - **Ubuntu 24.04 LTS** (`6.8.0-101-generic`, slice 0 / `amba_shm0`, `UART2` $\rightarrow$ `console1`): started via systemd `serial-getty@ttyAMBA0.service`.
  - **Alpine Linux 3.20** (`6.6.142-0-virt`, slice 1 / `amba_shm1`, `UART3` $\rightarrow$ `console2`): modules loaded via `/etc/modules` (`amba_virt`, `amba_dma`, `amba_uart`) and getty spawned via `/etc/inittab` (`ttyAMBA0::respawn:/sbin/getty -L 115200 ttyAMBA0 vt100`).
- **ACPI Matching:** Declares `amba_uart_acpi_match[]` for `AMBA0001` and hooks `.acpi_match_table` into `amba_uart_platform_driver`.
- **Resource Parsing:** Resolves MMIO via `platform_get_resource()` (`0x0c000000`) and IRQ via `platform_get_irq()` (guest vIRQ `50`, mapped to GIC `144 Level`). Reads properties via `device_property_read_u32()`:
  - `reg-io-width`: Enforces 4 (rejects non-4).
  - `clock-frequency`: Defaults to 24,000,000 Hz.
- **Host-Bound DMA Endpoint (`endpoint_id = 0`):** `amba_uart.c` does not hardcode a UART index; it submits DMA transfers with `endpoint_id = 0`, allowing the same driver binary to drive `UART2` on Ubuntu and `UART3` on Alpine as determined by the host lease.
- **Device Node:** Registers `/dev/ttyAMBA0`.
- **Sysfs Control Interfaces (`/sys/bus/platform/drivers/amba_uart/`):**
  - `use_dma` (`RW`, default `1`): `1` enables Generic-DMA1 hardware offloading; `0` forces CPU PIO.
  - `force_poll` (`RW`, default `0`): `1` forces 1 ms `hrtimer` polling mode; `0` enables hardware Level IRQ (vIRQ 50 / GSI 144).
  - `dma_rx` (`RW`): Writes buffer size to arm continuous RX DMA test stream.
  - `clk_hz` (`RO`): Reports UART peripheral input clock (24 MHz).
- **Diagnostics Node (`/proc/tty/driver/amba_uart`):**
  - Reports port state, `dma:active`, active IRQ (`irq:50`), TX/RX byte counts, and hardware error counters (`fe`, `oe`, `pe`, `brk`).

---

## 5. QNX Neutrino 8.0 Guest Subsystem (Reference Only — No UART Assigned)

Under the pinned `0..5` board configuration ([EVE-EdgeApp-Instances.md](EVE-EdgeApp-Instances.md) and [QNX-Guest-VM.md](QNX-Guest-VM.md)), `qnx_8_0.<node>` runs on slice `4` (`amba_shm4`) with `rng-mmio` and **no UART passthrough** (`UART2` belongs to Ubuntu and `UART3` belongs to Alpine).

The QNX serial driver (`guest-os/qnx/amba-uart/devc-seramb.c`) and getty supervisor (`guest-os/qnx/amba-uart/qnx-getty.c`) remain in the repository as reference components compatible with the updated `amba_virt.h` DMA lease UAPI.

---

## 6. Build & Deployment Automation

### 6.1 Building Guest OS Artifacts
```bash
# Build Ubuntu, Alpine, and QNX guest packages:
make guest-all

# Rebuild the bootable Alpine 3.20 QCOW2 image with amba_uart.ko, amba_dma.ko, and /etc/inittab getty:
./guest-os/build_guest.sh --distro=alpine --alpine-image
```
Output artifacts are staged under `build/guest/ubuntu/`, `build/guest/alpine/`, and `build/guest/qnx/`.

### 6.2 Deploying to Live Target Boards

Full from-zero instance provisioning is documented in [EVE-EdgeApp-Instances.md](EVE-EdgeApp-Instances.md). To update guest drivers over SSH on a running board:

```bash
# Deploy to Ubuntu HVM (UART2 / console1):
./guest-os/deploy_guest.sh n1-655-devkit-ubuntu --reload --enable-serial

# Deploy to Alpine HVM (UART3 / console2):
./guest-os/deploy_guest.sh n1-655-devkit-alpine --distro=alpine --reload
```

---

## 7. Operational Verification Runbook

### 7.1 Accessing the Consoles

| Target Domain | Hardware UART | Serial Port Endpoint | Telnet Bridge Port | Login Credentials |
|---|---|---|---|---|
| **EVE Dom0 (`console0`)** | `UART0` (`0xffe4000000`) | `ttyCH9344USB8` (Devkit) / `ttyCH9344USB0` (Pro) | `6070` (Devkit) / `6060` (Pro) | `root` (no password) |
| **Ubuntu 24.04 (`console1`)** | `UART2` (`0xffe0018000`) | `ttyCH9344USB9` (Devkit) / `ttyCH9344USB1` (Pro) | `6071` (Devkit) / `6061` (Pro) | `ubuntu` / `ubuntu` |
| **Alpine 3.20 (`console2`)** | `UART3` (`0xffe0019000`) | `ttyCH9344USB10` (Devkit) / `ttyCH9344USB2` (Pro) | `6072` (Devkit) / `6062` (Pro) | `alpine` / `alpine` |

### 7.2 Verifying In-Guest and Dom0 State

```bash
# On Ubuntu HVM (console1 / UART2) or Alpine HVM (console2 / UART3):
cat /sys/bus/platform/drivers/amba_uart/use_dma /sys/bus/platform/drivers/amba_uart/force_poll
# Expected: 1 and 0
cat /proc/tty/driver/amba_uart
# Expected: 0: uart:amba_uart dma:active mmio:0x0C000000 irq:50 tx:<count> rx:<count> RTS|CTS|DTR|DSR|CD
grep amba_uart /proc/interrupts
# Expected: 50: <count> ... GIC-0 144 Level amba_uart

# On Dom0: verify vfio-platform binding and DMA32 lease telemetry:
readlink /sys/bus/platform/devices/ffe0018000.uart/driver
readlink /sys/bus/platform/devices/ffe0019000.uart/driver
# Expected while Ubuntu and Alpine run: ../../../../bus/platform/drivers/vfio-platform
dmesg | grep amba_virt_dma
```

---

## 8. Troubleshooting

1. **Verifying Automatic `vfio-platform` Bind and Restore Lifecycle:**
   - Pillar (`kvm.go` / `hypervisor.go`) manages `driver_override` and `vfio-platform` binding automatically. Do not run manual sysfs bind scripts on a running node.
   - While `ubuntu_24_04.<node>` and `alpine_3_20.<node>` are running, `ffe0018000.uart` and `ffe0019000.uart` bind to `vfio-platform` (`ffe0017000.uart` stays on `ambarella-uart` for Bluetooth).
   - When an instance is stopped or deleted, Pillar clears `driver_override` and restores `ambarella-uart`.
2. **Checking `/dev/amba_dma_ctl` and `dma32@17c00000` Readiness:**
   - If a UART-assigned guest fails to start, verify on Dom0 that `/proc/device-tree/reserved-memory/dma32@17c00000` exists (`0x17c00000`–`0x1bbfffff` in `/proc/iomem`) and that `amba_virt_dma` is loaded (`ls -l /dev/amba_dma_ctl /dev/amba_dma_lease0..3`). If `/dev/amba_dma_ctl` is absent, Pillar refuses to start a UART domain rather than emitting an unbacked lease file.

---

## 9. Silicon Qualification Results (`n1-655-devkit` & `n1-655-pro`)

All measurements in this section are recorded directly from the hardware qualification artifacts in `plan/plan_kernel_vfio.artifacts/envelope-8-devkit/` and `plan/plan_kernel_vfio.artifacts/envelope-9-pro/` on image `0.0.0-ambarella-eve-21.19.0-k-arm64-v6.1.112-generic-5f934bbbe612-dirty-gcc-kvm-arm64` using the `aimon` physical serial console tools (`embdevenv_console_write`, `embdevenv_console_expect`, `embdevenv_console_read`).

### 9.1 Console Identity & No-Cross-Talk Swap Check

| Board | Console (`aimon` Target) | Assigned Guest & Ticket | Physical Controller & Lease | Verified Guest `uname` / TTY | Cross-Console Leakage |
|---|---|---|---|---|---|
| `n1-655-devkit` | `console1` (Port `6071`) | `ubuntu_24_04.n1-655-devkit` (`amba_shm0`) | `ffe0018000.uart` (`UART2`), `lease_id=0`, `endpoint=2`, CID `901` | `Linux ubuntu 6.8.0-101-generic aarch64`, `/dev/ttyAMBA0` | `0` lines on `console2` |
| `n1-655-devkit` | `console2` (Port `6072`) | `alpine_3_20.n1-655-devkit` (`amba_shm1`) | `ffe0019000.uart` (`UART3`), `lease_id=1`, `endpoint=3`, CID `902` | `Linux (none) 6.6.142-0-virt aarch64`, `/dev/ttyAMBA0` | `0` lines on `console1` |
| `n1-655-pro` | `console1` (Port `6061`) | `ubuntu_24_04.n1-655-pro` (`amba_shm0`) | `ffe0018000.uart` (`UART2`), `lease_id=0`, `endpoint=2`, CID `1301` | `Linux ubuntu 6.8.0-101-generic aarch64`, `/dev/ttyAMBA0` | `0` lines on `console2` |
| `n1-655-pro` | `console2` (Port `6062`) | `alpine_3_20.n1-655-pro` (`amba_shm1`) | `ffe0019000.uart` (`UART3`), `lease_id=1`, `endpoint=3`, CID `1308` | `Linux (none) 6.6.142-0-virt aarch64`, `/dev/ttyAMBA0` | `0` lines on `console1` |

### 9.2 Bidirectional 4 KiB Bulk Wire Verification, Level IRQ & Generic-DMA1 Telemetry

Every test ran with `use_dma=1` and `force_poll=0` (`dma:active`, `mmio:0x0C000000`, `irq:50` / `GIC-0 144 Level`), transferring 4,096 pseudo-random binary bytes in each direction over the physical serial wire and comparing SHA-256 digests between host and guest (`fe:0 oe:0 pe:0 brk:0`):

| Board | Console / Guest / UART | Direction | Raw Bytes (Wire Base64) | Verified SHA-256 Digest | Guest IRQ 50 (Pre $\rightarrow$ Post) | Guest `amba_uart` Counters (Pre $\rightarrow$ Post) | Host `amba_virt_dma` Lease & Channel Evidence |
|---|---|---|---|---|---|---|---|
| `n1-655-devkit` | `console1` / Ubuntu / `UART2` | Guest $\rightarrow$ Host | `4096` (`5464` B) | `063814d743f2655146e63935497f413f4ec4c17370e9bfed9d44dd5f019b3d45` | `501` $\rightarrow$ `1952` (`+1451`) | `tx:6898->12667` (`+5769`), `rx:462->6663` (`+6201`) | `lease 0` (`0x17d00000`), `dma1chan0 dreq=13`, `rx_requests_total: 3->5`, `rx_bytes_total: 5889->9503` |
| `n1-655-devkit` | `console1` / Ubuntu / `UART2` | Host $\rightarrow$ Guest | `4096` (`5464` B) | `5c83923449dd413e49b2b26a07b1511af8d28c0adf1a2255608dac24f8f81213` | *(included above)* | *(included above)* | *(host-to-guest RX over physical UART2 FIFO + Level IRQ 50)* |
| `n1-655-devkit` | `console2` / Alpine / `UART3` | Guest $\rightarrow$ Host | `4096` (`5464` B) | `fc8493c0a3a6c20bce430bb6cc10817e7017b922b64a0a6705211aae74c6813c` | `75` $\rightarrow$ `1573` (`+1498`) | `tx:738->7219` (`+6481`), `rx:63->6243` (`+6180`) | `lease 1` (`0x18d00000`), `dma1chan2 dreq=15`, `rx_requests_total: 0->2`, `rx_bytes_total: 0->5464` (`4095 + 1369`) |
| `n1-655-devkit` | `console2` / Alpine / `UART3` | Host $\rightarrow$ Guest | `4096` (`5464` B) | `60e402886d9d3552a49adff62c583acae9c5333182719dd081c9e74e75f99259` | *(included above)* | *(included above)* | *(host-to-guest RX over physical UART3 FIFO + Level IRQ 50)* |
| `n1-655-pro` | `console1` / Ubuntu / `UART2` | Guest $\rightarrow$ Host | `4096` (`5536` B) | `9930ce7d609062792e11d41bd3ac4125e2998b8094af1e909e1902df52b4c4f9` | `92` $\rightarrow$ `6687` (`+6595`) | `tx:985->21164` (`+20179`), `rx:63->5977` (`+5914`) | `lease 0` (`0x17d00000`), `dma1chan0 dreq=13`, `rx_requests_total: 1->3`, `rx_bytes_total: 425->4794` (`4095 + 274`) |
| `n1-655-pro` | `console1` / Ubuntu / `UART2` | Host $\rightarrow$ Guest | `4096` (`5536` B) | `532ef93b35c92b30c21bd960f5f7f2a699c8d317304768a7e317390928b50e19` | *(included above)* | *(included above)* | *(host-to-guest RX over physical UART2 FIFO + Level IRQ 50)* |
| `n1-655-pro` | `console2` / Alpine / `UART3` | Guest $\rightarrow$ Host | `4096` (`5536` B) | `d32df00bff84e892364dee4a1d9038e611c5494618de46d8b6b2c5b5ed0993bc` | `90` $\rightarrow$ `6708` (`+6618`) | `tx:738->12896` (`+12158`), `rx:62->5981` (`+5919`) | `lease 1` (`0x18d00000`), `dma1chan2 dreq=15`, `rx_requests_total: 0->2`, `rx_bytes_total: 0->4411` (`4095 + 316`) |
| `n1-655-pro` | `console2` / Alpine / `UART3` | Host $\rightarrow$ Guest | `4096` (`5536` B) | `8683ef7b12d96730f416f452b9eb808cc672f621f2eae4ddd255574d1fb04462` | *(included above)* | *(included above)* | *(host-to-guest RX over physical UART3 FIFO + Level IRQ 50)* |

### 9.3 Cross-Guest Endpoint Isolation (`-EPERM`) & Bind Lifecycle Qualification

1. **Cross-Guest DMA Endpoint Isolation (`11-uart-isolation-eperm.log`):**
   - On both `n1-655-devkit` and `n1-655-pro`, an in-guest request from Alpine (`endpoint=3` lease) asking for `requested_endpoint=2` (`UART2`) returned `status=-1` (`-EPERM`) with `transferred=0`, leaving `console1` (Ubuntu) undisturbed (`0` extraneous bytes) and host `rx_requests_total` / `rx_bytes_total` unchanged.
   - Conversely, an in-guest request from Ubuntu (`endpoint=2` lease) asking for `requested_endpoint=3` (`UART3`) returned `status=-1` (`-EPERM`) with `transferred=0`, leaving `console2` (Alpine) undisturbed.
2. **Host `vfio-platform` Bind/Unbind Lifecycle (`12-uart-bind-lifecycle.log`):**
   - On both `n1-655-devkit` and `n1-655-pro`, while `ubuntu_24_04.<node>` and `alpine_3_20.<node>` were active, sysfs reported `ffe0018000.uart` and `ffe0019000.uart` bound to `/sys/bus/platform/drivers/vfio-platform` (`driver_override='vfio-platform'`) and `ffe0017000.uart` bound to `/sys/bus/platform/drivers/ambarella-uart`.
   - Stopping `alpine_3_20.<node>` automatically cleared `driver_override` on `ffe0019000.uart`, restored `/sys/bus/platform/drivers/ambarella-uart`, drained and zero-sanitized `lease 1` (`2729 us` on `n1-655-pro`), and left `zedbox_d_state_threads=0`.
   - Restarting `alpine_3_20.<node>` rebound `ffe0019000.uart` to `vfio-platform`, allocated a fresh epoch on `lease 1`, and restored interactive login on `console2` (`/dev/ttyAMBA0`, `dma:active`, `irq:50`).
