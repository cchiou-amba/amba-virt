# Lean Ubuntu 24.04 AArch64 HVM Image Builder

This directory contains the builder and deployment pipeline for the lean Ubuntu 24.04 LTS (Noble) AArch64 HVM guest image for Ambarella EVE edge nodes (`n1-655-pro` and `n1-655-devkit`).

## Architecture & Design Contract

- **Base Image**: Canonical Ubuntu 24.04 LTS ARM64 Server Cloud Image, pinned and verified via `base-image.lock`.
- **Target Mode**: EVE AArch64 KVM HVM (`HV_HVM`).
- **Networking**: Netplan DHCP on `en*` with MAC-based DUID (`dhcp-identifier: mac`).
- **Console**: Serial `ttyAMA0` (115200 baud) and graphical `tty0`.
- **Storage**: Deterministic UEFI fallback (`/boot/efi/EFI/BOOT/BOOTAA64.EFI`) with automatic rootfs expansion on boot via `grow-rootfs.service`.
- **Authentication**: Pre-configured user `ubuntu` with password `ubuntu` and passwordless sudo. Cloud-init is removed.
- **Logging**: Journald persistent runtime limits capped at 16 MiB (`rsyslog` removed).
- **Module Compatibility**: Retains Canonical ARM64 generic kernel, module trees, and header matching for Ambarella virtualization drivers (`amba_virt`, `ambarella-gdma`, `amba_cavalry`).

## Prerequisites

1. **Host Utilities**:
   - `bootstrap-host.sh` automatically installs declared prerequisites on Ubuntu/Debian:
     `qemu-system-arm`, `qemu-efi-aarch64`, `qemu-utils`, `cloud-image-utils`, `genisoimage`, `python3`, `python3-pexpect`, `openssh-client`, `sshpass`, `curl`, `gnupg`, `ca-certificates`.
2. **Build Architecture**:
   - Uses `qemu-system-aarch64 -machine virt,accel=tcg -cpu max` for full system ARM emulation. No host KVM, host kernel, or container privileges required.
3. **Deployment Credentials**:
   - `ZCLI_TOKEN` exported in environment.

## Usage

### 1. Build Lean QCOW2 Image

```bash
# Via top-level Makefile:
make guest-ubuntu-image

# Or directly:
./guest-os/ubuntu/ubuntu-build/build.sh
```

Supported flags:
- `--clean`: Remove `cache/` and `output/` before building.
- `--dry-run`: Validate prerequisites, tools, and lock file without modifying images.
- `-h, --help`: Display usage.

Artifacts are produced in:
```text
guest-os/ubuntu/ubuntu-build/output/dist/
├── ubuntu-24.04-arm64-cloudimg.qcow2
├── ubuntu-24.04-arm64-cloudimg.qcow2.sha256
└── build-manifest.json
```

### 2. Publish & Deploy to Target Boards

```bash
# Via top-level Makefile:
make guest-ubuntu-install

# Or directly:
./guest-os/ubuntu/ubuntu-build/deploy.sh
```

Supported flags:
- `--dry-run`: Display all staging, registration, and instance creation commands without mutating cloud or boards.
- `--datastore=NAME`: Target HTTP datastore in ZedControl (auto-detected if omitted).
- `-h, --help`: Display usage.
