# Ambarella HVM Guest OS Cross-Compilation Guide

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
# Ubuntu 24.04 LTS: amba_virt.ko + amba_cavalry.ko + amba-virt-client
make guest-ubuntu

# Alpine Linux 3.20: amba_virt.ko + amba_cavalry.ko + amba-virt-client (static musl)
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

## 3. Directory Layout

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
│   │   ├── amba_virt_hvm.c     # Guest PCI driver (ivshmem discovery & IRQ)
│   │   ├── amba_virt_core.c    # Character device & IOCTL handlers
│   │   └── Makefile            # Out-of-tree kbuild Makefile
│   └── amba-cavalry/           # Cavalry NPU proxy frontend (amba_cavalry.ko)
│       ├── amba_cavalry_hvm.c  # /dev/cavalry frontend kernel driver
│       └── include/            # UAPI cavalry ioctl headers
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
├── userspace/                   # Imported Ambarella userspace runtimes
│   ├── cavalry_mem/            # Memory allocator abstraction
│   └── nnctrl/                 # VisORC neural network control library
└── docker/
    ├── Dockerfile.ubuntu       # Ubuntu 24.04 ARM64 container image definition
    └── Dockerfile.alpine       # Alpine 3.20 ARM64 container image definition
```

---

## 4. Staged Artifacts & Verification

Compiled binaries are staged into `build/guest/<distro>/`:

```text
build/guest/
├── ubuntu/
│   ├── amba_virt.ko            # Ubuntu 24.04 kernel module (6.8.0-139-generic)
│   ├── amba_cavalry.ko         # Ubuntu 24.04 Cavalry proxy module
│   ├── amba-virt-client        # Dynamically linked glibc ARM64 binary
│   ├── amba-virt-cli           # Diagnostic CLI tool
│   ├── cavalry_hvm_demo        # AI model test runner
│   └── cavalry_hvm_yolo        # YOLO object detection binary
├── alpine/
│   ├── amba_virt.ko            # Alpine 3.20 kernel module (6.6.142-0-virt)
│   ├── amba_cavalry.ko         # Alpine 3.20 Cavalry proxy module
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
