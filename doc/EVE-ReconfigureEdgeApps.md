# Do not reconfigure existing NOHYPER apps

**Do not try to add Cavalry / GPIO / IAV to `ubuntu_24_04-container`.**
gmwtus will not add interfaces to an edge-app that already has instance
records. **Halted still counts.** `--stop` only deactivates; it does
not unlock `edge-app update`. That is controller validation, not an
account policy. Deleting those records would drop their implicit
volumes. Do not do that.

The HVM (`ubuntu_24_04` / `ubuntu_24_04.n1-655-devkit`) is fine. Leave
it alone. vsock is already there (CID 2, port 5555). Do not attach VisORC
to the VM.

ivshmem *is* now driven by an `ioMemberList` entry. `amba_shm` is an
`IO_TYPE_OTHER` bundle with empty `phyaddrs`, so it hands no hardware to the
guest; assigning it is what makes `kvm.go` emit the `ivshmem-plain` device and
size its backing file. Because the same "already has instances" rule applies to
`ubuntu_24_04`, that needed a new edge-app too —
[Native-ivshmem-Support-in-EVE-BaseOS.md](Native-ivshmem-Support-in-EVE-BaseOS.md) §6.1.

**Do this instead:** create a **new** edge-app that already lists those
interfaces, then create a **new** instance and pass `--adapter=` at
create. That is a **new rootfs**. Keep the old containers (eth0 only).
Start them if they are still Halted from the failed `--stop` tests.

Failed in-place wrappers are under
[scripts/failed/](../scripts/failed/README.md). They exit without
talking to the controller.

`$ZCLI_TOKEN` must already be in the environment. Wrappers never export
it. Catalog: [ZedControl-scripts.md](ZedControl-scripts.md). Models:
[EVE-Ambarella-Models.md](EVE-Ambarella-Models.md). Transport PoC:
[poc/README.md](../poc/README.md).

## Names (n1-655-devkit)

| Role | Instance | Edge-app | Node |
|---|---|---|---|
| HVM (keep, do not change) | `ubuntu_24_04.n1-655-devkit` | `ubuntu_24_04` | `n1-655-devkit` |
| Old NOHYPER (keep, eth0 only) | `ubuntu_24_04_container.n1-655-devkit` | `ubuntu_24_04-container` | `n1-655-devkit` |
| New NOHYPER + VisORC | `ubuntu_24_04_container_visorc.n1-655-devkit` | `ubuntu_24_04-container-visorc` | `n1-655-devkit` |

Pro: instance `ubuntu_24_04_container_visorc.n1-655-pro`, node
`n1-655-pro`, network `eth0:defaultLocal-n1-655-pro`. One VisORC
`assigngrp` per node.

## What failed on gmwtus

| Attempt | Result |
|---|---|
| `edge-app update` extra ifs on `ubuntu_24_04-container` | First `BadReqBody` (`show --detail` UI/`null` fields). After sanitize: `new Interfaces cannot be added` |
| `--stop` Devkit and Pro containers | Halted / Inactive; `appInstCount` still **2**; update still refused |
| Instance `--adapter=` without template `intfname`s | zcli: `Interface X not found in edge app template` |

`appInstCount` is the instance **record** count, not run-state.

| Action | Disks |
|---|---|
| `--stop` / `--start` on the **old** containers | **Keeps** volumes. They stay eth0-only |
| **New** edge-app + **new** instance | **New** rootfs |
| Delete old instances to empty `appInstCount` | **Wipes** `…_0_m_0`. Do not |

## Create a new VisORC container

### 0. Start the old containers if they are Halted

```bash
./scripts/show_instances.sh --edge-app=ubuntu_24_04-container
./scripts/restart_instance.sh ubuntu_24_04_container.n1-655-devkit --start
./scripts/restart_instance.sh ubuntu_24_04_container.n1-655-pro --start
```

### 1. Inspect (HVM untouched)

```bash
./scripts/show_instances.sh --edge-node=n1-655-devkit
./scripts/show_app.sh ubuntu_24_04-container
./scripts/show_app.sh ubuntu_24_04
```

### 2. Local JSON for a new bundle

```bash
./scripts/pull_app.sh ubuntu_24_04-container
./scripts/clone_app.sh ubuntu_24_04-container ubuntu_24_04-container-visorc
./scripts/add_app_direct.sh ubuntu_24_04-container-visorc \
  --if=cavalry --if=gpio0 --if=iav
```

`pull_app.sh` strips `show --detail` UI fields. `add_app_direct.sh`
edits **local** JSON only (`directattach: true`).

`add_app_direct.sh` refuses `HV_HVM` for interfaces backed by real hardware.
Window markers are allowed: an `IO_TYPE_OTHER` bundle with no
`Ifname`/`PciLong`/`Serial`/`UsbAddr` carries no physical resource, so `amba_shm`
goes onto an HVM without `--force` while `cavalry` still does not.

### 3. Create the edge-app (not update)

```bash
./scripts/create_app.sh ubuntu_24_04-container-visorc --version=1.0 --dry-run
./scripts/create_app.sh ubuntu_24_04-container-visorc --version=1.0
./scripts/show_app.sh ubuntu_24_04-container-visorc
```

`--version` is `userDefinedVersion`. ACE `acVersion` stays `1.2.0`.
Expect `eth0`, `cavalry`, `gpio0`, `iav`.

### 4. Create the instance with adapters at create

`assigngrp` `cavalry` covers `/dev/cavalry` and `/dev/cavalry_profile`.

**`--adapter=INTF:NAME` takes the adapter's `logicallabel`, not its
`assigngrp`.** The two differ for `gpio0`, whose bundle is in `assigngrp`
`gpio`, and passing the group is rejected:

```
Error IncompleteData: Invalid adapter for device: n1-655-devkit,
Io name is: gpio: model does not have adapter
```

So it is `gpio0:gpio0`, even though `set_adapters.sh` calls the field `GRP`.

```bash
./scripts/create_instance.sh ubuntu_24_04_container_visorc.n1-655-devkit \
  --edge-app=ubuntu_24_04-container-visorc \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio0 --adapter=iav:iav \
  --allow-visorc --dry-run
./scripts/create_instance.sh ubuntu_24_04_container_visorc.n1-655-devkit \
  --edge-app=ubuntu_24_04-container-visorc \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio0 --adapter=iav:iav \
  --allow-visorc
```

An `assigngrp` can only be held by one instance at a time, so a new container
that wants `cavalry`/`gpio0`/`iav`/`USB` cannot coexist with the old one still
holding them. Either give the new container only the adapters the old one does
not have, or delete the old instance first and accept the volume loss.

Two more things that bite when cloning:

- The clone inherits the original's **portmap**, and two instances on one node
  cannot both claim the same host port. `handleAppNetworkCreate: … have
  overlapping portmaps` leaves the instance in `Error`. Edit `lport` in the
  local JSON before `create_app.sh`.
- ACLs are **snapshotted into the instance at create**. Updating the edge-app
  afterwards does not propagate; the instance keeps the port it was born with.
  Fix the manifest first, or delete and recreate the instance.

`--allow-visorc` is required. Wait until the instance is Online, then
on that **new** container:

```bash
ls -l /dev/cavalry /dev/cavalry_profile /dev/gpiochip0 /dev/iav
```

`amba_virt.ko` stays [poc/README.md](../poc/README.md).
