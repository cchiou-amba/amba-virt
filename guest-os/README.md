# Ambarella HVM Guest OS Cross-Compilation Guide

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](../doc/Architecture.md#security-boundary)).

*Copyright (C) 2026, Ambarella International LLC*

This directory contains drivers, resource managers, test client applications, and AI runtime suites for guest operating systems running as Hardware Virtual Machines (HVM) on the **Ambarella N1-655** edge virtualization platform across supported Linux distributions (Ubuntu 24.04, Alpine 3.20), RTOS environments (BlackBerry QNX Neutrino RTOS 8.0), and cloud guest images.

---

## 1. Quick Start

### Build All Guest Artifacts
```bash
# Build guest artifacts consecutively (~20s total)
make guest-all
# Or alias
make guest
```

### Build Specific Guest Distributions
```bash
# Ubuntu 24.04 LTS: amba_virt.ko + amba_cavalry.ko + iav_virt.ko + amba-virt-client
make guest-ubuntu

# Alpine Linux 3.20: amba_virt.ko + amba_cavalry.ko + iav_virt.ko + amba-virt-client (static musl)
make guest-alpine

# BlackBerry QNX 8.0: resource managers + clients + AI runtime suites
make guest-qnx

# Build full bootable QNX 8.0 QCOW2 cloud image
make guest-qnx-image

# Windows 11 ARM64 HVM cloud image (DISM/bcdboot pipeline)
make guest-windows
```

### Clean Targets
```bash
# Clean staged artifacts (preserves cached extracted headers)
make clean-guest

# Full clean including extracted guest kernel headers cache
make distclean-guest
```

---

## 2. Fast Path: Native Host Cross-Compilation vs. QEMU Emulation

The guest build pipeline supports two execution strategies:

| Strategy | Host Requirements | Ubuntu 24.04 Build Time | Alpine 3.20 Build Time | QNX 8.0 Build Time | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Fast Path (Native Host)** | `gcc-aarch64-linux-gnu` + `g++-aarch64-linux-gnu` | **~7.8 seconds** | **~7.9 seconds** | **~4.0 seconds** | **Recommended.** Runs directly on host CPU. |
| **Fallback Path (Docker QEMU)** | Docker + `tonistiigi/binfmt` (QEMU emulation) | ~1m 22 seconds | ~1m 20 seconds | N/A (QNX uses native `qcc`) | Automatically used if cross-compilers missing. |

### Enabling the Fast Path on Your Host

On an Ubuntu / Debian host system:
```bash
sudo apt-get update
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

For BlackBerry QNX 8.0 compilation:
```bash
# Source the QNX Software Development Platform (SDP) environment
source ~/qnx/qnx800/qnxsdp-env.sh
```

---

## 3. Pinned 6-Guest Slice & Adapter Placement (`0..5`)

On both `n1-655-devkit` and `n1-655-pro`, the six HVM guest instances are created in deterministic `0..5` order so each admission ticket `amba_shm<N>` maps to `/dev/amba_virt_shm<N>` ([../doc/EVE-EdgeApp-Instances.md](../doc/EVE-EdgeApp-Instances.md)):

| Slice Index | Edge-App | Ticket (`amba_shm:<ticket>`) | Extra Adapter | Camera Policy | Console / Guest Drivers |
|---|---|---|---|---|---|
| `0` | `ubuntu_24_04` | `amba_shm0` (`/dev/amba_virt_shm0`) | `UART2:UART2` (`0xffe0018000`) | `on` (`1`, primary IAV) | `console1` (`amba_uart.ko` + `amba_dma.ko`, `amba_cavalry.ko`, `iav_virt.ko`) |
| `1` | `alpine_3_20` | `amba_shm1` (`/dev/amba_virt_shm1`) | `COM3:UART3` (`0xffe0019000`) | `on` (`1`) | `console2` (`amba_uart.ko` + `amba_dma.ko`, `amba_cavalry.ko`, `iav_virt.ko`) |
| `2` | `debian_13` | `amba_shm2` (`/dev/amba_virt_shm2`) | *(none)* | `on` (`1`) | `amba_virt.ko`, `amba_cavalry.ko`, `iav_virt.ko` |
| `3` | `fedora_44` | `amba_shm3` (`/dev/amba_virt_shm3`) | *(none)* | `on` (`1`) | `amba_virt.ko`, `amba_cavalry.ko`, `iav_virt.ko` |
| `4` | `qnx_8_0` | `amba_shm4` (`/dev/amba_virt_shm4`) | `rng-mmio:rng-mmio` | `off` (`0`) | `amba-virt-resmgr`, `amba-cavalry-resmgr` (`SLICE_CLAIM_REQ`, `off=0x100000000`) |
| `5` | `windows_11_arm64` | `amba_shm5` (`/dev/amba_virt_shm5`) | *(none)* | `off` (`0`) | Host/QEMU `ivshmem-plain` wiring |
| `6..7` | *(free spare slices)* | `amba_shm6`, `amba_shm7` | — | `off` (`0`) | Spare 1 GiB Cavalry + 64 MiB low pool slices |

---

## 4. Directory Layout

```text
guest-os/
├── apps/                        # Userspace demo applications & AI test suites
│   ├── cavalry-demo/           # Neural network validation runner (cavalry_hvm_demo)
│   └── cavalry-yolo/           # End-to-end YOLO object detection (cavalry_hvm_yolo)
├── client/                      # Multi-OS test & benchmark client
│   ├── amba-virt-client.cxx    # Shared C++17 unit test & benchmark application
│   ├── amba-virt-cli.cxx       # Diagnostic and inspection CLI tool
│   ├── amba_virt_dev.hxx       # RAII character device wrapper
│   ├── Makefile                # Cross-platform Makefile (Linux glibc/musl + QNX)
│   ├── compat/                 # Header-only fallback test harness
│   └── include/uapi/           # Local UAPI headers symlink
├── linux/                       # Linux guest kernel drivers (EL1 monolithic)
│   ├── amba-virt/              # Core transport driver (amba_virt.ko)
│   │   ├── amba_virt_hvm.c     # Guest PCI driver (ivshmem, vsock claim)
│   │   ├── amba_virt_core.c    # Character device & IOCTL handlers
│   │   └── Makefile            # Out-of-tree kbuild Makefile
│   ├── amba-cavalry/           # Cavalry NPU proxy frontend (amba_cavalry.ko)
│   │   ├── amba_cavalry_hvm.c  # /dev/cavalry frontend kernel driver
│   │   └── include/            # UAPI cavalry ioctl headers
│   ├── amba-uart/              # Hardware UART passthrough driver (amba_uart.ko; ACPI AMBA0001)
│   ├── amba-dma/               # Virtual dmaengine provider (amba_dma.ko; 16 MiB DMA32 bounce lease)
│   └── amba-iav/               # Virtual /dev/iav frontend (iav_virt.ko); see doc/IavVirtualization.md
├── qnx/                         # QNX Neutrino RTOS 8.0 drivers (EL0 microkernel)
│   ├── amba-virt/              # Core transport resource manager (amba-virt-resmgr)
│   │   ├── amba_virt_resmgr.c  # /dev/amba_virt character device daemon
│   │   ├── libamba_virt.c      # C client library (libamba_virt.so / libamba_virt.a)
│   │   └── Makefile            # QNX qcc Makefile (-Vgcc_ntoaarch64le)
│   ├── amba-cavalry/           # Cavalry NPU resource manager (amba-cavalry-resmgr)
│   │   ├── amba_cavalry_resmgr.c# /dev/cavalry POSIX resource manager
│   │   └── Makefile            # QNX qcc Makefile
│   └── qnx-build/              # QNX HVM disk image builder
│       └── build.sh            # Automated QCOW2 cloud image creator
├── apps/iav-ref/                # IAV reference applications (make iav-ref): capture, events, stream, pipeline, decode, overlay
├── alpine/                      # Alpine Linux 3.20 HVM guest build & provisioning guide (alpine/README.md)
├── debian/                       # Lean Debian 13 (trixie) HVM image builder (debian-build/); see debian-build/README.md
├── fedora/                       # Lean Fedora 44 HVM image builder (fedora-build/); see fedora-build/README.md
├── userspace/                   # Imported Ambarella userspace runtimes
│   ├── cavalry_mem/            # Memory allocator abstraction
│   └── nnctrl/                 # VisORC neural network control library
└── docker/
    ├── Dockerfile.ubuntu       # Ubuntu 24.04 ARM64 build environment (Docker fallback path)
    └── Dockerfile.alpine       # Alpine 3.20 ARM64 build environment (Docker fallback path)
```

---

### Virtual `/dev/iav` (`linux/amba-iav/`)

`iav_virt.ko` registers `/dev/iav` (mode 0660) in the guest and runs the real IAV ioctl and
mmap ABI through the host broker. It needs `amba_virt.ko` (its vsock channel and PCI probe).
The ABI is the real driver's, so a legacy application is not ported; what is supported is
limited by a host policy, not by a second API:

| Guest | ioctls served | Everything else |
|---|---|---|
| Every guest | 15 query, descriptor and poll ioctls | `EPERM` |
| The primary guest (bound to `primary-slice` in the host's `/etc/amba-virt/iav.conf`) | 128, including pipeline configuration and run control, JPEG decode and stream overlays | `EPERM` |

An ioctl the real driver does not have fails with `ENOTTY`. The guest needs the `IAV_DEV`
capability (`open` fails with `EPERM` without it); while the host server is away calls fail
with the transport error (`ETIMEDOUT`, `ENOTCONN`) and the next call reconnects. Frames are
read zero-copy through the IAV windows (DSP, bitstream; the primary guest also has the decode and overlay
windows), which `amba-virt-server` attaches automatically when the guest's admission ticket has camera enabled (`amba_shm0`..`amba_shm3` have `camera=on`; `amba_shm4`..`amba_shm7` have `camera=off`). A fresh guest needs
`sudo modprobe iav_virt` (the demo service does it at start); a mapped window must be read and written with aligned
accesses of at most eight bytes (a plain `memcpy` at an unaligned offset destroys the guest VM). The suite that qualifies all this is built into
`amba_virt_demo` (`--iav-qual`; add `--role primary` or `--role consumer` to say which role
the run expects). The reference applications (`apps/iav-ref/`, built by `make iav-ref`, static aarch64, the same binary
on the bare board and in a guest) show the stream, capture, event, pipeline, decode and overlay paths. Details, limits and
the runbook: [../doc/IavVirtualization.md](../doc/IavVirtualization.md).

## 5. Staged Artifacts & Verification

Compiled binaries are staged into `build/guest/<distro>/`:

```text
build/guest/
├── ubuntu/
│   ├── amba_virt.ko            # Ubuntu 24.04 kernel module (6.8.0-101-generic)
│   ├── amba_cavalry.ko         # Ubuntu 24.04 Cavalry proxy module
│   ├── amba_dma.ko             # Ubuntu 24.04 virtual dmaengine provider
│   ├── amba_uart.ko            # Ubuntu 24.04 hardware UART2 passthrough driver
│   ├── iav_virt.ko             # Ubuntu 24.04 virtual /dev/iav frontend
│   ├── amba-virt-client        # Dynamically linked glibc ARM64 binary
│   ├── amba-virt-cli           # Diagnostic CLI tool
│   ├── cavalry_hvm_demo        # AI model test runner
│   └── cavalry_hvm_yolo        # YOLO object detection binary
├── alpine/
│   ├── amba_virt.ko            # Alpine 3.20 kernel module (6.6.142-0-virt)
│   ├── amba_cavalry.ko         # Alpine 3.20 Cavalry proxy module
│   ├── amba_dma.ko             # Alpine 3.20 virtual dmaengine provider
│   ├── amba_uart.ko            # Alpine 3.20 hardware UART3 passthrough driver
│   ├── iav_virt.ko             # Alpine 3.20 virtual /dev/iav frontend
│   ├── amba-virt-client        # Statically linked standalone musl ARM64 binary
│   ├── amba-virt-cli           # Diagnostic CLI tool
│   ├── cavalry_hvm_demo        # AI model test runner
│   └── cavalry_hvm_yolo        # YOLO object detection binary
└── qnx/
    ├── amba-virt-resmgr        # QNX 8.0 AArch64 transport resource manager
    ├── amba-cavalry-resmgr     # QNX 8.0 AArch64 Cavalry NPU resource manager
    ├── libamba_virt.so         # QNX 8.0 shared C library
    ├── libamba_virt.a          # QNX 8.0 static library
    ├── amba-virt-client        # QNX 8.0 unit test & benchmark client
    ├── amba-virt-cli           # QNX 8.0 diagnostic CLI tool
    ├── cavalry_hvm_demo        # QNX 8.0 AI model test runner
    ├── cavalry_hvm_yolo        # QNX 8.0 YOLO object detection binary
    └── qnx-8.0-arm64-cloudimg.qcow2 # Bootable QNX cloud image
```

### Verifying Binary Architecture

```bash
# Verify ELF architectures
file build/guest/*/*

# Inspect Linux kernel module metadata
modinfo build/guest/ubuntu/amba_virt.ko
modinfo build/guest/alpine/amba_virt.ko
```
