# EVE Ambarella hardware models

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

ZEDEDA Cloud inventory for Ambarella N1-655. **Cloud `ioMemberList` is
authoritative.** This is not an app-instance or QEMU recipe.

Related: [Architecture.md](Architecture.md) (Dom0 server and device ownership),
[AmbaVirtServer.md](AmbaVirtServer.md) (vsock + ivshmem server),
[ZedControl-scripts.md](ZedControl-scripts.md) (zcli wrappers),
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md) (deploy HVM guests),
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md) (do not update in place),
[drivers/amba_virt/](../drivers/amba_virt/README.md) and [guest-os/](../guest-os/).

Brand **Ambarella** is `ORIGIN_LOCAL`. Two models exist. Do not invent a third.

| Cloud name | Title | UUID | State | rev |
|---|---|---|---|---|
| `N1-655-Cooper-Pro` | Ambarella N1-655 Cooper Pro | `0edf44c9-af69-428c-a835-ac001715e6d3` | Active | 4 |
| `N1-655-Cooper-Devkit` | Ambarella N1-655 Cooper Devkit | `dc7d22b4-855b-4267-be8f-0e5ced74b728` | Active | 3 |

Both are ARM64, 4 CPUs, 32G memory, 32G storage, watchdog on, HSM/LEDs off.

> [!NOTE]
> Published model attributes and adapter notes:
>
> - **`watchdog` is operational (`true`).** Following Wave 2 driver integration,
>   `ambarella_wdt.c` is active (`fff4001000.wdt`, creating `/dev/watchdog`),
>   confirming the model attribute `"watchdog": "true"`.
> - **`cavalry`, `cavalry_profile`, `gpio0`, `iav`, and `amba_virt` are
>   not in the model.** `amba-virt-server` in EVE Dom0 loads their modules
>   and owns the nodes. They are not assignable.
> - **`hwrng` is published.** `/dev/hwrng` is created by the active
>   `ambarella-rng.c` hardware RNG driver and published in the model.
>
> Everything else below reflects the deployed image.
DTS `model` is `n1-655 cooper pro`. Source JSON:
[models/N1-655-Cooper-Pro.json](../models/N1-655-Cooper-Pro.json),
[models/N1-655-Cooper-Devkit.json](../models/N1-655-Cooper-Devkit.json).
Refresh those files from the controller with
[`scripts/get_models.sh`](../scripts/get_models.sh)
(`ZCLI_TOKEN` already in the environment; `--dry-run` prints without writing).
All ZedControl wrappers: [ZedControl-scripts.md](ZedControl-scripts.md).

## ztype map

| Cloud `ztype` | EVE `PhyIoType` | Number |
|---|---|---|
| `IO_TYPE_ETH` | `PhyIoNetEth` | 1 |
| `IO_TYPE_USB` | `PhyIoUSB` | 2 |
| `IO_TYPE_COM` | `PhyIoCOM` | 3 |
| `IO_TYPE_HDMI` | `PhyIoHDMI` | 7 |
| `IO_TYPE_USB_CONTROLLER` | `PhyIoUSBController` | 13 |
| `IO_TYPE_CAN` | `PhyIoCAN` | 15 |
| `IO_TYPE_OTHER` | `PhyIoOther` | 255 |

Those are the types in use. Prefer a specific type over `IO_TYPE_OTHER` wherever EVE
has native semantics for it.

`PhyIoOther` uses `phyaddrs.Ifname` as a host chardev. EVE injects that path
into a container's OCI spec when an app is assigned the `assigngrp`. The
reference design assigns none of the Ambarella chardevs this way.

## Assignment rules

- Empty `assigngrp`: EVE keeps the adapter; apps cannot take it.
- Same non-empty `assigngrp`: one unit, assignable to one app instance.
- `usage` `ADAPTER_USAGE_MANAGEMENT`: EVE management port. Do not assign it
  to an app even if `assigngrp` is set.
- `cavalry`, `gpio0`, `iav`, and `amba_virt` are not model adapters.
  HVMs reach Cavalry and the camera through `amba-virt-server`.
- Assign `amba_shm` or `amba_shm1` only to HVMs (window markers). Each
  `shmsize` is `1G`.

## Shared `ioMemberList`

Both models have the same adapters (`zcli model show … --detail`):

| phylabel | logicallabel | ztype | assigngrp | phyaddrs / cbattr | usage |
|---|---|---|---|---|---|
| USB | USB | `IO_TYPE_USB_CONTROLLER` | USB | — | unspecified |
| eth0 | eth0 | `IO_TYPE_ETH` | eth0 | `Ifname=eth0` | **management** |
| UART0 | UART0 | `IO_TYPE_OTHER` | *(empty)* | `cbattr uart=0` | **management** |
| UART1 | UART1 | `IO_TYPE_OTHER` | uart1 | `cbattr uart=1` | unspecified |
| UART2 | UART2 | `IO_TYPE_OTHER` | uart2 | `cbattr uart=2` | unspecified |
| UART3 | UART3 | `IO_TYPE_OTHER` | uart3 | `cbattr uart=3` | unspecified |
| UART4 | UART4 | `IO_TYPE_OTHER` | uart4 | `cbattr uart=4` | unspecified |
| hwrng | hwrng | `IO_TYPE_OTHER` | hwrng | `Ifname=/dev/hwrng` | unspecified |
| rng-mmio | rng-mmio | `IO_TYPE_OTHER` | rng-mmio | `cbattr rng=mmio` | unspecified |
| amba_shm | amba_shm | `IO_TYPE_OTHER` | amba_shm | `shmpath=/dev/amba_virt_shm`, `shmsize=1G` | unspecified |
| amba_shm1 | amba_shm1 | `IO_TYPE_OTHER` | amba_shm1 | `shmpath=/dev/amba_virt_shm1`, `shmsize=1G` | unspecified |

`amba_shm` and `amba_shm1` have empty `phyaddrs`. Assigning one to an HVM is
a window marker: `kvm.go` emits `ivshmem-plain`. Each window is shared by
every virtual driver in that HVM. Pulled from the controller on 2026-10-10
after `cavalry`, `cavalry_profile`, `gpio0`, `iav`, and `amba_virt` were
removed.
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md).

### Planned UART Adapter Bundles

UART1/UART2 will use `IO_TYPE_OTHER` bundles with:

- empty `PciLong`, `Ifname`, `Serial`, and `UsbAddr`;
- an exclusive `assigngrp`;
- UART-specific `cbattr`.

This reuses the delivered ivshmem recognition shape and fails safe if a
`Serial` value appears. `IO_TYPE_OTHER` does not mean “UART,” and the bundle is
not inert: its `cbattr` designates real silicon.

The UART parser and `amba_shm` bulk-window parser must be mutually exclusive on
their `cbattr` keys. `IoOther` and `IoNVME` both have numeric value 255, so type
alone cannot identify the bundle.

The controller already publishes UART0–UART4. Each has empty `phyaddrs` and
`cbattr` `uart` set to that index. UART0 has an empty `assigngrp` and
management usage. There are no `COM1`/`COM2`/`COM3` entries.

### Hardware RNG & VirtIO RNG Virtualization

- **`hwrng` Dom0 Retention**: `/dev/hwrng` is created by `ambarella-rng.c` on Dom0. It is published in the model but must remain **unassigned** to any app instance. Dom0 retains exclusive ownership of physical TRNG registers (`0xe002f000`), constantly seeding the Dom0 kernel CSPRNG (`/dev/urandom`).
- **Stock VirtIO RNG for HVMs**: All ARM64 `virt` HVM guests (Ubuntu, Alpine, Windows) automatically receive a stock `virtio-rng-pci` device backed by Dom0's `/dev/urandom` (with rate limit `max-bytes = "4096"`, `period = "1000"`). Like `vhost-vsock-pci`, this is a standard platform device and requires no custom model adapter assignments.
- **QNX `rng-mmio` Adapter**: QNX guest VMs require MMIO transport (`virtio-rng-device` on `virtio-mmio-bus.0`) rather than PCI. To select MMIO, QNX instances are assigned the `rng-mmio` adapter (`IO_TYPE_OTHER`, empty `phyaddrs`, `cbattr: {"rng": "mmio"}`), which pairs with native QNX `random -l devr-virtio.so:mem=<address>`.
- **Exclusions**: OCI containers and x86 domains do not receive VirtIO RNG devices.

`/dev/ucode` was not added (not confirmed on the host). Assigning a missing
`Ifname` makes EVE skip that device in the OCI spec (`getDeviceInfo` fails).

The Dom0-owned nodes exist on the host once the server is ready (`/dev/iav`
only while the camera pipeline is configured):

```bash
ls -l /dev/cavalry /dev/cavalry_profile /dev/gpiochip0 /dev/iav /dev/amba_virt
```

## Assign adapters to edge apps

The model only **publishes** adapters. The reference design assigns one
Ambarella adapter, `amba_shm`, to each HVM at instance create. Worked deploy:
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md). Do not edit an existing
bundle in place; that is
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md).

Wrappers: [ZedControl-scripts.md](ZedControl-scripts.md). `$ZCLI_TOKEN` must
already be in the environment.

```text
+--------------------+  amba_shm   +--------------------------------+  window marker  +----------------------+
| Model ioMemberList |------------>| HVM instance create --adapter  |---------------->| ivshmem-plain (BAR2) |
+--------------------+             +--------------------------------+                 +----------------------+
```

Confirm an HVM holds a window marker and not a host chardev:

```bash
./scripts/show_instances.sh
./scripts/show_instances.sh ubuntu_24_04.n1-655-pro
```

## DTS mapping

| Cloud adapter | N1-655 DT | Notes |
|---|---|---|
| eth0 | `mac0` (`ethernet0`) | Management; leave with EVE. |
| USB | `usb_cdnsp` | Assignable as group `USB`. |
| COM1 `/dev/ttyS0` | `uart0` (`serial0`, stdout) | Console. Do not assign away from EVE. |
| cavalry, cavalry_profile | `sub_scheduler0` | Dom0 server only; never assigned. |
| gpio0 | `gpio@0` (123 lines) | Dom0 server only (camera power lines); never assigned. |
| iav | `compatible = "ambarella,iav"` | VIN/DSP through this chardev; Dom0 server only. |

## Not in cloud model

| Expected | Why it matters | Suggested ztype if added later |
|---|---|---|
| `mac1` / eth1 | Second RGMII (not enabled in `n1_655.dts`) | `IO_TYPE_ETH` |
| uart1–uart4 | HVM UART MMIO/vGIC adapter; planned, not delivered | `IO_TYPE_OTHER` with empty `phyaddrs` and UART `cbattr` |
| `can0`–`can2` | dtsi CAN | `IO_TYPE_CAN` |
| `/dev/ucode` | IAV companion if present on host | `IO_TYPE_OTHER` (`assigngrp` `iav`) |
| PowerVR GPU | `ambarella,pvr-gpu` | `IO_TYPE_HDMI` + CDI, if used |

## Out of model (not `ioMemberList`)

- **virtio-vsock** is stock on every HVM (`vhost-vsock-pci`). Not an adapter.

**ivshmem is in the model** as `amba_shm` (empty `phyaddrs`, `cbattr.shmsize`).
The QEMU device is still emulated; the adapter only requests the window.
The Dom0 server reaches that DRAM through `/dev/amba_virt`, not by opening
the backing file.

See [drivers/amba_virt/README.md](../drivers/amba_virt/README.md), [AmbaVirtServer.md](AmbaVirtServer.md),
[EVE-Native-ivshmem-Support.md](EVE-Native-ivshmem-Support.md).
