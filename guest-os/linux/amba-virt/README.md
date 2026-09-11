# Ambarella Virtualization Linux Guest Driver (`amba_virt_hvm`)

This directory contains the Linux guest PCI frontend kernel module (`amba_virt.ko`) for Ubuntu HVM guests running under EVE-OS.

## Architecture

1. **PCI Ivshmem Frontend**:
   - Probes PCI vendor `0x1af4`, device `0x1110` (`ivshmem-plain`).
   - Maps PCI BAR 2 (the zero-copy shared DRAM window).
2. **Control Plane (`virtio-vsock`)**:
   - Connects to the host EVE container at CID 2, port 5555.
3. **UAPI Character Device**:
   - Registers `/dev/amba_virt` inside the Linux guest.
   - Applications interact via standard POSIX `open`, `read`, `write`, `ioctl`, and `mmap`.

## Building

This driver must be built against the **Ubuntu HVM kernel headers**, never against `eve-kernel`:

```bash
# On the Ubuntu HVM (or cross-compiling with Ubuntu linux-headers):
make KDIR_HVM=/lib/modules/$(uname -r)/build
```

Loading into the guest:
```bash
sudo insmod amba_virt.ko
```
