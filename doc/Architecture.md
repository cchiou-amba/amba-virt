# System architecture (EVE-OS / N1-655)

Ambarella SoC boots **EVE-OS** (KVM). **Zededa Zedcontroller** orchestrates the
HVM guests and their device assignment. Hardware stays on the hypervisor side:
one trusted **`amba-virt-server`** in EVE Dom0 owns VisORC, the camera, and the
other shared accelerators. KVM guests talk to it over **virtio-vsock**
(control) and **ivshmem** (bulk). There is no broker container.

Related: [AmbaVirtServer.md](AmbaVirtServer.md) (server process tree, boot order, transport, and IPC benchmarks),
[CavalryVirtualization.md](CavalryVirtualization.md) (Cavalry proxy),
[EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (cloud PhyIo models),
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md) (deploy HVM guests),
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md) (do not update in place),
[EVE-BaseOS-AmbarellaDrivers.md](EVE-BaseOS-AmbarellaDrivers.md) (driver layout and the `eve/pkg/amba-virt` layer),
[EVE-OutOfTree-KMODs.md](EVE-OutOfTree-KMODs.md) (dual compile modes and signing),
[EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md) (OTA firmware updates),
[ZedControl-scripts.md](ZedControl-scripts.md) (zcli wrappers),
[EVE-Native-ivshmem-Support.md](EVE-Native-ivshmem-Support.md)
(native KVM ivshmem implementation),
[UART-Passthrough.md](UART-Passthrough.md) (UART MMIO and vGIC design),
[AmbaVirtDMA.md](AmbaVirtDMA.md) (host-owned peripheral DMA),
[EVE-Multiple-HVM.md](EVE-Multiple-HVM.md) (CID and slice arbitration across HVMs),
[Guest-OS-Cross-Compilation.md](Guest-OS-Cross-Compilation.md) (guest toolchain),
[drivers/amba_virt/](../drivers/amba_virt/README.md) and [guest-os/](../guest-os/).

## Diagram

Zedcontroller places HVM guests (KVM virtual machines) on the SoC. EVE Dom0
runs `amba-virt-server` from the EVE root filesystem, started by LinuxKit init
before the EVE system services. Guests never own VisORC directly. Control goes
over virtio-vsock (CID 2, port 5555). Bulk data stays in ivshmem.

```text
                        +-----------------------+
                        | Zededa Zedcontroller  |
                        +-----------------------+
                                    |
                                    | "HVM spec + adapters"
                                    v
+-------------------------------------------------------------------------------+
| Ambarella SoC | EVE-OS KVM                                                    |
|                                                                               |
|  +--------------------+   +-------------------+   +------------------------+  |
|  | HVM (Ubuntu guest) |   | Hypervisor (QEMU) |   | EVE Dom0 (host)        |  |
|  | EL1 / EL0          |   |                   |   | root userspace on the  |  |
|  |                    |   |  +-------------+  |   | EL2 host kernel        |  |
|  |   +------------+   |   |  |    QEMU     |  |   |                        |  |
|  |   | Guest app  |   |   |  +-------------+  |   |  +------------------+  |  |
|  |   +------------+   |   |      |      |     |   |  | amba-virt-server |  |  |
|  |         |          |   |      |      |     |   |  +------------------+  |  |
|  |         v          |   |      |      |     |   |     /            \     |  |
|  |  +---------------+ |   |      |      |     |   |    v              v    |  |
|  |  |/dev/amba_virt | |   |      |      |     |   |+------------+ +------+ |  |
|  |  +---------------+ |   |      |      |     |   ||/dev/       | |/dev/ | |  |
|  |         |          |   |      |      |     |   ||  amba_virt | | cav  | |  |
|  |         v          |   |      |      |     |   |+------------+ +------+ |  |
|  |  +---------------+ |   |      |      |     |   |    |              |    |  |
|  |  | amba_virt.ko  | |   |      |      |     |   |    v              v    |  |
|  |  +---------------+ |   |      |      |     |   |+------------+ +------+ |  |
|  |      |       |     |   |      |      |     |   ||amba_virt.ko| |cav.ko| |  |
|  +------|-------|-----+   |      |      |     |   |+------------+ +------+ |  |
|         |       |         |      |      |     |   |    ^              |    |  |
|         |       |         |      v      v     |   +----|--------------|----+  |
|         |       |         |  +------+ +-----+ |        |              v       |
|         |       |         |  |vhost-| |ivsh-| |        |          +--------+  |
|         |       |         |  |vsock-| |mem- | |        |          | VisORC |  |
|         |       |         |  | pci  | |plain| |        |          | other  |  |
|         |       |         |  |(CID2)| |(file| |        |          |   HW   |  |
|         |       |         |  +------+ +-----+ |        |          +--------+  |
| control:|       |         +-----|--------|----+        |                      |
|  vsock  |       |               |        |             |                      |
|  :5555  +-------|-------------->+        |             |                      |
|                 |               |        |             |                      |
|                 |               +--------|------------>+                      |
|                 |                        |             |                      |
|                 |   bulk: PCI ivshmem BAR|             |                      |
|                 +----------------------->+             |                      |
|                                          |             |                      |
|                                          +------------>+                      |
|                                                                               |
+-------------------------------------------------------------------------------+
```

```text
+-----------------------+                               +-------------------------+
| Guest                 |                               | EVE Dom0                |
|                       |                               |                         |
| +-------------------+ |                               | +---------------------+ |
| | app ioctl / mmap  | |                               | |  amba-virt-server   | |
| +-------------------+ |                               | +---------------------+ |
|           |           |   framed vsock                |     |             |     |
|           v           |   (Cavalry, camera, DMA ctrl) |     v             v     |
| +-------------------+ |------------------------------>| +--------+  +---------+ |
| |  /dev/amba_virt   | |                               | |/dev/   |  | shared  | |
| +-------------------+ |   mmap ivshmem (tensors, DVI) | |cavalry |  |  DRAM   | |
|           |           |------------------------------>| +--------+  +---------+ |
|                       |                               |                         |
+-----------------------+                               +-------------------------+
```

Do **not** copy bulk buffers on vsock. Do **not** use vsock port 2000 (EVE
VComLink). The server must not connect to a guest CID.

---

## Security boundary

`amba-virt-server` is a trusted root Dom0 service, shipped in the signed EVE
image. The untrusted boundary is the HVM RPC over vsock and the guest
`/dev/amba_virt` UAPI. Every guest request is validated by the server or by a
Dom0 kernel reference monitor before it reaches hardware. The guest supplies
offsets, lengths, and opaque handles, never host physical addresses.

## Privilege model

```text
+----------------------------------------------------------------------------+
| EL3  Secure monitor (unused)                                               |
+----------------------------------------------------------------------------+
                                     |
                                     v
+----------------------------------------------------------------------------+
| EL2  EVE-OS kernel + KVM (Dom0 userspace, including amba-virt-server,      |
|      runs on this kernel)                                                  |
+----------------------------------------------------------------------------+
                                     |
                                     v
+----------------------------------------------------------------------------+
| EL1  HVM guest kernel (amba_virt.ko)                                       |
+----------------------------------------------------------------------------+
                                     |
                                     v
+----------------------------------------------------------------------------+
| EL0  HVM guest userspace (app, CLI)                                        |
+----------------------------------------------------------------------------+
```

| Role | What it is | ARM exception level |
|---|---|---|
| EVE-OS / KVM | Hypervisor OS on the SoC | EL2 (host kernel) |
| **`amba-virt-server`** | Root userspace daemon in EVE Dom0, from the EVE root filesystem | Host userspace on the EL2 kernel |
| **Linux HVM** | KVM Linux guest domain (Ubuntu / Alpine) | EL1 (kernel) / EL0 (userspace) |
| **QNX 8.0 HVM** | KVM QNX Neutrino RTOS microkernel guest domain | EL1 (microkernel) / EL0 (resource managers & apps) |
| Secure monitor | Not used by this stack | EL3 |

Do not call the guest VM EL3. The server does not "run at EL2" as a
hypervisor; it is an ordinary root process on the hypervisor OS. It loads,
parameterizes, and removes the accelerator kernel modules itself with
`finit_module` and `delete_module`, from the module tree in the EVE image.
Build out-of-tree host modules against `eve-kernel` (or the running EVE
`/lib/modules/$(uname -r)/build`). Build Linux HVM modules against target guest
distribution headers (`gcc-aarch64-linux-gnu`). Build QNX HVM resource managers
and applications using QNX SDP 8.0 (`qcc -Vgcc_ntoaarch64le`). Do not give the
guest the Ambarella kernel tree.

### Dom0 process hierarchy

```text
LinuxKit init  (/etc/init.d/021-amba-virt-server -> /usr/bin/amba-virt-server)
    |
    +-- amba-virt-server supervisor   (/run/amba-virt/server.pid; init returns
            |                          once the worker reports ready)
            |
            +-- amba-virt-server worker   (restarted with backoff if it dies)
                    |
                    +-- dsp_monitor_service    (only while the camera pipeline is up)
                    +-- amba-virt-aaa          (only while the camera pipeline is up)
```

The server, its private glibc under `/usr/lib/amba-virt/lib`, the camera
userspace, firmware, configuration, and the signed modules all come from the
`eve/pkg/amba-virt` image layer. Nothing is installed at runtime, and no
binary is read from `/persist`. Details:
[AmbaVirtServer.md](AmbaVirtServer.md) and
[EVE-BaseOS-AmbarellaDrivers.md](EVE-BaseOS-AmbarellaDrivers.md).

The cloud model assigns guest-facing adapters only: the ivshmem window
(`amba_shm`) and, where used, a UART. Accelerator nodes such as `/dev/cavalry`
and `/dev/iav` are never assigned to an app. Cloud model inventory:
[EVE-Ambarella-Models.md](EVE-Ambarella-Models.md).

---

## Transport

| Channel | Mechanism | Use |
|---|---|---|
| Control | virtio-vsock, framed SOCK_STREAM | ioctl-sized messages, PING/ECHO, Cavalry and camera control |
| Bulk | ivshmem (shared DRAM) | tensors, DVI, CMA-sized buffers |

**virtio-vsock** is stock on EVE HVMs (`vhost-vsock-pci` / `eve-vsock0`).
Direction is **guest → host CID 2**. Do **not** use port **2000** (EVE
VComLink). The transport uses port **5555**. EVE does not publish guest CID↔UUID;
the server must not connect to a guest CID.

**One ivshmem window on the host.** The driver splits it into two slices
(`amba_virt_shm` / `amba_virt_shm0`, and `amba_virt_shm1`). Each HVM maps the
slice QEMU gave it as BAR 2. Cavalry, DMA, and later frontends share that
slice. Do not add a second `amba_shm` per driver. Control stays on vsock.
Bulk is an offset and length into the slice. The guest claims the slice by
sending the nonce from the claim page, after loading
`vmw_vsock_virtio_transport`. The server binds that guest CID only when the
nonce matches, and it clamps the Cavalry pool to the usable slice. A second
window per HVM remains deferred:
[EVE-Multiple-HVM.md](EVE-Multiple-HVM.md).

The window is guest staging, not host AMA. N1-655 `cavalry_reserved` is 12 GB
on the host; the VP DMA-reads those HPAs. The proxy copies or token-rewrites
between ivshmem and that pool. PCI BAR size must be a power of two, so 12G
cannot be the BAR, and `/dev/shm` is only ~8.9 GB.

**Production `cbattr.shmsize` is `1G`.** 16M is PoC `ping`/`shm` only. Guest
RAM is a different number; the window is extra and must be charged in full by
`ivshmemVMMOverhead`.
[EVE-Native-ivshmem-Support.md](EVE-Native-ivshmem-Support.md).
The `amba_shm` adapter on the HVM is what makes `kvm.go` emit `ivshmem-plain`.
The server reaches the same DRAM through `/dev/amba_virt`, not by opening the
backing file. The server withholds readiness until `/dev/amba_virt_shm*`
exists, so the backing nodes are present before EVE starts any guest.

Do not copy bulk data over vsock.

### UART and Peripheral DMA

UART virtualization separates register access from peripheral DMA authority:

- **Register Aperture (True MMIO Passthrough)**:
  - Narrow, non-bus-master peripheral controllers (specifically SoC UART2 at
    `0xffe0018000`, 4 KiB, GIC SPI 115) are passed through directly via
    `vfio-platform` into the guest Stage-2 page tables.
  - The guest accesses real hardware registers directly without QEMU emulation
    or userspace proxy loops.
  - Level-sensitive physical interrupts are forwarded via KVM `irqfd` and
    in-kernel EOI resampling (`resamplefd`), eliminating userspace doorbell ACK
    round-trips.
  - **Negative Constraint**: Do not use VFIO for bus-mastering peripherals without
    an IOMMU/SMMU. Unrestricted VFIO assignment and guest access to Generic-DMA1
    MMIO registers remain strictly forbidden.

- **Peripheral DMA (Split Authority & Dual Windows)**:
  - Generic-DMA1 (`0xffe0021000`) remains strictly under trusted Dom0 kernel
    authority (`amba_virt_dma.ko`).
  - High-memory DRAM bulk transfers continue using the 1 GiB `ivshmem-plain` window.
  - Low-memory 32-bit DMA transfers use a dedicated, carveout-backed 16 MiB
    DMA32 `ivshmem-plain` window (from a 64 MiB `no-map` reservation at `0x6c000000`).
  - The guest frontend (`amba_dma`) allocates data buffers within its 16 MiB
    DMA32 slice and submits offset-only, capability-checked DMA requests over
    vsock RPC to the DMA broker in `amba-virt-server`.
  - The Dom0 kernel reference monitor validates bounds, maps addresses to
    channel-safe 32-bit DMA addresses, and programs Generic-DMA1 descriptors.

The implementation and qualification status are tracked in [UART-Passthrough.md](UART-Passthrough.md).

Both ends of the transport compile a matching kernel module (`amba_virt.ko`) that
exposes `/dev/amba_virt` (mmap of the shared region + framed vsock send/recv).
`amba-virt-server` in Dom0 opens that chardev and the real hardware nodes. See
[drivers/amba_virt/](../drivers/amba_virt/README.md).

vhost-user is an optional later transport, not the plan.

---

## Arbitration

One hardware owner: the `amba-virt-server` worker in Dom0. Multiple EL1 guests
may connect.

The server must:

- Serialize exclusive operations (`START_VP`, VisORC reset, firmware load).
- Quota ALLOC from the shared Cavalry pool so one guest cannot exhaust CMA.
- Admit `RUN_DAGS` with a queue (fair or priority). Host `cavalry.ko` already
  sequences jobs by `seq_num`; the proxy still decides **who may submit**.
- Recycle guest state on disconnect (memory, sessions).

Handles, DAGs, and sessions live in the worker process. When the worker
restarts, the CID bindings are restored from the kernel, but guest handles are
not; a guest that presents a stale handle is told so and re-registers.

---

## Out of scope

- MMIO / VFIO passthrough of VisORC into the guest (exclusive HW, GPA≠HPA,
  breaks multi-guest and EVE isolation).
- Custom VirtIO device ID as the primary control path.
- Connecting the server to a guest vsock CID.
- An amba-virt broker container. Generic EVE `HV_NOHYPER` apps remain an EVE
  feature, but they are not part of this design and are never given
  `cavalry`, `iav`, or `amba_virt`.
