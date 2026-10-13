# Lean Fedora 44 AArch64 HVM Image Builder

Builds, publishes and deploys the lean Fedora 44 AArch64 HVM guest image for Ambarella EVE edge nodes
(`n1-655-devkit`). It follows the recipe of the lean Ubuntu image (`guest-os/ubuntu/ubuntu-build/`).

## Design Contract

- **Base image**: the pinned Fedora 44 Cloud Base `Generic` aarch64 image `44-1.7` (kernel `6.19.10-300.fc44.aarch64`, btrfs root with
  zstd compression and the subvolumes `root`, `boot`, `home`, `var`), pinned and verified by SHA-256 in `base-image.lock`.
  A file in `cache/` with the right checksum is used without downloading.
- **Method**: boot the base image once under QEMU/TCG with a NoCloud seed that runs `assets/provision-image.sh`
  (user `fedora` / `fedora`, systemd-networkd instead of NetworkManager, btrfs root growth service, journald caps,
  package removal with a simulation guard, no cloud-init, SELinux left enforcing), then boot it again without the seed and
  check it (serial login, SSH login, package, SELinux and filesystem assertions).
- **Kernel**: the Fedora kernel is kept. Guest drivers are built against `kernel-devel-6.19.10-300.fc44`.
- **Console**: serial `ttyAMA0` 115200 baud and `tty1`; `quiet`; GRUB timeout 0.
- **Output**: `output/dist/fedora-44-aarch64-cloudimg.qcow2`, its `.sha256` and `build-manifest.json`.

## Usage

```bash
make guest-fedora-image        # build the image (hours under TCG)
make guest-fedora              # build the guest kernel modules and the client
make guest-fedora-install      # stage the image, register it and the edge-app, create the instance (needs ZCLI_TOKEN)
```

Flags of `build.sh`: `--clean`, `--dry-run`. Flags of `deploy.sh`: `--datastore=NAME`, `--skip-instance`, `--dry-run`.

After the instance is up, deploy the guest drivers and the demo service:

```bash
./guest-os/deploy_guest.sh n1-655-devkit-fedora --provision --reload --test
```

## Names

| Item | Value |
|---|---|
| Edge-app | `fedora_44` |
| Instance | `fedora_44.n1-655-devkit` |
| Image in ZedControl | `fedora-44-aarch64-hvm` |
| Accelerator memory | the admission ticket `amba_shm:amba_shm5`; the server grants the slices and attaches the camera windows by the ticket's camera policy |
| SSH | `ssh fedora@<node> -p 2622` (alias `n1-655-devkit-fedora`), password `fedora` |
| Demo | port 8083 |
