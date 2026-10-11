# Do not reconfigure existing edge-apps in place

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

Current deploy recipe (new apps, adapters at create):
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md).

**Do not try to add interfaces to an edge-app that already has instance
records.** gmwtus refuses it. **Halted still counts.** `--stop` only
deactivates; it does not unlock `edge-app update`. That is controller
validation, not an account policy. Deleting those records would drop their
implicit volumes. Do not do that to get around the rule.

vsock is already on every HVM (CID 2, port 5555). Do not attach VisORC to any
app: `cavalry`, `gpio0`, `iav`, and `amba_virt` belong to `amba-virt-server` in
EVE Dom0.

ivshmem *is* driven by an `ioMemberList` entry. `amba_shm` is an
`IO_TYPE_OTHER` bundle with empty `phyaddrs`, so it hands no hardware to the
guest; assigning it is what makes `kvm.go` emit the `ivshmem-plain` device and
size its backing file. Because the "already has instances" rule applied to
`ubuntu_24_04`, adding it needed a new edge-app —
[EVE-Native-ivshmem-Support.md](EVE-Native-ivshmem-Support.md) §6.1.

**Do this instead:** create a **new** edge-app that already lists the
interfaces, then create a **new** instance and pass `--adapter=` at create.
That is a **new rootfs**.

`$ZCLI_TOKEN` must already be in the environment. Wrappers never export
it. Catalog: [ZedControl-scripts.md](ZedControl-scripts.md). Models:
[EVE-Ambarella-Models.md](EVE-Ambarella-Models.md). Transport driver:
[drivers/amba_virt/README.md](../drivers/amba_virt/README.md).

## What failed on gmwtus

| Attempt | Result |
|---|---|
| `edge-app update` with extra interfaces on an app that has instances | First `BadReqBody` (`show --detail` UI/`null` fields). After sanitize: `new Interfaces cannot be added` |
| `--stop` the existing instances | Halted / Inactive; `appInstCount` unchanged; update still refused |
| Instance `--adapter=` without template `intfname`s | zcli: `Interface X not found in edge app template` |

`appInstCount` is the instance **record** count, not run-state.

| Action | Disks |
|---|---|
| `--stop` / `--start` on the existing instance | **Keeps** volumes. Interfaces unchanged |
| **New** edge-app + **new** instance | **New** rootfs |
| Delete old instances to empty `appInstCount` | **Wipes** `…_0_m_0` |

## Create a new edge-app with the extra interface

### 1. Inspect

```bash
./scripts/show_instances.sh --edge-node=n1-655-devkit
./scripts/show_app.sh ubuntu_24_04
```

### 2. Local JSON for a new bundle

```bash
./scripts/pull_app.sh ubuntu_24_04
./scripts/clone_app.sh ubuntu_24_04 ubuntu_24_04-v2
./scripts/add_app_direct.sh ubuntu_24_04-v2 --if=amba_shm
```

`pull_app.sh` strips `show --detail` UI fields. `add_app_direct.sh`
edits **local** JSON only (`directattach: true`).

`add_app_direct.sh` refuses `HV_HVM` for interfaces backed by real hardware.
Window markers are allowed: an `IO_TYPE_OTHER` bundle with no
`Ifname`/`PciLong`/`Serial`/`UsbAddr` carries no physical resource, so `amba_shm`
goes onto an HVM without `--force` while `cavalry` does not.

The planned UART adapter also has empty EVE resource fields, but it is not an
inert window marker: UART-specific `cbattr` designates real silicon. Provision
it only after the UART-aware safety check and Pillar parser are implemented.
The UART and `amba_shm` parsers must be mutually exclusive on `cbattr` content.

Like every other interface, a future UART adapter must already be listed on a
new edge-app and assigned when its new instance is created. Do not try to add
it to an existing HVM in place, and do not substitute current `COM2`/`COM3`
`Serial=/dev/ttyS*` entries; those emit QEMU `pci-serial`.

### 3. Create the edge-app (not update)

```bash
./scripts/create_app.sh ubuntu_24_04-v2 --version=1.0 --dry-run
./scripts/create_app.sh ubuntu_24_04-v2 --version=1.0
./scripts/show_app.sh ubuntu_24_04-v2
```

`--version` is `userDefinedVersion`. ACE `acVersion` stays `1.2.0`.

### 4. Create the instance with adapters at create

**`--adapter=INTF:NAME` takes the adapter's `logicallabel`, not its
`assigngrp`.** The two differ for `gpio0`, whose bundle is in `assigngrp`
`gpio`, and passing the group is rejected:

```
Error IncompleteData: Invalid adapter for device: n1-655-devkit,
Io name is: gpio: model does not have adapter
```

```bash
./scripts/create_instance.sh ubuntu_24_04_v2.n1-655-devkit \
  --edge-app=ubuntu_24_04-v2 \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=amba_shm:amba_shm --dry-run
```

An `assigngrp` can only be held by one instance at a time, so a new instance
that wants a group the old one holds cannot coexist with it. Either give the
new instance only groups the old one does not hold, or delete the old instance
first and accept the volume loss.

### 4.1 Worked example: the IAV guests

The virtual `/dev/iav` gives a guest two more adapters, a DSP window and a bitstream
window ([IavVirtualization.md](IavVirtualization.md)). The existing apps cannot take
them, and the two data slices are held by the old instances, so each node's Ubuntu and
Alpine guests were replaced:

1. Add the four `amba_iav_*` adapters to the model (`models/*.json`, shape of
   `amba_shm`), **stop every instance on the node**, then `./scripts/push_models.sh <model>`.
   Never push a model while an app is active.
2. `./scripts/clone_app.sh ubuntu_24_04 ubuntu_24_04_iav`, then
   `./scripts/add_app_direct.sh ubuntu_24_04_iav --if=amba_iav_dsp --if=amba_iav_bsb`
   (the same for Alpine), then `./scripts/create_app.sh <app> --version=1.0`. The new app
   uses the same image as the old one.
3. Delete the old instances (`zcli edge-app-instance delete <name> -f`; their volumes
   are lost) and create the new ones:
   Ubuntu `--adapter=amba_shm:amba_shm --adapter=amba_iav_dsp:amba_iav_dsp --adapter=amba_iav_bsb:amba_iav_bsb`;
   Alpine `--adapter=amba_shm:amba_shm1 --adapter=amba_iav_dsp:amba_iav_dsp1 --adapter=amba_iav_bsb:amba_iav_bsb1`;
   both with `--no-custom-configuration`.
4. A fresh guest has the base image only. Provision it (modules, demo, model, service
   enabled at boot) with `./guest-os/deploy_guest.sh <alias> --provision --reload`. After
   that a cold boot of the node starts the demo with no manual step.

A guest that takes a dynamic address because the old instance's DHCP lease is still
held (`/run/zedrouter/dnsmasq.leases/bn1`) has no working port forward until the node
reboots, which clears the leases. Reach it through Dom0 meanwhile:
`SSH_OPTS="-J <node>" ./guest-os/deploy_guest.sh <user>@<guest-ip> --distro=<ubuntu|alpine> --provision`.

Two more things that bite when cloning:

- The clone inherits the original's **portmap**, and two instances on one node
  cannot both claim the same host port. `handleAppNetworkCreate: … have
  overlapping portmaps` leaves the instance in `Error`. Edit `lport` in the
  local JSON before `create_app.sh`.
- ACLs are **snapshotted into the instance at create**. Updating the edge-app
  afterwards does not propagate; the instance keeps the port it was born with.
  Fix the manifest first, or delete and recreate the instance.
