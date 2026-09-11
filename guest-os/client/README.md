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

### Functional Tests (CppUTest)
```bash
./amba-virt-client -v
```

### IPC Latency and Throughput Benchmark
```bash
./amba-virt-client -bench
```
