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
  - The guest connects to host **CID 2** on dedicated ports (Port `5555` for Tenant 0, Port `5556` for Tenant 1). Port `2000` is reserved for EVE VComLink management and is explicitly rejected.
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

## 6. Comprehensive Test Specification (CppUTest)

The functional validation suite is implemented using **CppUTest** within `guest-os/client/amba-virt-client.cxx`.

### 6.1 VFS System Call Compliance (`TEST_GROUP(VFS)`)

| System Call | Scenarios Tested | Expected Behavior |
|---|---|---|
| `open()` | Valid read/write open, missing device node, flags (`O_RDWR`, `O_NONBLOCK`, `O_CLOEXEC`), permissions (`0666`) | Succeeds for valid flags; assigns private file context; lazily binds PCI resource |
| `close()` | Standard close, close with active mapped buffers, close with in-flight vsock requests | Decrements reference count; cleanly releases descriptor state |
| `read()` / `write()` | Invoking standard POSIX `read()` and `write()` | Driver relies on `ioctl` framing; character device returns `-EINVAL` without kernel crash |
| `lseek()` | `lseek(fd, 0, SEEK_SET)`, `SEEK_CUR`, `SEEK_END` | Character device returns `-ESPIPE` (illegal seek) |
| `ioctl()` | `AMBA_VIRT_IOC_GET_INFO`, `AMBA_VIRT_IOC_CONNECT`, `AMBA_VIRT_IOC_SEND`, `AMBA_VIRT_IOC_RECV` | Valid inputs return 0; invalid pointer arguments return `-EFAULT`; unmapped commands return `-ENOTTY` |
| `mmap()` | Map full `shm_size`, map 4KB/64KB pages, unaligned offsets, boundary mapping, over-allocation | Maps PCI BAR 2 pages into user space with `PROT_READ \| PROT_WRITE`; over-allocation returns `-EINVAL` |
| `munmap()` / `msync()` | Unmap active pages, invoke `msync(MS_SYNC)` / `msync(MS_ASYNC)` across mapped bounds | Unmaps pages without faulting; memory synchronizes cleanly |
| `fcntl()` | Toggle `O_NONBLOCK`, verify `F_GETFL`/`F_SETFL`, descriptor duplication via `dup()`, `dup2()` | Properly duplicates descriptor; shares underlying kernel file object |
| `poll()` / `select()` | Edge-triggered and level-triggered polling on `/dev/amba_virt` | Returns readability/writability mask based on vsock queue occupancy |

### 6.2 virtio-vsock Protocol Edge Cases (`TEST_GROUP(Vsock)`)
- **Zero-Byte Payloads**: Sending `AMBA_VIRT_IOC_SEND` with `len = 0` rejected with `-EINVAL`.
- **Maximum Payload Boundaries**: Payloads tested at 1B, 64B, 512B, 1024B, 4096B (`AMBA_VIRT_MAX_MSG`), and 4097B (rejected with `-EINVAL`).
- **Timeout Semantics**:
  - `timeout_ms = 0`: Defaults to standard 5000ms timeout.
  - `timeout_ms = 50`: Rapid timeout when awaiting non-existent response; returns `-ETIMEDOUT`.
  - `timeout_ms < 0`: Blocking wait mode until peer sends acknowledgment.
- **Connection Churn**: Rapid open/connect/close bursts (1,000 cycles) to verify socket descriptor reclamation in both guest and host kernels.

### 6.3 ivshmem Shared Memory Integrity (`TEST_GROUP(SHM)`)
- **Pattern Verification**: End-to-end writes across multi-megabyte buffers using pseudo-random bit sequences (PRBS31), incremental 64-bit integer counters, and walking-one bit patterns.
- **Cacheline & Page Alignments**: Reads and writes across 64-byte cacheline boundaries, 4KB page boundaries, and unaligned offsets.
- **Notification Handshake**:
  1. Client writes structured payload into shared memory slice at `shm_offset`.
  2. Client issues `AMBA_VIRT_IOC_SEND` containing `shm_offset`, `len`, and `FLAG_SHM_NOTIFY`.
  3. Server verifies data integrity, performs an in-place transformation (e.g., bitwise inversion or CRC calculation), and replies with `FLAG_SHM_ACK`.
  4. Client verifies the modified pattern matches expected transformed state.

### 6.4 Multi-FD Concurrency & Scaling (`TEST_GROUP(MultiFD)`)
- Multiple file descriptors opened concurrently by independent threads within a single process.
- Scaled concurrency across $T \in \{1, 2, 4, 8, 16\}$ threads.
- Measures serialization overhead and verifies that mutex locks protect the transport without deadlocking.

---

## 7. Performance Benchmarking Methodology

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

## 8. Compilation, Execution & Diagnostic Triage

### 8.1 Building the Server
```bash
# Compile host daemon for AArch64 inside NOHYPER or cross-compiler:
make -C drivers/amba_virt/tools
```
Output: `build/bin/amba-virt-server`.

### 8.2 Launching the Server Daemon
```bash
# Run server in background inside NOHYPER container:
./amba-virt-server > /tmp/amba-virt-server.log 2>&1 &
```

### 8.3 Diagnostic & Error Recovery Triage

| Failure Mode / Symptom | Root Cause | Remediation Protocol |
|---|---|---|
| `ioctl(CONNECT): Connection refused` | `amba-virt-server` not active | Check daemon on NOHYPER (`pgrep amba-virt-server`). Inspect `/tmp/amba-virt-server.log`. Restart server on port 5555. |
| `open(/dev/amba_virt): No such file or directory` (HVM) | Guest `amba_virt.ko` not inserted | Run `lspci -nn \| grep 1af4:1110`. If present, run `sudo insmod ~/amba_virt.ko`. Check `dmesg \| tail -n 20`. |
| `open(/dev/amba_virt): No such file or directory` (NOHYPER) | Container missing cgroup rule or adapter not bound | Verify `zcli edge-app-instance show <instance>`. Ensure `amba_virt` `IO_TYPE_OTHER` adapter is assigned in the hardware model. |
| `mmap: Invalid argument (EINVAL)` | Buffer size exceeds 1 GiB window | Query driver via `AMBA_VIRT_IOC_GET_INFO`. Ensure buffer offsets and mapping sizes are page-aligned and $\le$ `shm_size`. |
| Latency regression ($p99 > 2000\text{ }\mu\text{s}$) | CPU throttling or core frequency scaling | Inspect CPU governor on host (`cpupower frequency-info`). Verify background container CPU utilization. |
