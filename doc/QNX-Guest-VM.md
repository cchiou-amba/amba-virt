# BlackBerry QNX Neutrino 8.0 AArch64 HVM Guest on EVE

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

*Copyright (C) 2026, Ambarella International LLC*

This guide covers building the BlackBerry QNX Neutrino RTOS 8.0 AArch64 HVM guest image and user-space resource managers from the repository, staging and registering the QCOW2 image in ZedControl, creating the `qnx_8_0` edge-app manifest, instantiating `qnx_8_0.<node>` on `amba_shm4` (`/dev/amba_virt_shm4`) with `rng-mmio`, verifying tenant registration on slice `4` (`off=0x100000000`), and restoring the instance if recreated.

Related documents:
- [EVE-EdgeApp-Instances.md](EVE-EdgeApp-Instances.md) (pinned `0..5` mapping and per-app recipe for all six guests)
- [guest-os/qnx/qnx-build/README.md](../guest-os/qnx/qnx-build/README.md) (`mkqnximage` workspace layout and patches)
- [guest-os/README.md](../guest-os/README.md) (cross-compilation guide for all HVM guest operating systems)
- [CavalryVirtualization.md](CavalryVirtualization.md) (Cavalry NPU virtualization and slice claim protocol)

---

## 1. Names & Pinned Assignment

| Item | Value |
|---|---|
| **Edge-App** | `qnx_8_0` ([apps/qnx_8_0.json](../apps/qnx_8_0.json)) |
| **Instance** | `qnx_8_0.n1-655-devkit`, `qnx_8_0.n1-655-pro` |
| **Image in ZedControl** | `qnx-8.0-arm64-cloudimg` (`cloud-images/qnx-8.0-arm64-cloudimg.qcow2`) |
| **Adapters at Create** | `--adapter=amba_shm:amba_shm4 --adapter=rng-mmio:rng-mmio` (no UART passthrough; `UART2` is assigned to Ubuntu on `amba_shm0` and `UART3` to Alpine on `amba_shm1`) |
| **Dom0 Slice** | Slice `4` (`/dev/amba_virt_shm4`, `camera=0`, window offset `0x100000000`, claimed via `AMBA_VIRT_MSG_SLICE_CLAIM_REQ` by `amba-cavalry-resmgr`) |
| **SSH Access** | `ssh n1-655-devkit-qnx` / `ssh n1-655-pro-qnx` (`ssh root@<node-ip> -p 2322`, password `root`) |

---

## 2. Building QNX Resource Managers and the Bootable QCOW2 Image

1. **Ensure QNX SDP 8.0 is installed** (`~/qnx800/qnxsdp-env.sh` or `~/qnx/qnx800/qnxsdp-env.sh`).
2. **Build the QNX resource managers and client binaries** (`amba-virt-resmgr`, `amba-cavalry-resmgr`, `libamba_virt.so`, `amba-virt-client`, `amba-virt-cli`):
   ```bash
   make guest-qnx
   ```
3. **Build the bootable QNX 8.0 GPT/UEFI disk image and convert to compressed QCOW2**:
   ```bash
   make guest-qnx-image
   ```
   Or invoke the builder directly:
   ```bash
   ./guest-os/build_guest.sh --distro=qnx --qnx-image
   ```

Staged artifacts:
- `guest-os/qnx/qnx-build/output/dist/qnx-8.0-arm64-cloudimg.qcow2`
- `build/guest/qnx/amba-virt-resmgr` (`/dev/amba_virt` POSIX resource manager)
- `build/guest/qnx/amba-cavalry-resmgr` (`/dev/cavalry` POSIX resource manager; sends `AMBA_VIRT_MSG_SLICE_CLAIM_REQ` followed by `AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ` at startup)
- `build/guest/qnx/amba-virt-client` and `build/guest/qnx/amba-virt-cli`

---

## 3. Staging and Registering the Image in ZedControl

Copy the built QCOW2 image into the local HTTP datastore directory and uplink its SHA-256 and byte size in ZedControl:

```bash
export ZCLI_TOKEN="<your-token>"

QCOW2="guest-os/qnx/qnx-build/output/dist/qnx-8.0-arm64-cloudimg.qcow2"
IMG_SHA=$(sha256sum "$QCOW2" | awk '{print $1}')
IMG_SIZE=$(stat -c %s "$QCOW2")

cp -f "$QCOW2" ~/public_html/cloud-images/qnx-8.0-arm64-cloudimg.qcow2

# Create the image record if not already present:
./scripts/zcli -- image create qnx-8.0-arm64-cloudimg \
  --title="QNX 8.0 ARM64 HVM" \
  --type=Application \
  --image-format=qcow2 \
  --arch=ARM64 \
  --datastore-name=LocalHTTP \
  --image-url=cloud-images/qnx-8.0-arm64-cloudimg.qcow2

# Uplink checksum and size:
./scripts/zcli -- image uplink qnx-8.0-arm64-cloudimg \
  --datastore-name=LocalHTTP \
  --image-url=cloud-images/qnx-8.0-arm64-cloudimg.qcow2 \
  --image-sha="$IMG_SHA" \
  --image-size="$IMG_SIZE"
```

---

## 4. Creating or Updating the `qnx_8_0` Edge-App Manifest

The manifest [apps/qnx_8_0.json](../apps/qnx_8_0.json) defines `eth0` (forwarding host TCP `2322` to guest TCP `22`), `amba_shm`, and `rng-mmio`:

```bash
# Create the edge-app for the first time:
./scripts/create_app.sh qnx_8_0 --version=1.0

# Or push updates when zero instances of qnx_8_0 exist:
./scripts/push_app.sh qnx_8_0
```

---

## 5. Creating and Restoring the `qnx_8_0.<node>` Instance

Under the pinned `0..5` mapping ([EVE-EdgeApp-Instances.md](EVE-EdgeApp-Instances.md#1-pinned-slice-ticket-and-console-mapping-05)), `qnx_8_0.<node>` is created at index `4` (after `ubuntu_24_04`, `alpine_3_20`, `debian_13`, and `fedora_44`) with `--adapter=amba_shm:amba_shm4` and `--adapter=rng-mmio:rng-mmio`:

```bash
./scripts/create_instance.sh qnx_8_0.n1-655-devkit \
  --edge-app=qnx_8_0 \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=amba_shm:amba_shm4 \
  --adapter=rng-mmio:rng-mmio \
  --no-custom-configuration
```

Deploy or reload the QNX resource managers over SSH once the guest boots:

```bash
./guest-os/deploy_guest.sh n1-655-devkit-qnx --reload --test
```

### Restoring `qnx_8_0.<node>` After a Failed or Recreated Instance

If `qnx_8_0.<node>` must be deleted and recreated on a running board:
1. Delete the instance and wait for removal:
   ```bash
   ./scripts/zcli -- edge-app-instance delete qnx_8_0.n1-655-devkit -f
   ./scripts/show_instances.sh --edge-node=n1-655-devkit
   ```
2. Verify on Dom0 that slice `4` (`/dev/amba_virt_shm4`) is free in `amba-virt-ctl pools`, then run the `./scripts/create_instance.sh` command above.

---

## 6. Verifying QNX Guest and Dom0 Slice 4 State

```bash
# 1. Verify SSH login and running QNX resource managers inside the guest
ssh n1-655-devkit-qnx "uname -a; pidin | grep -E 'amba-virt-resmgr|amba-cavalry-resmgr'; ls -l /dev/amba_virt /dev/cavalry"

# 2. Verify Dom0 slice 4 lease and server tenant registration (off=0x100000000)
ssh n1-655-devkit "/hostfs/usr/bin/amba-virt-ctl slice-leases; grep 'idx=4' /run/amba-virt/server.log"
```
