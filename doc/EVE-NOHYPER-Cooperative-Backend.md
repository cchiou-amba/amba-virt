# EVE + NOHYPER Cooperative Architecture & Hardware Management

## 1. Overview & Architectural Topology

The Ambarella edge virtualization platform (`amba-virt`) implements a 3-tier cooperative architecture across EVE-OS Dom0, privileged NOHYPER containers, and guest HVM virtual machines (Ubuntu, Alpine, QNX).

```text
+-------------------------------------------------------------------------------------------------------+
| Outside World / Remote CLI / External Management Network                                              |
+-------------------------------------------------------------------------------------------------------+
                                                     │ (Network Access to NOHYPER / HVMs)
                                                     │ (Bare-metal SSH is DISABLED)
                                                     ▼
+───────────────────────────────────────────────────────────────────────────────────────────────────────+
| Ambarella N1-655 SoC  │  EVE-OS Edge Virtualization Platform                                          |
|                                                                                                       |
| +─────────────────────────────+  +─────────────────────────────+  +─────────────────────────────────+ |
| | Guest VM: Ubuntu 24.04 HVM  |  | Guest VM: Alpine 3.20 HVM   |  | Guest VM: QNX Neutrino 8.0 HVM  | |
| | - User NN / YOLO Apps       |  | - Lightweight Edge Worker   |  | - Hard Real-Time Control App    | |
| | - amba-virt-cli (caps/mon/fw|  | - amba-virt-cli             |  | - QNX Resource Managers         | |
| | - Virtual Drivers (Kernel): |  | - Virtual Drivers (Kernel): |  | - Virtual Drivers (resmgr):     | |
| |   * /dev/cavalry (shim)     |  |   * /dev/cavalry (shim)     |  |   * /dev/cavalry (resmgr)       | |
| |   * /dev/amba_gdma          |  |   * /dev/amba_gdma          |  |   * /dev/amba_gdma (resmgr)     | |
| |   * /dev/iav, /dev/amba_otp |  |   * /dev/iav, /dev/amba_otp |  |   * /dev/iav, /dev/amba_otp     | |
| | - amba_virt_guest transport |  | - amba_virt_guest transport |  | - QNX Pulse / Sigevent Notif    | |
| |   (Notifier Chain / uevent) |  |   (Notifier Chain / uevent) |  |   (State: ONLINE <-> OFFLINE)   | |
| |   (Probe Init Query + Sync) |  |   (Probe Init Query + Sync) |  |   (Probe Init Query + Sync)     | |
| +──────────────┬──────────────+  +──────────────┬──────────────+  +────────────────┬────────────────+ |
|                │ (vsock + BAR)                  │ (vsock + BAR)                    │ (vsock + BAR)    |
|                ▼                                ▼                                  ▼                  |
| +───────────────────────────────────────────────────────────────────────────────────────────────────+ |
| | Hypervisor Layer (KVM / QEMU Domains)                                                             | |
| | - vhost-vsock-pci (CID 2, Port 5555 Control Plane & Async Event Push Stream)                      | |
| | - PCI ivshmem-plain (1 GiB Zero-Copy Shared DRAM BAR per Guest)                                   | |
| +──────────────────────────────────────────────┬────────────────────────────────────────────────────+ |
|                                                │                                                      |
|                                                ▼                                                      |
| +───────────────────────────────────────────────────────────────────────────────────────────────────+ |
| | NOHYPER Container (Privileged Virtualization Host & Gateway Domain)                               | |
| |                                                                                                   | |
| |   +--------------------------------------+     +----------------------------------------------+   | |
| |   | amba-virt-ctl (Host Admin CLI)       |────>| /run/amba-virt/admin.sock (UNIX Domain Sock) |   | |
| |   | - backend status / module / pipeline |     +──────────────────────┬───────────────────────+   | |
| |   +--------------------------------------+                            │                           | |
| |                                                                       │                           | |
| |   +───────────────────────────────────────────────────────────────────▼───────────────────────+   | |
| |   | amba-virt-server (Virtualization Arbitrator & Gateway Daemon)                             |   | |
| |   | - Shared Dependency Matrix (virt_driver_matrix.h / struct virt_module_dep)                |   | |
| |   | - 200ms Graceful In-Flight Task Drain Protocol with Hardware Reset Fallback               |   | |
| |   | - Kernel Transport: AMBA_VIRT_IOC_RECV, AMBA_VIRT_IOC_SEND                                |   | |
| |   | - Kernel Push Broadcaster: AMBA_VIRT_IOC_PUSH & AMBA_VIRT_IOC_LIST_GUESTS                 |   | |
| |   | - Asynchronous State Broadcast Engine (AMBA_VIRT_MSG_DEV_STATE_EVENT, MsgType 40+)       |   | |
| |   | - Introspection & Capability Discovery Engine (AMBA_VIRT_QUERY_DRIVER_CAPS)               |   | |
| |   | - Persistent Outbound TCP Client Manager (Auto-reconnect, Token Auth Handshake,           |   | |
| |   |   Heartbeat Ping Loop [5-10s], Full Status Sync on Reconnect [BACKEND_OP_FULL_STATUS])    |   | |
| |   | - Strict Isolation & ACL Validator (virt_acl.c, virt_mem_pool.c bounds checks)           |   | |
| |   | - Structured Logging Engine (LOG_EVENT with EVT-xxx IDs)                                  |   | |
| |   +──────────────────────────────────────────────┬────────────────────────────────────────────+   | |
| +──────────────────────────────────────────────────┼────────────────────────────────────────────────+ |
|                                                    │ Persistent Bidirectional TCP Channel             |
|                                                    │ (Token-Auth, Bridge-Bound, Single-Conn Lock)     |
|                                                    ▼                                                  |
| +───────────────────────────────────────────────────────────────────────────────────────────────────+ |
| | EVE-OS Bare-Metal (Dom0 Host Kernel & Privileged Hardware Control Plane)                          | |
| |                                                                                                   | |
| |   +───────────────────────────────────────────────────────────────────────────────────────────+   | |
| |   | amba-virt-backend (Bare-Metal Helper & Execution Daemon)                                  |   | |
| |   | - Shipped in EVE image at /usr/bin/amba-virt-backend (autostarted via Dom0 /etc/init.d/)  |   | |
| |   | - Listens on TCP port 5556, single authenticated session                                  |   | |
| |   | - Module Lifecycle (loads from image /lib/modules/.../extra/ or /persist/modules/)        |   | |
| |   | - Two-stage module store protocol (BACKEND_OP_MODULE_STORE probe/upload)                  |   | |
| |   | - Parameterised module insertion (finit_module with parameter strings)                    |   | |
| |   | - Hardware Reset Executor (VisORC NPU reset register write)                               |   | |
| |   | - Procfs Watcher Thread (/proc/modules polling pushing BACKEND_EVENT_MODULE_CHANGED)      |   | |
| |   +───────────────────────────────────────────────────────────────────────────────────────────+   | |
| +───────────────────────────────────────────────────────────────────────────────────────────────────+ |
+───────────────────────────────────────────────────────────────────────────────────────────────────────+
```

---

## 2. Component Protocols & Lifecycle

### 2.1 Token-Based Authentication & Image Location
- `amba-virt-backend` is built into the EVE image and installed at `/usr/bin/amba-virt-backend` (part of `pkg/dom0-ztools`). Dom0 autostarts it via `/etc/init.d/020-amba-virt-backend`. It does not reside in `/persist/bin`.
- `amba-virt-backend` binds to port 5556 on the internal container bridge.
- On startup, it reads or creates a cryptographically random token in `/persist/etc/amba-virt-backend.token` (`0600`).
- `amba-virt-server` mounts `/persist/etc` and sends `BACKEND_MSG_AUTH_REQ` within 2 seconds.
- Only a single active authenticated session is permitted.

### 2.2 2-Stage Drain & Hardware Reset Protocol
When unloading backing host drivers:
1. `amba-virt-server` immediately issues `AMBA_VIRT_IOC_PUSH` broadcasting `AMBA_VIRT_DEV_STATE_OFFLINE` to all active guest CIDs.
2. Virtual drivers reject new ioctls with `-EHOSTDOWN` (Linux) or `ENETDOWN` (QNX).
3. `cavalry_proxy` begins a 200ms graceful drain waiting for in-flight DAGs.
4. If requests drain $\le 200\text{ms}$, local descriptors close cleanly.
5. If in-flight requests hang $> 200\text{ms}$, server sends `BACKEND_OP_HARDWARE_RESET` to halt the VisORC accelerator before `delete_module()` is invoked.

### 2.3 Module-Load Payload Split (`BACKEND_OP_MODULE_LOAD`)
Opcode: `BACKEND_OP_MODULE_LOAD` (`0x07`).
- **Name-Only (Legacy)**: If the payload contains no `NUL`, the payload is treated as the module name and parameters are empty.
- **Parameterised Load**: If the payload contains a `NUL`:
  - Bytes before the first `NUL` define the module name (at most 63 bytes, no slashes).
  - Bytes after the first `NUL` up to `hdr.len` (truncated at a second `NUL`) define the parameter string (at most 255 bytes, restricted to `[A-Za-z0-9_ =.,]`).
- The backend invokes `finit_module(fd, params, 0)`.
- If the module already exists in the kernel (`-EEXIST`), it is treated as a success.
- Payloads exceeding 320 bytes (`BACKEND_MODULE_LOAD_MAX_PAYLOAD`) are rejected with `-EMSGSIZE` and their wire bytes drained to maintain socket framing.
- The backend executes only the named module; cascade loading is disabled to prevent dropping explicit parameters.

### 2.4 Two-Stage Module Store Protocol (`BACKEND_OP_MODULE_STORE`)
To transfer out-of-tree kernel modules from NOHYPER Debian packages to the host kernel without manual staging:
1. **Probe (Stage 1)**: The client sends `BACKEND_OP_MODULE_STORE` (`0x11`) with payload `<basename>` (no file bytes).
   - If `/lib/modules/<release>/extra/<basename>` exists in the immutable EVE image, backend responds with disposition `0x00` (`ALREADY_PRESENT_IMAGE`).
   - If `/persist/modules/<basename>` exists on host persist, backend responds with `0x00` (`ALREADY_PRESENT_PERSIST`).
   - Otherwise, backend responds with disposition `0x01` (`UPLOAD_REQUIRED`).
2. **Upload (Stage 2)**: On `UPLOAD_REQUIRED`, the client reads `/usr/lib/amba-virt/modules/<basename>` and sends a second `BACKEND_OP_MODULE_STORE` with payload `<basename>\0<file_bytes>` (up to 64 MiB limit).
   - The backend streams the uploaded bytes into a temporary file under `/persist/modules/`, syncs via `fsync()`, and atomically renames to the destination.
   - The client then proceeds to issue `BACKEND_OP_MODULE_LOAD`.

### 2.5 Persist Filesystem Governance & Preserve List
The `/persist` filesystem is strictly reserved for EVE state and controlled runtime tokens:
- **Clean Install State**: At EVE installation, zero `amba-virt` files exist on `/persist`. Old bringup tools (`load-ambarella-drivers.sh`, `fresh-bringup/`, `/persist/firmware/`, `/persist/bin/`) are retired and deleted.
- **Permitted Runtime State**:
  - `/persist/etc/amba-virt-backend.token` (runtime authentication token).
  - `/persist/modules/<name>.ko` (kernel modules uploaded dynamically via Section 2.4).
  - No other amba-virt files or directories belong on `/persist`. The backend logs to stdout and never creates `/persist/log/`.
- **EVE Preserved Directories**: The following native EVE directories are preserved and untouched:
  `/persist/etc/`, `appdata/`, `certs/`, `clear-node/`, `config/`, `containerd/`, `downloads/`, `eve/`, `eve-current/`, `img/`, `kcrashes/`, `netdump/`, `newlog/`, `rkt/`, `status/`, `vault/`, `wpa_supplicant/`.

---

## 3. Administration & CLI

### Host Administration (`amba-virt-ctl`)
```bash
# Check backend connection and loaded host module bitmask
amba-virt-ctl backend status

# Load / Unload modules
amba-virt-ctl module load cavalry.ko
amba-virt-ctl module unload cavalry.ko

# Start functional pipelines
amba-virt-ctl pipeline start npu
amba-virt-ctl pipeline start camera

# Query firmware inventory
amba-virt-ctl firmware
```

### Guest Virtual Driver & Availability Introspection (`amba-virt-cli`)
```bash
# Query virtual driver availability
amba-virt-cli drivers

# Query host module inventory
amba-virt-cli modules

# Stream real-time hardware state transitions
amba-virt-cli monitor
```

---

## 4. Formalized Regression Harness

The test harness runs 5 specialized test suites guarding boundaries, race conditions, fuzzing, rogue exploits, and hardware safety:
- **Suite A**: Boundary & Limit Stress (`test_boundary_stress.c`)
- **Suite B**: Desync & State Reconciliation (`test_state_desync.c`)
- **Suite C**: Fuzzing & Corruption Protection (`test_fuzz_corruption.c`)
- **Suite D**: Rogue HVM Security Exploits (`test_security_exploits.c`)
- **Suite E**: Hardware Safety & Drain Timeouts (`test_hardware_safety.c`)

Run the full automated test suite:
```bash
./drivers/amba_virt/tools/run_regression_cooperative.sh [--host <target-node>]
```
