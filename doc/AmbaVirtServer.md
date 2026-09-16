# Ambarella Virtualization Server Daemon (`amba-virt-server`) & Transport Architecture

*Copyright (C) 2026, Ambarella International LLC.*

## 1. Overview & System Role

The **`amba-virt-server`** is the privileged host-side virtualization daemon running within the EVE-OS **NOHYPER** container (`n1-655-devkit-nohyper` / `n1-655-pro-nohyper`). It acts as the central **hardware arbitrator and virtualization proxy** connecting untrusted KVM HVM guest VMs (e.g., Ubuntu 24.04, Alpine Linux 3.20, QNX) to physical Ambarella silicon accelerators:
- **VisORC NPU (Cavalry)**: Deep learning neural network acceleration (`/dev/cavalry`).
- **GDMA Engine**: Hardware-accelerated 2D pitch copy and memory DMA (`/dev/gdma`).
- **Image Audio Video (IAV)**: Video sensor capture pipelines and DSP encoding (`/dev/iav`).

```text
+-----------------------------------------------------------------------------------------------+
| HVM Guest (Ubuntu / Alpine EL1)                                                               |
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
| NOHYPER Host (Ambarella SoC Privileged EL2)                             |                     |
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
| **NOHYPER Host** | `amba-virt-server` | Arbitrator daemon, `cavalry_proxy`, GDMA bounds | `gcc` (`-std=c11`, `-lpthread`) |

---

## 3. Server Daemon Architecture & Internals

The `amba-virt-server` source code resides in [`drivers/amba_virt/tools/`](../drivers/amba_virt/tools/):
- **`amba-virt-server.c`**: Core daemon lifecycle, multi-tenant socket listener, client connection worker threads, and message routing.
- **`cavalry_proxy.c` / `cavalry_proxy.h`**: Ambarella Cavalry virtualization proxy, AMA memory management, handle tracking, and VisORC hardware arbitration.

### 3.1 Multi-Tenant Connection Model

```text
                        +---------------------------------------------+
                        | amba-virt-server main()                     |
                        | - Binds AF_VSOCK (VMADDR_CID_ANY, Port 5555)|
                        | - Sets up signal handlers (SIGINT, SIGTERM) |
                        | - Inits cavalry_proxy (maps Host AMA & BAR) |
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
                 | - Reads 40B RPC frame |          | - Reads 40B RPC frame |
                 | - Validates tenant CID|          | - Validates tenant CID|
                 | - Dispatches to proxy |          | - Dispatches to proxy |
                 +-----------------------+          +-----------------------+
```

### 3.2 Wire RPC Protocol Framing

Control messages are exchanged using a compact 40-byte control structure defined in [`include/uapi/amba_virt.h`](../drivers/amba_virt/include/uapi/amba_virt.h):

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

---

## 4. Cavalry Virtualization Proxy (`cavalry_proxy.c`)

The Cavalry proxy multiplexes the single physical Ambarella VisORC NPU across guest VMs, supporting two distinct execution models:

### 4.1 Path A (Legacy Drop-In Compatibility)
- **Operation**: The guest stages DVI microcode and input/output tensors directly in its shared BAR slice. The proxy validates port offsets against the tenant's BAR bounds, rewrites virtual tokens to Host Physical Addresses (HPAs), and calls `ioctl(fd, CAVALRY_RUN_DAGS)` on the real `/dev/cavalry`.
- **Target Use**: Unmodified Ambarella `nnctrl` applications running on trusted guest VMs.
- **Limitation**: Vulnerable to TOCTOU microcode mutation because VisORC has no S-MMU on the data path.

### 4.2 Path B (Hardened Class D Host AMA Isolation)
- **`VCAV_OP_REGISTER_DAG`**: The guest stages model DVI in its BAR and calls register. The proxy allocates memory in the **Host-Private AMA pool** (`CAVALRY_ALLOC_MEM`), deep-copies the DVI microcode and sub-scheduler DAGs, relocates internal DAG pointers, and returns a 32-bit `dag_id`. The guest frees its staging buffer.
- **`VCAV_OP_ALLOC_HANDLE`**: The guest allocates input and output tensor buffers, receiving opaque 32-bit `handle_id`s. The proxy tracks slice bounds in `g_handles`.
- **`VCAV_OP_RUN_REGISTERED_DAG`**: The guest dispatches inference passing only `dag_id` and `handle_id`s. The proxy validates handle ownership, binds DVI from Host AMA and handles from the guest BAR, and submits via `CAVALRY_RUN_DAGS_MEMFD`.
- **Security Guarantee**: Zero TOCTOU exposure. Corrupting the guest BAR during inference cannot tamper with microcode or sub-DAG execution.

### 4.3 Automated Session Teardown & Reclamation
- Each guest `/dev/cavalry` file open assigns a unique 32-bit `session_id`.
- The proxy tags all slices, handles, and registered DAGs with `(client_cid, session_id)`.
- When the guest process closes `/dev/cavalry` or terminates unexpectedly (`SIGKILL`), the guest driver sends `VCAV_OP_CLOSE_SESSION` (or the server detects vsock disconnect).
- The server automatically frees all associated handles, host AMA allocations, and BAR slices, completely eliminating memory leaks on process death.

### 4.4 VisORC Hardware Serialization (`arena_mutex`)
Because the SoC possesses a single physical VisORC core, concurrent inference submissions from multiple worker threads or multiple guest VMs are serialized using a POSIX mutex (`pthread_mutex_t arena_mutex`) within `cavalry_proxy.c`:
1. Acquire `arena_mutex`.
2. Issue `dma_wmb()` memory barrier.
3. Submit ioctl to physical `/dev/cavalry`.
4. Await hardware completion interrupt.
5. Issue `dma_rmb()` memory barrier.
6. Release `arena_mutex`.

---

## 5. GDMA Hardware Arbitration

GDMA (General DMA) operations are mediated through the server to prevent untrusted guests from programming physical DMA registers:
1. **Spatial Bounds Checking**: The host server verifies that all source, destination, and pitch parameters fall strictly within the guest's assigned GDMA channel aperture (`[0, 31 MiB)`).
2. **Translation**: Offsets are translated to HPAs:
   $$\text{HPA} = \text{Tenant.base\_phys} + \text{offset}$$
3. **Execution**: The host server dispatches the request to the physical kernel GDMA driver (`dma_memcpy` / `dma_pitch_memcpy`).

---

## 6. Threat Model & Security Boundaries

To guarantee robust isolation and zero cross-tenant leakage in a multi-tenant edge deployment where one guest VM might be untrusted, compromised, or running unverified customer workloads, `amba-virt-server` and the underlying virtualization stack enforce a defense-in-depth security model across seven layers.

### 6.1 Hardware vs. Software Security Responsibility Matrix

| # | Security Boundary & Threat Vector | Threat / Attack Surface | Enforcement Mechanism | Primary Domain | Hardware Silicon Role | Software Implementation Role |
|---|---|---|---|---|---|---|
| **1** | **DRAM Spatial Isolation** | Guest A crafts pointers to read private weights or overwrite memory belonging to Guest B or Host Dom0. | Disjoint Stage-2 Page Tables (`GPA -> HPA`); non-overlapping physical CVMEM partitions. | **Hardware-Enforced (HW)**<br>*(SW Configured)* | **ARM64 Stage-2 MMU (`VTTBR_EL2`)**: Translates all GPA memory accesses; hardware MMU silicon immediately traps out-of-range bus accesses as Stage-2 Data Aborts at wire speed. | **Hypervisor / EVE OS (EL2)**: Configures disjoint translation tables during VM creation; provisions non-overlapping 1 GiB ivshmem physical windows. |
| **2** | **DMA Boundary (No Data-Path SMMU)** | Malicious guest attempts to trick VisORC DMA into reading/writing arbitrary host DRAM. | Host address translation & Path B AMA isolation. | **Software-Enforced (SW)**<br>*(HW-Backed)* | **VisORC DMA Engine**: Executes DMA transfers using physical addresses programmed into descriptors. *(No guest-accessible IOMMU on the data path).* | **Host Kernel (`cavalry.ko`) & Server**: Guest has zero direct DMA MMIO access. Host software translates guest BAR offsets to validated host physical addresses. |
| **3** | **Microcode Integrity & TOCTOU** | Guest A modifies DVI microcode or DAG descriptor pointers in shared memory while VisORC executes. | **Path B (Hardened Class D)**: Deep-copy of DVI binaries into Host-Private AMA pool. | **Software-Enforced (SW)**<br>*(HW-Backed)* | **ARM64 Stage-2 MMU**: The Host AMA pool is completely unmapped from all guest Stage-2 page tables, making physical access impossible. | **`amba-virt-server`**: Ingests loaded graph, deep-copies microcode into host AMA before execution. Eliminates runtime TOCTOU tampering. |
| **4** | **Transport Identity Authentication** | Compromised Guest B sends RPC messages claiming to be Guest A (Tenant 0) to manipulate its sessions. | Virtio-vsock kernel peer authentication (`peer_addr.svm_cid`) on single listener port 5555. | **Software-Enforced (SW)**<br>*(Kernel / Hypervisor)* | **CPU Exception Traps (`HVC`/`SMC`)**: Hypervisor intercepts VM virtio notifications to route packets between guest and host virtio queues. | **Host Kernel (`vhost_vsock`) & Server**: Hypervisor binds immutable CID to VM. Host kernel authenticates peer CID; daemon extracts CID via `getpeername()` and maps to tenant context dynamically. |
| **5** | **Tensor Handle Authorization** | Guest B guesses or forges a `handle_id` to read or overwrite another guest's active tensors. | Opaque handle table with strict tenant ownership validation and boundary checks. | **Software-Enforced (SW)** | *None* (pure logical abstraction in host software). | **`amba-virt-server`**: Associates every handle with the authenticated `tenant_id`. Rejects access to unowned handles with `-EACCES` (13). Enforces `[offset, offset + size) <= 1 GiB`. |
| **6** | **VisORC Silicon Execution Arbitration** | Concurrent guest submissions interleave on the single VisORC NPU core, corrupting execution pipeline. | Host-level mutual exclusion (`visorc_hw_mutex`) protecting the physical hardware device. | **Hybrid (HW + SW)** | **VisORC NPU Core & Hardware IRQ**: Single physical execution engine processes one job queue; asserts `IRQ_CAVALRY` upon hardware completion. | **`amba-virt-server`**: Acquires POSIX `pthread_mutex_t visorc_hw_mutex` prior to issuing `CAVALRY_RUN_DAGS` to `/dev/cavalry`; blocks other tenants until the hardware IRQ fires and lock releases. |
| **7** | **GDMA Hardware Arbitration** | Concurrent guest DMA copies corrupt GDMA hardware channels or step on peripheral state. | Host-side `gdma_hw_mutex` and 31 MiB aperture clamp. | **Hybrid (HW + SW)** | **Ambarella GDMA Engine**: Executes 2D pitch copies between physical addresses. | **`amba-virt-server`**: Clamps offsets to `[0, 31 MiB)` and serializes physical GDMA dispatches via `gdma_hw_mutex`. |

> [!NOTE]
> **Residual State Sanitization**: Cache/SPM sanitization across tenants between job runs is an unverified future backlog item and is explicitly excluded from the active security guarantees of this milestone.

### 6.2 Deep Dive: Detailed Threat Vectors and Mitigations

#### 1. Direct Physical Memory Snooping / Clobbering (Hardware Stage-2 MMU)
- **Threat**: A compromised guest kernel (running at EL1) attempts to inspect or corrupt physical memory belonging to Dom0 or another HVM guest.
- **Hardware Silicon Role**: The ARM64 processor enforces two-stage address translation. While the guest OS controls Stage-1 page tables (translating Guest Virtual Address $\to$ Guest Physical Address), the hardware Memory Management Unit (MMU) forces every bus transaction through Stage-2 translation (translating GPA $\to$ Host Physical Address) using the base pointer in `VTTBR_EL2`.
- **Software Role**: EVE OS / Hypervisor sets up mutually exclusive Stage-2 page tables. Each tenant is granted GPA access exclusively to its own sliced ivshmem aperture (discovered dynamically at boot). Any attempt by Tenant 0 to access addresses in Tenant 1's range triggers an immediate hardware translation fault (`Data Abort` routed directly to EL2 hypervisor).

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
- **Software Role**: `amba-virt-server` clamps all guest GDMA coordinates strictly to `[0, 31 MiB)` and serializes physical device access with `pthread_mutex_t gdma_hw_mutex`.

---

## 7. Comprehensive Test Specification (CppUTest)

The functional validation suite is implemented using **CppUTest** within `guest-os/client/amba-virt-client.cxx`.

### 7.1 VFS System Call Compliance (`TEST_GROUP(VFS)`)

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

### 7.2 virtio-vsock Protocol Edge Cases (`TEST_GROUP(Vsock)`)
- **Zero-Byte Payloads**: Sending `AMBA_VIRT_IOC_SEND` with `len = 0` rejected with `-EINVAL`.
- **Maximum Payload Boundaries**: Payloads tested at 1B, 64B, 512B, 1024B, 4096B (`AMBA_VIRT_MAX_MSG`), and 4097B (rejected with `-EINVAL`).
- **Timeout Semantics**:
  - `timeout_ms = 0`: Defaults to standard 5000ms timeout.
  - `timeout_ms = 50`: Rapid timeout when awaiting non-existent response; returns `-ETIMEDOUT`.
  - `timeout_ms < 0`: Blocking wait mode until peer sends acknowledgment.
- **Connection Churn**: Rapid open/connect/close bursts (1,000 cycles) to verify socket descriptor reclamation in both guest and host kernels.

### 7.3 ivshmem Shared Memory Integrity (`TEST_GROUP(SHM)`)
- **Pattern Verification**: End-to-end writes across multi-megabyte buffers using pseudo-random bit sequences (PRBS31), incremental 64-bit integer counters, and walking-one bit patterns.
- **Cacheline & Page Alignments**: Reads and writes across 64-byte cacheline boundaries, 4KB page boundaries, and unaligned offsets.
- **Notification Handshake**:
  1. Client writes structured payload into shared memory slice at `shm_offset`.
  2. Client issues `AMBA_VIRT_IOC_SEND` containing `shm_offset`, `len`, and `FLAG_SHM_NOTIFY`.
  3. Server verifies data integrity, performs an in-place transformation (e.g., bitwise inversion or CRC calculation), and replies with `FLAG_SHM_ACK`.
  4. Client verifies the modified pattern matches expected transformed state.

### 7.4 Multi-FD Concurrency & Scaling (`TEST_GROUP(MultiFD)`)
- Multiple file descriptors opened concurrently by independent threads within a single process.
- Scaled concurrency across $T \in \{1, 2, 4, 8, 16\}$ threads.
- Measures serialization overhead and verifies that mutex locks protect the transport without deadlocking.

---

## 8. Performance Benchmarking Methodology

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

## 9. Compilation, Execution & Diagnostic Triage

### 9.1 Building the Server
```bash
# Compile host daemon for AArch64 inside NOHYPER or cross-compiler:
make -C drivers/amba_virt/tools
```
Output: `build/bin/amba-virt-server`.

### 9.2 Launching the Server Daemon
```bash
# Run server in background inside NOHYPER container:
./amba-virt-server > /tmp/amba-virt-server.log 2>&1 &
```

### 9.3 Diagnostic & Error Recovery Triage

| Failure Mode / Symptom | Root Cause | Remediation Protocol |
|---|---|---|
| `ioctl(CONNECT): Connection refused` | `amba-virt-server` not active | Check daemon on NOHYPER (`pgrep amba-virt-server`). Inspect `/tmp/amba-virt-server.log`. Restart server on port 5555. |
| `open(/dev/amba_virt): No such file or directory` (HVM) | Guest `amba_virt.ko` not inserted | Run `lspci -nn \| grep 1af4:1110`. If present, run `sudo insmod ~/amba_virt.ko`. Check `dmesg \| tail -n 20`. |
| `open(/dev/amba_virt): No such file or directory` (NOHYPER) | Container missing cgroup rule or adapter not bound | Verify `zcli edge-app-instance show <instance>`. Ensure `amba_virt` `IO_TYPE_OTHER` adapter is assigned in the hardware model. |
| `mmap: Invalid argument (EINVAL)` | Buffer size exceeds 1 GiB window | Query driver via `AMBA_VIRT_IOC_GET_INFO`. Ensure buffer offsets and mapping sizes are page-aligned and $\le$ `shm_size`. |
| Latency regression ($p99 > 2000\text{ }\mu\text{s}$) | CPU throttling or core frequency scaling | Inspect CPU governor on host (`cpupower frequency-info`). Verify background container CPU utilization. |
