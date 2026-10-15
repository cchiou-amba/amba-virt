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
- Assign an admission ticket (`amba_shm0` to `amba_shm7`) only to HVMs. A ticket is
  `cbattr shmassign=server` with no `shmpath`: it names no device. ZEDEDA lets one instance hold a given
  `assigngrp`, so every guest on a board needs a ticket of its own; the tickets are an exclusivity label
  for that rule and not eight memory pools. `amba-virt-server` leases the guest its Cavalry slice, its
  private IAV slice and, by the camera policy below, the shared camera windows. Only the ticket named
  `amba_shm0` is pinned to Cavalry slice 0; every other ticket takes the lowest free slice from 1. The
  digit in `amba_shm1` to `amba_shm7` does not select Cavalry slice N (when the six instances are created in the
  pinned `0..5` order on a clean board, `amba_shm<N>` maps to `/dev/amba_virt_shm<N>`; see
  [EVE-EdgeApp-Instances.md](EVE-EdgeApp-Instances.md)). A bundle that only sets `shmpath`
  keeps that path and is not a dynamic grant.

## Shared `ioMemberList`

Both models have the same adapters (`zcli model show … --detail`):

| phylabel | logicallabel | ztype | assigngrp | phyaddrs / cbattr | usage |
|---|---|---|---|---|---|
| USB | USB | `IO_TYPE_USB_CONTROLLER` | USB | — | unspecified |
| eth0 | eth0 | `IO_TYPE_ETH` | eth0 | `Ifname=eth0` | **management** |
| UART0 | UART0 | `IO_TYPE_OTHER` | *(empty)* | `cbattr uart=0` | **management** |
| UART1 | UART1 | `IO_TYPE_OTHER` | uart1 | `cbattr uart=1` | unspecified |
| UART2 | UART2 | `IO_TYPE_OTHER` | uart2 | `cbattr uart=2` (`0xffe0018000`, GIC SPI 115 / guest vIRQ 50, `vfio-platform`) | unspecified |
| UART3 | UART3 | `IO_TYPE_OTHER` | uart3 | `cbattr uart=3` (`0xffe0019000`, GIC SPI 116 / guest vIRQ 50, `vfio-platform`) | unspecified |
| UART4 | UART4 | `IO_TYPE_OTHER` | uart4 | `cbattr uart=4` | unspecified |
| hwrng | hwrng | `IO_TYPE_OTHER` | hwrng | `Ifname=/dev/hwrng` | unspecified |
| rng-mmio | rng-mmio | `IO_TYPE_OTHER` | rng-mmio | `cbattr rng=mmio` | unspecified |

### Admission tickets

The tickets below are the model's memory adapters. They have empty `phyaddrs`; assigning one to an HVM is
what makes `kvm.go` ask `amba-virt-server` for the guest's slices (`kvm.go` emits `ivshmem-plain` devices
backed by the leases). The digit is a label only.

<!-- tickets:begin -->
| ticket | ztype | assigngrp | cbattr | camera intent |
|---|---|---|---|---|
| `amba_shm0` | `IO_TYPE_OTHER` | `amba_shm0` | `shmassign=server`, `shmsize=1G`, `camera=on` | on; pinned to Cavalry slice 0 |
| `amba_shm1` | `IO_TYPE_OTHER` | `amba_shm1` | `shmassign=server`, `shmsize=1G`, `camera=on` | on |
| `amba_shm2` | `IO_TYPE_OTHER` | `amba_shm2` | `shmassign=server`, `shmsize=1G`, `camera=on` | on |
| `amba_shm3` | `IO_TYPE_OTHER` | `amba_shm3` | `shmassign=server`, `shmsize=1G`, `camera=on` | on |
| `amba_shm4` | `IO_TYPE_OTHER` | `amba_shm4` | `shmassign=server`, `shmsize=1G` | off (no `camera` key) |
| `amba_shm5` | `IO_TYPE_OTHER` | `amba_shm5` | `shmassign=server`, `shmsize=1G` | off |
| `amba_shm6` | `IO_TYPE_OTHER` | `amba_shm6` | `shmassign=server`, `shmsize=1G` | off |
| `amba_shm7` | `IO_TYPE_OTHER` | `amba_shm7` | `shmassign=server`, `shmsize=1G` | off |
<!-- tickets:end -->

Each ticket is an exclusivity label for ZEDEDA's one-instance-per-group rule and does not select Cavalry slice N.
`amba_shm0` is the one exception that has a meaning of its own: it is pinned to slice 0, and the guest that
holds slice 0 is the primary IAV client under the image's `/etc/amba-virt/iav.conf` (`primary-slice 0`).
Every other ticket (`amba_shm1`..`amba_shm7`) gets the lowest free slice from 1 and is a consumer
(see [AmbaVirtServer.md](AmbaVirtServer.md) section 13.4). `shmsize` is the slice size the server grants.

### Camera windows and the camera policy

The shared camera windows of the virtual `/dev/iav` (the DSP partition, the bitstream buffer, the decode
partition and the overlay partition, [IavVirtualization.md](IavVirtualization.md)) are not model adapters.
`amba-virt-server` attaches them to a guest when it leases the guest's ticket, from the one set of host
nodes `/dev/amba_virt_iav_{dsp,bsb,dec,ovl}`. Every attached guest maps the same nodes. The decode and
overlay windows go only to the guest on Cavalry slice 0.

Whether a guest gets the windows is decided by the server for each reserve, from three inputs:

| Input | Where it comes from | Values |
|---|---|---|
| Fleet intent | the ticket's `camera` cbattr in the model | `on`, `off`; no key means off |
| Override | a per-ticket exception on one node, persisted by `amba-virt-server` | `on`, `off`, `inherit` (the default: no override) |
| Windows ready | the server configured the DSP and bitstream nodes when it started | yes or no |

`effective = windows ready AND (the override if it is not inherit, else the intent is on)`. A guest whose
effective answer is off gets its Cavalry slice and private slice and no camera windows. The decision is taken
when the guest starts: a change applies at the guest's next start. There is no way for a guest to ask for
the windows. The override is for a single node's exception (forcing the camera off during an incident, or on
for a trial); the normal way to give a guest the windows is the ticket's `camera` key in the model. EVE keeps the `cbattr`
of an adapter it already knows until the node's adapter state is rebuilt, so a change to the `camera` key of an
existing ticket in the model reaches the guests only after the node reboots (a ticket the node has not seen yet
takes its `camera` key at once). `get-camera` shows the intent the server last received for each ticket.

Commands (Dom0, through `/hostfs/usr/bin/amba-virt-ctl`):

| Command | What it does |
|---|---|
| `set-camera <ticket> <on\|off\|inherit>` | sets or clears the override; `ERR PERSIST` if it could not be stored (then nothing changed) |
| `get-camera [<ticket>]` | `OK COUNT <n>` and one line per ticket: `override`, the last intent seen, and `effective` |
| `camera-status` | `OK windows=<ready\|none> dsp=.. bsb=.. dec=.. ovl=.. ready_ms=..`: the configured windows and when (the daemon log's `CLOCK_MONOTONIC` milliseconds) |

The overrides are stored in `/persist/etc/amba-virt/camera-tickets`: UTF-8, one line per ticket,
`<ticket> <on|off>`, at most 64 lines and 4096 bytes, mode 0600, written atomically (a temporary file,
`fsync`, `rename`). A missing file means every ticket is `inherit`. A file that is not valid (a bad line, a
duplicate, over a limit) is logged and treated as empty. An entry for a ticket no model defines is kept until
`set-camera <ticket> inherit` removes it.

The windows are configured once, when the server starts and before it reports ready, and cannot change under
a running guest. Reconfiguring them (a camera resource change) needs every attached guest stopped; the window
driver refuses while any guest maps a node. A guest with the camera windows can write the shared real range and
the padding behind it (the N1-655 has no SMMU); that is accepted for every guest the policy attaches them to.

### Retired adapters

<!-- retired-adapters:begin -->
These adapters no longer exist in the models; remove any instance that still holds one before pushing a
model, because the controller cannot keep an assignment to an adapter that is gone:

- `amba_shm` (unnumbered slice-0 ticket): replaced by `amba_shm0`.
- `amba_iav_dsp`, `amba_iav_bsb`, `amba_iav_dec`, `amba_iav_ovl` and the same four with a `1` suffix
  (path-bearing window adapters, in two sets): replaced by the camera policy above.
- `amba_iav_slice` (the private IAV slice marker): the private slice is leased with every ticket.
- The host nodes `/dev/amba_virt_iav_{dsp,bsb,dec,ovl}1`: the window module has one set of nodes.
<!-- retired-adapters:end -->

Deploy recipe: [EVE-EdgeApp-Instances.md](EVE-EdgeApp-Instances.md) and [EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md).

### UART Adapter Bundles (`UART0`–`UART4`)

`UART0`–`UART4` are published in the models as `IO_TYPE_OTHER` bundles with:

- empty `PciLong`, `Ifname`, `Serial`, and `UsbAddr`;
- an exclusive `assigngrp` (`uart1`..`uart4`; `UART0` has an empty `assigngrp` and management usage);
- `cbattr` `{"uart": "<N>"}`.

This reuses the ivshmem recognition shape and fails safe if a `Serial` value appears. `IO_TYPE_OTHER` does not
mean "UART," and the bundle is not inert: its `cbattr` designates real silicon (`0xffe0018000` for `UART2`,
`0xffe0019000` for `UART3`). When an instance holds `UART2` (`ubuntu_24_04.<node>`) or `UART3`
(`alpine_3_20.<node>`), Pillar (`kvm.go` / `hypervisor.go`) automatically binds the platform device to
`vfio-platform`, allocates a 16 MiB DMA32 lease from `/dev/amba_dma_ctl` with `endpoint_id` set to the UART
number (`2` or `3`), and emits the `vfio-platform` device plus the ACPI `AMBA0001` DSDT node (guest vIRQ 50 /
GSI 144). On domain stop or failed start, Pillar restores `ambarella-uart`.

The UART parser and `amba_shm0..7` bulk-window parser are mutually exclusive on their `cbattr` keys.
There are no `COM1`/`COM2`/`COM3` entries in the hardware models.

### Hardware RNG & VirtIO RNG Virtualization

- **`hwrng` Dom0 Retention**: `/dev/hwrng` is created by `ambarella-rng.c` on Dom0. It is published in the model but must remain **unassigned** to any app instance. Dom0 retains exclusive ownership of physical TRNG registers (`0xe002f000`), constantly seeding the Dom0 kernel CSPRNG (`/dev/urandom`).
- **Stock VirtIO RNG for HVMs**: All ARM64 `virt` HVM guests (Ubuntu, Alpine, Debian, Fedora, Windows) automatically receive a stock `virtio-rng-pci` device backed by Dom0's `/dev/urandom` (with rate limit `max-bytes = "4096"`, `period = "1000"`). Like `vhost-vsock-pci`, this is a standard platform device and requires no custom model adapter assignments.
- **QNX `rng-mmio` Adapter**: QNX guest VMs require MMIO transport (`virtio-rng-device` on `virtio-mmio-bus.0`) rather than PCI. To select MMIO, QNX instances (`qnx_8_0.<node>`, `amba_shm4`) are assigned the `rng-mmio` adapter (`IO_TYPE_OTHER`, empty `phyaddrs`, `cbattr: {"rng": "mmio"}`), which pairs with native QNX `random -l devr-virtio.so:mem=<address>`.
- **Exclusions**: OCI containers and x86 domains do not receive VirtIO RNG devices.

`/dev/ucode` was not added (not confirmed on the host). Assigning a missing
`Ifname` makes EVE skip that device in the OCI spec (`getDeviceInfo` fails).

The Dom0-owned nodes exist on the host once the server is ready (`/dev/iav`
only while the camera pipeline is configured):

```bash
ls -l /dev/cavalry /dev/cavalry_profile /dev/gpiochip0 /dev/iav /dev/amba_virt /dev/amba_dma_ctl
```

## Assign adapters to edge apps

The model only **publishes** adapters. Each HVM manifest (`apps/*.json`) names its memory slot `amba_shm`, bound at instance create to one of the model's tickets (`--adapter=amba_shm:amba_shm<N>`). Full per-app recipe:
[EVE-EdgeApp-Instances.md](EVE-EdgeApp-Instances.md) and [EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md). Do not edit an existing
bundle in place; that is
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md).

Wrappers: [ZedControl-scripts.md](ZedControl-scripts.md). `$ZCLI_TOKEN` must
already be in the environment.

```text
+--------------------+  amba_shm0..7  +--------------------------------+  window marker  +----------------------+
| Model ioMemberList |--------------->| HVM instance create --adapter  |---------------->| ivshmem-plain (BAR2) |
+--------------------+                +--------------------------------+                 +----------------------+
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
| UART0 `/dev/ttyS0` | `uart0` (`serial0`, stdout) | Dom0 console (`console0`). Do not assign away from EVE. |
| UART2 | `ffe0018000.uart` (`uart2`) | Assigned to `ubuntu_24_04.<node>` (`console1`, `vfio-platform`). |
| UART3 | `ffe0019000.uart` (`uart3`) | Assigned to `alpine_3_20.<node>` (`console2`, `vfio-platform`). |
| cavalry, cavalry_profile | `sub_scheduler0` | Dom0 server only; never assigned. |
| gpio0 | `gpio@0` (123 lines) | Dom0 server only (camera power lines); never assigned. |
| iav | `compatible = "ambarella,iav"` | VIN/DSP through this chardev; Dom0 server only. |

## Not in cloud model

| Expected | Why it matters | Suggested ztype if added later |
|---|---|---|
| `mac1` / eth1 | Second RGMII (not enabled in `n1_655.dts`) | `IO_TYPE_ETH` |
| `can0`–`can2` | dtsi CAN | `IO_TYPE_CAN` |
| `/dev/ucode` | IAV companion if present on host | `IO_TYPE_OTHER` (`assigngrp` `iav`) |
| PowerVR GPU | `ambarella,pvr-gpu` | `IO_TYPE_HDMI` + CDI, if used |

## Out of model (not `ioMemberList`)

- **virtio-vsock** is stock on every HVM (`vhost-vsock-pci`). Not an adapter.

**ivshmem is in the model** as `amba_shm0`..`amba_shm7` (empty `phyaddrs`, `cbattr.shmsize`).
The QEMU device is still emulated; the adapter only requests the window.
The Dom0 server reaches that DRAM through `/dev/amba_virt`, not by opening
the backing file.

See [drivers/amba_virt/README.md](../drivers/amba_virt/README.md), [AmbaVirtServer.md](AmbaVirtServer.md),
[EVE-Native-ivshmem-Support.md](EVE-Native-ivshmem-Support.md).

