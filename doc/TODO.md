# amba-virt Project TODOs

This document tracks upcoming tasks, implementation milestones, and operational action items for the `amba-virt` transport between the Ubuntu HVM guest and NOHYPER host container.

---

## 1. Native ivshmem Support in EVE BaseOS

Full technical specification: [Native-ivshmem-Support-in-EVE-BaseOS.md](Native-ivshmem-Support-in-EVE-BaseOS.md).

- [ ] **Hypervisor Template Updates (`eve/eve/pkg/pillar/hypervisor/kvm.go`)**:
  - [ ] Define `qemuIvshmemTemplate` and `tQemuIvshmemContext` for QEMU device model generation:
    - `[object "amba_shm"]` (`qom-type = memory-backend-file`, `mem-path = /dev/shm/amba-virt`, `size = 16M`, `share = on`)
    - `[device "amba-ivshmem"]` (`driver = ivshmem-plain`, `memdev = amba_shm`, `master = on`)
  - [ ] Parse `qemuIvshmemTemplate` into `tQemuIvshmem` during package initialization (`init()`).
  - [ ] Implement self-healing backing file allocation (`ensureSharedMemoryFile`) in `CreateDomConfig()` to ensure `/dev/shm/amba-virt` exists with 16MB and `0666` permissions before QEMU launches.
  - [ ] Execute `tQemuIvshmem.Execute(file, ivshmemContext)` in `CreateDomConfig()` for HVM domains so QEMU boots with `1af4:1110` attached at initial launch.

---

## 2. EVE BaseOS Build & OTA Firmware Update

Firmware build and OTA procedures: [EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md).

- [ ] **Compile EVE BaseOS**:
  - [ ] Run `make -C eve/build eve` on the builder to compile the updated `pillar` microservice into `rootfs.img`.
  - [ ] Capture the generated version string from `eve/eve/dist/arm64/current/installer/eve_version`.
- [ ] **Stage & Register Firmware**:
  - [ ] Stage `rootfs.img` into the LocalHTTP datastore directory via `scripts/pub_eve_datastore.sh`.
  - [ ] Register and uplink the image in ZedControl with `zcli image create` and `zcli image uplink` (exact SHA-256 and byte size).
- [ ] **Deploy Over-The-Air (OTA) Update to Edge Node**:
  - [ ] Publish candidate image to node: `zcli edge-node eveimage-update <node> --image="<version>"`.
  - [ ] Activate partition switch and reboot: `zcli edge-node eveimage-update <node> --image="<version>" --activate`.
  - [ ] Monitor node return to `Online` under the 10-minute probationary test window.
  - [ ] Verify that `/persist` and all edge application instances (`ubuntu_24_04.<node>` and `ubuntu_24_04_container.<node>`) remain preserved.

---

## 3. End-to-End Transport Verification

- [x] **Ubuntu HVM Guest PCI Discovery**: both `1af4:1053` (vsock) and
      `1af4:1110` (ivshmem, as a RAM controller at `00:06.0`) are detected in
      `ubuntu_24_04_ivshmem.n1-655-devkit`.
- [x] **Guest Driver Probe & Device Node**: probe reports
      `shm phys 0x8000000000 size 16777216`, matching the BAR QMP shows
      host-side, and `/dev/amba_virt` appears automatically. Build the module in
      the VM (`make build-hvm`) — that guest runs `6.8.0-137-generic` and
      `modversions` rejects a vermagic built elsewhere.
- [x] **Container Host Server**:
  - [x] Host driver `amba_virt.ko` is built into the EVE kernel image and
        `modprobe`d from `/etc/init.d/000-mod-params`, so `/dev/amba_virt`
        exists before any NOHYPER container is created. Verified on
        `n1-655-devkit`: loads with the backing file still absent and reports
        `shm /dev/shm/amba-virt (pending)`.
  - [x] Assigning the `amba_virt` adapter to the NOHYPER instance makes EVE
        inject both the node and its cgroup device rule from the model:
        `crw-rw-rw- 507, 0 /dev/amba_virt` with `c 507:0 rwm` in `devices.list`,
        no `mknod` and no hand-edited whitelist. The major is dynamically
        allocated (507 here, not the 506 an earlier run saw), which is exactly
        why it has to come from the model.
  - [x] `bin/amba-virt-server` runs inside the NOHYPER container against that
        injected node.
- [x] **Userspace Smoke Tests**: all three pass on `n1-655-devkit` between
      `ubuntu_24_04_ivshmem` and `ubuntu_24_04_container_amba`.
  - [x] `info` → `proto=1 role=0 shm=16777216 connected=0 cid=2 port=5555`.
  - [x] `ping` → `PONG seq=1`.
  - [x] `shm` → `SHM_ACK seq=7 off=0 len=256 first=7`, and the host's
        `/dev/shm/amba-virt` starts `07 08 09 0a …`, so the guest's BAR writes
        land in the backing file directly rather than being relayed.

---

## 4. Upstream Integration & Packaging

- [x] **EVE In-Tree Host Module Integration**: `amba_virt.ko` is vendored into
      `eve-kernel` (`scripts/install_kmod_to_eve_kernel.sh`), built by
      `Dockerfile.gcc` alongside `cavalry`, and `modprobe`d from
      `000-mod-params`. It loads before any app exists because the backing file
      is now attached lazily on first open rather than at module init.
- [x] **Container Device Auto-Permissioning**: `/dev/amba_virt` is an
      `IO_TYPE_OTHER` member (`Ifname=/dev/amba_virt`, `assigngrp amba_virt`) in
      both N1-655 models and is assigned to the NOHYPER instance. Confirmed on
      hardware: EVE injects the node and its cgroup rule, so no manual `mknod`,
      no hand-edited whitelist and no hardcoded major.
- [ ] **Upstream the `kvm.go` ivshmem support**: the patch lives on a local
      branch in `eve/eve`. Decide whether to carry it as an Ambarella delta or
      propose it to lf-edge, and add the multi-window work from
      [EVE-Multiple-HVM.md](EVE-Multiple-HVM.md) if more than one HVM ever needs
      a window.
