# amba-virt

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](doc/Architecture.md#security-boundary)).

Virtualize Ambarella N1-655 devices for **EVE-OS** (KVM). Hardware stays on
the hypervisor host: one trusted `amba-virt-server` in EVE Dom0 owns VisORC,
the camera, and the other shared accelerators, and brokers them to unchanged
HVM guests over virtio-vsock and ivshmem. The server, its camera userspace,
firmware, and the signed out-of-tree Ambarella kernel modules (Cavalry /
VisORC, `amba_virt`, and optional vendor drivers) ship inside the EVE image as
the `eve/pkg/amba-virt` layer and start at boot before any guest.

| Path | What it is |
|---|---|
| [Makefile](Makefile) | Top-level build orchestration (`eve`, `drivers`, `eve-kernel`, `eve-kernel-headers`, `clean`) |
| [drivers/](drivers/) | Out-of-tree Ambarella kernel modules (`amba_virt`, `dsplog`, `pci_platform`, optional `cavalry`, `ambvideo`, `pwr_gpu`, `amba_otp`) |
| [eve/](eve/) | LF Edge EVE-OS submodule with Ambarella board support and the `pkg/amba-virt` Dom0 layer |
| [eve-kernel/](eve-kernel/) | EVE Linux kernel package definitions and configuration |
| [scripts/](scripts/) | Cloud (`zcli`) wrappers, EVE image publishing, guest deployment, and node power scripts |
| [models/](models/) | Hardware-details JSON for Cooper cloud models |
| [apps/](apps/) | Edge-app manifests and instance configurations |
| [eden/](eden/) | LF Edge Eden test harness and client orchestration framework for EVE |
| [tools/](tools/) | Host flashing and utility binaries (`usb-matrix` for x86 Linux USB programming) |
| [doc/](doc/) | Architecture, transport, Cavalry virtualization, and EVE guides |
| [guest-os/](guest-os/) | Guest OS kernel drivers, resource managers, test clients, and cloud images |

## Repository Setup & Submodules

Clone this repository with all public submodules initialized:

```bash
git clone --recursive https://github.com/cchiou-amba/amba-virt.git
cd amba-virt
```

## Building & Targets

The top-level `Makefile` unifies EVE BaseOS image generation with kernel header
extraction and out-of-tree driver compilation and signing across two distinct compile modes:

```bash
# Query active compile mode and configuration state
make mode

# Switch to development mode (host out-of-tree drivers, rapid iteration)
make set-mode-development

# Switch to production mode (hermetic in-tree drivers, zero-trust appliance)
make set-mode-production

# Build EVE BaseOS with the amba-virt Dom0 layer and all drivers (default goal)
make
make all

# Build all out-of-tree drivers + all guest side artifacts
make everything

# Build all HVM guest side artifacts
make guest

# Build the amba-virt Dom0 layer (server, camera userspace, firmware, signed modules)
make amba-virt-image

# Build EVE BaseOS live installer image for active mode (runs amba-virt-image)
make eve

# Build and sign all out-of-tree drivers found under drivers/ (dev mode)
make drivers

# Build EVE kernel package via Docker for the active mode
make eve-kernel

# Extract linux-headers and module signing keys from LinuxKit cache
make eve-kernel-headers

# Build specific guest distributions
make guest-ubuntu
make guest-alpine
make guest-qnx

# Clean build artifacts and staging directories
make clean

# Deep clean including extracted kernel headers and caches
make distclean
```

### Compile Modes: Development vs. Production

EVE-OS enforces kernel module signature verification (`CONFIG_MODULE_SIG_FORCE=y`).
The repository provides two operational modes configured via the `.mode` file:

- **Development Mode (`make set-mode-development`)**:
  - Builds `kernel-gcc` and extracts headers and signing keys to `build/certs/`.
  - Out-of-tree drivers are compiled on the host and signed with the persistent local key.
  - `make amba-virt-image` copies the signed modules named in the `/etc/amba-virt` module lists into the `eve/pkg/amba-virt` layer under `/lib/modules/<ver>/extra/`, and `make eve` builds them into the EVE root filesystem.
- **Production Mode (`make set-mode-production`)**:
  - Builds `kernel-ambarella` via Docker BuildKit with driver sources mapped in as build contexts.
  - Drivers and firmware are signed with an ephemeral single-use key and baked directly into `rootfs.img` under `/lib/modules/<ver>/extra/` and `/lib/firmware/`.
  - Fully hermetic zero-trust appliance image measured into TPM PCR 13.


### Out-of-Tree Driver Handling & Signing

All drivers residing under `drivers/` are discovered dynamically. Public builds include
`amba_virt`, `dsplog`, and `pci_platform`. Proprietary and NDA modules (such as
`drivers/cavalry`, `drivers/ambvideo`, `drivers/pwr_gpu`, and `drivers/amba_otp`) are
maintained in internal repositories, automatically compiled if present in the workspace,
and cleanly skipped if absent.

During `make eve-kernel-headers`, kernel module signing keys (`signing_key.pem`
and `signing_key.x509`) are extracted directly from the LinuxKit cache into
`build/certs/`. The `drivers` target builds each module against the extracted
headers, signs them with `sign-file` (SHA256), and stages them into:
- `build/modules/`: Signed kernel modules (`.ko`)
- `build/firmware/`: Required driver firmware binaries

### Deployment & Boot Order

Deployment is an EVE image update. Publish the image built by `make eve` and update the node
over the air ([doc/EVE-UpdateEVE-Firmware.md](doc/EVE-UpdateEVE-Firmware.md)), or install it
fresh ([doc/EVE-Installation.md](doc/EVE-Installation.md)). Nothing amba-virt is staged in
`/persist`.

On boot, LinuxKit init runs `/etc/init.d/021-amba-virt-server` before the EVE system services.
The server loads `ambcma.ko`, `cavalry.ko`, and `amba_virt.ko`, brings up the camera pipeline
when `/etc/amba-virt/camera.conf` exists, and reports ready only once `/dev/amba_virt_shm*`
exists, so every Ambarella device node is in place before EVE starts a guest. See
[doc/AmbaVirtServer.md](doc/AmbaVirtServer.md).

### USB Flash Programming (`usb-matrix`)

For bare-metal board bring-up, initial provisioning, and recovery over USB, the repository includes `tools/bin/usb-matrix`, an x86-64 Linux host utility:

```bash
# Scan and list connected Ambarella devices
./tools/bin/usb-matrix -l

# Program/burn firmware on CV3AD655 (N1-655)
sudo ./tools/bin/usb-matrix -c cv3ad655 -f /path/to/firmware.bin
```

See [tools/README.md](tools/README.md) for full usage, prerequisites (`libusb-1.0`), and boot mode preparation.

### Guest OS Cross-Compilation & Cloud Images

The repository provides automated cross-compilation and image build pipelines for HVM guest operating systems running on Ambarella N1-655:

- **Linux & QNX Fast Path**: Native host cross-compilation (**~7–8s per distribution**) via the host's native toolchains rather than slow QEMU CPU emulation.
- **Windows 11 on ARM64**: Automated unattended image assembly using UEFI/ACPI (`AAVMF` / `EDK2`) and paravirtualized VirtIO drivers.

```bash
# Recommended host prerequisite (Ubuntu/Debian host):
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Build individual or all guest distributions:
make guest-ubuntu     # Ubuntu 24.04: amba_virt.ko + amba_cavalry.ko + client + AI apps
make guest-alpine     # Alpine Linux 3.20: amba_virt.ko + amba_cavalry.ko + static client
make guest-qnx        # BlackBerry QNX 8.0: resource managers + clients + AI apps
make guest-all        # Build all three consecutively (~20s total)
make guest-qnx-image  # Bootable QNX 8.0 QCOW2 cloud image
make guest-windows    # Windows 11 ARM64 QCOW2 cloud image
```

Staged binaries are placed into `build/guest/{ubuntu,alpine,qnx}/` and cloud images into `build/guest/images/`. See [guest-os/README.md](guest-os/README.md), [doc/Guest-OS-Cross-Compilation.md](doc/Guest-OS-Cross-Compilation.md), and [guest-os/windows/README.md](guest-os/windows/README.md) for architectures and guides.

## Documentation

### Core Architecture & Transports
- [doc/Architecture.md](doc/Architecture.md) — System architecture, privilege levels, and device assignment.
- [doc/AmbaVirtServer.md](doc/AmbaVirtServer.md) — Host daemon (`amba-virt-server`), transport architecture, IPC benchmarks, and memory layouts.
- [doc/EVE-Native-ivshmem-Support.md](doc/EVE-Native-ivshmem-Support.md) — Native `ivshmem-plain` device support in EVE Pillar KVM.

### Hardware Accelerator & Peripheral Virtualization
- [doc/CavalryVirtualization.md](doc/CavalryVirtualization.md) — Cavalry VisORC NPU/VP virtualization, host DMA mapping, and proxy architecture.
- [doc/RNGVirtualization.md](doc/RNGVirtualization.md) — Host-mediated hardware TRNG virtualization, Dom0 silicon retention, and VirtIO RNG architecture.

### Driver & Firmware Operations
- [doc/EVE-BaseOS-AmbarellaDrivers.md](doc/EVE-BaseOS-AmbarellaDrivers.md) — The `eve/pkg/amba-virt` image layer, module and firmware layout, and boot order.
- [doc/EVE-OutOfTree-KMODs.md](doc/EVE-OutOfTree-KMODs.md) — Dual compile modes (Development vs. Production) and cryptographic signature enforcement.
- [doc/EVE-UpdateEVE-Firmware.md](doc/EVE-UpdateEVE-Firmware.md) — BaseOS OTA image upgrade workflows and A/B dual-partition preservation.

### Edge Application Provisioning & Cloud Models
- [doc/EVE-Ambarella-Models.md](doc/EVE-Ambarella-Models.md) — ZEDEDA Cloud hardware models (`N1-655-Cooper-Pro`, `N1-655-Cooper-Devkit`) and `ioMemberList`.
- [doc/EVE-EdgeApp-Provision.md](doc/EVE-EdgeApp-Provision.md) — End-to-end deployment guide for HVM edge application instances.
- [doc/EVE-ReconfigureEdgeApps.md](doc/EVE-ReconfigureEdgeApps.md) — Edge application interface update rules and immutable instance policies.
- [doc/ZedControl-scripts.md](doc/ZedControl-scripts.md) — Catalog of `zcli` orchestration wrappers for cloud orchestration.

### Guest Operating Systems & Scaling
- [guest-os/README.md](guest-os/README.md) — Guest OS developer guide, cross-compilation pipeline, and cloud images.
- [doc/Guest-OS-Cross-Compilation.md](doc/Guest-OS-Cross-Compilation.md) — Native host cross-compilation pipeline architecture and benchmarks.
- [doc/EVE-Multiple-HVM.md](doc/EVE-Multiple-HVM.md) — Multiple HVMs on one Dom0 server: CID and slice arbitration, memory overhead modeling, and future design.

### Host Tools
- [tools/README.md](tools/README.md) — Host tools guide and `usb-matrix` USB flash programming reference.

