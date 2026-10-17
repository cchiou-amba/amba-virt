# Ambarella Virtualization Portable Client & Benchmark (`amba-virt-client`)

This directory contains the portable POSIX C++ client, unit test suite (CppUTest), and performance benchmark for the Ambarella virtualization transport.

## Portability across Linux and QNX Neutrino

The client is designed to run interchangeably on:
- **Linux Guests** (Ubuntu HVM at EL1, using `amba_virt_hvm.ko`)
- **QNX Neutrino Guests** (QNX 8.0 HVM at EL1, using `amba_virt_resmgr`)

Both operating systems expose the standard POSIX character device interface at `/dev/amba_virt`. The device abstraction [`amba_virt_dev.hxx`](amba_virt_dev.hxx) encapsulates POSIX file I/O (`open`, `read`, `write`, `ioctl`, `mmap`, `poll`) and abstracts platform-specific device node discovery.

## Building

### On Linux Guest
```bash
make OS=linux
```

### On QNX Neutrino Guest
```bash
source ~/qnx800/qnxsdp-env.sh
make OS=qnx
```

## Running Tests & Benchmarks

Opening `/dev/amba_virt` requires root privileges (`0600`), so `amba-virt-client` and `amba-virt-cli` must be run under `sudo` on Linux guests.

### Full 47-Test Suite (Automatic Environment Detection)
At startup, `amba-virt-client` inspects the guest's active BAR partitions (`queryDevMem()`). All 47 tests across all 13 groups run directly via bare `sudo ./amba-virt-client` (or `sudo amba-virt-client -v`) whether `amba-virt-demo` and `amba_cavalry` are active (`live-coexistence` mode) or stopped (`clean-pool` mode):
```bash
sudo ./amba-virt-client
sudo ./amba-virt-client -v
```

Group filter flags (`-g <group>`, `-sg <substr>`, `-xg <substr>`) remain available for selective runs.

### IPC, ivshmem Handoff, and GDMA Benchmark
```bash
sudo ./amba-virt-client -bench -json bench.json
```
