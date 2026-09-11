# System architecture (EVE-OS / N1-655)

Ambarella SoC boots **EVE-OS** (KVM). **Zededa Zedcontroller** orchestrates two
app types and device assignment. Hardware stays on the hypervisor side.
KVM guests talk to a privileged container over **virtio-vsock** (control) and
**ivshmem** (bulk).

Related: [PoCVirtualDrivers.md](PoCVirtualDrivers.md) (transport),
[CavalryVirtualization.md](CavalryVirtualization.md) (Cavalry proxy),
[EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (cloud PhyIo models),
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md) (deploy HVM + NOHYPER),
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md) (do not update in place),
[ZedControl-scripts.md](ZedControl-scripts.md) (zcli wrappers),
[Native-ivshmem-Support-in-EVE-BaseOS.md](Native-ivshmem-Support-in-EVE-BaseOS.md)
(the EVE-side change),
[EVE-Multiple-HVM.md](EVE-Multiple-HVM.md) (scaling past one pair, deferred),
[drivers/amba_virt/](../drivers/amba_virt/README.md) and [guest-os/](../guest-os/).

## Diagram

Zedcontroller places two apps on the SoC: an Ubuntu **HVM** (KVM guest) and a
privileged **NOHYPER** container on the EVE host. Guests never own VisORC.
Control goes over virtio-vsock (CID 2, port 5555). Bulk data stays in ivshmem.

```mermaid
flowchart TB
  Zed["Zededa Zedcontroller"]

  subgraph soc ["Ambarella SoC  |  EVE-OS KVM"]
    direction LR

    subgraph hvm ["HVM  Ubuntu guest  EL1 / EL0"]
      direction TB
      App["Guest app"]
      Gdev["/dev/amba_virt"]
      Gkmod["amba_virt.ko"]
      App --> Gdev --> Gkmod
    end

    subgraph hyp ["Hypervisor  QEMU"]
      direction TB
      Qemu["QEMU"]
      Vsock["vhost-vsock-pci  host CID 2"]
      Shm["ivshmem-plain  backing file"]
      Qemu --- Vsock
      Qemu --- Shm
    end

    subgraph nh ["NOHYPER  privileged container  EL2 host"]
      direction TB
      Srv["proxy / arbitrator"]
      Hkmod["amba_virt.ko"]
      Hdev["/dev/amba_virt"]
      Cav["cavalry.ko  /dev/cavalry"]
      VP["VisORC / other HW"]
      Srv --> Hdev --> Hkmod
      Srv --> Cav --> VP
    end
  end

  Zed -->|"HVM spec"| Qemu
  Zed -->|"app + PhyIo assign"| nh
  Gkmod -->|"control: vsock :5555"| Vsock
  Vsock --> Hkmod
  Gkmod -->|"bulk: PCI ivshmem BAR"| Shm
  Shm --> Hkmod
```

```mermaid
flowchart LR
  subgraph guest ["Guest"]
    Gapp["app ioctl / mmap"]
    Gdev["/dev/amba_virt"]
    Gapp --> Gdev
  end

  Gdev -->|"framed vsock  PING, later Cavalry ctrl"| HostSrv
  Gdev -->|"mmap ivshmem  tensors, DVI"| ShmReg

  subgraph host ["NOHYPER"]
    HostSrv["userspace server"]
    ShmReg["shared DRAM"]
    RealCav["/dev/cavalry"]
    HostSrv --> RealCav
    HostSrv --> ShmReg
  end
```

Do **not** copy bulk buffers on vsock. Do **not** use vsock port 2000 (EVE
VComLink). The container must not connect to a guest CID.

---

## Privilege model

```mermaid
flowchart TB
  el3["EL3  secure monitor  unused"]
  el2["EL2  EVE-OS kernel + KVM  NOHYPER container runs here as a process"]
  el1["EL1  HVM guest kernel  amba_virt.ko"]
  el0["EL0  HVM guest userspace  app, CLI"]
  el3 --> el2 --> el1 --> el0
```

| Role | What it is | ARM exception level |
|---|---|---|
| EVE-OS / KVM | Hypervisor OS on the SoC | EL2 (host kernel) |
| **NOHYPER** | Privileged Ubuntu **container** on that host kernel, not a VM | EL2-side process |
| **HVM** | KVM guest (Ubuntu). Guest kernel / userspace | EL1 / EL0 |
| Secure monitor | Not used by this stack | EL3 |

Do not call the Ubuntu VM EL3. The NOHYPER container does not “run at EL2” as a
hypervisor; it runs **on the hypervisor OS**. `insmod` inside a privileged
NOHYPER app loads a module into the **EVE host kernel**. Build that module
against `eve-kernel` (or the running EVE `/lib/modules/$(uname -r)/build`).
Build HVM modules against Ubuntu `linux-headers`. Do not give the guest the
Ambarella kernel tree.

Zedcontroller assigns devices (PhyIo / assigngrp) to the NOHYPER app so it can
open `/dev/cavalry` and similar nodes. HVMs must not own VisORC. Cloud model
inventory: [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md).

---

## Transport

| Channel | Mechanism | Use |
|---|---|---|
| Control | virtio-vsock, framed SOCK_STREAM | ioctl-sized messages, PING/ECHO, later Cavalry ctrl |
| Bulk | ivshmem (shared DRAM) | tensors, DVI, CMA-sized buffers |

**virtio-vsock** is stock on EVE HVMs (`vhost-vsock-pci` / `eve-vsock0`).
Direction is **guest → host CID 2**. Do **not** use port **2000** (EVE
VComLink). The transport uses port **5555**. EVE does not publish guest CID↔UUID;
the container must not connect to a guest CID.

**One ivshmem window per HVM/NOHYPER pair.** Cavalry, DMA, SD/eMMC, and later
frontends share that BAR. Do not add a second `amba_shm` per driver. Control
stays on vsock; bulk is `shm_off` / `shm_len` into the window. A later host
allocator partitions offsets; Cavalry takes most of the pool by quota.
Several HVMs would need several windows:
[EVE-Multiple-HVM.md](EVE-Multiple-HVM.md) (deferred).

The window is guest staging, not host AMA. N1-655 `cavalry_reserved` is 12 GB
on the host; the VP DMA-reads those HPAs. The proxy copies or token-rewrites
between ivshmem and that pool. PCI BAR size must be a power of two, so 12G
cannot be the BAR, and `/dev/shm` is only ~8.9 GB.

**Production `cbattr.shmsize` is `1G`.** 16M is PoC `ping`/`shm` only. Guest
RAM is a different number; the window is extra and must be charged in full by
`ivshmemVMMOverhead`.
[Native-ivshmem-Support-in-EVE-BaseOS.md](Native-ivshmem-Support-in-EVE-BaseOS.md).
The `amba_shm` adapter on the HVM is what makes `kvm.go` emit `ivshmem-plain`.
The container reaches the same DRAM through `/dev/amba_virt`, not by opening
the backing file (NOHYPER `/dev/shm` is a private tmpfs).

Do not copy bulk data over vsock.

Both ends of the transport compile a matching kernel module (`amba_virt.ko`) that
exposes `/dev/amba_virt` (mmap of the shared region + framed vsock send/recv).
A userspace **server** in NOHYPER opens that chardev and later opens real
hardware. See [drivers/amba_virt/](../drivers/amba_virt/README.md).

vhost-user is an optional later transport, not the plan.

---

## Arbitration

One hardware owner: the NOHYPER process. Multiple EL1 guests may connect.

The server must:

- Serialize exclusive operations (`START_VP`, VisORC reset, firmware load).
- Quota ALLOC from the shared Cavalry pool so one guest cannot exhaust CMA.
- Admit `RUN_DAGS` with a queue (fair or priority). Host `cavalry.ko` already
  sequences jobs by `seq_num`; the proxy still decides **who may submit**.
- Recycle guest state on disconnect (memory, sessions).

---

## Out of scope

- MMIO / VFIO passthrough of VisORC into the guest (exclusive HW, GPA≠HPA,
  breaks multi-guest and EVE isolation).
- Custom VirtIO device ID as the primary control path.
- Connecting the container to a guest vsock CID.
