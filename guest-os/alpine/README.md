# Alpine Linux 3.20 AArch64 HVM Guest on Ambarella EVE

*Copyright (C) 2026, Ambarella International LLC*

Builds and deploys the out-of-tree kernel modules (`amba_virt.ko`, `amba_dma.ko`, `amba_uart.ko`, `amba_cavalry.ko`, `iav_virt.ko`), static musl userspace tools (`amba-virt-client`, `amba-virt-cli`), and OpenRC services for the Alpine Linux 3.20 AArch64 HVM guest (`alpine_3_20`) on `n1-655-devkit` and `n1-655-pro`.

## Design Contract

- **Base Image**: `alpine-3.20-arm64-qcow2` (`cloud-images/alpine-3.20-aarch64.qcow2`, kernel `6.6.142-0-virt` with ACPI `AMBA0001` support and OpenRC init).
- **Admission Ticket & Slice**: `amba_shm:amba_shm1` (`/dev/amba_virt_shm1`, slice `1`, `camera=1`).
- **Hardware UART Passthrough**: `UART3:UART3` (`ffe0019000.uart`, guest `/dev/ttyAMBA0` at `115200 8N1` with hardware level IRQ and Generic-DMA1 DMA via `amba_dma.ko` + `amba_uart.ko`), wired to physical `console2` (devkit telnet `6072`, pro telnet `6062`) with an OpenRC `/etc/inittab` getty.
- **Userspace & Demo**: Static musl binaries (`amba-virt-client`, `amba_virt_demo-musl`) and OpenRC service `/etc/init.d/amba-virt-demo` on port `8081`.

## Usage

```bash
# 1. Cross-compile Alpine 3.20 kernel modules and static musl client binaries
make guest-alpine

# 2. Create the instance on slice 1 with amba_shm1 and UART3
./scripts/create_instance.sh alpine_3_20.n1-655-devkit \
  --edge-app=alpine_3_20 \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=amba_shm:amba_shm1 \
  --adapter=UART3:UART3 \
  --no-custom-configuration

# 3. Deploy modules, demo service, and OpenRC ttyAMBA0 getty to the running guest
./guest-os/deploy_guest.sh n1-655-devkit-alpine --provision --reload --enable-serial --test
```

## Names

| Item | Value |
|---|---|
| Edge-app | `alpine_3_20` ([apps/alpine_3_20.json](../../apps/alpine_3_20.json)) |
| Instance | `alpine_3_20.n1-655-devkit`, `alpine_3_20.n1-655-pro` |
| Image in ZedControl | `alpine-3.20-arm64-qcow2` |
| Adapters at create | `--adapter=amba_shm:amba_shm1 --adapter=UART3:UART3` |
| SSH | `ssh root@<node-ip> -p 2422` (alias `n1-655-devkit-alpine` / `n1-655-pro-alpine`), password `alpine` |
| Physical Console | `console2` (`UART3`, `/dev/ttyAMBA0` at `115200`, devkit `6072`, pro `6062`) |
| Demo | port `8081` |

Full from-zero provisioning recipe for all six guests: [doc/EVE-EdgeApp-Instances.md](../../doc/EVE-EdgeApp-Instances.md).
