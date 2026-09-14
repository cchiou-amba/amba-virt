# EVE Ambarella hardware models

ZEDEDA Cloud inventory for Ambarella N1-655. **Cloud `ioMemberList` is
authoritative.** This is not an app-instance or QEMU recipe.

Related: [Architecture.md](Architecture.md) (PhyIo / NOHYPER),
[PoCVirtualDrivers.md](PoCVirtualDrivers.md) (vsock + ivshmem),
[ZedControl-scripts.md](ZedControl-scripts.md) (zcli wrappers),
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md) (deploy HVM + NOHYPER),
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
> - **`iav` is a pending adapter.** It advertises `/dev/iav`, but no
>   `iav.ko` exists in `eve-kernel` yet. It is preserved in the model
>   for backwards container compatibility; do not assign it to new apps
>   until the vendor BSP driver lands ([CV3_AD655_BSP_Request.md](../automation/doc/CV3_AD655_BSP_Request.md)).
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

Those are the types in use. The full 18-value enum, and which
N1-655 device should map to which, is in
[automation/doc/EnableAllKernelDeviceDrivers.md](../automation/doc/EnableAllKernelDeviceDrivers.md)
§8.2–8.3. Prefer a specific type over `IO_TYPE_OTHER` wherever EVE
has native semantics for it.

`PhyIoOther` uses `phyaddrs.Ifname` as a host chardev. EVE injects that path
into the NOHYPER OCI spec when the app is assigned the `assigngrp`.

## Assignment rules

- Empty `assigngrp`: EVE keeps the adapter; apps cannot take it.
- Same non-empty `assigngrp`: one unit, assignable to one app instance.
- `usage` `ADAPTER_USAGE_MANAGEMENT`: EVE management port. Do not assign it
  to an app even if `assigngrp` is set.
- Assign `cavalry` and `gpio` only to the NOHYPER app. HVMs must
  not get VisORC / Cavalry. (`iav` would follow the same rule, but
  the node does not exist — see the note above.)
- Assign `amba_virt` only to NOHYPER (`Ifname=/dev/amba_virt`).
- Assign `amba_shm` only to the HVM (window marker). One window per pair;
  `shmsize` is `1G`.

## Shared `ioMemberList`

Both models have the same adapters (`zcli model show … --detail`):

| phylabel | logicallabel | ztype | assigngrp | phyaddrs | usage |
|---|---|---|---|---|---|
| USB | USB | `IO_TYPE_USB_CONTROLLER` | USB | — | unspecified |
| COM1 | COM1 | `IO_TYPE_COM` | COM1 | `Serial=/dev/ttyS0` | unspecified |
| eth0 | eth0 | `IO_TYPE_ETH` | eth0 | `Ifname=eth0` | **management** |
| cavalry | cavalry | `IO_TYPE_OTHER` | cavalry | `Ifname=/dev/cavalry` | unspecified |
| cavalry_profile | cavalry_profile | `IO_TYPE_OTHER` | cavalry | `Ifname=/dev/cavalry_profile` | unspecified |
| gpio0 | gpio0 | `IO_TYPE_OTHER` | gpio | `Ifname=/dev/gpiochip0` | unspecified |
| hwrng | hwrng | `IO_TYPE_OTHER` | hwrng | `Ifname=/dev/hwrng` | unspecified |
| iav | iav | `IO_TYPE_OTHER` | iav | `Ifname=/dev/iav` | unspecified — pending vendor `iav.ko` |
| amba_virt | amba_virt | `IO_TYPE_OTHER` | amba_virt | `Ifname=/dev/amba_virt` | unspecified |
| amba_shm | amba_shm | `IO_TYPE_OTHER` | amba_shm | *(empty)* | unspecified |

`amba_shm` has empty `phyaddrs` and `cbattr` `shmpath=/dev/shm/amba-virt`,
`shmsize=1G`. Assigning it to an HVM is a window marker: `kvm.go` emits
`ivshmem-plain`. One window is shared by every virtual driver on that pair
(Cavalry, DMA, SD/eMMC, …). Do not add a second `amba_shm`. 16M is PoC-only.
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md).

`amba_virt` is the host chardev injected into NOHYPER. The container's
`/dev/shm` is a private tmpfs and cannot see the backing file.

`/dev/ucode` was not added (not confirmed on the host). Assigning a missing
`Ifname` makes EVE skip that device in the OCI spec (`getDeviceInfo` fails)
— which is exactly what happens with `iav` today.

Confirm on the edge node before assigning to NOHYPER. `/dev/iav` is
included here only to show that it is absent:

```bash
ls -l /dev/cavalry /dev/cavalry_profile /dev/gpiochip0
ls -l /dev/iav          # expected: No such file or directory
```

## Assign adapters to edge apps

The model only **publishes** adapters. No app gets `/dev/cavalry` until the
**edge-app instance** lists that `assigngrp`. Attach `cavalry` and `gpio`
to the NOHYPER container only. Keep them off the HVM. Do not attach
`iav` — the node does not exist.

Worked deploy (from scratch, adapters at create):
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md). Do not edit an
existing bundle in place; that is
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md).

Wrappers: [ZedControl-scripts.md](ZedControl-scripts.md). `$ZCLI_TOKEN` must
already be in the environment.

```text
+--------------------+   available   +-----------------------------------+   assigngrp cavalry/gpio   +--------------------------+
| Model ioMemberList |-------------->| NOHYPER instance create --adapter |--------------------------->| OCI devices in container |
+--------------------+               +-----------------------------------+                            +--------------------------+
```

### 1. Find both instances

```bash
./scripts/show_instances.sh
./scripts/show_instances.sh ubuntu_24_04_container.n1-655-pro
./scripts/show_instances.sh ubuntu_24_04.n1-655-pro
```

| Role | Typical name | Adapters |
|---|---|---|
| Old NOHYPER | `ubuntu_24_04_container.n1-655-pro` | eth0 only |
| New NOHYPER | `ubuntu_24_04_container_visorc.n1-655-pro` | cavalry, gpio, iav |
| HVM | `ubuntu_24_04.n1-655-pro` | **none** of those |

Cloud instance names may differ. `zcli update --adapter=` **replaces** all
interfaces (including networks). `set_adapters.sh` keeps current networks.

### 2. Edge-app interfaces (if missing)

The left side of `--adapter=intfname:assigngrp` must exist on the
**edge-app** manifest. Group `cavalry` covers both `/dev/cavalry` and
`/dev/cavalry_profile`.

```bash
./scripts/show_app.sh ubuntu_24_04-container
```

gmwtus will not add those names to a bundle that already has instances
(Halted included). Create `ubuntu_24_04-container-visorc` instead:
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md). Do not add
Cavalry / GPIO / IAV interfaces to the HVM edge-app.

### 3. Attach on the NOHYPER instance

Prefer `--adapter=` at **instance create**. For an instance whose
template already lists the ifs:

```bash
./scripts/set_adapters.sh ubuntu_24_04_container_visorc.n1-655-pro \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio --adapter=iav:iav \
  --allow-visorc --dry-run
./scripts/set_adapters.sh ubuntu_24_04_container_visorc.n1-655-pro \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio --adapter=iav:iav \
  --allow-visorc --restart
```

Use the real `intfname` from `show_app.sh` on the left. `--restart`
recreates OCI devices. GUI: instance → I/O Adapters → add those groups →
save → restart.

### 4. Confirm the HVM is clean

```bash
./scripts/show_instances.sh ubuntu_24_04.n1-655-pro
```

If cavalry / gpio / iav appear:

```bash
./scripts/set_adapters.sh ubuntu_24_04.n1-655-pro --clear-adapters --restart
```

HVMs must not own VisORC.

### 5. Verify on the node

Inside the **container** (not the VM):

```bash
ls -l /dev/cavalry /dev/cavalry_profile /dev/gpiochip0 /dev/iav
```

On the **HVM** those host Cavalry nodes must not be passed through.

## DTS mapping

| Cloud adapter | N1-655 DT | Notes |
|---|---|---|
| eth0 | `mac0` (`ethernet0`) | Management; leave with EVE. |
| USB | `usb_cdnsp` | Assignable as group `USB`. |
| COM1 `/dev/ttyS0` | `uart0` (`serial0`, stdout) | Console. Do not assign away from EVE. |
| cavalry, cavalry_profile | `sub_scheduler0` | NOHYPER only. |
| gpio0 | `gpio@0` (123 lines) | NOHYPER only. |
| iav | `compatible = "ambarella,iav"` | VIN/DSP through this chardev. |

## Not in cloud model

| Expected | Why it matters | Suggested ztype if added later |
|---|---|---|
| `mac1` / eth1 | Second RGMII (not enabled in `n1_655.dts`) | `IO_TYPE_ETH` |
| uart1–uart4 | Extra COM in dtsi | `IO_TYPE_COM` |
| `can0`–`can2` | dtsi CAN | `IO_TYPE_CAN` |
| `/dev/ucode` | IAV companion if present on host | `IO_TYPE_OTHER` (`assigngrp` `iav`) |
| PowerVR GPU | `ambarella,pvr-gpu` | `IO_TYPE_HDMI` + CDI, if used |

## Out of model (not `ioMemberList`)

- **virtio-vsock** is stock on every HVM (`vhost-vsock-pci`). Not an adapter.

**ivshmem is in the model** as `amba_shm` (empty `phyaddrs`, `cbattr.shmsize`).
The QEMU device is still emulated; the adapter only requests the window.
NOHYPER reaches that DRAM through `amba_virt`, not a bind-mount of the
backing file.

See [drivers/amba_virt/README.md](../drivers/amba_virt/README.md), [PoCVirtualDrivers.md](PoCVirtualDrivers.md),
[Native-ivshmem-Support-in-EVE-BaseOS.md](Native-ivshmem-Support-in-EVE-BaseOS.md).
