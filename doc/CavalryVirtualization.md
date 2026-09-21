# Cavalry Virtualization (N1-655 / EVE-OS)

*Copyright (C) 2026, Ambarella International LLC.*

Cavalry is Ambarella’s Vision Processor (VP / VisORC) Linux driver. Userspace
talks ioctl + mmap on `/dev/cavalry`. The kernel loads `cavalry.bin`, programs
VisORC MMIO, and submits jobs as **host-physical command descriptors**. The VP
DMA-reads those addresses.

On this platform the real driver stays in the **NOHYPER** container on EVE-OS.
KVM **HVM** guests (EL1) never load Ambarella `cavalry.ko`. They use the
vsock + ivshmem transport ([drivers/amba_virt/](../drivers/amba_virt/README.md)) and a
frontend that preserves the v3 ioctl ABI while supporting both **Path A** (legacy drop-in)
and **Path B** (Hardened Class D host isolation).

System picture: [Architecture.md](Architecture.md). Transport details:
[AmbaVirtServer.md](AmbaVirtServer.md). Deploy:
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md).

Guest ABI to preserve:
`eve-kernel/ambarella/include/cavalry_v3/cavalry_ioctl.h`
(copy that UAPI into this repo for the HVM; do not mount `eve-kernel` into
the guest).

Host driver: **cavalry_v3** (`compatible = "ambarella,sub-scheduler"`).

---

## Architecture (Proxy, Not Passthrough)

```text
+-------------------------------------------------------------+                               +-----------------------------------+
| Linux HVM EL1 (Ubuntu / Alpine)   | QNX 8.0 HVM EL0 (RTOS)  |                               | NOHYPER EL2-side (Host Server)    |
|                                   |                         |                               |                                   |
|   +---------------------------+   |   +-------------------+ |                               |   +---------------------------+   |
|   | NN App (nnctrl/YOLO)      |   |   | NN App (YOLO/Demo)| |                               |   | amba-virt-server          |   |
|   +---------------------------+   |   +-------------------+ |                               |   | (cavalry_proxy)           |   |
|                 | (ioctl)         |             | (devctl)  |                               |   +---------------------------+   |
|                 v                 |             v           |                               |      /                     \      |
|   +---------------------------+   |   +-------------------+ |                               |     v                       v     |
|   | /dev/cavalry frontend     |   |   | amba-cavalry-     | |                               | +---------------+   +-----------+ |
|   | (amba_cavalry.ko)         |   |   | resmgr (/dev/cav) | |   vsock (CID 2 : Port 5555)   | | amba_virt.ko  |   |cavalry.ko | |
|   +---------------------------+   |   +-------------------+ |------------------------------>| +---------------+   |/dev/cav.  | |
|                 | (kexport)       |             | (libamba) |                               |         ^           +-----------+ |
|                 v                 |             v           |                               |         |                 |       |
|   +---------------------------+   |   +-------------------+ |                               |         |                 v       |
|   | amba_virt.ko              |   |   | amba-virt-resmgr  | |   ivshmem-plain (shared DRAM) |         |              +------+ |
|   +---------------------------+   |   +-------------------+ |---+-------------------------------+---------+        |VisORC| |
|                                   |                         |   |                               |                  +------+ |
+-------------------------------------------------------------+   |                               +-----------------------------------+
                                                                  v
```

| Metric | This Architecture | Guest MMIO Passthrough (Rejected) |
|---|---|---|
| **Who runs `cavalry.ko`** | NOHYPER only | Guest |
| **Guest `/dev/cavalry`** | Frontend on `amba_virt` (`amba_cavalry.ko` on Linux, `amba-cavalry-resmgr` on QNX) | Unmodified Ambarella module |
| **Tensors & Microcode** | ivshmem; host translates or isolates in AMA | Guest GPA must equal HPA |
| **Multi-guest** | Proxy arbitrates & serializes | Exclusive VisORC lock |
| **EVE isolation** | Preserved via Stage-2 MMU | Breaks device assign / shared SoC |

### Why Not Passthrough

Guest `cavalry.ko` `ioremap`s absolute SoC addresses, `mmap`s host PFNs, and
puts those physical addresses in DAG descriptors. Unless every carve-out is
identity-mapped into the guest **and** the host never uses Cavalry, the VP
DMA-reads the wrong DRAM. Reset and clocks are SoC-global. Multiple HVMs cannot
safely share VisORC hardware without mediation.

---

## 1. What the Host Driver Is

The module matches DT `compatible = "ambarella,sub-scheduler"` and creates
`/dev/cavalry` (plus `/dev/cavalry_profile`) **inside NOHYPER** (after the
device is assigned).

| Layer | Role |
|---|---|
| Userspace (`cavalry_ioctl.h`) | ioctl + `mmap` of physically contiguous CV buffers |
| `cavalry.ko` | CMA/AMA allocator, firmware load, cmd/msg queues, doorbells, IRQs |
| `cavalry.bin` (VisORC ucode) | Scheduler + VP/FEX/FMA workers |
| Hardware | VisORC MMIO, RCT reset/clocks, scratchpad kicks, 4 IRQs |

**cavalry_v2** is CV2/CV22/CV25/CV28/CV5/CV52. **cavalry_v3** is N1, CV72/CV75,
CV7. This document applies to v3 / N1-655.

---

## 2. Host Hardware Map (N1-655)

Used by **NOHYPER** `cavalry.ko`, not the guest. From `n1_655.dts` and
`cavalry_v3/cavalry_visorc.c`.

### MMIO (Hardcoded `ioremap`, Not From DT)

| Name | HPA | Size | Use |
|---|---|---|---|
| VisORC APB | `0xffed000000` | 16 MB | Per-engine reset, L2 remap, status, audio ticks |
| RCT | `0xffed080000` | 4 KB | Cluster/VORC soft reset, PLLs |
| Kick sched | `0xfff0200004` | 16 B | ARM → ORC doorbell (`writel(1, ...)`) |
| Kick ARM ACK | `0xff00006000` | 16 B | ORC ↔ ARM handshake + IRQ timestamps |

N1-655 engines: NVP0 (`0x440000`), FEX0 (`0x3e0000`), Manager (`0x100000`).
L2 remap windows at `0x420000`, `0x3c0000`, `0x110000`.

### IRQs (DT `sub_scheduler0`)

```dts
sub_scheduler0 {
    compatible = "ambarella,sub-scheduler";
    interrupts = <0 50 0x4>, <0 51 0x4>, <0 52 0x4>, <0 53 0x4>,
                 <0 50 0x4>, <0 51 0x4>, <0 52 0x4>, <0 53 0x4>;
    memory-region = <&cavalry_reserved &cavalry_ucode>;
    shared-region = <&cavalry_shared>;
};
```

| Index | Host SPI | Name | Meaning |
|---|---|---|---|
| 0 | 50 | job submit | ORC accepted cmd-queue write |
| 1 | 51 | job done | Worker finished; MSG queue has `seq_num` |
| 2 | 52 | resv0 | ISR returns `IRQ_NONE` |
| 3 | 53 | resv1 | ISR returns `IRQ_NONE` |

### Reserved DRAM Carveouts

| Node | HPA | Size | Attr | Function |
|---|---|---|---|---|
| `cavalry_ucode` | `0x25c00000` | 4 MB | `no-map` | Firmware binary, hotlink, private queues |
| `cavalry_reserved` | `0x100000000` | 12 GB | `no-map` | User tensors, DVI microcode, AMA pool |
| `cavalry_shared` | (unset in DTS) | — | — | Optional share-to-DSP |

`no-map` → `ioremap_wc` + AMA. `phys_to_orc_addr(phys) = phys`. Every DAG
port the VP uses is a **real SoC physical address**.

### Private 4 MB (Ucode Region)

```text
+0x000000  UCODE     1 MB    cavalry.bin; version @ +0x40; init_data @ +0x80
+0x100000  HOTLINK   1 MB    4 × 256 KB slots
+0x200000  CMD q     32 KB   ARM writes u64 cmd_phys per slot
+0x208000  MSG q     32 KB   ORC writes matching seq_num (u32)
           LOG, FEX/FMA cmd, IDSP, STATS, PROFILE
```

---

## 3. Ambarella Memory Allocator (AMA)

### Why AMA Exists
VisORC NPU coprocessors operate directly with physical addresses without an ARM S-MMU on the high-bandwidth data paths. They require:
- **Strict physical contiguity**: DMA cannot cross arbitrary 4 KB page boundaries without scatter-gather hardware.
- **Zero fragmentation**: Standard Linux page allocators cannot guarantee large contiguous buffers (e.g. 50 MB to 2 GB) after system uptime.
- **Cache management**: Memory is mapped write-combined (`ioremap_wc`) or explicitly cache-flushed before and after hardware execution (`dma_sync`).

Ambarella's `cavalry.ko` provides the **AMA subsystem** to manage the 12 GB `cavalry_reserved` carveout (`[0x100000000 - 0x3ffffffff]`).

### AMA Interfaces (`/dev/cavalry`)
- `ioctl(CAVALRY_ALLOC_MEM, &mem)`: Allocates contiguous physical slice, returning physical address `mem.offset` (HPA).
- `mmap(..., mem.offset)`: Maps HPA to userspace with write-combining flags.
- `ioctl(CAVALRY_ALLOC_MEMFD, &mfd)`: Exports contiguous slice as a standard Linux `dma-buf` file descriptor.
- `ioctl(CAVALRY_FREE_MEM, &mem)`: Returns physical slice back to the pool.

---

## 4. Virtualization Memory Hierarchy

In `amba-virt`, the physical 12 GB CVMEM pool is partitioned between guest-shared BAR windows and host-private memory:

```text
Total 12 GB CVMEM Pool [0x100000000 - 0x3FFFFFFFF]
+------------------------------------------+------------------------------------+
| Guest Shared Memory Apertures (IVSHMEM)  | Host-Private AMA Pool              |
| Accessible to Guest VMs via Stage-2 MMU  | Inaccessible to Guest VMs          |
|                                          | Hidden behind Stage-2 Page Tables  |
+------------------------------------------+------------------------------------+
  |                                          |
  v                                          v
+----------------------------------------+ +------------------------------------+
| ivshmem-plain PCI BAR2 (0x1af4:1110)   | | Host AMA Registry                  |
|                                        | | Managed by host cavalry.ko AMA     |
| [0, 31 MiB):  GDMA Channel Pool        | | Stores:                            |
| [32 MiB, ...):Cavalry Tensor/Staging   | | - Registered DVI microcode         |
+----------------------------------------+ | - Sub-scheduler DAG descriptors    |
                                           | - PPV3 ping-pong graphs & poke list|
                                           | - Global model weights             |
                                           +------------------------------------+
```

### Guest Shared Memory (`ivshmem-plain` BAR)
- Mapped to guest as PCI BAR 2 (`1af4:1110`), backed by `/dev/amba_virt_shm*`.
- Partitioned into:
  - **`[0, 31 MiB)`**: Clamped GDMA channel buffer and RPC control arena.
  - **`[32 MiB, Window End)`**: Cavalry user tensors and temporary staging.

### Host-Private AMA Pool
- Physical memory located above guest BAR windows (`[0x140000000 / 0x180000000, 0x400000000)`).
- Completely hidden from guest VMs via ARM64 Stage-2 translation tables.
- Managed by `cavalry_proxy` to store registered DAGs and DVI microcode.

---

## 5. Dual-Engine Architecture: Path A vs. Path B

To resolve the trade-off between legacy binary compatibility and hardened multi-tenant isolation, `amba-virt` implements a dual-engine architecture:

```text
PATH A: Transparent Drop-In (Legacy Mode)

Guest VM (Ubuntu/Alpine)               Host Proxy (amba-virt-server)        VisORC Hardware
+------------------------+             +---------------------------+       +---------------+
| /dev/cavalry (shim)    |             | amba-virt-server          |       | VisORC NPU    |
| - DVI in Guest BAR     |--vsock RPC->| - Validates port offsets  |------>| Direct DMA    |
| - Tensors in Guest BAR |             | - Forwards RUN_DAGS       |       | Reads DVI &   |
+------------------------+             +---------------------------+       | Tensors from  |
                                                                           | Guest BAR     |
                                                                           +---------------+
* Threat Vector: Guest can mutate DVI microcode while VisORC executes (TOCTOU).
--------------------------------------------------------------------------------------------
PATH B: Hardened Class D (Strict Isolation)

Guest VM (Ubuntu/Alpine)               Host Proxy (amba-virt-server)        VisORC Hardware
+------------------------+             +---------------------------+       +---------------+
| 1. Stage DVI in BAR    |             |                           |       | VisORC NPU    |
| 2. REGISTER_DAG ------>|--vsock RPC->| Deep-copies DVI into      |       |               |
| 3. Free Staging in BAR |             | Host-Private AMA Memory   |       | Direct DMA    |
|                        |             | Relocates DAG pointers    |       | Reads DVI from|
| 4. ALLOC_HANDLE ------>|--vsock RPC->| Allocates opaque handle ID|       | Host-Private  |
| 5. RUN_REGISTERED_DAG  |--vsock RPC->| Validates handle bounds   |------>| AMA Memory.   |
|    (passes handle IDs) |             | Programs HPA into VisORC  |       | Reads/Writes  |
+------------------------+             +---------------------------+       | Tensors from  |
                                                                           | Guest Handles |
                                                                           +---------------+
* Zero TOCTOU Risk: DVI is immutable to guest; unallocated/forged handles rejected.
```

### Path A: Transparent Legacy Drop-In
- **Mechanism**: The guest loads model binaries (`.bin`), DVI microcode, and tensors directly into its shared BAR slice (`[32 MiB, Window End)`). The guest driver issues `CAVALRY_RUN_DAGS` over vsock. The host proxy validates port offsets and dispatches to hardware.
- **Advantage**: 100% binary compatibility. Applications calling `nnctrl_init()`, `nnctrl_load_net()`, and `nnctrl_run_net()` run with zero code changes.
- **Security Limitation**: **TOCTOU (Time-of-Check to Time-of-Use)** exposure. Because VisORC has no MMU, a compromised guest can overwrite DVI microcode or DAG pointers in the BAR while VisORC is executing.
- **BAR Sizing**: Requires a large guest BAR (2 GiB to 4 GiB+) to hold all resident models and tensors.

### Path B: Hardened Class D Host AMA Isolation
- **Mechanism**:
  1. **DAG Registration (`REGISTER_DAG`)**: The guest stages the DVI image in its BAR and calls `CAVALRY_IOC_REGISTER_DAG`. The host proxy allocates memory in the **Host-Private AMA pool**, deep-copies the DVI and sub-DAG descriptors, and returns an opaque `dag_id`.
  2. **Staging Reclamation**: The guest immediately frees its staging memory. DVI microcode now lives exclusively in host-private DRAM.
  3. **Opaque Handle Allocation (`ALLOC_HANDLE`)**: The guest allocates input and output tensor buffers, receiving 32-bit `handle_id`s.
  4. **Registered Execution (`RUN_REGISTERED_DAG`)**: The guest dispatches inference passing only `dag_id` and `handle_id`s. The host proxy binds DVI from Host AMA and handles from the guest BAR, submitting via `CAVALRY_RUN_DAGS_MEMFD`.
  5. **Session Reclamation (`CLOSE_SESSION`)**: When `/dev/cavalry` closes or a process terminates (`SIGKILL`), the host automatically frees all registered DAGs and handles associated with that `session_id`.
- **Advantage**:
  - **100% TOCTOU Immunity**: VisORC microcode is physically isolated behind Stage-2 translation tables; zeroing or corrupting the guest BAR cannot affect execution integrity.
  - **Decoupled Model Capacity**: A guest with a small 512 MiB or 1 GiB BAR can execute multi-gigabyte models stored in Host AMA.
- **BAR Sizing**: Requires only a small guest BAR (512 MiB to 1 GiB) sized for active I/O tensors and the 32 MiB GDMA pool.

### Side-by-Side Comparison

| Metric | Path A (Legacy Drop-in) | Path B (Hardened Class D) |
|---|---|---|
| **Software Impact** | Zero (unmodified `nnctrl` binary compatibility) | Automatic via dual-engine `libnnctrl` port |
| **DVI Location** | Guest BAR window (`[32 MiB, End)`) | Host-Private AMA memory |
| **Hardware Target** | Guest physical BAR address | Host physical AMA address |
| **Memory Isolation** | Software range check at ioctl dispatch | Hardware Stage-2 MMU isolation of microcode |
| **TOCTOU Immunity** | Vulnerable (guest can mutate BAR during run) | **100% Immune** (guest cannot map Host AMA) |
| **Buffer Addressing** | Raw BAR offsets | Opaque 32-bit `handle_id` tokens |
| **Teardown on Termination** | Manual slice cleanup | Automated per-session cleanup on `SIGKILL` |
| **Guest BAR Sizing** | Large (must fit all models + tensors) | **Small** (only active I/O tensors + GDMA) |

---

## 6. Why Path A / Path B is Cavalry-Specific

Path A vs. Path B is strictly an architectural property of **Cavalry (VisORC NPU)** and does not apply to simple peripheral engines like GDMA.

- **Cavalry (VisORC)**: Executes compiled microcode, sub-scheduler graphs, and branch instructions. Because it lacks an S-MMU, untrusted microcode can hijack the coprocessor into becoming an arbitrary DMA bus master. Path B is required to enforce microcode immutability.
- **GDMA (General DMA)**: A fixed-function hardware 2D copy engine (`dma_memcpy`, `dma_pitch_memcpy`). It accepts only basic coordinates: `(src_offset, dst_offset, width, height, pitch)`. GDMA cannot execute instructions or chase pointers. Clamping GDMA to the first **32 MiB** (`[0, 31 MiB)`) and enforcing offset bounds checking inside the host daemon completely eliminates cross-tenant corruption without requiring a Path B.

---

## 7. Multi-Tenant Architecture & Memory Sizing Guidelines

### 7.1 Multi-Tenant Defense-in-Depth Architecture

```text
+-------------------------------------------------------------------------------------------------------+
|                                           ARM64 Silicon / Dom0                                        |
|                                                                                                       |
|  Total Physical CVMEM Carveout (Dynamically Discovered base `shm_phys` and length)                    |
|  +-----------------------------+-----------------------------+--------------------------------------+ |
|  | Tenant 0 Slice (1 GiB)      | Tenant 1 Slice (1 GiB)      | Host-Private AMA Pool (Remaining)    | |
|  | [shm_phys + 0, + 1G)        | [shm_phys + 1G, + 2G)       | [shm_phys + 2G, carveout_end)        | |
|  +-----------------------------+-----------------------------+--------------------------------------+ |
|               |                             |                                    ^                    |
|               v                             v                                    |                    |
|     /dev/amba_virt_shm0           /dev/amba_virt_shm1                            |                    |
|  (Host amba_virt.ko Minor 0)   (Host amba_virt.ko Minor 1)                       |                    |
|               |                             |                                    |                    |
+---------------|-----------------------------|------------------------------------|--------------------+
                |                             |                                    |
  Stage-2 MMU   | Stage-2 MMU                 | (Hidden behind Stage-2 MMU)        |
                v                             v                                    |
+-----------------------------+ +-----------------------------+                    |
|   Ubuntu 24.04 HVM (EL1)    | |    Alpine Linux HVM (EL1)   |                    |
|                             | |                             |                    |
|  ivshmem BAR (1 GiB @ 1110) | |  ivshmem BAR (1 GiB @ 1110) |                    |
|  - [0, 31 MB): GDMA Pool    | |  - [0, 31 MB): GDMA Pool    |                    |
|  - [31, 32 MB): RPC Arena   | |  - [31, 32 MB): RPC Arena   |                    |
|  - [32 MB, 1GB): Tensor I/O | |  - [32 MB, 1GB): Tensor I/O |                    |
|                             | |                             |                    |
|  libnnctrl.so (Path B only) | |  libnnctrl.so (Path B only) |                    |
|  - Detects HVM on init      | |  - Detects HVM on init      |                    |
|  - Opaque Handle I/O        | |  - Opaque Handle I/O        |                    |
|  - Mutex: rpc_arena_mutex   | |  - Mutex: rpc_arena_mutex   |                    |
|                             | |                             |                    |
|  Frontend Drivers (Linux):  | |  Frontend Drivers (musl):   |                    |
|  - amba_virt.ko             | |  - amba_virt.ko             |                    |
|  - amba_cavalry.ko          | |  - amba_cavalry.ko          |                    |
|  - amba_gdma.ko             | |  - amba_gdma.ko             |                    |
+-----------------------------+ +-----------------------------+                    |
                \                             /                                    |
                 \-- Connect (Host CID 2, Port 5555)                               |
                  \                         /                                      |
                   v                       v                                       |
+----------------------------------------------------------------------------------|--------------------+
| amba-virt-server (Host Daemon in NOHYPER)                                        |                    |
|   - Unified Service Listener: Port 5555 (Standard well-known port for ALL guests)|                    |
|   - Authenticated Peer CID Demux: getpeername() -> caller CID                     |                    |
|   - Dynamic CID -> Tenant Context Lookup: `tenant_ctx[t_idx]`                     |                    |
|       * Tenant 0: HPA Base = shm_phys + 0,  Map = shm0_map, Arena = [31,32MB)     |                    |
|       * Tenant 1: HPA Base = shm_phys + 1G, Map = shm1_map, Arena = [31,32MB)     |                    |
|   - Path B Enforcement Policy: configurable via `--enforce-path-b` flag           |                    |
|   - Model Registry in Host AMA: Deep-copies DVI into Host AMA Pool --------------+                    |
|   - Opaque Handle Translation: Maps handle_id to Tenant.phys_base + offset                            |
|   - Hardware Serialization:                                                                           |
|       * visorc_hw_mutex: serializes ioctls to physical /dev/cavalry                                   |
|       * gdma_hw_mutex: serializes ioctls to physical /dev/gdma                                       |
+-------------------------------------------------------------------------------------------------------+
                                                 |
                                                 v
+-------------------------------------------------------------------------------------------------------+
| Ambarella N1-655 Silicon Engines (VisORC NPU Core & GDMA Engine)                                      |
|   - Executes Registered Job from Host AMA -> Hardware IRQ -> Unlocks visorc_hw_mutex              |
+-------------------------------------------------------------------------------------------------------+
```

### 7.2 Physical Memory Layout & Carveout Breakdown

The 12 GB physical CVMEM pool (`[0x100000000 - 0x3ffffffff]`) must be sized based on the active execution path:

### Symmetrical 6 GiB / 6 GiB Sizing (Path A Only)
- **Tenant 0 (Ubuntu)**: `shmsize = "6G"` (`[0x100000000, 0x280000000)`)
- **Tenant 1 (Alpine)**: `shmsize = "6G"` (`[0x280000000, 0x400000000)`)
- **Host AMA Pool Remaining**: **0 Bytes**
- **Trade-off**: Required if guests must keep massive models resident inside guest BARs under Path A. However, it completely exhausts CVMEM and **breaks Path B** by leaving zero memory for the host to store registered models.

### Asymmetrical Sizing with Host AMA Pool (Path B - Recommended)
- **Tenant 0 (Ubuntu)**: `shmsize = "1G"` (`[0x100000000, 0x140000000)`)
- **Tenant 1 (Alpine)**: `shmsize = "1G"` (`[0x140000000, 0x180000000)`)
- **Host-Private AMA Pool**: **10 GiB** (`[0x180000000, 0x400000000)`)
- **Trade-off**: Optimal for Path B. Guests have 1 GiB each (ample space for 32 MiB GDMA, active camera/video streaming tensors, and staging), while the host retains 10 GiB of physically contiguous AMA memory to register massive neural networks and multi-tenant graphs.

---

## 8. Wire Protocol & Session Management

Control messages travel over framed virtio-vsock connecting to Host CID 2 on standard service Port 5555 (with host demuxing tenants via authenticated `peer_addr.svm_cid`).

### RPC Framing (`amba_virt_cavalry_rpc`)
Compact 40-byte control structure:
```c
struct amba_virt_cavalry_rpc {
    __u32 opcode;          /* enum vcav_opcode */
    __s32 status;          /* 0 on success, negative errno on error */
    __u32 session_id;      /* Guest open file session token */
    __u32 dag_id;          /* Registered DAG ID or handle_id */
    __u32 bar_offset;      /* BAR offset returned or staging offset */
    __u32 size;            /* Allocation or buffer size */
    __u32 arena_len;       /* Length of serialized blob in RPC Arena */
    __u32 rval;            /* VP completion return code */
    __u32 exec_ticks;      /* VP hardware execution ticks */
    __u32 chip_id;         /* Hardware Chip ID from host */
};
```

### Opcodes (`enum vcav_opcode`)
- `VCAV_OP_ALLOC_MEM (1)`: Allocate Path A slice in `[32 MiB, Window End)`.
- `VCAV_OP_FREE_MEM (2)`: Free Path A slice.
- `VCAV_OP_RUN_DAGS (4)`: Path A transparent DAG dispatch.
- `VCAV_OP_CLOSE_SESSION (20)`: Teardown all handles and registered DAGs for session.
- `VCAV_OP_REGISTER_DAG (21)`: Deep-copy DVI from BAR staging to Host AMA.
- `VCAV_OP_UNREGISTER_DAG (22)`: Free Host AMA model allocation.
- `VCAV_OP_ALLOC_HANDLE (23)`: Allocate opaque I/O tensor handle.
- `VCAV_OP_FREE_HANDLE (24)`: Release tensor handle.
- `VCAV_OP_RUN_REGISTERED_DAG (25)`: Path B registered dispatch using handle IDs.

---

## 9. Arbitration & Concurrency

The host server (`amba-virt-server`) serializes access to the single physical VisORC core:
1. **Core Serialization (`arena_mutex`)**: A process-level mutex serializes hardware execution across all worker threads and guest tenants.
2. **Memory Barriers**: Explicit `dma_wmb()` before kicking VisORC and `dma_rmb()` upon job completion interrupt.
3. **Session Reclamation**: Guest disconnect or unexpected process termination automatically triggers `cavalry_proxy_close_session()`, eliminating slice leaks.
4. **Hang Recovery**: If VisORC hangs (`MSG_RVAL_VP_HANG`), the server owns soft reset recovery via RCT without exposing hardware control to guest VMs.

---

---

## 10. QNX Neutrino RTOS 8.0 Architecture (EL0 Resource Manager)

On BlackBerry QNX Neutrino RTOS 8.0, the Cavalry frontend runs as a userspace POSIX Resource Manager (`amba-cavalry-resmgr`) rather than a kernel module:

1. **Pathname Space Registration**: Exposes `/dev/cavalry` via `resmgr_attach()`, allowing unmodified applications using `libnnctrl` and `libcavalry_mem` to run seamlessly.
2. **Bounds Negotiation**: Sends `AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ` over `libamba_virt` during initialization to acquire the tenant BAR memory pool and 1 MB control arena.
3. **Cross-Process User Pointer Marshaling**: In QNX microkernel architecture, client user pointers (`reg_u->run_dags_ptr`) cannot be dereferenced directly by the resource manager process. Memory is safely retrieved using `pread()` on `/proc/<pid>/as` with PID discovery via `MsgInfo(ctp->rcvid, &info)`.
4. **Zero-Copy Projection**: Implements `io_mmap` returning physical BAR offsets (`g_cav.bar_phys + offset`) to client address spaces.

---

## 11. Source Map

| Path | Role |
|---|---|
| `drivers/amba_virt/tools/amba-virt-server.c` | Multi-tenant host server daemon |
| `drivers/amba_virt/tools/cavalry_proxy.c` | Host proxy, Path A/B handlers, AMA registration |
| `drivers/amba_virt/include/uapi/amba_virt.h` | 40-byte wire RPC ABI and protocol definitions |
| `guest-os/linux/amba-cavalry/amba_cavalry_hvm.c` | Linux guest kernel frontend driver (`/dev/cavalry`) |
| `guest-os/linux/amba-cavalry/include/cavalry_ioctl.h` | Multi-OS UAPI ioctl definitions (Linux + QNX) |
| `guest-os/linux/amba-cavalry/include/cavalry_ioctl_path_b.h` | Guest Path B IOCTL numbers (`0xC0..0xC4`) |
| `guest-os/qnx/amba-cavalry/amba_cavalry_resmgr.c` | QNX 8.0 `/dev/cavalry` POSIX resource manager |
| `guest-os/qnx/amba-virt/amba_virt_resmgr.c` | QNX 8.0 `/dev/amba_virt` transport resource manager |
| `guest-os/qnx/amba-virt/libamba_virt.c` | QNX 8.0 C client library (`libamba_virt.so` / `.a`) |
| `guest-os/userspace/nnctrl/` | Dual-engine userspace runtime library |
| `guest-os/apps/cavalry-demo/` | Multi-OS neural network test and validation harness |
| `guest-os/apps/cavalry-yolo/` | Multi-OS YOLO object detection inference application |
| `guest-os/linux/amba-gdma/amba_gdma_hvm.c` | Clamped GDMA virtual kernel driver |

