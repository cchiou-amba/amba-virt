# Ambarella Cavalry QNX Resource Manager (`amba_cavalry_resmgr`)

*Copyright (C) 2026, Ambarella International LLC*

This directory contains the QNX Neutrino RTOS 8.0 resource manager for Ambarella Cavalry NPU acceleration (`/dev/cavalry`).

---

## 1. Architectural Overview

In monolithic Linux guests, hardware virtualization is implemented via the `amba_cavalry.ko` kernel module operating in `EL1`. On **BlackBerry QNX Neutrino RTOS 8.0**, the driver executes entirely in **EL0 userspace** as a POSIX resource manager:

```text
+-------------------------------------------------------------------------+
| QNX 8.0 Guest Userspace (EL0)                                           |
|                                                                         |
|  +---------------------------------+   +-----------------------------+  |
|  | cavalry_hvm_demo / nnctrl / YOLO|   | amba-virt-client / CLI      |  |
|  +---------------------------------+   +-----------------------------+  |
|                 | (devctl / mmap)                     | (devctl)        |
|                 v                                     v                 |
|  +---------------------------------+   +-----------------------------+  |
|  | amba-cavalry-resmgr             |   | amba-virt-resmgr            |  |
|  | (/dev/cavalry)                  |   | (/dev/amba_virt)            |  |
|  +---------------------------------+   +-----------------------------+  |
|                 |                                     |                 |
|                 +---------(amba_virt_rpc API)--------->                 |
|                                                       | (vsock/bridge)  |
+-------------------------------------------------------|-----------------+
                                                        v
+-------------------------------------------------------------------------+
| Host Hypervisor & NOHYPER Control Plane                                 |
|   - amba-virt-server daemon (CID 2, Port 5555)                          |
|   - cavalry_proxy -> host /dev/cavalry -> VisORC Hardware               |
|   - ivshmem BAR 2 (Shared DRAM Aperture)                                |
+-------------------------------------------------------------------------+
```

---

## 2. Key Responsibilities

1. **Pathname Space Registration**:
   Registers `/dev/cavalry` via `resmgr_attach()` to provide binary compatibility with standard Ambarella neural network runtimes (`nnctrl`, `cavalry_mem`, `cavalry_hvm_demo`, `cavalry_hvm_yolo`).

2. **Memory Pool Negotiation**:
   During initialization, queries the host virtualization server via `AMBA_VIRT_MSG_DEV_SET_BOUNDS_REQ` to negotiate:
   - **Cavalry Memory Pool**: Sub-allocated buffer window within ivshmem BAR 2 (default 960 MB at offset `0x02000000`).
   - **RPC Control Arena**: Dedicated 1 MB staging buffer for DAG descriptors and network headers.

3. **Zero-Copy Memory Mapping (`io_mmap`)**:
   Translates `mmap()` requests from client processes into physical BAR 2 projections via `mmap_device_memory()`, allowing zero-copy tensor input/output sharing with the host VisORC.

4. **Microkernel Cross-Process Pointer Marshaling**:
   Because the resource manager runs in a distinct virtual address space from client applications, pointers passed in ioctls (e.g., `reg_u->run_dags_ptr` in `CAVALRY_IOC_REGISTER_DAG`) cannot be dereferenced directly. The manager accesses client memory via `/proc/<pid>/as` using `pread()`:
   ```c
   int fd = open("/proc/<client_pid>/as", O_RDONLY);
   pread(fd, dst_buf, len, (off_t)client_vaddr);
   ```

5. **ABI & Devctl Translation**:
   Translates standard Ambarella `CAVALRY_IOC_*` devctls into synchronous framed RPC packets dispatched to host `cavalry_proxy`.

---

## 3. Supported Devctls & Operations

| Operation | Devctl Command | Backend Translation |
|---|---|---|
| Query Chip ID | `CAVALRY_GET_CV_CHIP_ID` | `VCAV_OP_GET_CHIP_ID` RPC |
| Driver Status | `CAVALRY_GET_CAVALRY_STATUS` | `VCAV_OP_GET_STATUS` RPC |
| Memory Allocate | `CAVALRY_IOC_MEM_ALLOC` | Local pool sub-allocation in BAR 2 |
| Memory Free | `CAVALRY_IOC_MEM_FREE` | Local pool deallocation |
| Memory Map | `io_mmap` handler | Returns `g_cav.bar_phys + offset` |
| Cache Flush/Invalidate | `CAVALRY_SYNC_CACHE_MEM` | `VCAV_OP_SYNC_CACHE` RPC |
| Start/Stop VP | `CAVALRY_START_VP` / `CAVALRY_STOP_VP` | `VCAV_OP_START_VP` RPC |
| Register DAG / Net | `CAVALRY_IOC_REGISTER_DAG` | Copies DAG from client `/proc/<pid>/as` to arena, sends `VCAV_OP_REGISTER_DAG` RPC |
| Unregister DAG | `CAVALRY_IOC_UNREGISTER_DAG` | `VCAV_OP_UNREGISTER_DAG` RPC |
| Run DAGs (Inference) | `CAVALRY_RUN_DAGS` | Stages execution packet into arena, sends `VCAV_OP_RUN_DAGS` RPC |

---

## 4. Building & Running

### Build with QNX SDP 8.0

```bash
source ~/qnx/qnx800/qnxsdp-env.sh
make
```

### Running on Target Node

```bash
# Start amba-virt core transport daemon first
/data/bin/amba-virt-resmgr

# Start Cavalry NPU resource manager
/data/bin/amba-cavalry-resmgr

# Verify device registration
ls -l /dev/amba_virt /dev/cavalry
```
