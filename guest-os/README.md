# Ambarella HVM Guest OS Cross-Compilation Guide

This directory contains drivers, resource managers, and test client applications for guest operating systems running as Hardware Virtual Machines (HVM) on the **Ambarella N1-655** edge virtualization platform:

1. **Ubuntu 24.04 LTS HVM** (`glibc` 2.39 / Linux kernel 6.8+ AArch64)
2. **Alpine Linux 3.20 HVM** (`musl` / Linux `linux-virt` 6.6+ AArch64)
3. **BlackBerry QNX Neutrino 8.0 HVM** (Microkernel RTOS / Userspace Resource Manager)

---

## 1. Quick Start

### Build All Guest Artifacts
```bash
# Build Ubuntu, Alpine, and QNX artifacts consecutively (~20s total)
make guest-all
# Or alias
make guest
```

### Build Specific Guest Distributions
```bash
# Ubuntu 24.04 LTS: amba_virt.ko (kernel module) + amba-virt-client (glibc)
make guest-ubuntu

# Alpine Linux 3.20: amba_virt.ko (kernel module) + amba-virt-client (static musl)
make guest-alpine

# BlackBerry QNX 8.0: amba-virt-resmgr (resource manager) + amba-virt-client (qnx)
make guest-qnx

# Build full bootable QNX 8.0 QCOW2 cloud image
make guest-qnx-image

# Windows 11 ARM64 HVM cloud image (DISM/bcdboot pipeline, not amba_virt)
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

| Strategy | Host Requirements | Ubuntu 24.04 Build Time | Alpine 3.20 Build Time | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **Fast Path (Native Host)** | `gcc-aarch64-linux-gnu` + `g++-aarch64-linux-gnu` | **~7.8 seconds** | **~7.9 seconds** | **Recommended.** Runs directly on x86 hardware. |
| **Fallback Path (Docker QEMU)** | Docker + `tonistiigi/binfmt` (QEMU emulation) | ~1m 22 seconds | ~1m 20 seconds | Automatically used if cross-compilers missing. |

### Why Native Cross-Compilation is 10x Faster

When running inside an emulated container (`--platform linux/arm64`), Docker relies on `qemu-aarch64-static` to emulate ARM64 CPU instructions on an x86 host. For compilers (`gcc`, `g++`, `ld`) and Linux kernel build systems (`kbuild`) that spawn dozens of processes and parse hundreds of megabytes of C headers, QEMU binary translation incurs a **20x–50x CPU penalty**.

The **Fast Path** eliminates this overhead entirely:
1. **One-Time Header Extraction**: The build system creates builder container images once (`Dockerfile.ubuntu`, `Dockerfile.alpine`) and extracts authentic distro headers into `build/guest/headers/{ubuntu,alpine}`.
2. **Native Kbuild Helper Tools**: The kbuild host utilities (`fixdep`, `modpost`, `genksyms`) are compiled natively with host x86 `gcc` in <0.5s.
3. **Bare-Metal Host Execution**: Out-of-tree modules and client binaries are compiled using the host's native `aarch64-linux-gnu-gcc` and `aarch64-linux-gnu-g++` at full bare-metal CPU speed.

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
├── client/                     # Multi-OS test & benchmark client
│   ├── amba-virt-client.cxx    # Shared C++17 client application
│   ├── amba_virt_dev.hxx       # RAII character device wrapper
│   ├── Makefile                # Cross-platform Makefile (Linux glibc/musl + QNX)
│   ├── compat/                 # Header-only fallback test harness
│   │   └── CppUTest/           # Embedded CppUTest stub runner
│   └── include/uapi/           # Local UAPI headers symlink
├── linux/
│   └── amba-virt/              # Linux guest kernel driver
│       ├── amba_virt_hvm.c     # Guest PCI driver (ivshmem discovery & IRQ)
│       ├── amba_virt_core.c    # Character device & IOCTL handlers
│       ├── amba_virt_core.h    # Shared kernel driver definitions
│       └── Makefile            # Out-of-tree kbuild Makefile (KDIR_HVM)
├── qnx/
│   ├── amba-virt/              # QNX Neutrino 8.0 resource manager
│   │   ├── amba_virt_resmgr.c  # /dev/amba_virt character device daemon
│   │   └── Makefile            # QNX qcc Makefile (-Vgcc_ntoaarch64le)
│   └── qnx-build/              # QNX HVM disk image builder
│       └── build.sh            # Automated QCOW2 cloud image creator
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
│   └── amba-virt-client        # Dynamically linked glibc ARM64 binary
├── alpine/
│   ├── amba_virt.ko            # Alpine 3.20 kernel module (6.6.142-0-virt)
│   └── amba-virt-client        # Statically linked standalone musl ARM64 binary
└── qnx/
    ├── amba-virt-resmgr        # QNX 8.0 AArch64 resource manager daemon
    └── amba-virt-client        # QNX 8.0 AArch64 test client
```

### Verifying Binary Architecture

```bash
# Verify ELF architectures
file build/guest/*/*

# Inspect kernel module metadata
modinfo build/guest/ubuntu/amba_virt.ko
modinfo build/guest/alpine/amba_virt.ko
```

Expected `vermagic` outputs:
- **Ubuntu 24.04**: `6.8.0-139-generic SMP preempt mod_unload modversions aarch64`
- **Alpine 3.20**: `6.6.142-0-virt SMP preempt mod_unload modversions aarch64`
