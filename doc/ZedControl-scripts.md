# ZedControl scripts

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

Wrappers under [`scripts/`](../scripts/) talk to
`zedcontrol.gmwtus.zededa.net` through [`scripts/zcli`](../scripts/zcli).
They read `$ZCLI_TOKEN` from the environment. They do not export, prompt for,
or store the token. Override the controller with `$ZCLI_SERVER` if needed.

Catalog of wrappers in [`scripts/`](../scripts/). Hardware-model
inventory: [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md).
Firmware updates: [EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md).
Architecture: [Architecture.md](Architecture.md). Deploy HVM guests:
[EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md). **Do not** add
interfaces to an edge-app that already has instances:
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md).

## scripts/zcli

Authenticate and run ZEDEDA `zcli` in Docker (`zededa/zcli:latest`).

```bash
./scripts/zcli
./scripts/zcli -- --format=json model show N1-655-Cooper-Pro --detail
```

- No args: interactive bash + autocomplete (needs a TTY). `models/` and
  `apps/` are mounted read-only at `/home/zcli/models` and
  `/home/zcli/apps`.
- `./scripts/zcli -- <args>`: one-shot. Configure, run that command, print
  stdout. No TTY. Other wrappers use this path.

Do not run raw `docker run -it zededa/zcli:latest` and paste a token.

## scripts/get_models.sh

Pull Ambarella hardware-details JSON from the controller into `models/`.

```bash
./scripts/get_models.sh
./scripts/get_models.sh --dry-run
./scripts/get_models.sh N1-655-Cooper-Pro
./scripts/get_models.sh --brand=Ambarella
```

Default brand is Ambarella (the two Cooper models). Writes
`models/<CloudName>.json` with `arch`, `productURL`, `productStatus`,
`attr`, and `ioMemberList` (no logo, no cloud metadata). `--dry-run`
prints JSON and writes nothing.

## scripts/push_models.sh

Planned reverse of `get_models.sh`: `zcli model update` existing cloud
models from edited `models/*.json`. Not in the tree yet. Do not `model
create`.

## scripts/show_instances.sh

List deployed edge-app instances, or show one instance’s adapters and
networks.

```bash
./scripts/show_instances.sh
./scripts/show_instances.sh --edge-node=NAME
./scripts/show_instances.sh --edge-app=ubuntu_24_04
./scripts/show_instances.sh ubuntu_24_04.n1-655-pro
./scripts/show_instances.sh NAME --format=json
```

Use this first when I/O looks wrong. Typical names (cloud may differ):

| Role | Typical instance | Adapters |
|---|---|---|
| HVM | `ubuntu_24_04.n1-655-pro` | `amba_shm` (ivshmem window); never `cavalry`, `gpio`, `iav` |

The reference design has HVM guests only. `amba-virt-server` in EVE Dom0 owns
`cavalry`, `iav`, and `amba_virt`; no app is assigned them.

## scripts/show_app.sh

Show **edge-app** manifest interface names. Those names are the left side
of `--adapter=intfname:assigngrp`.

```bash
./scripts/show_app.sh ubuntu_24_04
```

An interface name missing here cannot be used by `set_adapters.sh` or
`create_instance.sh --adapter=`. gmwtus will not add interface names to an
edge-app that already has instances (Halted counts). Create a new bundle
instead:
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md).

## scripts/set_adapters.sh

Replace adapter attachments on a **running** instance without dropping
network instances.

`zcli edge-app-instance update --adapter=` replaces the whole
`interfaces` list. This wrapper reads the current instance, keeps every
`netinstname` row, and sends `--network-instance=` plus the new
`--adapter=` list together.

```bash
./scripts/set_adapters.sh ubuntu_24_04.n1-655-pro --adapter=… --dry-run
./scripts/set_adapters.sh NAME --adapter=… --restart
./scripts/set_adapters.sh ubuntu_24_04.n1-655-pro --clear-adapters --restart
```

- `--adapter=` **replaces** adapter attachments. Re-specify all you want.
- `--clear-adapters` removes adapters and keeps networks (HVM).
- The scripts refuse the `cavalry` / `gpio` / `iav` groups unless `--allow-visorc`
  is passed. The reference design never passes it: those devices belong to the
  Dom0 server.
- `--dry-run` prints the `zcli` command and does not update the controller.
- `--restart` then runs `edge-app-instance restart`. Update alone does not
  recreate OCI / QEMU devices.

## scripts/restart_instance.sh

Redeploy without changing adapters.

```bash
./scripts/restart_instance.sh NAME
./scripts/restart_instance.sh NAME --stop
./scripts/restart_instance.sh NAME --start
./scripts/restart_instance.sh NAME --refresh
./scripts/restart_instance.sh NAME --dry-run
```

Default is `restart`. `--refresh` only after an edge-app bundle version
bump. No `--purge`.

## scripts/pull_app.sh

Write the edge-app **manifest** to `apps/<Name>.json` (that directory is
mounted read-only in `zcli`; `apps/*.json` is gitignored). Strips
`show --detail` UI fields (`__*`, JSON `null`, `imagestatus`).

```bash
./scripts/pull_app.sh ubuntu_24_04
./scripts/pull_app.sh ubuntu_24_04 --dry-run
```

## scripts/add_app_direct.sh

Add Other / direct-attach interfaces on that **local** JSON (`directattach:
true`). Copies ACE keys from `eth0` only (not `__*` UI fields). Does not
call the controller. Refuses `HV_HVM` unless `--force`.

```bash
./scripts/add_app_direct.sh APP --if=NAME [--if=NAME...]
```

## scripts/clone_app.sh

Copy `apps/<SRC>.json` to `apps/<DST>.json` and set ACE `name`. Local only.

```bash
./scripts/clone_app.sh ubuntu_24_04 ubuntu_24_04-v2
```

## scripts/create_app.sh

`zcli edge-app create` from `apps/<Name>.json`. Use this for a **new**
bundle that already lists the interfaces it needs. Does not `update`.

```bash
./scripts/create_app.sh ubuntu_24_04-v2 --version=1.0 --dry-run
```

## scripts/create_instance.sh

`zcli edge-app-instance create` with `--network-instance=`,
`--adapter=`, and `--custom-configuration=` (cloud-init). Template
`intfname`s must exist.
`create_instance.sh` extracts `configuration.customConfig` from
`apps/<edge-app>.json` unless you pass `--custom-configuration=` or
`--no-custom-configuration`. An instance created without that flag gets
empty CIDATA; `update` cannot add it later.

```bash
./scripts/create_instance.sh ubuntu_24_04.n1-655-devkit \
  --edge-app=ubuntu_24_04 \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=… --dry-run
```

## scripts/push_app.sh

Update an existing edge-app manifest on ZEDEDA Cloud from local
`apps/<Name>.json`. Requires `appInstCount == 0` if adding new interfaces.

```bash
./scripts/push_app.sh ubuntu_24_04
./scripts/push_app.sh ubuntu_24_04 --dry-run
```

## Provisioning recipes

Current recipe (HVM guests, `amba_shm`): [EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md).

Walkthrough (legacy clone recipe): [EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md).

## scripts/power_cycle_node.sh (alias: scripts/reboot_node.sh)

Power cycle (remotely reboot) or prepare an Ambarella edge node for power off
via ZEDEDA Cloud controller. Supports `--wait` to monitor reboot progress
until the node transitions back to `Online`.

```bash
./scripts/power_cycle_node.sh n1-655-devkit
./scripts/power_cycle_node.sh n1-655-pro --wait
./scripts/power_cycle_node.sh n1-655-devkit --status
./scripts/power_cycle_node.sh n1-655-devkit --poweroff
./scripts/power_cycle_node.sh n1-655-devkit --dry-run
```

## scripts/pub_eve_datastore.sh

Stage an EVE `rootfs.img` into a versioned subdirectory on a datastore and
register/uplink the image in ZedControl via `zcli`.

```bash
./scripts/pub_eve_datastore.sh ~/public_html/eve-images/
./scripts/pub_eve_datastore.sh ~/public_html/eve-images/ --dry-run
./scripts/pub_eve_datastore.sh ~/public_html/eve-images/ --datastore=LocalHTTP
```

Walkthrough: [EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md).
