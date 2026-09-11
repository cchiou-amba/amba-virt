# Ambarella Virtualization QNX Driver (`amba_virt_resmgr`)

This directory contains the QNX Neutrino RTOS driver architecture for the Ambarella virtualization transport (`/dev/amba_virt`).

## QNX Microkernel vs. Linux Architecture

Unlike Linux, which loads hardware drivers into the kernel via kernel modules (`.ko`), QNX Neutrino uses a microkernel architecture where device drivers run as userspace **Resource Managers** (`resmgr`):

| Characteristic | Linux Guest (`amba_virt_hvm`) | QNX Guest (`amba_virt_resmgr`) |
|---|---|---|
| **Execution Space** | Kernel space (`EL1`) | Userspace process (`EL0`) |
| **Packaging** | Kernel module (`.ko`) | Userspace daemon binary (`resmgr`) |
| **PCI Attachment** | Kernel PCI driver (`pci_register_driver`) | Userspace PCI library (`pci_device_attach`) |
| **Memory Mapping** | `pci_iomap()` / `ioremap()` | `mmap_device_memory()` |
| **Device Node** | Character device `/dev/amba_virt` | Pathname space registration `/dev/amba_virt` via `resmgr_attach()` |
| **I/O Handling** | Kernel `file_operations` dispatch | POSIX message dispatch (`io_read`, `io_write`, `io_devctl`, `io_notify`) |

## How the QNX Resource Manager Operates

1. **PCI Discovery & Attachment**:
   Calls `pci_device_attach()` searching for vendor `0x1af4` and device `0x1110` (`ivshmem-plain`).
2. **BAR Memory Mapping**:
   Calls `mmap_device_memory()` on BAR 2 to map the shared zero-copy DRAM window into process virtual memory space.
3. **Interrupt Handling**:
   Calls `InterruptAttachEvent()` to bind the PCI MSI/INTx interrupt vector to a pulse event loop.
4. **POSIX Pathname Binding**:
   Calls `resmgr_attach()` to bind to `/dev/amba_virt`.
5. **POSIX Client Compatibility**:
   Processes standard POSIX messages from userspace clients (`open`, `read`, `write`, `devctl`/`ioctl`, `mmap`, `select`/`poll`), allowing portable clients like `amba-virt-client` to run without modification.

## Building

Building requires the QNX Software Development Platform (SDP 8.0):

```bash
source ~/qnx800/qnxsdp-env.sh
make
```
