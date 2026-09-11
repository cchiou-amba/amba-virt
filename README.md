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
| [doc/](doc/) | Architecture, transport, Cavalry virtualization, and EVE guides |
| [automation/doc/](automation/doc/) | Release notes, issues, and private kernel module distribution guide |

## Building & Targets

The top-level `Makefile` unifies EVE BaseOS image generation with kernel header
extraction and out-of-tree driver compilation and signing:

```bash
# Build full EVE BaseOS live image and out-of-tree drivers (default)
make

# Build EVE BaseOS image (automatically builds eve-kernel, headers, and drivers)
make eve

# Build and sign all out-of-tree drivers found under drivers/
make drivers

# Build EVE kernel package via LinuxKit
make eve-kernel

# Extract linux-headers and module signing keys from LinuxKit cache
make eve-kernel-headers

# Clean build artifacts and staging directories
make clean

# Deep clean including extracted kernel headers and caches
make distclean
```

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

## Documentation

- [doc/Architecture.md](doc/Architecture.md) — EVE architecture and device assignment.
- [doc/CavalryVirtualization.md](doc/CavalryVirtualization.md) — Cavalry architecture and memory management.
- [doc/EVE-BaseOS-AmbarellaDrivers.md](doc/EVE-BaseOS-AmbarellaDrivers.md) — Ambarella drivers in EVE BaseOS.
- [doc/Native-ivshmem-Support-in-EVE-BaseOS.md](doc/Native-ivshmem-Support-in-EVE-BaseOS.md) — ivshmem support in EVE BaseOS.
- [automation/doc/Issues.md](automation/doc/Issues.md) — Known issues, tracking, and upstream integration notes.
- [automation/doc/PrivateKernelModuleRelease.md](automation/doc/PrivateKernelModuleRelease.md) — Distributing private out-of-tree kernel modules without source.
