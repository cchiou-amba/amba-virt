# amba-virt

Virtualize Ambarella N1-655 devices for **EVE-OS** (KVM). Hardware stays on
the hypervisor host, and out-of-tree Ambarella kernel drivers (Cavalry / VisORC,
`amba_virt`, and optional vendor drivers) are built, signed, and staged for
deployment and automated loading on EVE BaseOS.

| Path | What it is |
|---|---|
| [Makefile](Makefile) | Top-level build orchestration (`eve`, `drivers`, `eve-kernel`, `eve-kernel-headers`, `clean`) |
| [drivers/](drivers/) | Out-of-tree Ambarella kernel modules (`cavalry`, `amba_virt`, optional `amba_otp`) |
| [eve/](eve/) | LF Edge EVE-OS submodule with Ambarella board support and storage-init boot hooks |
| [eve-kernel/](eve-kernel/) | EVE Linux kernel package definitions and configuration |
| [scripts/](scripts/) | Deployment, OTA update, driver loader, and management scripts |
| [models/](models/) | Hardware-details JSON for Cooper cloud models |
| [apps/](apps/) | Edge-app manifests and instance configurations |
| [tools/](tools/) | Host flashing and utility binaries (`usb-matrix` for x86 Linux USB programming) |
| [doc/](doc/) | Architecture, transport, Cavalry virtualization, and EVE guides |
| [guest-os/](guest-os/) | Guest OS kernel drivers, resource managers, and test clients (Ubuntu, Alpine, QNX) |
| [automation/doc/](automation/doc/) | Release notes, issues, and private kernel module distribution guide |

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

# Build EVE BaseOS + all NOHYPER drivers and apps (default goal)
make
make all

# Build NOHYPER drivers/apps + all guest side (Ubuntu, Alpine, QNX)
make everything

# Build all HVM guest side artifacts (Ubuntu, Alpine, QNX)
make guest

# Build all NOHYPER host drivers and apps
make nohyper

# Build EVE BaseOS live installer image for active mode
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
  - Staged to `/persist/modules/` on the target node via `./scripts/deploy_and_insmod.sh <node> --reload` with **~3-second iteration turnaround**.
- **Production Mode (`make set-mode-production`)**:
  - Builds `kernel-ambarella` via Docker BuildKit with driver sources mapped in as build contexts.
  - Drivers and firmware are signed with an ephemeral single-use key and baked directly into `rootfs.img` under `/lib/modules/<ver>/extra/` and `/lib/firmware/`.
  - Fully hermetic zero-trust appliance image measured into TPM PCR 13.


### Out-of-Tree Driver Handling & Signing

All drivers residing under `drivers/` are discovered dynamically. Optional NDA
modules (such as `drivers/amba_otp`) are automatically compiled if present and
cleanly skipped if absent.

During `make eve-kernel-headers`, kernel module signing keys (`signing_key.pem`
and `signing_key.x509`) are extracted directly from the LinuxKit cache into
`build/certs/`. The `drivers` target builds each module against the extracted
headers, signs them with `sign-file` (SHA256), and stages them into:
- `build/modules/`: Signed kernel modules (`.ko`)
- `build/firmware/`: Required driver firmware binaries
- `build/bin/`: Host helper daemons and tools (e.g. `amba-virt-server`)

### Deployment & Automated Early Boot Loading

To deploy staged drivers to an active EVE node:

```bash
./scripts/deploy_and_insmod.sh <target-node>
```

This script stages modules into `/persist/modules/`, firmware into
`/persist/firmware/`, and installs the runtime loader to
`/persist/bin/load-ambarella-drivers.sh`.

On system boot, EVE's `storage-init` service automatically executes
`/persist/bin/load-ambarella-drivers.sh` via chroot into `/hostfs` as soon as the
`/persist` partition is mounted, ensuring all Ambarella character devices
(`/dev/cavalry`, `/dev/amba_virt`, etc.) are initialized before edge applications
and runtime domains launch.

### USB Flash Programming (`usb-matrix`)

For bare-metal board bring-up, initial provisioning, and recovery over USB, the repository includes `tools/bin/usb-matrix`, an x86-64 Linux host utility:

```bash
# Scan and list connected Ambarella devices
./tools/bin/usb-matrix -l

# Program/burn firmware on CV3AD655 (N1-655)
sudo ./tools/bin/usb-matrix -c cv3ad655 -f /path/to/firmware.bin
```

See [tools/README.md](tools/README.md) for full usage, prerequisites (`libusb-1.0`), and boot mode preparation.

### Guest OS Cross-Compilation (Ubuntu, Alpine, QNX)

The repository provides automated cross-compilation for HVM guest operating systems running on Ambarella N1-655. To achieve rapid turnaround (**~7–8s per distribution**), builds execute on the host's native cross-compiler rather than through slow QEMU CPU emulation:

```bash
# Recommended host prerequisite (Ubuntu/Debian host):
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Build individual or all guest distributions:
make guest-ubuntu     # Ubuntu 24.04: amba_virt.ko + amba-virt-client
make guest-alpine     # Alpine Linux 3.20: amba_virt.ko + static client
make guest-qnx        # BlackBerry QNX 8.0: amba-virt-resmgr + client
make guest-all        # Build all three consecutively (~20s total)
```

Staged binaries are placed into `build/guest/{ubuntu,alpine,qnx}/`. See [guest-os/README.md](guest-os/README.md) and [doc/Guest-OS-Cross-Compilation.md](doc/Guest-OS-Cross-Compilation.md) for architecture, header extraction caching, and benchmarks.

## Documentation

- [doc/Architecture.md](doc/Architecture.md) — EVE architecture and device assignment.
- [guest-os/README.md](guest-os/README.md) — HVM guest OS cross-compilation developer guide.
- [doc/Guest-OS-Cross-Compilation.md](doc/Guest-OS-Cross-Compilation.md) — Fast cross-compilation architecture and benchmarks.
- [doc/CavalryVirtualization.md](doc/CavalryVirtualization.md) — Cavalry architecture and memory management.
- [doc/EVE-BaseOS-AmbarellaDrivers.md](doc/EVE-BaseOS-AmbarellaDrivers.md) — Ambarella drivers in EVE BaseOS.
- [doc/Native-ivshmem-Support-in-EVE-BaseOS.md](doc/Native-ivshmem-Support-in-EVE-BaseOS.md) — ivshmem support in EVE BaseOS.
- [tools/README.md](tools/README.md) — Host tools guide and `usb-matrix` USB flash programming reference.
- [automation/doc/Issues.md](automation/doc/Issues.md) — Known issues, tracking, and upstream integration notes.
- [automation/doc/PrivateKernelModuleRelease.md](automation/doc/PrivateKernelModuleRelease.md) — Distributing private out-of-tree kernel modules without source.
