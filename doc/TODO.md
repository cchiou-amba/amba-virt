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

- [ ] **Ubuntu HVM Guest PCI Discovery**:
  - [ ] Run `lspci -nn | grep -E "1af4|1110|1053"`.
  - [ ] Confirm both `1af4:1053` (`vhost-vsock-pci`) AND `1af4:1110` (`ivshmem-plain`) are detected.
- [ ] **Guest Driver Probe & Device Node**:
  - [ ] Load `virtio_vsock` (`modprobe virtio_vsock`).
  - [ ] Insert guest driver (`insmod kmod/guest/amba_virt.ko`).
  - [ ] Confirm PCI probe succeeds: `dmesg | grep amba_virt` reports BAR 2 physical address and size.
  - [ ] Confirm `/dev/amba_virt` is automatically created and accessible.
- [ ] **Container Host Server**:
  - [ ] Ensure host driver (`amba_virt.ko`) is inserted on EVE host.
  - [ ] Ensure container device cgroup whitelist includes major 506 (`c 506:* rwm`).
  - [ ] Run `bin/amba-virt-server` inside the NOHYPER container.
- [ ] **Userspace Smoke Tests**:
  - [ ] In HVM: `./bin/amba-virt-cli info` (verify proto, role, shm_size, vsock CID/port).
  - [ ] In HVM: `./bin/amba-virt-cli ping` (expect `PONG` over virtio-vsock).
  - [ ] In HVM: `./bin/amba-virt-cli shm` (expect `SHM_ACK` with 256 bytes zero-copy DRAM verification).

---

## 4. Upstream Integration & Packaging

- [ ] **EVE In-Tree Host Module Integration**:
  - [ ] Evaluate packaging `amba_virt.ko` host module directly within `eve-kernel` (or EVE dom0 packages) so host driver is built-in and auto-loaded on boot without manual `insmod`.
- [ ] **Container Device Auto-Permissioning**:
  - [ ] Add major 506 to standard device rules in EVE pillar container runtime configurations so NOHYPER containers do not require manual cgroup whitelisting.
