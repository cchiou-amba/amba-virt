# QNX 8.0 AArch64 HVM Image Build Workspace

This workspace builds the bootable QNX Neutrino RTOS 8.0 disk image for EVE-OS on Ambarella AArch64 platforms (e.g. N1-655 Devkit).

## Directory Layout

- `build.sh`: Master build script. Checks SDP patches, invokes `mkqnximage`, and converts raw disk to compressed QCOW2.
- `local/`: `mkqnximage` workspace configuration:
  - `options`: Build options (AArch64, UEFI boot, GPT disk layout, users, daemons).
  - `snippets/`: Custom startup scripts and files included into the build:
    - `startup_postpci.custom`: Starts `devc-virtio`, attaches root shell to `/dev/vcon1` for direct `tio` console connection.
    - `ifs_start.custom`: UEFI kernel initialization.
    - `system_files.custom` / `ifs_files.custom`: Extra system binaries and libraries.
- `patches/`: Binary patch helper for QNX SDP 8.0:
  - `apply_sdp_patches.py`: Automatically inspects and patches the host's QNX SDP 8.0 binaries (`devb-virtio`, `pci_hw-fdt.so.3.0`, and `mkqnximage`) dynamically in-place.
  - `0001-mkqnximage-qemu-random-entropy.patch`: Unified diff for `opt_scripts/qemu`.
- `output/`: Generated build artifacts (ignored by git):
  - `disk-qemu`: Raw GPT disk image.
  - `ifs.bin`: Bootable QNX Image Filesystem.
  - `dist/qnx-8.0-arm64-cloudimg.qcow2`: Compressed QCOW2 image ready for uplink to ZedControl.

## Quick Start

```bash
# 1. Source QNX SDP 8.0 environment
source ~/qnx800/qnxsdp-env.sh

# 2. Run the build script
./build.sh

# 3. Uplink generated QCOW2 to ZedControl
../amba-virt/scripts/zcli -- image uplink qnx-8.0-arm64-cloudimg \
  --image-sha="<SHA256>" \
  --image-size="<SIZE>"
```

## Connecting to the Guest

Once deployed on an EVE node (e.g. target node at `<target-node-ip>`):

```bash
# SSH over LAN via forwarded port 2322 (password: root, or using SSH key)
ssh -p 2322 root@<target-node-ip>

# Serial / VirtIO console access via EVE
eve enter debug "eve app console"
```
