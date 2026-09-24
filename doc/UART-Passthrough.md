# Ambarella UART Virtualization and Physical Serial Console Guide

*Copyright (C) 2026, Ambarella International LLC*

---

## 1. Overview & Architecture

This guide describes the hardware-qualified UART virtualization architecture and operational runbook for Ambarella CV3-AD655 edge platforms running EVE OS with multi-tenant guest virtual machines (Ubuntu 24.04 LTS and BlackBerry QNX Neutrino 8.0).

The architecture allows guest operating systems to directly own and program physical Ambarella DesignWare UART controllers via an `ivshmem-doorbell` PCI BAR aperture, backed by Dom0 MMIO lease drivers:

```text
ZEDEDA Cloud
  IO_TYPE_OTHER adapter bundles (UART1, UART2, UART3, UART4)
  empty phyaddrs + cbattr {"uart": "N"} + exclusive assigngrp
          |
          v
EVE Pillar / QEMU Lifecycle
  ivshmem-plain:    1 GiB shared DRAM window (amba_shm / amba_shm1)
  ivshmem-doorbell: BAR 0 = Doorbell ACK, BAR 1 = MSI-X Table, BAR 2 = 4 KiB UART MMIO
          |
          +-----------------------------------+-----------------------------------+
          |                                   |                                   |
          v                                   v                                   v
Ubuntu 24.04 HVM                    QNX Neutrino 8.0 HVM                Dom0 Host Driver / Bridge
  amba_uart.ko                        devc-seramb (io-char)               amba_virt_uart.ko
  BAR 2 @ 0x804060c000                qnx-getty daemon                    amba_virt_uart_server
  1 ms hrtimer FIFO poll              BAR 2 @ 0x804020c000                claims MMIO 0xffe0018000/0xffe0019000
  /dev/ttyAMBA0                       /dev/ser3 (OPOST | ONLCR)           masks physical SPI 115/116
          |                                   |                                   |
          v                                   v                                   v
CH9344 Port 1 (Console 2)           CH9344 Port 2 (Console 3)           CH9344 Port 0 (Dom0 Console)
ttyCH9344USB9 (Devkit)              ttyCH9344USB10 (Devkit)             ttyCH9344USB8 (Devkit)
ttyCH9344USB1 (Pro)                 ttyCH9344USB2 (Pro)                 ttyCH9344USB0 (Pro)
Telnet Port 6071                    Telnet Port 6072                    Telnet Port 6070
```

---

## 2. Authoritative Silicon & PCB Hardware Topology

Cross-referenced against the CV3-AD655 Hardware Programming Reference Manual, Datasheet, and Carrier PCB Schematics:

| Controller | Register Base | Physical IRQ | DMA Reqs | PCB Hardware Routing | Assigned Tenant / Endpoint |
|---|---|---|---|---|---|
| **UART0** (`uart0_apb`) | `0xffe4000000` | GIC SPI 192 | — | Carrier Sheet 8 $\rightarrow$ CH9344 Port 0 | **Dom0 / EVE / U-Boot Console** (`ttyCH9344USB8` / Port 6070) |
| **UART1** (`uart1_ahb`) | `0xffe0017000` | GIC SPI 114 | TX 11, RX 12 | Carrier Sheet 17 $\rightarrow$ AzureWave AW-XM612-SUR Bluetooth (U31) | **Bluetooth 5.4 HCI Subsystem** (Internal) |
| **UART2** (`uart2_ahb`) | `0xffe0018000` | GIC SPI 115 | TX 13, RX 14 | Carrier Sheet 8 $\rightarrow$ CH9344 Port 1 | **Ubuntu 24.04 HVM** (Console 2 / `ttyCH9344USB9` / Port 6071) |
| **UART3** (`uart3_ahb`) | `0xffe0019000` | GIC SPI 116 | TX 15, RX 16 | Carrier Sheet 8 $\rightarrow$ CH9344 Port 2 (SMIO 5..8 mux) | **QNX 8.0 HVM** (Console 3 / `ttyCH9344USB10` / Port 6072) |
| **UART4** (`uart4_ahb`) | `0xffe001a000` | GIC SPI 117 | TX 17, RX 18 | Pin conflicts with Wi-Fi Power Enable & Ethernet 0 | *Reserved / Do Not Assign* |
| **Generic-DMA1** | `0xffe0021000` | GIC SPI 131 | Multi-channel | Monolithic bus master | **Dom0 Only** (Mediated via `amba-virt-server` vsock) |

---

## 3. Host Subsystem (`amba_virt_uart`)

### 3.1 Kernel Driver (`drivers/amba_virt/amba_virt_uart.c`)
- Claims physical memory regions for UART2 (`0xffe0018000`) and UART3 (`0xffe0019000`).
- Exposes restricted mmap-capable character devices `/dev/amba_virt_uart2` and `/dev/amba_virt_uart3`.
- Retains physical GIC SPI 115/116 interrupts in Dom0 and disables them to prevent interrupt storms while FIFO polling mode is active.
- Provides ioctl interfaces (`AMBA_VIRT_UART_IOC_SET_EVENTFD`, `AMBA_VIRT_UART_IOC_ACK_IRQ`, `AMBA_VIRT_UART_IOC_RESET`).

### 3.2 Protocol Server Daemon (`drivers/amba_virt/tools/amba_virt_uart_server.c`)
- Listens on UNIX domain sockets `/run/amba_virt_uart2.sock` and `/run/amba_virt_uart3.sock`.
- Implements the ivshmem protocol version 0: transmits the UART MMIO character device file descriptor as BAR 2 and bound eventfds via `SCM_RIGHTS` to QEMU.
- Handles reverse doorbell ACK events via `epoll()`.

---

## 4. Linux Guest Driver (`amba_uart.ko`)

- **Location:** [`guest-os/linux/amba-uart/`](../guest-os/linux/amba-uart/)
- **PCI Attachment:** Matches vendor `0x1af4` and device `0x1110`. Maps 64-bit BAR 2 (GPA `0x804060c000`) to access physical DesignWare UART registers.
- **FIFO Polling Engine:** Operates with `IER = 0` (interrupts disabled) using a 1 ms high-resolution timer (`hrtimer`).
  - **RX:** Polls `UART_LS_DR` and `UART_RFL`, pushing characters into the TTY subsystem flip buffer.
  - **TX:** Drains characters from the circular transmit buffer into `UART_TH_OFFSET` while `UART_US_TFNF` is asserted.
- **Device Node:** Registers `/dev/ttyAMBA0`.
- **System Service:** `serial-getty@ttyAMBA0.service` provides an interactive login prompt.

---

## 5. QNX Neutrino 8.0 Guest Subsystem

### 5.1 Serial Driver (`devc-seramb`)
- **Location:** [`guest-os/qnx/amba-uart/devc-seramb.c`](../guest-os/qnx/amba-uart/devc-seramb.c)
- **Framework:** Native QNX `io-char` character device library (`libio-char.a`, `<sys/io-char.h>`).
- **PCI Discovery:** Scans the PCI hierarchy using `pci_device_cfg_rd32`, matches `UART3-dev` (BDF `0x38`) by verifying Doorbell BAR 0 and MMIO BAR 2 (GPA `0x804020c000`), enables PCI Memory Space decoding (`cmd |= 0x06`), and retains the `pci_devhdl_t` handle.
- **MMIO Mapping:** Maps the 4 KiB register aperture using `mmap_device_memory()`.
- **FIFO Polling Thread:** 1 ms thread polling `UART_LS_DR` / `UART_RFL` for RX (feeding `tti()` and signaling `iochar_send_event()`) and draining `tty.obuf` into `UART_TH_OFFSET` when `UART_US_TFNF` is asserted.
- **Line Discipline & Newline Translation:** Configures POSIX termios flags (`c_oflag = OPOST | ONLCR`, `c_iflag = ICRNL | IXON`, `c_lflag = ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN`) and calls `ttc(TTC_INIT_EDIT)` to eliminate terminal staircasing.
- **Namespace Registration:** Registers device node `/dev/ser3`.

### 5.2 Getty & Session Supervisor (`qnx-getty`)
- **Location:** [`guest-os/qnx/amba-uart/qnx-getty.c`](../guest-os/qnx/amba-uart/qnx-getty.c)
- **Session Leadership:** Implements `fork()` + `setsid()` so `/dev/ser3` is acquired as the true controlling terminal (`/dev/tty`).
- **Job Control:** Establishes a valid process group with full job control (`Ctrl-C`, `Ctrl-Z`, `fg`, `bg`) without warnings.
- **Auto-Respawn:** Launches `/system/bin/login -f root` and automatically catches session termination (`waitpid()`), immediately presenting a fresh prompt upon `# exit` or logout.

---

## 6. Build & Deployment Automation

### 6.1 Building All Guest OS Artifacts
To cross-compile all kernel modules, drivers, resource managers, and daemons:
```bash
# Build Ubuntu, Alpine, and QNX guest packages:
./guest-os/build_guest.sh --distro=all

# Or build specifically for QNX:
./guest-os/build_guest.sh --distro=qnx
```
Output artifacts are staged under `build/guest/ubuntu/` and `build/guest/qnx/`.

### 6.2 Deploying to Live Target Boards

#### Deploy to Ubuntu HVM (`n1-655-devkit-ubuntu`):
```bash
./guest-os/deploy_guest.sh n1-655-devkit-ubuntu --reload --enable-serial
```

#### Deploy to QNX HVM (`n1-655-devkit-qnx`):
```bash
./guest-os/deploy_guest.sh n1-655-devkit-qnx --reload --enable-serial
```

---

## 7. Operational Verification Runbook

### 7.1 Accessing the Consoles

| Target Domain | Serial Port Endpoint | Telnet Bridge Port | Login Credentials |
|---|---|---|---|
| **EVE Dom0 Console** | `/dev/ttyCH9344USB8` (Devkit) / `ttyCH9344USB0` (Pro) | `telnet <host> 6070` | `root` (no password) |
| **Ubuntu 24.04 Console 2** | `/dev/ttyCH9344USB9` (Devkit) / `ttyCH9344USB1` (Pro) | `telnet <host> 6071` | `ubuntu` / `ubuntu` |
| **QNX 8.0 Console 3** | `/dev/ttyCH9344USB10` (Devkit) / `ttyCH9344USB2` (Pro) | `telnet <host> 6072` | `root` (auto-login / blank) |

### 7.2 Verifying In-Guest State

#### On Ubuntu HVM:
```bash
# Check loaded module and MMIO statistics:
sudo cat /proc/tty/driver/amba_uart
# Expected: 0: uart:amba_uart mmio:0x804060C000 irq:70 tx:<count> rx:<count> RTS|CTS|DTR|DSR|CD
```

#### On QNX HVM:
```bash
# Check running driver and getty processes:
pidin -p devc-seramb
pidin -p qnx-getty

# Check registered serial device:
ls -l /dev/ser*
# Expected: /dev/ser1, /dev/ser3
```

---

## 8. Troubleshooting

1. **Host Port In Use (`-EBUSY` on server restart):**
   - If `amba_virt_uart_server` fails with `-EBUSY`, QEMU is still holding the character device descriptor.
   - Stop the domain before restarting the server:
     ```bash
     ./scripts/restart_instance.sh <domain-name> --stop
     setsid ./amba_virt_uart_server -u all -f </dev/null >uart_server.log 2>&1 &
     ./scripts/restart_instance.sh <domain-name> --start
     ```
2. **Duplicate Readers on QNX Console:**
   - Slay all old instances before starting a new getty:
     ```bash
     slay -f qnx-getty qnx-serial-console.sh devc-seramb
     /system/bin/devc-seramb -p /dev/ser3 &
     /system/bin/qnx-getty /dev/ser3 &
     ```
