# Provisioning All Six Edge-App Instances from Zero on EVE

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

*Copyright (C) 2026, Ambarella International LLC*

This runbook gives the complete, from-zero provisioning recipe for all six HVM edge applications (`ubuntu_24_04`, `alpine_3_20`, `debian_13`, `fedora_44`, `qnx_8_0`, and `windows_11_arm64`) on `n1-655-devkit` and `n1-655-pro`, the pinned `0..5` slice and adapter mapping, the deletion and ordering rules that make ZedControl admission tickets and Dom0 `/dev/amba_virt_shm0..5` slices agree, and the rollback procedure to revert an EVE BaseOS image update.

Related documents:
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (hardware models, `amba_shm0..7` tickets, `UART1..4`, `rng-mmio`)
- [EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md) (why edge-apps and instances must be created with adapters at birth)
- [EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md) (Ubuntu 24.04 cloud-init reference)
- [QNX-Guest-VM.md](QNX-Guest-VM.md) (QNX 8.0 HVM guest build, stage, manifest, and deployment guide)
- [EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md) (building, publishing, updating, and reverting EVE BaseOS firmware)
- [UART-Passthrough.md](UART-Passthrough.md) (hardware UART2/UART3 passthrough with IRQ and Generic-DMA1 DMA)

---

## 1. Pinned Slice, Ticket, and Console Mapping (`0..5`)

Both `n1-655-devkit` and `n1-655-pro` enforce the same deterministic `0..5` assignment. Creating the six instances in this order on a clean board binds each admission ticket `amba_shm<N>` to the matching Dom0 device `/dev/amba_virt_shm<N>`:

```text
0: ubuntu_24_04.<node>      -> amba_shm0 (/dev/amba_virt_shm0, camera=1)  + UART2 -> console1
1: alpine_3_20.<node>       -> amba_shm1 (/dev/amba_virt_shm1, camera=1)  + UART3 -> console2
2: debian_13.<node>         -> amba_shm2 (/dev/amba_virt_shm2, camera=1)
3: fedora_44.<node>         -> amba_shm3 (/dev/amba_virt_shm3, camera=1)
4: qnx_8_0.<node>           -> amba_shm4 + rng-mmio (/dev/amba_virt_shm4, camera=0)
5: windows_11_arm64.<node>  -> amba_shm5 (/dev/amba_virt_shm5, camera=0)
```

| Slice | Edge-App | Instance (`<node>`) | Ticket (`--adapter`) | Extra Adapters | Camera | Host Port -> Guest | SSH Alias / Console |
|---|---|---|---|---|---|---|---|
| `0` | `ubuntu_24_04` | `ubuntu_24_04.<node>` | `amba_shm:amba_shm0` | `UART2:UART2` | `on` (`1`) | SSH `2222->22`, Demo `8080->8080` | `<node>-ubuntu`, `console1` (devkit `6071`, pro `6061`) |
| `1` | `alpine_3_20` | `alpine_3_20.<node>` | `amba_shm:amba_shm1` | `COM3:UART3` | `on` (`1`) | SSH `2422->22`, Demo `8081->8081` | `<node>-alpine`, `console2` (devkit `6072`, pro `6062`) |
| `2` | `debian_13` | `debian_13.<node>` | `amba_shm:amba_shm2` | *(none)* | `on` (`1`) | SSH `2522->22`, Demo `8082->8082` | `<node>-debian` |
| `3` | `fedora_44` | `fedora_44.<node>` | `amba_shm:amba_shm3` | *(none)* | `on` (`1`) | SSH `2622->22`, Demo `8083->8083` | `<node>-fedora` |
| `4` | `qnx_8_0` | `qnx_8_0.<node>` | `amba_shm:amba_shm4` | `rng-mmio:rng-mmio` | `off` (`0`) | SSH `2322->22` | `<node>-qnx` |
| `5` | `windows_11_arm64` | `windows_11_arm64.<node>` | `amba_shm:amba_shm5` | *(none)* | `off` (`0`) | RDP `3389->3389`, VNC | RDP `:3389` (`windows` / `windows`) |

Slices `6` and `7` (`amba_shm6`, `amba_shm7`) remain unallocated spare tickets (`camera=0`).

---

## 2. Controller Rules: Deleting Instances, Pushing Models, and Ticket Ordering

1. **What the Controller Refuses While an Adapter Ticket Is Held**:
   - ZedControl enforces one instance per `assigngrp` (including `Halted` / `Inactive` instances). Two instances on the same node cannot hold the same `amba_shm<N>`, `UART2`, `UART3`, or `rng-mmio` adapter simultaneously.
   - ZedControl also refuses `model update` ([scripts/push_models.sh](../scripts/push_models.sh)) if any existing instance on a node using that model holds an adapter that is renamed or removed by the model update.
   - On the node, modifying or reordering `ioMemberList` adapters while instances are active causes `domainmgr.releaseAdapters()` to panic. Always delete all instances on the node before pushing a model change.

2. **How `amba_shm<N>` Tickets and Dom0 `/dev/amba_virt_shm<N>` Slices Agree**:
   - `amba_shm0` emits `RESERVE_SLICE_AT <uuid> <bytes> 0 amba_shm0 <intent>`, which always reserves Dom0 slice `0` (`/dev/amba_virt_shm0`).
   - `amba_shm1..7` emit `RESERVE_SLICE_MIN <uuid> <bytes> 1 amba_shm<N> <intent>`, which allocates the lowest free Dom0 slice index `>= 1`.
   - Therefore, when provisioning from zero (after deleting existing instances so slices `0..7` are all free in `amba-virt-ctl pools`), creating the instances strictly in the `0..5` order (`ubuntu_24_04`, `alpine_3_20`, `debian_13`, `fedora_44`, `qnx_8_0`, `windows_11_arm64`) guarantees that `amba_shm<N>` is backed by `/dev/amba_virt_shm<N>` for every `N` in `0..5`.

3. **Teardown & Model Push Sequence Before Clean Provisioning**:
   ```bash
   export ZCLI_TOKEN="<your-token>"

   # 1. Inspect current instances on the target node (n1-655-devkit or n1-655-pro)
   ./scripts/show_instances.sh --edge-node=n1-655-devkit

   # 2. Delete all six instances on the node
   ./scripts/zcli -- edge-app-instance delete ubuntu_24_04.n1-655-devkit -f
   ./scripts/zcli -- edge-app-instance delete alpine_3_20.n1-655-devkit -f
   ./scripts/zcli -- edge-app-instance delete debian_13.n1-655-devkit -f
   ./scripts/zcli -- edge-app-instance delete fedora_44.n1-655-devkit -f
   ./scripts/zcli -- edge-app-instance delete qnx_8_0.n1-655-devkit -f
   ./scripts/zcli -- edge-app-instance delete windows_11_arm64.n1-655-devkit -f

   # 3. Push hardware models after instances are deleted
   ./scripts/push_models.sh
   ```

---

## 3. Per-Application Provisioning Recipes (Slice `0` to `5`)

### 3.1 Slice 0: Ubuntu 24.04 (`ubuntu_24_04`) — `amba_shm0` + `UART2` (`console1`)

- **Image Source & Build**:
  Built from the pinned Ubuntu 24.04 ARM64 cloud image via [guest-os/ubuntu/ubuntu-build/README.md](../guest-os/ubuntu/ubuntu-build/README.md) (`user: ubuntu`, `password: ubuntu`, cloud-init removed, serial `ttyAMA0` + passthrough `ttyAMBA0`):
  ```bash
  make guest-ubuntu-image
  make guest-ubuntu
  ```
  Output: `guest-os/ubuntu/ubuntu-build/output/dist/ubuntu-24.04-arm64-cloudimg.qcow2` and `build/guest/ubuntu/*.ko`.
- **Datastore & Image Registration**:
  ```bash
  ./guest-os/ubuntu/ubuntu-build/deploy.sh --datastore=LocalHTTP --dry-run
  ./scripts/zcli -- image uplink ubuntu-24.04-server-cloudimg-arm64 \
    --datastore-name=LocalHTTP \
    --image-url=ubuntu-24.04-arm64-cloudimg.qcow2 \
    --image-sha="<SHA256>" \
    --image-size="<BYTES>"
  ```
- **Edge-App Manifest (`apps/ubuntu_24_04.json`)**:
  Lists `eth0` (`2222->22`, `8080->8080`), `amba_shm`, and `UART1..UART4` ([apps/ubuntu_24_04.json](../apps/ubuntu_24_04.json)):
  ```bash
  ./scripts/create_app.sh ubuntu_24_04 --version=1.0
  # Or if updating an existing app with zero instances:
  ./scripts/push_app.sh ubuntu_24_04
  ```
- **Instance Creation (Slice `0` + `UART2`)**:
  ```bash
  ./scripts/create_instance.sh ubuntu_24_04.n1-655-devkit \
    --edge-app=ubuntu_24_04 \
    --edge-node=n1-655-devkit \
    --network-instance=eth0:defaultLocal-n1-655-devkit \
    --adapter=amba_shm:amba_shm0 \
    --adapter=UART2:UART2 \
    --no-custom-configuration
  ```
- **Guest Driver & Demo Provisioning**:
  ```bash
  ./guest-os/deploy_guest.sh n1-655-devkit-ubuntu --provision --reload --enable-serial --test
  ```
- **Reachability**:
  - SSH: `ssh n1-655-devkit-ubuntu` (`ssh ubuntu@<node-ip> -p 2222`, password `ubuntu`).
  - Physical Console (`UART2`): `console1` via `embdevenv` (`6071` on devkit, `6061` on pro; guest device `/dev/ttyAMBA0` at `115200 8N1`).

---

### 3.2 Slice 1: Alpine Linux 3.20 (`alpine_3_20`) — `amba_shm1` + `UART3` (`console2`)

- **Image Source & Build**:
  Uses the Alpine 3.20 AArch64 QCOW2 image (`cloud-images/alpine-3.20-aarch64.qcow2`, registered as `alpine-3.20-arm64-qcow2`) and out-of-tree kernel modules + static musl binaries built by [guest-os/alpine/README.md](../guest-os/alpine/README.md):
  ```bash
  make guest-alpine
  ```
  Output: `build/guest/alpine/{amba_virt.ko,amba_dma.ko,amba_uart.ko,amba_cavalry.ko,iav_virt.ko,amba-virt-client}`.
- **Datastore & Image Registration**:
  ```bash
  ./scripts/zcli -- image create alpine-3.20-arm64-qcow2 \
    --title="Alpine 3.20 ARM64 QCOW2" \
    --type=Application \
    --image-format=qcow2 \
    --arch=ARM64 \
    --datastore-name=LocalHTTP \
    --image-url=cloud-images/alpine-3.20-aarch64.qcow2
  ./scripts/zcli -- image uplink alpine-3.20-arm64-qcow2 \
    --datastore-name=LocalHTTP \
    --image-url=cloud-images/alpine-3.20-aarch64.qcow2 \
    --image-sha="<SHA256>" \
    --image-size="<BYTES>"
  ```
- **Edge-App Manifest (`apps/alpine_3_20.json`)**:
  Lists `eth0` (`2422->22`, `8081->8081`), `amba_shm`, `COM1`, `COM2`, and `COM3` ([apps/alpine_3_20.json](../apps/alpine_3_20.json)):
  ```bash
  ./scripts/create_app.sh alpine_3_20 --version=1.0
  # Or if updating an existing app with zero instances:
  ./scripts/push_app.sh alpine_3_20
  ```
- **Instance Creation (Slice `1` + template slot `COM3` assigned to model adapter `UART3`)**:
  ```bash
  ./scripts/create_instance.sh alpine_3_20.n1-655-devkit \
    --edge-app=alpine_3_20 \
    --edge-node=n1-655-devkit \
    --network-instance=eth0:defaultLocal-n1-655-devkit \
    --adapter=amba_shm:amba_shm1 \
    --adapter=COM3:UART3 \
    --no-custom-configuration
  ```
- **Guest Driver & Demo Provisioning**:
  ```bash
  ./guest-os/deploy_guest.sh n1-655-devkit-alpine --provision --reload --enable-serial --test
  ```
- **Reachability**:
  - SSH: `ssh n1-655-devkit-alpine` (`ssh root@<node-ip> -p 2422` or `alpine@<node-ip> -p 2422`, password `alpine`).
  - Physical Console (`UART3`): `console2` via `embdevenv` (`6072` on devkit, `6062` on pro; OpenRC `/etc/inittab` getty on `/dev/ttyAMBA0` at `115200 8N1`).

---

### 3.3 Slice 2: Debian 13 (`debian_13`) — `amba_shm2`

- **Image Source & Build**:
  Built from the pinned Debian 13 (trixie) ARM64 image with kernel `6.12.111+deb13-arm64` via [guest-os/debian/debian-build/README.md](../guest-os/debian/debian-build/README.md):
  ```bash
  make guest-debian-image
  make guest-debian
  ```
  Output: `guest-os/debian/debian-build/output/dist/debian-13-arm64-cloudimg.qcow2` and `build/guest/debian/*.ko`.
- **Datastore & Image Registration**:
  ```bash
  ./guest-os/debian/debian-build/deploy.sh --datastore=LocalHTTP --skip-instance --dry-run
  ./scripts/zcli -- image uplink debian-13-trixie-arm64-hvm \
    --datastore-name=LocalHTTP \
    --image-url=debian-13-arm64-cloudimg.qcow2 \
    --image-sha="<SHA256>" \
    --image-size="<BYTES>"
  ```
- **Edge-App Manifest (`apps/debian_13.json`)**:
  Lists `eth0` (`2522->22`, `8082->8082`), `amba_shm`, and `UART1..UART4` ([apps/debian_13.json](../apps/debian_13.json)):
  ```bash
  ./scripts/create_app.sh debian_13 --version=1.0
  # Or if updating an existing app with zero instances:
  ./scripts/push_app.sh debian_13
  ```
- **Instance Creation (Slice `2`)**:
  ```bash
  ./scripts/create_instance.sh debian_13.n1-655-devkit \
    --edge-app=debian_13 \
    --edge-node=n1-655-devkit \
    --network-instance=eth0:defaultLocal-n1-655-devkit \
    --adapter=amba_shm:amba_shm2 \
    --no-custom-configuration
  ```
- **Guest Driver & Demo Provisioning**:
  ```bash
  ./guest-os/deploy_guest.sh n1-655-devkit-debian --provision --reload --test
  ```
- **Reachability**:
  - SSH: `ssh n1-655-devkit-debian` (`ssh debian@<node-ip> -p 2522`, password `debian`).

---

### 3.4 Slice 3: Fedora 44 (`fedora_44`) — `amba_shm3`

- **Image Source & Build**:
  Built from the pinned Fedora 44 Cloud Base `aarch64` image (`6.19.10-300.fc44.aarch64`) via [guest-os/fedora/fedora-build/README.md](../guest-os/fedora/fedora-build/README.md):
  ```bash
  make guest-fedora-image
  make guest-fedora
  ```
  Output: `guest-os/fedora/fedora-build/output/dist/fedora-44-aarch64-cloudimg.qcow2` and `build/guest/fedora/*.ko`.
- **Datastore & Image Registration**:
  ```bash
  ./guest-os/fedora/fedora-build/deploy.sh --datastore=LocalHTTP --skip-instance --dry-run
  ./scripts/zcli -- image uplink fedora-44-aarch64-hvm \
    --datastore-name=LocalHTTP \
    --image-url=fedora-44-aarch64-cloudimg.qcow2 \
    --image-sha="<SHA256>" \
    --image-size="<BYTES>"
  ```
- **Edge-App Manifest (`apps/fedora_44.json`)**:
  Lists `eth0` (`2622->22`, `8083->8083`), `amba_shm`, and `UART1..UART4` ([apps/fedora_44.json](../apps/fedora_44.json)):
  ```bash
  ./scripts/create_app.sh fedora_44 --version=1.0
  # Or if updating an existing app with zero instances:
  ./scripts/push_app.sh fedora_44
  ```
- **Instance Creation (Slice `3`)**:
  ```bash
  ./scripts/create_instance.sh fedora_44.n1-655-devkit \
    --edge-app=fedora_44 \
    --edge-node=n1-655-devkit \
    --network-instance=eth0:defaultLocal-n1-655-devkit \
    --adapter=amba_shm:amba_shm3 \
    --no-custom-configuration
  ```
- **Guest Driver & Demo Provisioning**:
  ```bash
  ./guest-os/deploy_guest.sh n1-655-devkit-fedora --provision --reload --test
  ```
- **Reachability**:
  - SSH: `ssh n1-655-devkit-fedora` (`ssh fedora@<node-ip> -p 2622`, password `fedora`).

---

### 3.5 Slice 4: BlackBerry QNX 8.0 (`qnx_8_0`) — `amba_shm4` + `rng-mmio` (No UART)

- **Image Source & Build**:
  Built with QNX SDP 8.0 (`mkqnximage`) via [QNX-Guest-VM.md](QNX-Guest-VM.md) and [guest-os/qnx/qnx-build/README.md](../guest-os/qnx/qnx-build/README.md):
  ```bash
  make guest-qnx
  make guest-qnx-image
  ```
  Output: `guest-os/qnx/qnx-build/output/dist/qnx-8.0-arm64-cloudimg.qcow2` and `build/guest/qnx/{amba-virt-resmgr,amba-cavalry-resmgr,amba-virt-client}`.
- **Datastore & Image Registration**:
  ```bash
  cp -f guest-os/qnx/qnx-build/output/dist/qnx-8.0-arm64-cloudimg.qcow2 ~/public_html/cloud-images/qnx-8.0-arm64-cloudimg.qcow2
  ./scripts/zcli -- image uplink qnx-8.0-arm64-cloudimg \
    --datastore-name=LocalHTTP \
    --image-url=cloud-images/qnx-8.0-arm64-cloudimg.qcow2 \
    --image-sha="<SHA256>" \
    --image-size="<BYTES>"
  ```
- **Edge-App Manifest (`apps/qnx_8_0.json`)**:
  Lists `eth0` (`2322->22`), `amba_shm`, `UART1..UART4`, and `rng-mmio` ([apps/qnx_8_0.json](../apps/qnx_8_0.json)):
  ```bash
  ./scripts/create_app.sh qnx_8_0 --version=1.0
  # Or if updating an existing app with zero instances:
  ./scripts/push_app.sh qnx_8_0
  ```
- **Instance Creation (Slice `4` + `rng-mmio`, No UART)**:
  ```bash
  ./scripts/create_instance.sh qnx_8_0.n1-655-devkit \
    --edge-app=qnx_8_0 \
    --edge-node=n1-655-devkit \
    --network-instance=eth0:defaultLocal-n1-655-devkit \
    --adapter=amba_shm:amba_shm4 \
    --adapter=rng-mmio:rng-mmio \
    --no-custom-configuration
  ```
- **Guest Resource Manager Deployment**:
  ```bash
  ./guest-os/deploy_guest.sh n1-655-devkit-qnx --reload --test
  ```
- **Reachability**:
  - SSH: `ssh n1-655-devkit-qnx` (`ssh root@<node-ip> -p 2322`, password `root`).

---

### 3.6 Slice 5: Windows 11 IoT Enterprise ARM64 (`windows_11_arm64`) — `amba_shm5`

- **Image Source & Build**:
  Built from the Windows 11 IoT Enterprise LTSC ARM64 ISO and `virtio-win.iso` via [guest-os/windows/README.md](../guest-os/windows/README.md):
  ```bash
  make guest-windows
  ```
  Output: `guest-os/windows/windows-build/output/dist/windows-11-arm64-cloudimg.qcow2`.
- **Datastore & Image Registration**:
  ```bash
  cp -f guest-os/windows/windows-build/output/dist/windows-11-arm64-cloudimg.qcow2 ~/public_html/cloud-images/windows-11-arm64-cloudimg.qcow2
  ./scripts/zcli -- image create windows-11-arm64-cloudimg \
    --title="Windows 11 IoT Enterprise ARM64" \
    --type=Application \
    --image-format=qcow2 \
    --arch=ARM64 \
    --datastore-name=LocalHTTP \
    --image-url=cloud-images/windows-11-arm64-cloudimg.qcow2
  ./scripts/zcli -- image uplink windows-11-arm64-cloudimg \
    --datastore-name=LocalHTTP \
    --image-url=cloud-images/windows-11-arm64-cloudimg.qcow2 \
    --image-sha="<SHA256>" \
    --image-size="<BYTES>"
  ```
- **Edge-App Manifest (`apps/windows_11_arm64.json`)**:
  Lists `eth0` (`3389->3389`), `amba_shm`, and `UART1..UART4` ([apps/windows_11_arm64.json](../apps/windows_11_arm64.json)):
  ```bash
  ./scripts/create_app.sh windows_11_arm64 --version=1.0
  # Or if updating an existing app with zero instances:
  ./scripts/push_app.sh windows_11_arm64
  ```
- **Instance Creation (Slice `5`)**:
  ```bash
  ./scripts/create_instance.sh windows_11_arm64.n1-655-devkit \
    --edge-app=windows_11_arm64 \
    --edge-node=n1-655-devkit \
    --network-instance=eth0:defaultLocal-n1-655-devkit \
    --adapter=amba_shm:amba_shm5 \
    --no-custom-configuration
  ```
- **Reachability**:
  - RDP: `<node-ip>:3389` (user `windows`, password `windows`) or VNC console.

---

## 4. Expected Dom0 State After All Six Instances Start

After creating `0..5` in order on `<node>` (`n1-655-devkit` or `n1-655-pro`), verify on the workstation and on Dom0:

```bash
# 1. Controller instance summary and adapter assignments
./scripts/show_instances.sh --edge-node=n1-655-devkit

# 2. Dom0 slice pool and active tenant leases (slices 0..5 leased, 6..7 free)
ssh n1-655-devkit "/hostfs/usr/bin/amba-virt-ctl pools; /hostfs/usr/bin/amba-virt-ctl slice-leases"

# 3. QEMU domain configs (/dev/amba_virt_shm0..5 and vfio-platform on UART2/UART3)
ssh n1-655-devkit "grep -H -E 'domain_name|amba_virt_shm|ffe001[89]000\.uart|amba_dma_lease' /run/hypervisor/kvm/*/xen*.cfg"
```

Expected state:
- `/dev/amba_virt_shm0`..`/dev/amba_virt_shm5` are leased in `0..5` order (`ubuntu=0, alpine=1, debian=2, fedora=3, qnx=4, windows=5`); `/dev/amba_virt_shm6` and `/dev/amba_virt_shm7` are free.
- `ffe0018000.uart` (`UART2`) is bound to `vfio-platform` and attached to `ubuntu_24_04.<node>` with a `/dev/amba_dma_lease*` window.
- `ffe0019000.uart` (`UART3`) is bound to `vfio-platform` and attached to `alpine_3_20.<node>` with a `/dev/amba_dma_lease*` window.
- `server.log` (`/run/amba-virt/server.log`) shows `registered tenant` entries for slices `0..4` (`ubuntu`, `alpine`, `debian`, `fedora`, and `qnx` with `off=0x100000000`).

---

## 5. Reverting an EVE BaseOS Image Update (Rollback Target)

The pre-VFIO known-good rollback image recorded from `n1-655-devkit` and `n1-655-pro` is:
- **Image Name**: `0.0.0-ambarella-eve-ffb9f9ba-k-arm64-v6.1.112-generic-b00cd5423ca9-custom-gcc-kvm-arm64`
- **Datastore Relative URL**: `eve-images/0.0.0-ambarella-eve-ffb9f9ba-k-arm64-v6.1.112-generic-b00cd5423ca9-custom-gcc-kvm-arm64/rootfs.img`
- **SHA-256**: `483fcdd4f84f8d4340e720d8f0b174ea856ccb7bdf3fd3f5ab6761efd3b1ae0b`
- **Size**: `387268608` bytes

Verify that the rollback image record and staged file are present before updating a node, and revert to it if needed (full details in [EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md#9-reverting-to-the-previous-eve-image-rollback)):

```bash
ROLLBACK_IMG="0.0.0-ambarella-eve-ffb9f9ba-k-arm64-v6.1.112-generic-b00cd5423ca9-custom-gcc-kvm-arm64"

# 1. Verify the rollback image is registered in ZedControl and staged on disk
./scripts/zcli -- image show "$ROLLBACK_IMG"
ls -lh ~/public_html/eve-images/"$ROLLBACK_IMG"/rootfs.img

# 2. Revert the node in ZedControl (two-step stage + activate)
./scripts/zcli -- edge-node eveimage-update n1-655-devkit --image="$ROLLBACK_IMG"
./scripts/zcli -- edge-node eveimage-update n1-655-devkit --image="$ROLLBACK_IMG" --activate
```
