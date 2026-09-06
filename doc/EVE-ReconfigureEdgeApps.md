# Do not reconfigure existing NOHYPER apps

**Do not try to add Cavalry / GPIO / IAV to `ubuntu_24_04-container`.**
gmwtus will not add interfaces to an edge-app that already has instance
records. **Halted still counts.** `--stop` only deactivates; it does
not unlock `edge-app update`. That is controller validation, not an
account policy. Deleting those records would drop their implicit
volumes. Do not do that.

The HVM (`ubuntu_24_04` / `ubuntu_24_04.n1-655-devkit`) is fine. Leave
it alone. vsock is already there (CID 2, port 5555). ivshmem is a QEMU
extra, not an `ioMemberList` adapter. Do not attach VisORC to the VM.

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
edits **local** JSON only (`directattach: true`). It refuses `HV_HVM`
unless `--force`.

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

```bash
./scripts/create_instance.sh ubuntu_24_04_container_visorc.n1-655-devkit \
  --edge-app=ubuntu_24_04-container-visorc \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio --adapter=iav:iav \
  --allow-visorc --dry-run
./scripts/create_instance.sh ubuntu_24_04_container_visorc.n1-655-devkit \
  --edge-app=ubuntu_24_04-container-visorc \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio --adapter=iav:iav \
  --allow-visorc
```

`--allow-visorc` is required. Wait until the instance is Online, then
on that **new** container:

```bash
ls -l /dev/cavalry /dev/cavalry_profile /dev/gpiochip0 /dev/iav
```

`amba_virt.ko` stays [poc/README.md](../poc/README.md).
