# Guest OS Cross-Compilation Architecture & Performance Guide

- **Status**: Production Architecture & Engineering Guide
- **Target Platform**: Ambarella N1-655 SoC / EVE-OS Hypervisor
- **References**: [guest-os/README.md](../guest-os/README.md), [Architecture.md](Architecture.md), [EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md), [guest-os/windows/README.md](../guest-os/windows/README.md)

---

## 1. System Overview

On the **Ambarella N1-655** architecture, the hypervisor host runs **EVE-OS** (KVM). Edge workloads run inside Hardware Virtual Machine (HVM) guest domains. To enable low-latency communication with Ambarella hardware accelerators (Cavalry neural processors and VisORC DSPs), guest operating systems load the `amba_virt` transport layer:

```text
+--------------------------------------------------------------------+
|                Ambarella N1-655 Hypervisor Host                    |
|  +---------------------------+  +-------------------------------+  |
|  |    EVE BaseOS (Dom0)      |  |  Privileged NOHYPER Container |  |
|  |  - Kernel: 6.1-linuxkit   |  |  - amba-virt-server           |  |
|  |  - Drivers: cavalry.ko,   |  |  - Hardware acceleration proxy|  |
|  |    amba_virt (host).ko    |  +---------------+---------------+  |
|  +-------------+-------------+                  |                  |
+----------------|--------------------------------|------------------+
                 |       vhost-vsock-pci &        |
                 |      ivshmem-plain (1 GiB)     |
+----------------|--------------------------------|------------------+
|                v                                v                  |
|  +--------------------------------------------------------------+  |
|  |                 KVM HVM Guest VM (AArch64)                   |  |
|  |  - Supported Guest Operating Systems                         |  |
|  |  - Kernel Driver: amba_virt (guest).ko / amba-virt-resmgr    |  |
|  |  - Userspace App: amba-virt-client                           |  |
|  +--------------------------------------------------------------+  |
+--------------------------------------------------------------------+
```

Because development workstations and CI/CD runners run on **x86_64 Linux**, all guest artifacts (`amba_virt.ko`, `amba-virt-client`, and `amba-virt-resmgr`) must be cross-compiled cleanly and rapidly.

---

## 2. The Speed Problem: Why QEMU Emulation Stalls

In naive Docker-based cross-compilation workflows, developers run `--platform linux/arm64` containers on x86 hosts. Under this configuration, the host Linux kernel uses `binfmt_misc` to route all ARM64 ELF executions through `qemu-aarch64-static`.

### Performance Impact of QEMU User Emulation
- **Compilation overhead**: Compilers (`gcc-13`, `g++-13`) parse hundreds of megabytes of C/C++ ASTs and emit machine code. Translating billions of emulated instructions at runtime reduces CPU throughput by **90%–95%**.
- **Process spawning storm**: Linux kbuild and CMake/Make spawn hundreds of short-lived helper processes (`fixdep`, `modpost`, `sed`, `awk`, `sh`). Each process invocation incurs QEMU runtime initialization, memory mapping, and translation cache overhead.
- **Measured build duration**: Compiling a single 2-file kernel module (`amba_virt.ko`) took **53.5 seconds** inside an emulated container, resulting in a total build time exceeding **1 minute 22 seconds** per guest OS.

---

## 3. The Fast Path: Host Native Cross-Compilation

To achieve rapid developer turnaround (~7s total build time), `amba-virt` implements the same design pattern used by EVE BaseOS (`eve-kernel-headers` and `make drivers` in `Makefile`):

```text
                          Top-Level Makefile
                                  │
            ┌─────────────────────┼─────────────────────┐
            ▼                     ▼                     ▼
      make guest-ubuntu     make guest-alpine     make guest-qnx
            │                     │                     │
            ▼                     ▼                     ▼
     Ubuntu 6.8 Headers    Alpine 6.6 Headers    QNX SDP 8.0 (qcc)
    (build/guest/headers) (build/guest/headers)  -Vgcc_ntoaarch64le
            │                     │                     │
            ├─────────────────────┤                     │
            │   Native Host GCC   │                     │
            │ aarch64-linux-gnu-  │                     │
            │   (~4s per target)  │                     │
            ▼                     ▼                     ▼
    build/guest/ubuntu/   build/guest/alpine/   build/guest/qnx/
      ├── amba_virt.ko      ├── amba_virt.ko      ├── amba-virt-resmgr
      ├── amba_cavalry.ko   ├── amba_cavalry.ko   ├── amba-cavalry-resmgr
      ├── amba-virt-client  ├── amba-virt-client  ├── libamba_virt.so
      ├── amba-virt-cli     ├── amba-virt-cli     ├── amba-virt-client
      ├── cavalry_hvm_demo  ├── cavalry_hvm_demo  ├── amba-virt-cli
      └── cavalry_hvm_yolo  └── cavalry_hvm_yolo  ├── cavalry_hvm_demo
                                                  └── cavalry_hvm_yolo
```

### The Kbuild Host Helper Bootstrap Challenge

When out-of-tree kernel headers are installed from upstream distribution packages (`linux-headers-generic` on Ubuntu or `linux-virt-dev` on Alpine), the binary tools located in `scripts/` (`fixdep`, `modpost`, `genksyms`) are precompiled for the **target architecture** (ARM64).

When an x86 host executes `make -C <headers> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules`:
1. kbuild invokes `scripts/basic/fixdep` to process dependency files.
2. If `fixdep` is an ARM64 binary, the host attempts to run it via QEMU or fails with `/lib/ld-linux-aarch64.so.1: not found`.
3. If `EXPORT_SYMBOL` is used, kbuild invokes `scripts/genksyms/genksyms`.
4. At the final link stage, kbuild executes `scripts/mod/modpost` to validate symbol versions.

### The Solution: Rapid Host Helper Bootstrapping

Rather than rebuilding the entire kernel tree or running under slow QEMU emulation, `scripts/build_guest.sh` extracts the distribution headers once to `build/guest/headers/` and compiles the three small host helper tools with the host's native `gcc`:

```bash
# 1. Compile fixdep with host x86 gcc (~0.05s)
gcc -O2 -o "${kdir}/scripts/basic/fixdep" "${kcommon}/scripts/basic/fixdep.c"

# 2. Compile modpost with host x86 gcc (~0.15s)
gcc -O2 -I"${kdir}/scripts/mod" -I"${kcommon}/scripts/mod" \
    -o "${kdir}/scripts/mod/modpost" \
    "${kcommon}/scripts/mod/modpost.c" \
    "${kcommon}/scripts/mod/file2alias.c" \
    "${kcommon}/scripts/mod/sumversion.c" \
    "${kcommon}/scripts/mod/symsearch.c"

# 3. Compile genksyms with host x86 gcc (~0.08s)
gcc -O2 -I"${kdir}/scripts/genksyms" -I"${kcommon}/scripts/genksyms" \
    -o "${kdir}/scripts/genksyms/genksyms" \
    "${kcommon}/scripts/genksyms/genksyms.c" \
    "${kdir}/scripts/genksyms/lex.lex.c" \
    "${kdir}/scripts/genksyms/parse.tab.c"
```

Once bootstrapped, kbuild executes entirely on native x86 hardware, delegating code emission to `/usr/bin/aarch64-linux-gnu-gcc`.

---

## 4. Benchmark Performance Metrics

Measurements performed on x86_64 host (Intel Core i9 / 32 cores):

| Stage | Docker QEMU Emulation | Host Native Cross-Compiler | Performance Ratio |
| :--- | :--- | :--- | :--- |
| **Ubuntu `amba_virt.ko`** | 53.56 s | **4.45 s** | **12.0x faster** |
| **Ubuntu `amba-virt-client`** | 29.26 s | **3.89 s** | **7.5x faster** |
| **Ubuntu Clean + Build** | 1m 22.39 s | **7.86 s** | **10.5x faster** |
| **Alpine Clean + Build** | ~1m 20.00 s | **7.99 s** | **10.0x faster** |
| **QNX 8.0 Clean + Build** | 4.02 s | **4.01 s** | Native speed |
| **All Guests (`make guest-all`)** | ~2m 45.00 s | **19.84 s** | **8.3x faster** |

---

## 5. Host Prerequisites & Setup

### Debian / Ubuntu Host
```bash
sudo apt-get update
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

### BlackBerry QNX 8.0 Setup
Ensure QNX SDP 8.0 is installed and the environment script is sourced:
```bash
source ~/qnx/qnx800/qnxsdp-env.sh
```

---

## 6. Build Targets Reference

| Target | Description | Output Location |
| :--- | :--- | :--- |
| `make guest` | Build all guest targets consecutively | `build/guest/{ubuntu,alpine,qnx}` |
| `make guest-ubuntu` | Build Ubuntu 24.04 kernel modules, clients, and AI apps | `build/guest/ubuntu/` |
| `make guest-alpine` | Build Alpine 3.20 kernel modules, clients, and AI apps | `build/guest/alpine/` |
| `make guest-qnx` | Build QNX 8.0 resource managers, C library, clients, and AI apps | `build/guest/qnx/` |
| `make guest-qnx-image` | Generate bootable QNX 8.0 QCOW2 cloud image | `build/guest/qnx/qnx-8.0-arm64-cloudimg.qcow2` |
| `make guest-windows` | Build Windows 11 ARM64 HVM QCOW2 cloud image | `guest-os/windows/windows-build/output/dist/windows-11-arm64-cloudimg.qcow2` |
| `make clean-guest` | Clean staged binaries (preserves cached headers) | `build/guest/{ubuntu,alpine,qnx}` |
| `make distclean-guest` | Remove all guest artifacts and cached headers | `build/guest/` |
