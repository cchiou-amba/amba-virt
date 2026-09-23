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
| |   | - Autostarted via /persist/bin/load-ambarella-drivers.sh (Backoff: 5 crashes/60s -> 30s)  |   | |
| |   | - Startup Self-Test (/persist/modules, /persist/firmware, /proc/modules validation)       |   | |
| |   | - Module Lifecycle (insmod, rmmod, cascade loader)                                        |   | |
| |   | - Hardware Reset Executor (VisORC NPU reset register write)                               |   | |
| |   | - Procfs Watcher Thread (/proc/modules polling pushing BACKEND_EVENT_MODULE_CHANGED)      |   | |
| |   | - Firmware Inventory & Verification Service (BACKEND_OP_FIRMWARE_VERSIONS)                |   | |
| |   +───────────────────────────────────────────────────────────────────────────────────────────+   | |
| +───────────────────────────────────────────────────────────────────────────────────────────────────+ |
+───────────────────────────────────────────────────────────────────────────────────────────────────────+
```

---

## 2. Component Protocols & Lifecycle

### 2.1 Token-Based Authentication
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
