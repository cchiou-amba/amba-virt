# Ambarella NOHYPER Target Applications & Test Suites

This directory contains target-side applications, benchmark utilities, and validation suites designed to run directly within the bare-metal **NOHYPER** host container (`n1-655-devkit-nohyper` / `n1-655-pro-nohyper`).

## Directory Layout

| Subdirectory | Description | Target Node |
|---|---|---|
| [`nohyper/cavalry_shm_test`](cavalry_shm_test/) | Native Ambarella Cavalry (VisORC VP) ivshmem USER window validation suite | `n1-655-devkit-nohyper` |

## Architecture & Execution Context

- **NOHYPER Environment**: A bare-metal container running on LF Edge EVE-OS with direct device node injection (`/dev/cavalry`, `/dev/amba_virt`, `/dev/iav`, `/dev/gpio`).
- **Hardware Access**: Directly accesses physical hardware resources and shared memory windows without hypervisor Stage-2 overhead.
- **Cross-Compilation**: Compiled for `aarch64` using `aarch64-linux-gnu-gcc`.
