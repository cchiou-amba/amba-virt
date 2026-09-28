# Host-Mediated Hardware TRNG Virtualization

Copyright (C) 2026, Ambarella International LLC.

## 1. Overview & Architectural Goals

The Ambarella CV3 / N1-655 hardware architecture integrates a physical True Random Number Generator (TRNG) located at memory-mapped physical address `0xe002f000`. In high-performance, virtualized edge computing deployments running EVE-OS, multiple independent guest virtual machines (Linux, QNX Neutrino RTOS, Windows 11 ARM64) and bare-metal containers concurrently require cryptographically secure, high-throughput entropy for cryptographic key generation, TLS/SSH session handshakes, process address space layout randomization (ASLR), and secure IPC.

This document describes the system architecture, security boundary model, virtualization design, and OS-specific implementation of the host-mediated VirtIO RNG virtualization subsystem.

```
+-----------------------------------------------------------------------------+
|                               Guest Domains                                 |
|                                                                             |
|  +--------------------+  +--------------------+  +-----------------------+  |
|  |   Linux (Ubuntu)   |  |   QNX RTOS (8.0)   |  |  Windows 11 (ARM64)   |  |
|  |                    |  |                    |  |                       |  |
|  |  virtio-rng-pci    |  | virtio-rng-device  |  |   virtio-rng-pci      |  |
|  |  (in-tree driver)  |  |  (devr-virtio.so)  |  |    (viorng.sys)       |  |
|  +---------+----------+  +---------+----------+  +-----------+-----------+  |
+------------|-----------------------|-------------------------|---------------+
             | (PCI Bus)             | (MMIO Bus: 0x0a000000)  | (PCI Bus)
+------------v-----------------------v-------------------------v---------------+
|                         EVE-OS Dom0 / QEMU Layer                             |
|                                                                             |
|  [QEMU Instance 1]         [QEMU Instance 2]         [QEMU Instance 3]       |
|  -object rng-random        -object rng-random        -object rng-random      |
|  filename=/dev/urandom     filename=/dev/urandom     filename=/dev/urandom   |
|  rate: 4096 B / 1000 ms    rate: 4096 B / 1000 ms    rate: 4096 B / 1000 ms  |
|             |                         |                         |           |
+-------------|-------------------------|-------------------------|-----------+
              +--------------------+----+-------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------------+
|                           Dom0 Linux Kernel                                 |
|                                                                             |
|   Dom0 CSPRNG Core Subsystem: /dev/urandom (Primary Entropy Source)         |
|                                  ^                                          |
|                                  | Continuous Reseeding                     |
|                                  |                                          |
|   Ambarella TRNG Driver: ambarella-hwrng (/dev/hwrng)                       |
+----------------------------------|------------------------------------------+
                                   | Physical MMIO
                                   v
+-----------------------------------------------------------------------------+
|              Silicon Hardware: Ambarella CV3 TRNG (0xe002f000)               |
+-----------------------------------------------------------------------------+
```

---

## 2. Security & Entropy Model

### 2.1 Exclusive Dom0 Silicon Retention

1. **Anti-Contention**: The Ambarella CV3 TRNG hardware (`rng@e002f000`) is a single, unmultiplexed hardware block. Exposing direct register access or passthrough (`vfio-platform`) to an individual guest VM or container starves Dom0 and all sibling guests of physical entropy.
2. **Silicon Ownership**: The Dom0 Linux kernel exclusively binds the hardware TRNG via the `ambarella-hwrng` driver (`/dev/hwrng`). The corresponding model adapter `hwrng` is published in hardware manifests for inventory discovery but **must remain unassigned** to any guest domain.
3. **CSPRNG Seeding**: Hardware entropy samples gathered from silicon continuously reseed the Dom0 kernel CSPRNG pool.

### 2.2 Host CSPRNG Mediation (`/dev/urandom`)

1. **Non-Blocking Availability**: QEMU backends attach to `/dev/urandom` using `-object rng-random,id=rng0,filename=/dev/urandom`. Reading `/dev/urandom` prevents QEMU launch freezes or runtime guest thread starvation that would occur if multiple domains contended directly on `/dev/hwrng`.
2. **Quota & Rate Limiting**: Every virtualized RNG device enforces a default quota of 4096 bytes per 1000 ms (`max-bytes = "4096"`, `period = "1000"`). This prevents malicious or runaway guest loops from consuming excessive host CPU cycles or entropy bandwidth.
3. **Side-Channel Isolation**: Virtual machines observe host CSPRNG output; they cannot measure raw silicon oscillator jitter or execute timing-based side-channel attacks against the physical TRNG circuitry.

---

## 3. Virtualization Architecture & Hypervisor Design

### 3.1 Hypervisor Orchestration (`eve/pkg/pillar/hypervisor/kvm.go`)

EVE's `domainmgr` orchestrator renders QEMU launch configurations deterministically via `CreateDomConfig`. For ARM64 HVM guests, RNG devices are configured based on assignable model adapter attributes (`cbattr`):

```go
// Default: Stock PCI VirtIO RNG for Linux and Windows HVMs
[object "rng0"]
  qom-type = "rng-random"
  filename = "/dev/urandom"

[device "virtio-rng0"]
  driver = "virtio-rng-pci"
  rng = "rng0"
  max-bytes = "4096"
  period = "1000"
```

### 3.2 Transport Selection Rules

1. **Standard ARM64 HVM (Linux & Windows)**:
   - Emits `virtio-rng-pci` attached to the PCIe root complex.
   - Zero custom model adapter assignment required.
2. **QNX Neutrino RTOS (MMIO Transport)**:
   - When an assigned adapter bundle carries `cbattr: {"rng": "mmio"}` (e.g., adapter `rng-mmio`), `CreateDomConfig` emits `virtio-rng-device` attached to `virtio-mmio-bus.0`:
   ```ini
   [object "rng0"]
     qom-type = "rng-random"
     filename = "/dev/urandom"

   [device "virtio-rng0"]
     driver = "virtio-rng-device"
     rng = "rng0"
     bus = "virtio-mmio-bus.0"
     max-bytes = "4096"
     period = "1000"
   ```
3. **Explicit Opt-Out (`rng=off`)**:
   - If an assigned adapter carries `cbattr: {"rng": "off"}`, neither PCI nor MMIO RNG devices are instantiated.
4. **Exclusions**:
   - **NOHYPER / OCI Containers**: Containers share the host kernel directly and do not run hypervisors; VirtIO RNG is excluded.
   - **x86 Architectures**: x86 domains do not consume ARM-specific VirtIO RNG templates.
5. **Strict Validation & Error Handling**:
   - Unknown values (e.g., `cbattr: {"rng": "invalid"}`) cause `CreateDomConfig` to fail with a descriptive error.
   - Conflicting attributes (e.g., combining `rng` with `uart`, `shmpath`, or `shmsize` in the same bundle) are rejected immediately.
   - Backend file validation ensures non-character device paths or missing files fail cleanly without corrupting running domain configurations.

---

## 4. Operating System Implementations

### 4.1 Host Dom0 (EVE-OS / Linux)

- **Silicon Binding**: Kernel module `ambarella-hwrng` registers with the Linux `hw_random` framework.
- **Verification Nodes**:
  - `/sys/class/misc/hw_random/rng_current` -> `ambarella-hwrng`
  - `/sys/class/misc/hw_random/rng_available` -> `ambarella-hwrng`
  - `/dev/hwrng` (Character device, major 10, minor 183)

### 4.2 Linux HVM Guests (Ubuntu 24.04 / Alpine 3.20)

- **Transport**: `virtio-rng-pci` (PCI device ID `1af4:1005` / modern `1af4:1044`).
- **Driver**: In-tree kernel driver `virtio_rng` (`drivers/char/hw_random/virtio-rng.c`).
- **Behavior**:
  - The guest kernel discovers the VirtIO PCI device during bus enumeration.
  - Automatically registers as `virtio_rng.0` in `/sys/class/misc/hw_random/rng_current`.
  - Kernel entropy daemon continuously mixes VirtIO entropy into `/dev/random` and `/dev/urandom`.

### 4.3 QNX Neutrino RTOS (8.0 / 7.1)

- **Transport Rationale**: QNX Neutrino RTOS standard BSPs on virtualized ARM64 platforms consume VirtIO RNG via MMIO transport.
- **Library Provenance**:
  - Driver DLL: `$QNX_TARGET/aarch64le/lib/dll/devr-virtio.so` (shipped in QNX Software Development Platform).
- **Deterministic MMIO Address**:
  - QEMU attaches `virtio-rng-device` to `virtio-mmio-bus.0` at base MMIO address `0x0a000000` (spanning `0x000000000a000000`–`0x000000000a0001ff`).
- **Image Construction & Startup Scripts**:
  - `guest-os/qnx/qnx-build/local/snippets/ifs_files.custom`:
    ```text
    sbin/devc-virtio
    lib/dll/devr-virtio.so
    ```
  - `guest-os/qnx/qnx-build/local/snippets/ifs_start.custom`:
    ```sh
    random -l devr-virtio.so:mem=0x0a000000
    ```
- **Runtime Operation**:
  - The QNX `random` server launches with `-l devr-virtio.so:mem=0x0a000000`.
  - Maps physical MMIO range `0x0a000000` into its process address space.
  - Spawns a dedicated entropy polling thread to feed `/dev/random` and `/dev/urandom`.

### 4.4 Windows 11 ARM64

- **Transport**: `virtio-rng-pci`.
- **Driver**: `viorng.sys` from the Red Hat / Fedora `virtio-win` driver repository.
- **Behavior**: Binds to the VirtIO RNG PCI vendor/device ID and feeds the Windows Cryptographic Next Generation (CNG) entropy pool.

---

## 5. Model Manifest Specification

Platform model definitions in `models/` declare the `rng-mmio` adapter to enable QNX MMIO attachment in ZedControl Cloud manifests:

```json
{
  "ztype": "IO_TYPE_OTHER",
  "phylabel": "rng-mmio",
  "phyaddrs": {},
  "logicallabel": "rng-mmio",
  "assigngrp": "rng-mmio",
  "usage": "ADAPTER_USAGE_UNSPECIFIED",
  "cbattr": {
    "rng": "mmio"
  },
  "usagePolicy": {},
  "cost": 0,
  "vfs": null,
  "parentassigngrp": ""
}
```

---

## 6. Verification & Empirical Qualification Checklist

| Subsystem / Gate | Verification Command / Target | Acceptance Criteria |
|---|---|---|
| **Dom0 Silicon Retention** | `cat /sys/class/misc/hw_random/rng_current` | Must output `ambarella-hwrng`. |
| **Dom0 Hardware Entropy** | `dd if=/dev/hwrng bs=32 count=1 \| hexdump -C` | Returns 32 non-constant hardware bytes. |
| **Linux Guest VirtIO RNG** | `cat /sys/class/misc/hw_random/rng_current` (Ubuntu) | Must output `virtio_rng.0`. |
| **Linux Entropy Read** | `dd if=/dev/hwrng bs=64 count=1 \| hexdump -C` (Ubuntu) | Returns 64 non-constant bytes. |
| **QNX MMIO Determinism** | QMP `info qtree` across 2 consecutive restarts | `virtio-rng0` attached at `0x0a000000` deterministically. |
| **QNX Module Mapping** | `pidin -p random mem` (QNX) | Shows `devr-virtio.so` and `memory ( a000000) 4096`. |
| **QNX Entropy Read** | `dd if=/dev/random bs=64 count=1 \| od -t x1` (QNX) | Returns 64 non-constant entropy bytes. |
| **Hypervisor Unit Tests** | `cd eve/pkg/pillar && go test -v -count=1 ./hypervisor/` | 100% PASS on all unit & fault-injection tests. |
| **Multi-VM PID Stability** | 30-second PID monitoring across all running domains | Zero PID churn across Alpine, Ubuntu, Windows, QNX. |

---

## 7. Operational Invariants

1. **Never passthrough `/dev/hwrng` directly**: Do not assign `hwrng` adapter to any guest VM.
2. **Never mix RNG with UART or IVSHMEM**: Adapters must strictly separate `rng` attributes from `uart`, `shmpath`, or `shmsize`.
3. **Preserve Subsystem Parity**: QNX MMIO RNG deployment must retain pre-existing serial console (`/dev/vcon1`), shared memory (`amba_shm1`), UART pass-through, and network (`vtnet0`) capabilities.
