# Lean Debian 13 (trixie) AArch64 HVM Image Builder

Builds, publishes and deploys the lean Debian 13 AArch64 HVM guest image for Ambarella EVE edge nodes
(`n1-655-pro`). It follows the recipe of the lean Ubuntu image (`guest-os/ubuntu/ubuntu-build/`).

## Design Contract

- **Base image**: the pinned Debian 13 `genericcloud` ARM64 image `20261001-2618` (its cloud kernel is replaced during provisioning; see Kernel),
  pinned and verified by SHA-512 in `base-image.lock`. A file in `cache/` with the right checksum is used without downloading.
- **Method**: boot the base image once under QEMU/TCG with a NoCloud seed that runs `assets/provision-image.sh`
  (user `debian` / `debian`, netplan on `en*`, root growth service, journald caps, package purge with a simulation guard,
  no cloud-init), then boot it again without the seed and check it (serial login, SSH login, package assertions).
- **Kernel**: the generic Debian kernel `6.12.111+deb13-arm64` is installed and the cloud kernel removed, because the cloud kernel has no dmaengine
  core (`CONFIG_DMADEVICES` is not set) and `amba_dma.ko` cannot link against it. Guest drivers are built against `linux-headers-6.12.111+deb13-arm64`.
- **Console**: serial `ttyAMA0` 115200 baud and `tty0`; `quiet`; GRUB timeout 0.
- **Output**: `output/dist/debian-13-arm64-cloudimg.qcow2`, its `.sha256` and `build-manifest.json`.

## Usage

```bash
make guest-debian-image        # build the image (hours under TCG)
make guest-debian              # build the guest kernel modules and the client
make guest-debian-install      # stage the image, register it and the edge-app, create the instance (needs ZCLI_TOKEN)
```

Flags of `build.sh`: `--clean`, `--dry-run`. Flags of `deploy.sh`: `--datastore=NAME`, `--skip-instance`, `--dry-run`.

After the instance is up, deploy the guest drivers and the demo service:

```bash
./guest-os/deploy_guest.sh n1-655-pro-debian --provision --reload --test
```

## Names

| Item | Value |
|---|---|
| Edge-app | `debian_13` |
| Instance | `debian_13.n1-655-devkit`, `debian_13.n1-655-pro` |
| Image in ZedControl | `debian-13-trixie-arm64-hvm` |
| Accelerator memory | the admission ticket `amba_shm:amba_shm2` (`/dev/amba_virt_shm2`, slice `2`, `camera=1`); the server grants the slices and attaches the camera windows by the ticket's camera policy |
| SSH | `ssh debian@<node> -p 2522` (alias `n1-655-devkit-debian` / `n1-655-pro-debian`), password `debian` |
| Demo | port 8082 |

Full from-zero provisioning recipe for all six guests: [doc/EVE-EdgeApp-Instances.md](../../../doc/EVE-EdgeApp-Instances.md).
