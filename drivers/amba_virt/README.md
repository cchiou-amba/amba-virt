# Ambarella Virtualization Host Driver (`amba_virt`)

This directory contains the host-side NOHYPER kernel module (`amba_virt.ko`) and arbitrator daemon (`tools/amba-virt-server`) for EVE-OS on Ambarella platforms.

## Architecture

The host driver provides the host endpoint of the virtualization transport:
1. **Zero-Copy Shared Memory**: Maps the shared host backing file (`/dev/shm/amba-virt`, default 1 GiB for production) backing the guest's `ivshmem-plain` window.
2. **Control Plane (`virtio-vsock`)**: Listens on guest-to-host vsock port `5555` (CID 2). Port `2000` is reserved for EVE VComLink and is explicitly rejected.
3. **UAPI Character Device**: Exposes `/dev/amba_virt` on the EVE host with framing and ioctl semantics defined in [`include/uapi/amba_virt.h`](include/uapi/amba_virt.h).

## Directory Structure

```text
drivers/amba_virt/
├── Kbuild                   # Out-of-tree kbuild definition
├── Makefile                 # Driver & daemon build driver
├── README.md                # This document
├── amba_virt_nohyper.c      # Host char dev registration & vsock server
├── amba_virt_core.c         # Core ringbuffer and transport engine
├── amba_virt_core.h         # Core transport internal definitions
├── include/
│   └── uapi/
│       ├── amba_virt.h      # Public UAPI framing & ioctls
│       └── amba_virt_test.h # Public test ioctl definitions
└── tools/
    ├── Makefile             # Userspace daemon Makefile
    └── amba-virt-server.c   # Arbitrator & echo server daemon
```

## Building

### Out-of-Tree (Development Mode)

The driver is automatically discovered by the top-level `Makefile`. To build and sign:

```bash
# From repository root:
make drivers
# or build amba_virt individually:
make amba_virt
```

Outputs:
- Kernel module: `build/modules/amba_virt.ko` (cryptographically signed)
- Arbitrator daemon: `build/bin/amba-virt-server`

### In-Tree (Production Mode)

In production mode, `eve-kernel/Dockerfile.ambarella` mounts `drivers/amba_virt/` via `--build-context amba-virt=drivers/amba_virt` and compiles the driver directly into `rootfs.img`.
