# Ambarella Virtualization Server Daemon (`amba-virt-server`) & Transport Architecture

*Copyright (C) 2026, Ambarella International LLC.*

## 1. Overview & System Role

The **`amba-virt-server`** is the privileged host-side virtualization daemon running within the EVE-OS **NOHYPER** container (`n1-655-devkit-nohyper` / `n1-655-pro-nohyper`). It acts as the central **hardware arbitrator, virtualization proxy, and control plane** connecting untrusted KVM HVM guest VMs (e.g., Ubuntu 24.04, Alpine Linux 3.20, QNX) to physical Ambarella silicon accelerators:
- **VisORC NPU (Cavalry)**: Deep learning neural network acceleration (`/dev/cavalry`).
- **GDMA Engine**: Hardware-accelerated 2D pitch copy and memory DMA (`/dev/gdma`).
- **Image Audio Video (IAV)**: Video sensor capture pipelines and DSP encoding (`/dev/iav`).

```text
+-----------------------------------------------------------------------------------------------+
| HVM Guest (Ubuntu / Alpine / QNX EL1)                                                         |
|                                                                                               |
|   +---------------------------------------+   +-------------------------------------------+   |
|   | Guest Applications / Cavalry Frontend |   | amba-virt-client (CppUTest / Benchmarks)  |   |
|   +---------------------------------------+   +-------------------------------------------+   |
|                      \                                     /                                  |
|                       v                                   v                                   |
|   +---------------------------------------------------------------------------------------+   |
|   |                         /dev/amba_virt (UAPI chardev)                                 |   |
|   +---------------------------------------------------------------------------------------+   |
|                                              |                                                |
|                                              v                                                |
|   +---------------------------------------------------------------------------------------+   |
|   |                   kmod/hvm/amba_virt.ko (PCI Driver for 1af4:1110)                    |   |
|   +---------------------------------------------------------------------------------------+   |
|                           |                                       |                           |
+---------------------------|---------------------------------------|---------------------------+
                            | VFS ioctl(SEND/RECV)                  | VFS mmap() / BAR 2
                            v                                       v
+-----------------------------------------------------------------------------------------------+
| EVE-OS / QEMU Hypervisor Layer                                                                |
|                                                                                               |
|   +-----------------------------------+               +-----------------------------------+   |
|   |  vhost-vsock-pci (CID 2, :5555)   |               |     ivshmem-plain (1af4:1110)     |   |
|   +-----------------------------------+               +-----------------------------------+   |
|                     |                                                   |                     |
|                     | AF_VSOCK stream                                   | memory-backend-file |
|                     v                                                   v (1 GiB DRAM window) |
+---------------------|---------------------------------------------------|---------------------+
                      |                                                   |
+---------------------|---------------------------------------------------|---------------------+
| NOHYPER Host (Ambarella SoC Privileged Dom0 Control Plane)              |                     |
|                                                                         |                     |
|   +---------------------------------------------------------------------------------------+   |
|   |                          kmod/nohyper/amba_virt.ko <----------------+                 |   |
|   +---------------------------------------------------------------------------------------+   |
|                                              |                                                |
|                                              v                                                |
|   +---------------------------------------------------------------------------------------+   |
|   |                        /dev/amba_virt (Dynamic chardev)                               |   |
|   +---------------------------------------------------------------------------------------+   |
|                                              |                                                |
|                                              v                                                |
|   +---------------------------------------------------------------------------------------+   |
|   |                 amba-virt-server (Daemon / Arbitrator / Proxy)                        |   |
|   |                 - Dynamic Extent Allocator (virt_mem_pool)                            |   |
|   |                 - Capability Access Control Engine (virt_acl)                         |   |
|   |                 - Guest Introspection & Topology Engine (virt_query)                  |   |
|   |                 - Dom0 Administrative Socket (/run/amba-virt/admin.sock)              |   |
|   +---------------------------------------------------------------------------------------+   |
|                                              |                                                |
|                                              v                                                |
|   +---------------------------------------------------------------------------------------+   |
|   |              Ambarella Hardware Drivers (/dev/cavalry, /dev/gdma, /dev/iav)           |   |
|   +---------------------------------------------------------------------------------------+   |
|                                              |                                                |
|                                              v                                                |
|   +---------------------------------------------------------------------------------------+   |
|   |       Ambarella Silicon Hardware (VisORC NPU, GDMA, IDSP / VIN, RCT)                  |   |
|   +---------------------------------------------------------------------------------------+   |
|                                                                                               |
+-----------------------------------------------------------------------------------------------+
```

### 1.1 The NOHYPER Container as Dom0 Linux
Architecturally, the NOHYPER container functions analogously to **Xen's Dom0 Linux** (or the Hyper-V Root Partition / KVM host control domain):
- **Privileged Host Execution Domain**: NOHYPER runs as an unconfined Ubuntu container on EVE-OS with direct access to physical silicon character devices (`/dev/cavalry`, `/dev/gdma`, `/dev/iav`) and shared memory backing files.
- **Rich Userspace Tooling**: Because NOHYPER is a full Ubuntu Linux environment (standard glibc, bash, Python, GCC, coreutils), it hosts both the server daemon (`amba-virt-server`) and the dedicated host management utility (**`amba-virt-ctl`**).
- **Omnipotent Administrative Authority**: Just as Xen Dom0 possesses hypervisor control privileges over unprivileged guest domains (DomU), NOHYPER Dom0 possesses **unconditional root authority to inspect, configure, modify, throttle, resize quotas, or quarantine (reclaim vsock/AMA/handles)** any guest VM on the platform.

```text
+-----------------------------------------------------------------------------------------------+
| NOHYPER Container (Ambarella Dom0 Linux Control Plane)                                        |
| - Full Ubuntu 24.04 Environment (glibc, Python, GCC, system tooling)                          |
| - Root Silicon Access: /dev/cavalry, /dev/gdma, /dev/iav, /dev/amba_virt                      |
|                                                                                               |
|   +---------------------------------------+   +-------------------------------------------+   |
|   | amba-virt-ctl (Dom0 Management CLI)   |   | Persistent Store (/persist/policies.json) |   |
|   +---------------------------------------+   +-------------------------------------------+   |
|                      \                                     /                                  |
|                       v                                   v                                   |
|   +---------------------------------------------------------------------------------------+   |
|   | amba-virt-server (Arbitrator Daemon)                                                  |   |
|   | - Admin IPC: /run/amba-virt/admin.sock (0600 file permission root auth)               |   |
|   | - Extent Allocator, ACL Engine, Reaper, Introspection Query Engine                    |   |
|   +---------------------------------------------------------------------------------------+   |
|                                              |                                                |
+----------------------------------------------|------------------------------------------------+
                                               |
             +---------------------------------+---------------------------------+
             | AF_VSOCK :5555 (Control Plane)  | ivshmem BAR 2 (Shared Memory)   |
             v                                 v                                 v
+-----------------------------+ +-----------------------------+ +-------------------------------+
| DomU: Ubuntu 24.04 HVM      | | DomU: Alpine Linux 3.20 HVM | | DomU: QNX 8.0 HVM             |
| (Tenant A)                  | | (Tenant B)                  | | (Tenant C)                    |
| - amba_cavalry.ko (insmod)  | | - amba_cavalry.ko (insmod)  | | - qnx_cavalry resmgr          |
| - amba_gdma.ko    (insmod)  | | - amba_gdma.ko    (insmod)  | | - qnx_gdma driver             |
| - In-Band Negotiator        | | - Inherits Dom0 Defaults    | | - Real-Time Control           |
+-----------------------------+ +-----------------------------+ +-------------------------------+
```

### 1.2 The Hypervisor / BAR Hard Limit Boundary
While NOHYPER acts as an administrative control plane, `amba-virt-server` does **not** own EVE/QEMU physical memory allocation or VM lifecycle. It is constrained by a fundamental physical hard limit:
- **ivshmem BAR Constraints**: The daemon cannot allocate memory beyond the domain-create-time ivshmem BAR provisioned by the hypervisor (typically a static 1 GiB window per guest).
- **Asymmetric Sizing Requires Reboot**: Any attempt to provision quotas beyond this static window (e.g., expanding a guest to 1.5 GiB) requires altering the underlying EVE/QEMU model definitions (via ZEDEDA Cloud / `zcli`) and rebooting the guest VM. `amba-virt-server` dynamically sub-slices, manages, and arbitrates memory strictly within the bounds of this pre-existing mapped BAR.

---

## 2. Foundational Transport Architecture

The virtualization transport employs two separate channels to optimize control latency and bulk data bandwidth:

### 2.1 One BAR, Many Frontends
Rather than provisioning separate PCI apertures for each virtualized peripheral, each HVM guest is provisioned with a single `ivshmem-plain` device (`1af4:1110`, BAR 2). All virtualized frontends (Cavalry NPU, GDMA channels, and future IAV camera buffers) share this single aperture (`cbattr.shmsize` in EVE models, 1 GiB production). Offsets and buffer lengths within the window are coordinated over vsock control messages.

### 2.2 Channel Separation & Transport Roles
- **Control Plane (`virtio-vsock`)**:
  - Delivers framed, synchronous and asynchronous control RPCs (initialization, memory allocation, job dispatch, completion interrupts).
  - The guest connects to host **CID 2** on standard service Port `5555`. The host daemon dynamically demultiplexes incoming tenant sessions via the kernel-authenticated caller Context ID (`peer_addr.svm_cid`). Port `2000` is reserved for EVE VComLink management and is explicitly rejected.
- **Data Plane (`ivshmem`)**:
  - Provides zero-copy shared DRAM mapping between guest user space and host physical memory.
  - Payloads (image frames, neural network weight tensors, activation maps) reside directly in shared memory; only 32-bit offsets and size descriptors traverse the vsock control channel.

### 2.3 Compilation Matrix

| Execution Domain | Component | Role | Headers / Toolchain |
|---|---|---|---|
| **HVM Guest** | `amba_virt.ko` | PCI ivshmem BAR2 driver + kernel vsock client | Distro `linux-headers-$(uname -r)` |
| **HVM Guest** | `amba_cavalry.ko` | Guest `/dev/cavalry` frontend driver | Distro kernel headers |
| **HVM Guest** | `amba_gdma.ko` | Guest kernel GDMA export driver | Distro kernel headers |
| **HVM Guest** | `amba-virt-client` | CppUTest functional test suite & IPC benchmark | `g++` (`-std=c++17`, `-lpthread`) |
| **NOHYPER Host** | `amba_virt.ko` | Shared memory backing file mmap + vsock server | Ambarella `eve-kernel` |
| **NOHYPER Host** | `amba-virt-server` | Arbitrator daemon, memory pool, ACL, proxy | `gcc` (`-std=c11`, `-lpthread`) |
| **NOHYPER Host** | `amba-virt-ctl` | Dom0 host administration CLI utility | `gcc` (`-std=c11`, `-lpthread`) |

---

## 3. Server Daemon Architecture & Internals

The `amba-virt-server` source code resides in [`drivers/amba_virt/tools/`](../drivers/amba_virt/tools/):
- **`amba-virt-server.c`**: Core daemon lifecycle, multi-tenant socket listener, client connection worker threads, and message routing.
- **`virt_mem_pool.c` / `virt_mem_pool.h`**: Extent-based dynamic memory pool manager, collision detection, and sub-aperture carving.
- **`virt_acl.c` / `virt_acl.h`**: Capability-based Access Control List engine and persistent policy manager.
- **`virt_query.c` / `virt_query.h`**: Guest self, peer, and silicon topology introspection query subsystem.
- **`virt_admin_ipc.c` / `virt_admin_ipc.h`**: Local Dom0 administration UNIX socket listener (`/run/amba-virt/admin.sock`).
- **`cavalry_proxy.c` / `cavalry_proxy.h`**: Ambarella Cavalry virtualization proxy, AMA memory management, handle tracking, and VisORC hardware arbitration.

### 3.1 Multi-Tenant Connection Model

```text
                        +---------------------------------------------+
                        | amba-virt-server main()                     |
                        | - Binds AF_VSOCK (VMADDR_CID_ANY, Port 5555)|
                        | - Sets up signal handlers (SIGINT, SIGTERM) |
                        | - Initializes virt_mem_pool & virt_acl      |
                        | - Inits cavalry_proxy (maps Host AMA & BAR) |
                        | - Binds Dom0 admin.sock (/run/amba-virt/)   |
                        +---------------------------------------------+
                                               |
                                               v
                        +---------------------------------------------+
                        | Connection Accept Loop                      |
                        | accept() -> spawns pthread worker per client|
                        +---------------------------------------------+
                                       /              \
                                      v                v
                 +-----------------------+          +-----------------------+
                 | Worker Thread (CID 15)|          | Worker Thread (CID 6) |
                 | Tenant 0: Ubuntu HVM  |          | Tenant 1: Alpine HVM  |
                 | - Reads wire RPC frame|          | - Reads wire RPC frame|
                 | - Validates tenant CID|          | - Validates tenant CID|
                 | - Checks ACL bitmask  |          | - Checks ACL bitmask  |
                 | - Dispatches to proxy |          | - Dispatches to proxy |
                 +-----------------------+          +-----------------------+
```

### 3.2 Subsystem Request Dispatch Pipeline

```text
+-----------------------------------------------------------------------------------------------+
| amba-virt-server Internal Subsystem Architecture & Request Pipeline                           |
|                                                                                               |
|                         +-----------------------------------+                                 |
|                         | AF_VSOCK Listener (Port 5555)     |                                 |
|                         +-----------------------------------+                                 |
|                                           |                                                   |
|                                           v                                                   |
|                         +-----------------------------------+                                 |
|                         | Client Worker Thread Pool         |                                 |
|                         | - getpeername() -> Kernel CID     |                                 |
|                         +-----------------------------------+                                 |
|                                           |                                                   |
|                                           v                                                   |
|                         +-----------------------------------+                                 |
|                         | Ingress ACL & Capability Filter   |                                 |
|                         | (virt_acl: validate CID & caps)   |                                 |
|                         +-----------------------------------+                                 |
|                             /         |           |        \                                  |
|                            /          |           |         \                                 |
|                           v           v           v          v                                |
|             +-----------------+ +-----------+ +-------+ +------------------+                  |
|             | virt_mem_pool   | | cavalry_  | | gdma_ | | virt_query       |                  |
|             | - Extent Alloc  | | proxy     | | arb   | | - QUERY_SELF     |                  |
|             | - Quota Bounds  | | - Path A  | | - 1D  | | - QUERY_PEERS    |                  |
|             | - Slicing/Carve | | - Path B  | | - 2D  | | - QUERY_TOPO     |                  |
|             +-----------------+ +-----------+ +-------+ +------------------+                  |
|                       ^               |           |                                           |
|                       |               v           v                                           |
|                       |         +---------------------------+                                 |
|                       |         | Hardware Mutex Monitors   |                                 |
|                       |         | (visorc_hw_mutex /        |                                 |
|                       |         |  gdma_hw_mutex)           |                                 |
|                       |         +---------------------------+                                 |
|                       |                       |                                               |
|                       |                       v                                               |
|                       |         +---------------------------+                                 |
|                       |         | Ambarella Silicon Drivers |                                 |
|                       |         | (/dev/cavalry, /dev/gdma) |                                 |
|                       |         +---------------------------+                                 |
|                       |                                                                       |
|   +-------------------------------------------------------------+                             |
|   | Dom0 Administrative Control Channel                         |                             |
|   | amba-virt-ctl  <--->  UNIX Socket (/run/amba-virt/admin.sock) |                             |
|   | - Dynamic Quota Override   - Live Tenant Eviction / Reaper  |                             |
|   | - Persistent Policy Sync   - System Topology & Diagnostics  |                             |
|   +-------------------------------------------------------------+                             |
+-----------------------------------------------------------------------------------------------+
```

### 3.3 Wire RPC Protocol Framing

Control messages are exchanged using compact wire control structures defined in [`include/uapi/amba_virt.h`](../drivers/amba_virt/include/uapi/amba_virt.h).

For legacy and standard inference operations, the 40-byte `struct amba_virt_cavalry_rpc` is utilized:

```c
struct amba_virt_cavalry_rpc {
    __u32 opcode;          /* enum vcav_opcode (ALLOC, RUN, REGISTER, etc.) */
    __s32 status;          /* 0 on success, negative errno on error */
    __u32 session_id;      /* Guest open file session token */
    __u32 dag_id;          /* Registered DAG ID or handle_id */
    __u32 bar_offset;      /* BAR offset returned or staging offset */
    __u32 size;            /* Allocation or buffer size */
    __u32 arena_len;       /* Length of serialized blob in RPC Arena */
    __u32 rval;            /* VP hardware completion return code */
    __u32 exec_ticks;      /* VP hardware execution ticks */
    __u32 chip_id;         /* Hardware Chip ID from host */
};
```

When variable-sized descriptors (e.g. `struct cavalry_run_dags` or model registration metadata) are sent, the client copies the payload into the dedicated **RPC Arena** in shared memory (`CAVALRY_RPC_ARENA_OFFSET`), and passes `arena_len` in the RPC frame. The host server deep-copies the arena contents into private RAM before parsing, eliminating TOCTOU inspection races.

### 3.4 Access Control List (ACL) & Capability Policy Engine (`virt_acl`)

#### Kernel-Authenticated Identity
Vhost-vsock binds an immutable Context ID (CID) to each guest VM at domain creation. When a connection is received on Port 5555, `amba-virt-server` extracts the caller's CID via `getpeername()`. Because this CID is provided directly by the host Linux kernel, it is impossible for guest userspace or guest kernel code to forge.

#### Granular Capability Bitmask
```c
#define AMBA_VIRT_CAP_NONE              0x00000000U
#define AMBA_VIRT_CAP_PING              0x00000001U /* Ping / pong / echo / benchmark */
#define AMBA_VIRT_CAP_QUERY_SELF        0x00000002U /* Query own info & memory map */
#define AMBA_VIRT_CAP_QUERY_PEERS       0x00000004U /* Query other tenant existence & status */
#define AMBA_VIRT_CAP_QUERY_TOPO        0x00000008U /* Query silicon chip & device topology */
#define AMBA_VIRT_CAP_MEM_ALLOC         0x00000010U /* Request dynamic memory allocation */
#define AMBA_VIRT_CAP_MEM_RESIZE        0x00000020U /* Resize existing memory allocations */
#define AMBA_VIRT_CAP_DEV_CONFIG        0x00000040U /* In-band device boundary declaration at insmod */
#define AMBA_VIRT_CAP_GDMA_COPY         0x00000080U /* 1D linear GDMA transfers */
#define AMBA_VIRT_CAP_GDMA_PITCH        0x00000100U /* 2D pitch GDMA transfers */
#define AMBA_VIRT_CAP_CAVALRY_PATH_B    0x00000200U /* Hardened Path B model registration & exec */
#define AMBA_VIRT_CAP_CAVALRY_PATH_A    0x00000400U /* Legacy unhardened Path A (restricted!) */
#define AMBA_VIRT_CAP_CAVALRY_REGISTER  0x00000800U /* Ingest new DVI models into host AMA */
#define AMBA_VIRT_CAP_IAV_STREAM        0x00001000U /* Video stream ingestion */

/* Note: AMBA_VIRT_ROLE_ADMIN (0xFFFFFFFF) is strictly reserved for the Dom0 UNIX socket (admin.sock).
 * It must NEVER be assigned as a vsock capability bitmask. ACL mutation, policy changes, and eviction 
 * belong ONLY on the admin.sock listener. */

/* Standard Predefined Roles */
#define AMBA_VIRT_ROLE_UNTRUSTED \
    (AMBA_VIRT_CAP_PING | AMBA_VIRT_CAP_QUERY_SELF | AMBA_VIRT_CAP_CAVALRY_PATH_B | AMBA_VIRT_CAP_CAVALRY_REGISTER)

#define AMBA_VIRT_ROLE_STANDARD \
    (AMBA_VIRT_ROLE_UNTRUSTED | AMBA_VIRT_CAP_MEM_ALLOC | AMBA_VIRT_CAP_DEV_CONFIG | \
     AMBA_VIRT_CAP_GDMA_COPY | AMBA_VIRT_CAP_GDMA_PITCH)
```

---

## 4. Dom0 Control Plane & Administrative Tooling (`amba-virt-ctl`)

Because NOHYPER is a full Ubuntu Linux container, `amba-virt-ctl` is compiled as a rich native userspace utility. It interfaces directly with `amba-virt-server` over a dedicated local UNIX domain socket:
- **Socket Path**: `/run/amba-virt/admin.sock`
- **Authentication**: Socket file permissions strictly set to `0600` (asserting root access within container). `SO_PEERCRED` alone is insufficient as it applies to all processes in the container.

### 4.1 Dom0 Management Commands Suite
```bash
# 1. Platform & Tenant Overview
amba-virt-ctl status
amba-virt-ctl list-guests

# 2. Detailed Guest Inspection
amba-virt-ctl get-guest 15

# 3. Dynamic Runtime Policy & Quota Adjustment
amba-virt-ctl set-quota 15 1024M
amba-virt-ctl set-priority 15 high
amba-virt-ctl set-acl 15 standard

# 4. Forceful Boundary Override
amba-virt-ctl set-bounds 15 cavalry 0x04000000 768M

# 5. Immediate Rogue Guest Quarantine & Teardown
amba-virt-ctl evict 15

# 6. Interactive Persistent Policy Editor
amba-virt-ctl policy edit 15
amba-virt-ctl policy export 15 -o /persist/ubuntu-perception.policy
amba-virt-ctl policy import /persist/ubuntu-perception.policy
```

### 4.2 Persistent Storage in Edge-App Volume (`/persist`)
- Policies are written to the edge-app mounted volume `/persist/etc/amba-virt/policies.json` or `/etc/amba-virt/policy.d/<tenant>.json`.
- On Ambarella EVE-OS, `/persist` is the persistent writable storage partition passed into NOHYPER that survives reboots, powercycles, and OTA firmware updates.
- Modifying policies via `amba-virt-ctl` immediately notifies `amba-virt-server` via `/run/amba-virt/admin.sock`, reloading rules dynamically without terminating running guest sessions.

---

## 5. Dynamic Memory Management & Extent-Based Slicing (`VIRT_MEM`)

### 5.1 Limitations of Rigid Static Slices
Previously, `amba-virt-server` registered connecting guests into slot $i$ with a hardcoded $1\text{ GiB}$ slice offset ($i \times 1\text{ GiB}$). Within the 1 GiB hypervisor BAR aperture:
- **Inefficient Quota Enforcement**: A small Alpine monitoring VM only requires a 128 MiB quota, but the rigid static layout prevented enforcing smaller quotas.
- **No Dynamic Sub-Allocation**: Guest VMs could not carve out dedicated sub-apertures for specific virtual devices dynamically.

### 5.2 Dynamic Extent-Based Memory Pool Manager (`virt_mem_pool`)
`amba-virt-server` incorporates an internal **Dynamic Memory Extent Allocator**:
- **Block Granularity**: Configurable chunk size (2 MiB / 4 MiB alignment).
- **Per-Tenant Quota**:
  - `quota_min`: Minimum reserved memory guaranteed to tenant.
  - `quota_max`: Maximum ceiling memory tenant is permitted to allocate.
  - `allocated_bytes`: Current live allocated memory.
- **Dynamic Virtual Device Slicing**:
  - Tenants can carve out dedicated sub-apertures within their memory allocation for specific virtual devices:
    - **GDMA Aperture**: Custom bounds within the guest slice.
    - **Cavalry RPC Arena**: 1–2 MiB control buffer.
    - **Cavalry Tensor Buffer Pool**: Dynamic tensor input/output pool.
    - **Scratchpad / General SHM**: Generic zero-copy buffer space.
    - **IAV Framebuffer Pool (Future)**: Camera stream ring buffers.

```text
+-----------------------------------------------------------------------------------------------+
| Guest ivshmem BAR 2 Memory Aperture (Physical Limit: 1 GiB = 0x40000000)                      |
|                                                                                               |
| Base: 0x00000000                                                             Top: 0x40000000  |
| +-----------+----------+----------------------------+-----------+---------------------------+ |
| | GDMA      | Reserved | Cavalry Virtual Subsystem  | Scratch / | Unallocated Extent Pool   | |
| | Aperture  | Alignment| (Pool Size: e.g. 768 MiB)  | SHM Ring  | (Available for expansion  | |
| | (e.g. 64M)| (2 MiB)  | - RPC Arena: 1-2 MiB       | (e.g.     |  up to tenant quota_max)  | |
| |           |          | - Tensor Buffers (DVI/AMA) |  188 MiB) |                           | |
| +-----------+----------+----------------------------+-----------+---------------------------+ |
| |<--------------------------- Active Allocated Extents ------------------------>|             | |
| |<----------------------- Tenant Quota Ceiling (e.g. 1024 MiB) ---------------->|             | |
| |<============================= Hypervisor BAR Hard Limit (1 GiB) ===========================>| |
+-----------------------------------------------------------------------------------------------+
```

### 5.3 Wire Protocol Definitions for Memory Management

```c
/* Memory Request Types */
enum amba_virt_mem_op {
    AMBA_VIRT_MEM_OP_ALLOC   = 1, /* Allocate new extent/slice */
    AMBA_VIRT_MEM_OP_FREE    = 2, /* Release extent/slice */
    AMBA_VIRT_MEM_OP_RESIZE  = 3, /* Expand or shrink slice */
    AMBA_VIRT_MEM_OP_CARVE   = 4, /* Sub-allocate device aperture */
};

struct amba_virt_mem_req {
    __u32 op;              /* enum amba_virt_mem_op */
    __u32 size;            /* Requested bytes */
    __u32 align;           /* Alignment (default 4096 or 2 MiB) */
    __u32 dev_affinity;    /* 0=GENERIC, 1=CAVALRY, 2=GDMA, 3=IAV */
    __u32 bar_offset;      /* Offset for FREE or RESIZE */
    __u32 flags;           /* Allocation flags (e.g. ZERO_INIT) */
};

struct amba_virt_mem_resp {
    __s32 status;          /* 0 on success, negative errno */
    __u32 bar_offset;      /* Allocated guest BAR offset */
    __u32 allocated_size;  /* Actual allocated size (aligned) */
    __u64 phys_addr;       /* Physical HPA (0 if unprivileged) */
    __u32 total_allocated; /* Tenant current total allocated */
    __u32 total_free;      /* Tenant remaining quota */
};
```

---

## 6. 2-Tier Resource Negotiation & In-Band Device Slicing

### 6.1 The Grand Unified 2-Tier Architecture

Rather than treating host-configured policies and guest dynamic drivers as mutually exclusive, `amba-virt-server` integrates them into a **2-tier resource negotiation architecture**:

| Tier | Role | Component | Authority |
|---|---|---|---|
| **Tier 1: Guardrails & Default Policy** | Authoritative Quota Ceiling, ACL Capabilities, Default Device Layout | `amba-virt-ctl` on NOHYPER Dom0 & `/persist/etc/amba-virt/policies.json` | Host Administrator / Platform Integrator |
| **Tier 2: In-Band Boundary Declaration** | Fine-Tuned Memory Sizing at `insmod` time | Guest Drivers (`amba_cavalry.ko`, `amba_gdma.ko`) in DomU via vsock | Edge-App Designer |

```text
+-----------------------------------------------------------------------------------------------+
| 2-Tier Resource Negotiation & Device Memory Slicing Architecture                              |
|                                                                                               |
|   TIER 1: Host Guardrails & Default Policy (Dom0 Control Plane)                               |
|   +---------------------------------------------------------------------------------------+   |
|   | Host Administrator / Platform Integrator                                              |   |
|   | - Configures /persist/etc/amba-virt/policies.json via amba-virt-ctl                   |   |
|   | - Defines absolute quota ceilings, ACL capability bitmasks, priority levels           |   |
|   | - Provides fallback device partitions for out-of-the-box guests (e.g. YOLOX)          |   |
|   +---------------------------------------------------------------------------------------+   |
|                                              |                                                |
|                                              v                                                |
|   +---------------------------------------------------------------------------------------+   |
|   | amba-virt-server Host Arbitration Engine (in NOHYPER Dom0)                            |   |
|   | - Enforces host quota ceiling as immutable upper bound                                |   |
|   | - Checks AMBA_VIRT_CAP_DEV_CONFIG capability on in-band requests                      |   |
|   | - Evaluates spatial collisions and computes suggested_offset on overlap               |   |
|   | - Serializes concurrent driver registrations via tenant_mem_lock                      |   |
|   +---------------------------------------------------------------------------------------+   |
|                                              ^                                                |
|                                              | AF_VSOCK Port 5555 (In-Band Wire Protocol)     |
|   TIER 2: Dynamic In-Band Slicing (DomU Guest Execution Plane)                            |
|   +---------------------------------------------------------------------------------------+   |
|   | Edge-App Designer / Guest Kernel Drivers                                              |   |
|   | - amba_cavalry.ko pool_size=768M rpc_arena_size=2M preferred_offset=auto              |   |
|   | - amba_gdma.ko    aperture_size=64M            preferred_offset=auto                  |   |
|   | - Dispatches AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ at driver probe / insmod time           |   |
|   | - Performs automated collision recovery via suggested_offset                          |   |
|   +---------------------------------------------------------------------------------------+   |
+-----------------------------------------------------------------------------------------------+
```

### 6.2 Deterministic Conflict Resolution Matrix

When multiple drivers inside the same guest (or across different guests) request memory, conflicts can arise. The host server enforces the following deterministic conflict resolution policies:

| Conflict Scenario | Description / Trigger | Server Resolution Policy | Error Code & Return Payload |
|---|---|---|---|
| **1. Explicit Offset Collision** | Driver requests a fixed `preferred_offset`, but that range overlaps with another active device in the tenant's BAR. | The server **rejects** the colliding range. It scans the tenant's extent map for the next available aligned region and provides it as a recommendation. | `status = -EEXIST`<br>`err_code = AMBA_VIRT_ERR_OFFSET_COLLISION`<br>`suggested_offset = <next_free>`<br>`colliding_dev = <dev_type>` |
| **2. Dynamic Auto Offset (No Collision)** | Drivers specify `preferred_offset = AMBA_VIRT_OFFSET_AUTO` (`0xFFFFFFFF`). | The server uses a first-fit extent allocator to place the aperture into disjoint, non-overlapping space. Guarantees **zero collision**. | `status = 0`<br>`granted_offset = <assigned>` |
| **3. Concurrent Driver Load Race** | Multiple drivers (`amba_cavalry` and `amba_gdma`) load simultaneously via `systemd` or background scripts. | Tenant-level mutex (`pthread_mutex_t tenant_mem_lock`) serializes requests atomically. First request claims space; second request receives adjacent space without race conditions. | Strictly serialized; both succeed with non-overlapping offsets. |
| **4. Quota Ceiling Exceeded (Exact)** | Driver requests size exceeding the tenant's remaining memory quota, with `flags & AMBA_VIRT_DEV_F_EXACT`. | The server **rejects** the request without altering active allocations. It returns the maximum currently available bytes in `max_avail_size`. | `status = -ENOMEM`<br>`err_code = AMBA_VIRT_ERR_QUOTA_EXCEEDED`<br>`max_avail_size = <remaining_bytes>` |
| **5. Quota Ceiling Exceeded (Best Effort)** | Driver specifies `flags & AMBA_VIRT_DEV_F_BEST_EFFORT`. | **Note:** Best-effort clamp is unsafe for Cavalry and GDMA (they must use exact-or-fail). For safe devices (e.g. scratch buffers), server **clamps** the allocation to max available quota. | `status = 0`<br>`granted_size = <clamped_size>`<br>`max_avail_size = <clamped_size>` |
| **6. Duplicate Device Re-Registration** | `amba_cavalry` loads while an active Cavalry device is already registered for this tenant (e.g. previous crash or reload). | If `flags & AMBA_VIRT_DEV_F_REPLACE` is NOT set: server rejects to prevent clobbering live sessions.<br>If `flags & AMBA_VIRT_DEV_F_REPLACE` is set: server performs deep teardown of old sessions and registers the new slice. | Without replace:<br>`status = -EBUSY`<br>`err_code = AMBA_VIRT_ERR_DEV_ALREADY_REG`<br>With replace:<br>`status = 0` (old torn down, new registered). |
| **7. Granularity & Alignment Discrepancy** | Requested size or offset is not aligned to the required boundary (e.g. 2 MiB boundary for VisORC NPU). | Server rounds up requested size or rejects unaligned offset with alignment mask. | `status = -EINVAL`<br>`err_code = AMBA_VIRT_ERR_INVALID_ALIGN`<br>`suggested_offset = ALIGN_2MB(offset)` |
| **8. Permission / ACL Violation** | Untrusted guest CID attempts to configure a device aperture without holding `AMBA_VIRT_CAP_DEV_CONFIG`. | Server drops request immediately and logs security event. | `status = -EPERM`<br>`err_code = AMBA_VIRT_ERR_PERM_DENIED` |
| **9. Total BAR Aperture Overflow** | Requested offset + size exceeds the physical ivshmem BAR2 window provisioned by the hypervisor. | Server rejects request as physically unmappable. | `status = -ERANGE`<br>`err_code = AMBA_VIRT_ERR_BAR_OVERFLOW` |

### 6.3 Structured Protocol Framing & Error Return Protocol

```c
/* Virtual Device Types */
#define AMBA_VIRT_DEV_TYPE_NONE        0u
#define AMBA_VIRT_DEV_TYPE_CAVALRY     1u
#define AMBA_VIRT_DEV_TYPE_GDMA        2u
#define AMBA_VIRT_DEV_TYPE_IAV         3u
#define AMBA_VIRT_DEV_TYPE_SCRATCH     4u
#define AMBA_VIRT_MAX_DEV_TYPES        8u

/* Automatic Offset Placement Sentinel */
#define AMBA_VIRT_OFFSET_AUTO          0xFFFFFFFFU

/* Request Flags */
#define AMBA_VIRT_DEV_F_NONE           0x00000000U
#define AMBA_VIRT_DEV_F_EXACT          0x00000001U /* Reject if exact size/offset cannot be met */
#define AMBA_VIRT_DEV_F_BEST_EFFORT    0x00000002U /* Allow clamping to maximum available quota */
#define AMBA_VIRT_DEV_F_REPLACE        0x00000004U /* Force replacement of existing allocation */
#define AMBA_VIRT_DEV_F_ZERO_INIT      0x00000008U /* Zero-initialize shared memory extent */

/* Granular Error / Reason Codes */
enum amba_virt_err_code {
    AMBA_VIRT_ERR_NONE             = 0,
    AMBA_VIRT_ERR_PERM_DENIED      = 1, /* Missing capability or unauthorized tenant */
    AMBA_VIRT_ERR_QUOTA_EXCEEDED   = 2, /* Requested size exceeds tenant quota */
    AMBA_VIRT_ERR_HOST_OOM         = 3, /* Host shared physical pool exhausted */
    AMBA_VIRT_ERR_OFFSET_COLLISION = 4, /* Explicit offset collides with active allocation */
    AMBA_VIRT_ERR_DEV_ALREADY_REG  = 5, /* Device already registered (requires F_REPLACE) */
    AMBA_VIRT_ERR_INVALID_ALIGN    = 6, /* Offset or size violates alignment (2 MiB / 4 KiB) */
    AMBA_VIRT_ERR_UNKNOWN_DEVICE   = 7, /* Unsupported virtual device type */
    AMBA_VIRT_ERR_BAR_OVERFLOW     = 8, /* Offset + size exceeds hypervisor BAR window */
};

/* In-Band Device Boundary Request */
struct amba_virt_dev_bounds_req {
    __u32 dev_type;          /* enum amba_virt_dev_type */
    __u32 requested_size;    /* Requested aperture size in bytes */
    __u32 preferred_offset;  /* Base BAR offset, or AMBA_VIRT_OFFSET_AUTO */
    __u32 rpc_arena_size;    /* Dedicated control/RPC arena size (e.g. 1 MiB) */
    __u32 align;             /* Required alignment (default 2 MiB) */
    __u32 flags;             /* Bitmask of AMBA_VIRT_DEV_F_* */
};

/* In-Band Device Boundary Response */
struct amba_virt_dev_bounds_resp {
    __s32 status;            /* 0 on success, negative errno (-EPERM, -ENOMEM, -EEXIST, etc.) */
    __u32 err_code;          /* enum amba_virt_err_code for diagnostic classification */
    __u32 granted_offset;    /* Confirmed base offset in guest BAR */
    __u32 granted_size;      /* Confirmed allocated size in bytes */
    __u32 rpc_arena_offset;  /* Confirmed RPC arena offset */
    __u32 max_avail_size;    /* Maximum bytes currently available (for recovery on -ENOMEM) */
    __u32 suggested_offset;  /* Next free aligned offset (for recovery on -EEXIST) */
    __u32 colliding_dev;     /* Device type causing collision if status == -EEXIST */
};
```

### 6.4 Guest Driver Error Recovery Protocol & Flowchart

```text
[insmod amba_cavalry.ko]
           |
           v
Issue AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ
           |
           v
    Check resp.status
       /    |     \
      /     |      \
status=0   |     status < 0
    |       |          |
    v       |          +-------------------------------------------------------+
Config     |          |                                                       |
Memory     |          v                                                       v
Pool       |     status == -EEXIST                                       status == -ENOMEM
    |       |   (Offset Collision)                                       (Quota Exceeded)
    v       |          |                                                       |
Success    |          v                                                       v
(dmesg)    |     Auto-retry at                                           Did driver set
           |     resp.suggested_offset?                                  BEST_EFFORT?
           |     - If YES: Re-issue REQ                                       |
           |     - If NO: Fail insmod with                                    v
           |       "offset collides with dev %u"                         Fail insmod with
           |                                                             "requested %u MB >
           |                                                              quota %u MB"
           |
           +----------------------------------+
           |                                  |
           v                                  v
     status == -EPERM                   status == -EBUSY
    (Permission Denied)               (Already Registered)
           |                                  |
           v                                  v
     Fail insmod with                   Fail insmod with
     "Host ACL denied cap 0x%08x"       "Dev active; use force_replace=1"
```

### 6.5 Concrete Driver Code Implementation Pattern (`amba_cavalry_hvm.c`)

```c
static int amba_cavalry_negotiate_bounds(struct amba_cavalry_dev *cav)
{
    struct amba_virt_dev_bounds_req req;
    struct amba_virt_dev_bounds_resp resp;
    int ret;
    int retries = 1;

    memset(&req, 0, sizeof(req));
    req.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
    req.requested_size = g_pool_size;
    req.preferred_offset = g_preferred_offset; /* default AMBA_VIRT_OFFSET_AUTO */
    req.rpc_arena_size = g_rpc_arena_size;     /* default 1 MiB */
    
    /* Best-effort clamp is unsafe for Cavalry. Must use exact-or-fail. */
    req.flags = AMBA_VIRT_DEV_F_EXACT;

    if (g_force_replace)
        req.flags |= AMBA_VIRT_DEV_F_REPLACE;

retry:
    /* Add vsock timeout (e.g. 5 seconds) to prevent infinite probe hang */
    ret = amba_virt_send_dev_bounds_req_timeout(&req, &resp, 5000);
    if (ret) {
        pr_err("amba_cavalry: vsock RPC transport failure or timeout (%d)\n", ret);
        return ret;
    }

    if (resp.status == 0) {
        cav->pool_base = resp.granted_offset;
        cav->pool_size = resp.granted_size;
        cav->rpc_arena_offset = resp.rpc_arena_offset;
        pr_info("amba_cavalry: registered at BAR offset 0x%08x (%u MB), arena at 0x%08x\n",
                cav->pool_base, cav->pool_size / (1024 * 1024), cav->rpc_arena_offset);
        return 0;
    }

    /* Structured Error Handling & Diagnostics */
    switch (resp.status) {
    case -EEXIST:
        pr_warn("amba_cavalry: offset 0x%08x collides with active dev %u\n",
                req.preferred_offset, resp.colliding_dev);
        if (retries > 0 && req.preferred_offset != AMBA_VIRT_OFFSET_AUTO && resp.suggested_offset != 0) {
            pr_info("amba_cavalry: auto-retrying at suggested offset 0x%08x\n", resp.suggested_offset);
            req.preferred_offset = resp.suggested_offset;
            retries--;
            goto retry;
        }
        break;
    case -ENOMEM:
        pr_err("amba_cavalry: requested %u MB exceeds available quota (%u MB available)\n",
               req.requested_size / (1024 * 1024), resp.max_avail_size / (1024 * 1024));
        break;
    case -EPERM:
        pr_err("amba_cavalry: host ACL denied registration (err_code=%u). Check tenant permissions\n",
               resp.err_code);
        break;
    case -EBUSY:
        pr_err("amba_cavalry: Cavalry device already registered for this VM. Pass 'force_replace=1' to overwrite\n");
        break;
    default:
        pr_err("amba_cavalry: boundary negotiation failed (status=%d, err_code=%u)\n",
               resp.status, resp.err_code);
        break;
    }

    return resp.status;
}
```

---

## 7. Guest Introspection & Topology Query API (`VIRT_QUERY`)

### 7.1 Introspection Capabilities
A rich query API exposed over vsock Port 5555 enables guests to discover their environment, peer workloads, and hardware capabilities:
1. **Self Introspection (`AMBA_VIRT_QUERY_SELF`)**:
   - Authenticated Caller CID (kernel-verified).
   - Assigned Tenant Index.
   - Memory allocation summary: base offset, current size, quota ceiling, high-water mark.
   - Virtual device states: Cavalry sessions/DAGs/handles, GDMA aperture.
   - Assigned ACL capability bitmask.
2. **Peer Topology Introspection (`AMBA_VIRT_QUERY_PEERS`)**:
   - List of active tenant VMs: `{ cid, tenant_idx, status, mem_allocated, active_devices }`.
   - Gated by ACL: If caller lacks `AMBA_VIRT_CAP_QUERY_PEERS`, request returns `-EPERM`.
3. **Hardware & Silicon Topology (`AMBA_VIRT_QUERY_DEV_TOPOLOGY`)**:
   - Ambarella Chip ID (CV5, CV7, N1), silicon stepping, hardware revisions.
   - VisORC NPU core count, clock frequency, microcode ABI version.
   - GDMA engine channel capabilities and pitch constraints.
   - Host physical memory layout: total CVMEM size. *(Note: `phys_addr` must remain 0 on vsock queries to prevent HPA leaks to untrusted guests)*.
4. **Virtual Device Memory Mapping (`AMBA_VIRT_QUERY_DEV_MEM`)**:
   - Granular breakdown of memory addresses assigned to each virtual device within the tenant's slice.

### 7.2 Wire Protocol Definitions for Query API

```c
enum amba_virt_query_op {
    AMBA_VIRT_QUERY_SELF         = 1,
    AMBA_VIRT_QUERY_PEERS        = 2,
    AMBA_VIRT_QUERY_DEV_TOPOLOGY = 3,
    AMBA_VIRT_QUERY_DEV_MEM      = 4,
    AMBA_VIRT_QUERY_METRICS      = 5,
};

struct amba_virt_query_req {
    __u32 query_op;        /* enum amba_virt_query_op */
    __u32 target_cid;      /* Specific peer CID or 0 for all */
    __u32 dev_id;          /* Specific device or 0 for all */
    __u32 flags;           /* Request options */
};

struct amba_virt_peer_desc {
    __u32 cid;
    __u32 tenant_idx;
    __u32 status;          /* 0=OFFLINE, 1=ONLINE, 2=QUARANTINED */
    __u32 mem_allocated_mb;
    __u32 active_sessions;
    __u32 active_dags;
    __u32 active_handles;
    __u32 caps;
};

struct amba_virt_query_resp {
    __u32 query_op;
    __s32 status;
    __u32 count;           /* Number of entries in payload */
    __u8  payload[3900];   /* Structured response payload */
};
```

---

## 8. Cavalry Virtualization Proxy (`cavalry_proxy.c`)

The Cavalry proxy multiplexes the single physical Ambarella VisORC NPU across guest VMs, supporting two distinct execution models:

### 8.1 Path A (Legacy Drop-In Compatibility)
- **Operation**: The guest stages DVI microcode and input/output tensors directly in its shared BAR slice. The proxy validates port offsets against the tenant's BAR bounds, rewrites virtual tokens to Host Physical Addresses (HPAs), and calls `ioctl(fd, CAVALRY_RUN_DAGS)` on the real `/dev/cavalry`.
- **Target Use**: Unmodified Ambarella `nnctrl` applications running on trusted guest VMs.
- **Limitation**: Vulnerable to TOCTOU microcode mutation because VisORC has no S-MMU on the data path.

### 8.2 Path B (Hardened Class D Host AMA Isolation)
- **`VCAV_OP_REGISTER_DAG`**: The guest stages model DVI in its BAR and calls register. The proxy allocates memory in the **Host-Private AMA pool** (`CAVALRY_ALLOC_MEM`), deep-copies the DVI microcode and sub-scheduler DAGs, relocates internal DAG pointers, and returns a 32-bit `dag_id`. The guest frees its staging buffer.
- **`VCAV_OP_ALLOC_HANDLE`**: The guest allocates input and output tensor buffers, receiving opaque 32-bit `handle_id`s. The proxy tracks slice bounds in `g_handles`.
- **`VCAV_OP_RUN_REGISTERED_DAG`**: The guest dispatches inference passing only `dag_id` and `handle_id`s. The proxy validates handle ownership, binds DVI from Host AMA and handles from the guest BAR, and submits via `CAVALRY_RUN_DAGS_MEMFD`.
- **Security Guarantee**: Zero TOCTOU exposure. Corrupting the guest BAR during inference cannot tamper with microcode or sub-DAG execution.

### 8.3 Automated Session Teardown & Reclamation
- Each guest `/dev/cavalry` file open assigns a unique 32-bit `session_id`.
- The proxy tags all slices, handles, and registered DAGs with `(client_cid, session_id)`.
- When the guest process closes `/dev/cavalry` or terminates unexpectedly (`SIGKILL`), the guest driver sends `VCAV_OP_CLOSE_SESSION` (or the server detects vsock disconnect).
- The server automatically frees all associated handles, host AMA allocations, and BAR slices, completely eliminating memory leaks on process death.

### 8.4 VisORC Hardware Serialization (`arena_mutex`)
Because the SoC possesses a single physical VisORC core, concurrent inference submissions from multiple worker threads or multiple guest VMs are serialized using a POSIX mutex (`pthread_mutex_t arena_mutex`) within `cavalry_proxy.c`:
1. Acquire `arena_mutex`.
2. Issue `dma_wmb()` memory barrier.
3. Submit ioctl to physical `/dev/cavalry`.
4. Await hardware completion interrupt.
5. Issue `dma_rmb()` memory barrier.
6. Release `arena_mutex`.

---

## 9. GDMA Hardware Arbitration

GDMA (General DMA) operations are mediated through the server to prevent untrusted guests from programming physical DMA registers:
1. **Spatial Bounds Checking**: The host server verifies that all source, destination, and pitch parameters fall strictly within the guest's assigned GDMA channel aperture (e.g., `[0, 64 MiB)` or configured slice).
2. **Translation**: Offsets are translated to HPAs:
   $$\text{HPA} = \text{Tenant.base\_phys} + \text{offset}$$
3. **Execution**: The host server dispatches the request to the physical kernel GDMA driver (`dma_memcpy` / `dma_pitch_memcpy`), serialized via `gdma_hw_mutex`.

---

## 10. Threat Model & Security Boundaries

To guarantee robust isolation and zero cross-tenant leakage in a multi-tenant edge deployment where one guest VM might be untrusted, compromised, or running unverified customer workloads, `amba-virt-server` and the underlying virtualization stack enforce a defense-in-depth security model across seven layers.

### 10.1 Hardware vs. Software Security Responsibility Matrix

| # | Security Boundary & Threat Vector | Threat / Attack Surface | Enforcement Mechanism | Primary Domain | Hardware Silicon Role | Software Implementation Role |
|---|---|---|---|---|---|---|
| **1** | **DRAM Spatial Isolation** | Guest A crafts pointers to read private weights or overwrite memory belonging to Guest B or Host Dom0. | Disjoint Stage-2 Page Tables (`GPA -> HPA`); non-overlapping physical CVMEM partitions. | **Hardware-Enforced (HW)**<br>*(SW Configured)* | **ARM64 Stage-2 MMU (`VTTBR_EL2`)**: Translates all GPA memory accesses; hardware MMU silicon immediately traps out-of-range bus accesses as Stage-2 Data Aborts at wire speed. | **Hypervisor / EVE OS (EL2)**: Configures disjoint translation tables during VM creation; provisions non-overlapping 1 GiB ivshmem physical windows. |
| **2** | **DMA Boundary (No Data-Path SMMU)** | Malicious guest attempts to trick VisORC DMA into reading/writing arbitrary host DRAM. | Host address translation & Path B AMA isolation. | **Software-Enforced (SW)**<br>*(HW-Backed)* | **VisORC DMA Engine**: Executes DMA transfers using physical addresses programmed into descriptors. *(No guest-accessible IOMMU on the data path).* | **Host Kernel (`cavalry.ko`) & Server**: Guest has zero direct DMA MMIO access. Host software translates guest BAR offsets to validated host physical addresses. |
| **3** | **Microcode Integrity & TOCTOU** | Guest A modifies DVI microcode or DAG descriptor pointers in shared memory while VisORC executes. | **Path B (Hardened Class D)**: Deep-copy of DVI binaries into Host-Private AMA pool. | **Software-Enforced (SW)**<br>*(HW-Backed)* | **ARM64 Stage-2 MMU**: The Host AMA pool is completely unmapped from all guest Stage-2 page tables, making physical access impossible. | **`amba-virt-server`**: Ingests loaded graph, deep-copies microcode into host AMA before execution. Eliminates runtime TOCTOU tampering. |
| **4** | **Transport Identity Authentication** | Compromised Guest B sends RPC messages claiming to be Guest A (Tenant 0) to manipulate its sessions. | Virtio-vsock kernel peer authentication (`peer_addr.svm_cid`) on single listener port 5555. | **Software-Enforced (SW)**<br>*(Kernel / Hypervisor)* | **CPU Exception Traps (`HVC`/`SMC`)**: Hypervisor intercepts VM virtio notifications to route packets between guest and host virtio queues. | **Host Kernel (`vhost_vsock`) & Server**: Hypervisor binds immutable CID to VM. Host kernel authenticates peer CID; daemon extracts CID via `getpeername()` and maps to tenant context dynamically. |
| **5** | **Tensor Handle Authorization** | Guest B guesses or forges a `handle_id` to read or overwrite another guest's active tensors. | Opaque handle table with strict tenant ownership validation and boundary checks. | **Software-Enforced (SW)** | *None* (pure logical abstraction in host software). | **`amba-virt-server`**: Associates every handle with the authenticated `tenant_id`. Rejects access to unowned handles with `-EACCES` (13). Enforces bounds check against granted aperture. |
| **6** | **VisORC Silicon Execution Arbitration** | Concurrent guest submissions interleave on the single VisORC NPU core, corrupting execution pipeline. | Host-level mutual exclusion (`visorc_hw_mutex`) protecting the physical hardware device. | **Hybrid (HW + SW)** | **VisORC NPU Core & Hardware IRQ**: Single physical execution engine processes one job queue; asserts `IRQ_CAVALRY` upon hardware completion. | **`amba-virt-server`**: Acquires POSIX `pthread_mutex_t visorc_hw_mutex` prior to issuing `CAVALRY_RUN_DAGS` to `/dev/cavalry`; blocks other tenants until the hardware IRQ fires and lock releases. |
| **7** | **GDMA Hardware Arbitration** | Concurrent guest DMA copies corrupt GDMA hardware channels or step on peripheral state. | Host-side `gdma_hw_mutex` and dynamic aperture clamp. | **Hybrid (HW + SW)** | **Ambarella GDMA Engine**: Executes 2D pitch copies between physical addresses. | **`amba-virt-server`**: Clamps offsets strictly to granted aperture bounds and serializes physical GDMA dispatches via `gdma_hw_mutex`. |

> [!NOTE]
> **Residual State Sanitization**: Cache/SPM sanitization across tenants between job runs is an unverified future backlog item and is explicitly excluded from the active security guarantees of this milestone.

### 10.2 Deep Dive: Detailed Threat Vectors and Mitigations

#### 1. Direct Physical Memory Snooping / Clobbering (Hardware Stage-2 MMU)
- **Threat**: A compromised guest kernel (running at EL1) attempts to inspect or corrupt physical memory belonging to Dom0 or another HVM guest.
- **Hardware Silicon Role**: The ARM64 processor enforces two-stage address translation. While the guest OS controls Stage-1 page tables (translating Guest Virtual Address $\to$ Guest Physical Address), the hardware Memory Management Unit (MMU) forces every bus transaction through Stage-2 translation (translating GPA $\to$ Host Physical Address) using the base pointer in `VTTBR_EL2`.
- **Software Role**: EVE OS / Hypervisor sets up mutually exclusive Stage-2 page tables. Each tenant is granted GPA access exclusively to its own sliced ivshmem aperture. Any attempt by Tenant 0 to access addresses in Tenant 1's range triggers an immediate hardware translation fault (`Data Abort` routed directly to EL2 hypervisor).

#### 2. DMA Boundary & Device Memory (No Data-Path SMMU)
- **Threat**: VisORC DMA engines possess bus master capability without an IOMMU/SMMU on the data path. If an untrusted guest could supply raw physical addresses to the DMA engine, it could read or overwrite any memory region in the entire SoC.
- **Hardware Silicon Role**: VisORC DMA controllers perform memory transfers based on the addresses programmed into their hardware descriptor registers.
- **Software Role**: The guest VM has **zero** MMIO access to physical DMA hardware registers. All DMA transactions are brokered by `amba-virt-server` and the host `cavalry.ko` driver. The host software validates that every source and destination buffer resides strictly within the caller's validated tenant slice before writing descriptors to the hardware engine.

#### 3. Microcode Integrity & TOCTOU Immunity (Path B Host AMA Carveout)
- **Threat**: Under legacy Path A, DVI microcode instructions and DAG descriptors remain in shared memory. A malicious guest could modify the microcode or swap buffer pointers *after* validation but *during* VisORC execution (Time-of-Check to Time-of-Use exploit).
- **Hardware Silicon Role**: The ARM Stage-2 MMU completely unmaps the Host-Private AMA carveout from all guest VMs.
- **Software Role**: `amba-virt-server` enforces Path B. When a guest invokes `CAVALRY_IOC_REGISTER_DAG`, the server deep-copies the loaded `cavalry_run_dags` graph and DVI microcode blob into host-private AMA memory. VisORC executes exclusively from host-private AMA. Because the guest has zero physical mapping to this memory, TOCTOU tampering is physically impossible.

#### 4. Transport Identity Authentication (Kernel-Enforced VSOCK CID)
- **Threat**: Guest B connects to the host daemon and issues commands pretending to be Tenant 0 (e.g., attempting to free Tenant 0's registered models or inspect outputs).
- **Hardware Silicon Role**: Hardware CPU traps (`HVC`/`SMC`) trigger VM exits whenever virtio virtqueue registers are toggled, ensuring hypervisor mediation.
- **Software Role**: The hypervisor assigns an immutable Context ID (CID) to each VM's virtio-vsock device. When a tenant connects to host port 5555, `amba-virt-server` calls `getpeername()` on the socket file descriptor. The returned `peer_addr.svm_cid` is provided directly by the host Linux kernel and cannot be forged by guest userspace. The daemon dynamically maps this CID to the tenant context.

#### 5. Tensor Handle Authorization (Opaque Handle Table)
- **Threat**: Tenant B attempts to access or execute a registered network belonging to Tenant 0 by guessing its handle or DAG ID.
- **Hardware Silicon Role**: None.
- **Software Role**: `amba-virt-server` maintains an internal lookup table mapping `handle_id -> {tenant_id, base_offset, size, flags}`. Whenever an operation (`RUN_REGISTERED_DAG`, `FREE_HANDLE`, `SYNC_CACHE`) is received, the server asserts that `handle->tenant_id == session->tenant_id`. Mismatched requests are rejected with `-EACCES` (13), preventing cross-tenant access.

#### 6. Silicon Execution Core Arbitration (Hardware Mutex Serialization)
- **Threat**: Multiple guests dispatch inference requests simultaneously, interleaving execution in the NPU pipeline and producing corrupt outputs or hardware hangs.
- **Hardware Silicon Role**: The physical Ambarella VisORC NPU core executes one job stream at a time and raises `IRQ_CAVALRY` upon completion.
- **Software Role**: `amba-virt-server` wraps physical device interaction in a serialized monitor (`pthread_mutex_t visorc_hw_mutex`). When a tenant dispatches an inference job, it holds `visorc_hw_mutex` until VisORC signals completion via IRQ. Requests from other tenants queue on the mutex, ensuring strictly serialized hardware execution without resource thrashing.

#### 7. GDMA Hardware Arbitration (GDMA Mutex & Offset Clamp)
- **Threat**: Multiple guests dispatch concurrent DMA operations, interleaving channel configurations or programming out-of-bounds addresses.
- **Hardware Silicon Role**: Physical Ambarella GDMA engine executes 2D pitch copies between physical bus addresses.
- **Software Role**: `amba-virt-server` clamps all guest GDMA coordinates strictly to the granted device aperture and serializes physical device access with `pthread_mutex_t gdma_hw_mutex`.

---

## 11. Comprehensive Test Specification (CppUTest)

The functional validation suite is implemented using **CppUTest** within `guest-os/client/amba-virt-client.cxx`.

### 11.1 VFS System Call Compliance (`TEST_GROUP(VFS)`)

| System Call | Scenarios Tested | Expected Behavior |
|---|---|---|
| `open()` | Valid read/write open, missing device node, flags (`O_RDWR`, `O_NONBLOCK`, `O_CLOEXEC`), permissions (`0666`) | Succeeds for valid flags; assigns private file context; lazily binds PCI resource |
| `close()` | Standard close, close with active mapped buffers, close with in-flight vsock requests | Decrements reference count; cleanly releases descriptor state |
| `read()` / `write()` | Invoking standard POSIX `read()` and `write()` | Driver relies on `ioctl` framing; character device returns `-EINVAL` without kernel crash |
| `lseek()` | `lseek(fd, 0, SEEK_SET)`, `SEEK_CUR`, `SEEK_END` | Character device returns `-ESPIPE` (illegal seek) |
| `ioctl()` | `AMBA_VIRT_IOC_GET_INFO`, `AMBA_VIRT_IOC_CONNECT`, `AMBA_VIRT_IOC_SEND`, `AMBA_VIRT_IOC_RECV` | Valid inputs return 0; invalid pointer arguments return `-EFAULT`; unmapped commands return `-ENOTTY` |
| `mmap()` | Map full `shm_size`, map 4KB/64KB pages, unaligned offsets, boundary mapping, over-allocation | Maps PCI BAR 2 pages into user space with `PROT_READ | PROT_WRITE`; over-allocation returns `-EINVAL` |
| `munmap()` / `msync()` | Unmap active pages, invoke `msync(MS_SYNC)` / `msync(MS_ASYNC)` across mapped bounds | Unmaps pages without faulting; memory synchronizes cleanly |
| `fcntl()` | Toggle `O_NONBLOCK`, verify `F_GETFL`/`F_SETFL`, descriptor duplication via `dup()`, `dup2()` | Properly duplicates descriptor; shares underlying kernel file object |
| `poll()` / `select()` | Edge-triggered and level-triggered polling on `/dev/amba_virt` | Returns readability/writability mask based on vsock queue occupancy |

### 11.2 virtio-vsock Protocol Edge Cases (`TEST_GROUP(Vsock)`)
- **Zero-Byte Payloads**: Sending `AMBA_VIRT_IOC_SEND` with `len = 0` rejected with `-EINVAL`.
- **Maximum Payload Boundaries**: Payloads tested at 1B, 64B, 512B, 1024B, 4096B (`AMBA_VIRT_MAX_MSG`), and 4097B (rejected with `-EINVAL`).
- **Timeout Semantics**:
  - `timeout_ms = 0`: Defaults to standard 5000ms timeout.
  - `timeout_ms = 50`: Rapid timeout when awaiting non-existent response; returns `-ETIMEDOUT`.
  - `timeout_ms < 0`: Blocking wait mode until peer sends acknowledgment.
- **Connection Churn**: Rapid open/connect/close bursts (1,000 cycles) to verify socket descriptor reclamation in both guest and host kernels.

### 11.3 ivshmem Shared Memory Integrity (`TEST_GROUP(SHM)`)
- **Pattern Verification**: End-to-end writes across multi-megabyte buffers using pseudo-random bit sequences (PRBS31), incremental 64-bit integer counters, and walking-one bit patterns.
- **Cacheline & Page Alignments**: Reads and writes across 64-byte cacheline boundaries, 4KB page boundaries, and unaligned offsets.
- **Notification Handshake**:
  1. Client writes structured payload into shared memory slice at `shm_offset`.
  2. Client issues `AMBA_VIRT_IOC_SEND` containing `shm_offset`, `len`, and `FLAG_SHM_NOTIFY`.
  3. Server verifies data integrity, performs an in-place transformation, and replies with `FLAG_SHM_ACK`.
  4. Client verifies the modified pattern matches expected transformed state.

### 11.4 Multi-FD Concurrency & Scaling (`TEST_GROUP(MultiFD)`)
- Multiple file descriptors opened concurrently by independent threads within a single process.
- Scaled concurrency across $T \in \{1, 2, 4, 8, 16\}$ threads.
- Measures serialization overhead and verifies that mutex locks protect the transport without deadlocking.

### 11.5 In-Band Device Configuration & Conflict Resolution (`TEST_GROUP(InBandDeviceConfig)`)
- **`TEST(InBandDeviceConfig, SuccessAutoOffset)`**: Asserts `preferred_offset = AMBA_VIRT_OFFSET_AUTO` successfully returns aligned, disjoint offsets.
- **`TEST(InBandDeviceConfig, CollisionReturnsSuggestedOffset)`**: Registers GDMA at `[0x00000000, 0x04000000)`; asserts explicit request at `0x02000000` returns `status = -EEXIST`, `err_code = AMBA_VIRT_ERR_OFFSET_COLLISION`, and valid `suggested_offset`.
- **`TEST(InBandDeviceConfig, QuotaExceededExactFails)`**: Asserts requesting size exceeding quota with `F_EXACT` returns `-ENOMEM` with `max_avail_size`.
- **`TEST(InBandDeviceConfig, QuotaExceededBestEffortClamps)`**: Asserts safe scratch requests with `F_BEST_EFFORT` clamp to `max_avail_size`.
- **`TEST(InBandDeviceConfig, DuplicateWithoutReplaceFails)`**: Asserts duplicate registration without `F_REPLACE` returns `-EBUSY`.
- **`TEST(InBandDeviceConfig, DuplicateWithReplaceSucceeds)`**: Asserts duplicate registration with `F_REPLACE` tears down prior allocation and registers cleanly.
- **`TEST(InBandDeviceConfig, PermissionDeniedWithoutCap)`**: Asserts unprivileged tenant lacking `AMBA_VIRT_CAP_DEV_CONFIG` receives `-EPERM`.

### 11.6 Introspection & Query API Verification (`TEST_GROUP(IntrospectionAPI)`)
- **`TEST(IntrospectionAPI, QuerySelfReportsCorrectBounds)`**: Validates `QUERY_SELF` returns caller CID, assigned tenant index, memory quota, and active device apertures.
- **`TEST(IntrospectionAPI, PeerVisibilityRestrictedByACL)`**: Asserts tenant lacking `AMBA_VIRT_CAP_QUERY_PEERS` receives `-EPERM`, while authorized tenant retrieves peer descriptor list.
- **`TEST(IntrospectionAPI, TopologyQueryRedactsPhysicalAddresses)`**: Asserts `QUERY_DEV_TOPOLOGY` populates chip ID, core counts, and frequencies, but leaves `phys_addr = 0` to prevent HPA disclosure.

### 11.7 Access Control List Enforcement (`TEST_GROUP(AccessControlList)`)
- Validates bitmask checking across all capability boundaries.
- Asserts that untrusted tenants cannot invoke legacy Path A or modify host memory partitions.

---

## 12. Performance Benchmarking Methodology

The benchmarking suite measures transport overhead under production conditions:

1. **Vsock Control Plane Round-Trip Latency (RTT)**:
   - Measures complete round-trip time: Client Send $\rightarrow$ Driver $\rightarrow$ Vsock $\rightarrow$ Host Server Echo $\rightarrow$ Driver $\rightarrow$ Client Receive.
   - Sweep: 16B, 64B, 256B, 1KB, 2KB, 4KB. Sample size: 10,000 iterations per size.
   - Evaluates Min, Median ($p50$), Mean, $p90$, $p95$, $p99$, Max, and ops/sec.
2. **Vsock Streaming Throughput**:
   - Streams unacknowledged vsock packets across the interface.
   - Reports data transfer rate (MB/s) and packet rate (kpps).
3. **ivshmem Zero-Copy Throughput & Handoff Latency**:
   - Buffer sweep: 4KB, 64KB, 256KB, 1MB, 4MB, 16MB, 64MB.
   - Reports write bandwidth (GB/s), notification latency ($\mu$s), and effective handoff bandwidth:
     $$\text{Effective Bandwidth} = \frac{\text{Buffer Size}}{\text{Write Time} + \text{Signaling Time} + \text{Peer Read Time}}$$
4. **Simulated Workload (Cavalry Tensor Dispatch)**:
   - Deposits 4MB image tensor + 64KB parameter block into ivshmem.
   - Signals inference start over vsock; host simulates or dispatches to hardware.
   - Evaluates sustained FPS and round-trip jitter.

---

## 13. Brainstormed Architectural Extensions

The following architectural subsystems represent targeted capabilities planned for future iterations of `amba-virt-server`:

### 13.1 Fair-Share & Priority Hardware Scheduler (QoS Engine)
- **Motivation**: Currently, `g_visorc_hw_mutex` and `g_gdma_hw_mutex` are simple unweighted FIFO mutexes. A batch background model running for 100ms can block a safety-critical real-time perception pipeline.
- **Design**:
  - 3-tier priority arbitration: `HIGH` (Real-Time Perception), `NORMAL` (Standard Inference), `LOW` (Background Analytics).
  - Weighted Fair Queueing (WFQ) / token bucket rate limiting to prevent noisy neighbors from saturating the NPU.
  - Hardware Watchdog Timer: Detects hung models exceeding deadline (e.g. 500ms) and invokes safe device reset without taking down the host.

### 13.2 Live Metrics, Telemetry & Observability Engine
- **Motivation**: Production deployments require visibility into NPU utilization, memory pressure, and RPC latency distributions.
- **Design**:
  - Live metric collection:
    - Inference latency histogram (min, median, p90, p95, p99, max).
    - Total hardware ticks, active duty cycle %.
    - GDMA throughput (MB/s).
    - Active handles, registered models, allocated DRAM per tenant.
    - Rate of dropped or ACL-denied requests.
  - Export channels:
    - Atomic JSON export to `/run/amba-virt/metrics.json`.
    - Query RPC opcode `AMBA_VIRT_QUERY_METRICS`.

### 13.3 Connection Health, Heartbeat & Auto-Reaper
- **Motivation**: If a guest VM crashes, hangs, or reboots, resources (AMA memory carveout, handles, BAR slices) could remain locked.
- **Design**:
  - Periodic keep-alive ping/pong with configurable timeout.
  - Socket disconnect hook triggers immediate deep teardown: frees all handles, deregisters DAGs, reclaims Host AMA buffers, and resets tenant allocations.
  - Tenant quarantine mechanism: Isolate a guest triggering repeated hardware faults without rebooting other tenants.

### 13.4 Virtual IAV (Camera / Video Subsystem) Architectural Hook
- **Motivation**: Ambarella's key hardware strength is video ISP (IAV).
- **Design**:
  - Reserve message opcodes and memory carveout structures for `VIRT_IAV`.
  - Provide shared ring-buffer mechanisms in shared DRAM for zero-copy camera frame streaming.

### 13.5 Staged Implementation Roadmap

```text
+-------------------------------------------------------------------------------+
| Phase 1: Protocol & UAPI Definitions                                          |
| - Update drivers/amba_virt/include/uapi/amba_virt.h                           |
| - Define wire opcodes, structs for DEV_BOUNDS, DEV_RELEASE, VIRT_QUERY, ACL   |
| - Bump AMBA_VIRT_PROTO to 3 to reject legacy static guest drivers             |
| - Define enum amba_virt_err_code and AMBA_VIRT_DEV_F_* flags                  |
| - Maintain strict backward compatibility with existing Cavalry RPCs           |
+-------------------------------------------------------------------------------+
                                       |
                                       v
+-------------------------------------------------------------------------------+
| Phase 2: Dynamic Memory Manager & Conflict Resolution Engine                  |
| - Implement drivers/amba_virt/tools/virt_mem_pool.c / .h                       |
| - Extent allocator with collision detection and suggested_offset computation  |
| - Per-tenant quota tracking and Best-Effort / Exact clamping logic            |
| - Duplicate registration prevention and atomic multi-tenant mutex             |
| - Wire up AMBA_VIRT_MSG_DEV_SET_BOUNDS_* in amba-virt-server.c                |
+-------------------------------------------------------------------------------+
                                       |
                                       v
+-------------------------------------------------------------------------------+
| Phase 3: NOHYPER Dom0 Control Plane & amba-virt-ctl                           |
| - Implement drivers/amba_virt/tools/virt_acl.c / .h                           |
| - Implement drivers/amba_virt/tools/virt_admin_ipc.c / .h (/run/.../admin.sock)|
| - Build build/bin/amba-virt-ctl for NOHYPER Dom0 control                      |
| - JSON policy datastore at /persist/etc/amba-virt/policies.json               |
| - Support dynamic quota overrides, live tenant eviction, and policy editing   |
| - Connection disconnect auto-reaper and state cleanup                         |
+-------------------------------------------------------------------------------+
                                       |
                                       v
+-------------------------------------------------------------------------------+
| Phase 4: Introspection & Topology Query API Subsystem                         |
| - Implement drivers/amba_virt/tools/virt_query.c / .h                         |
| - Handlers for QUERY_SELF, QUERY_PEERS, QUERY_DEV_TOPOLOGY, QUERY_DEV_MEM     |
| - ACL integration (redact or deny peer visibility for untrusted guests)      |
+-------------------------------------------------------------------------------+
                                       |
                                       v
+-------------------------------------------------------------------------------+
| Phase 5: Guest Driver In-Band insmod Parameter Support & Error Handling       |
| - Update guest-os/linux/amba-cavalry/amba_cavalry_hvm.c with module params    |
| - Implement amba_cavalry_negotiate_bounds() with retry on -EEXIST             |
| - Handle -ENOMEM, -EPERM, -EBUSY with clear kernel diagnostic logs            |
| - Update guest-os/linux/amba-gdma/amba_gdma_hvm.c with matching negotiation   |
+-------------------------------------------------------------------------------+
                                       |
                                       v
+-------------------------------------------------------------------------------+
| Phase 6: Client SDK & Automated Test Suite Updates                            |
| - Update guest-os/client/amba-virt-client.cxx with CppUTest test groups       |
| - Unit tests for all conflict resolution paths and error returns              |
| - Target silicon deployment and end-to-end verification                       |
+-------------------------------------------------------------------------------+
```

---

## 14. Compilation, Execution & Diagnostic Triage

### 14.1 Building Server & Control Tools
```bash
# Compile host daemon and control utility for AArch64 inside NOHYPER:
make -C drivers/amba_virt/tools
```
Outputs:
- `build/bin/amba-virt-server`: Daemon and arbitration proxy.
- `build/bin/amba-virt-ctl`: Dom0 command-line administration tool.

### 14.2 Launching the Server Daemon
```bash
# Run server in background inside NOHYPER container:
./amba-virt-server > /tmp/amba-virt-server.log 2>&1 &
```

### 14.3 Diagnostic & Error Recovery Triage

| Failure Mode / Symptom | Root Cause | Remediation Protocol |
|---|---|---|
| `ioctl(CONNECT): Connection refused` | `amba-virt-server` not active | Check daemon on NOHYPER (`pgrep amba-virt-server`). Inspect `/tmp/amba-virt-server.log`. Restart server on port 5555. |
| `open(/dev/amba_virt): No such file or directory` (HVM) | Guest `amba_virt.ko` not inserted | Run `lspci -nn \| grep 1af4:1110`. If present, run `sudo insmod ~/amba_virt.ko`. Check `dmesg \| tail -n 20`. |
| `open(/dev/amba_virt): No such file or directory` (NOHYPER) | Container missing cgroup rule or adapter not bound | Verify `zcli edge-app-instance show <instance>`. Ensure `amba_virt` `IO_TYPE_OTHER` adapter is assigned in the hardware model. |
| `amba_cavalry: offset collides with active dev` (`-EEXIST`) | In-band `preferred_offset` overlaps active allocation | Use `preferred_offset=auto` or inspect `dmesg` for suggested offset returned by `amba-virt-server`. |
| `amba_cavalry: requested %u MB exceeds quota` (`-ENOMEM`) | Driver pool size exceeds tenant memory ceiling | Use `amba-virt-ctl set-quota <cid> <size>` in Dom0 or reduce `pool_size` module parameter. |
| `amba_cavalry: host ACL denied registration` (`-EPERM`) | Guest CID lacks `AMBA_VIRT_CAP_DEV_CONFIG` capability | Run `amba-virt-ctl set-acl <cid> standard` in Dom0 or define static profile in `/persist/etc/amba-virt/policies.json`. |
| `amba_cavalry: Cavalry device already registered` (`-EBUSY`) | Previous driver instance crashed without clean `rmmod` | Pass `force_replace=1` on `insmod` to trigger server-side session cleanup and re-registration. |
| `amba-virt-ctl: connect failed (/run/amba-virt/admin.sock)` | Server not running or caller is not root | Verify daemon status with `pgrep amba-virt-server`. Ensure `amba-virt-ctl` is executed as root in NOHYPER. |
| `mmap: Invalid argument (EINVAL)` | Buffer size exceeds 1 GiB window or is unaligned | Query driver via `AMBA_VIRT_IOC_GET_INFO`. Ensure buffer offsets and mapping sizes are page-aligned and $\le$ `shm_size`. |
| Latency regression ($p99 > 2000\text{ }\mu\text{s}$) | CPU throttling or core frequency scaling | Inspect CPU governor on host (`cpupower frequency-info`). Verify background container CPU utilization. |
