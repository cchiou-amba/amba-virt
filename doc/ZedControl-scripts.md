# ZedControl scripts

Wrappers under [`scripts/`](../scripts/) talk to
`zedcontrol.gmwtus.zededa.net` through [`scripts/zcli`](../scripts/zcli).
They read `$ZCLI_TOKEN` from the environment. They do not export, prompt for,
or store the token. Override the controller with `$ZCLI_SERVER` if needed.

Catalog of every script in that directory. Hardware-model inventory:
[EVE-Ambarella-Models.md](EVE-Ambarella-Models.md). Architecture:
[Architecture.md](Architecture.md).

## scripts/zcli

Authenticate and run ZEDEDA `zcli` in Docker (`zededa/zcli:latest`).

```bash
./scripts/zcli
./scripts/zcli -- --format=json model show N1-655-Cooper-Pro --detail
```

- No args: interactive bash + autocomplete (needs a TTY). `models/` is
  mounted read-only at `/home/zcli/models`.
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
./scripts/show_instances.sh ubuntu_24_04_container.n1-655-pro
./scripts/show_instances.sh NAME --format=json
```

Use this first when I/O looks wrong. Typical names (cloud may differ):

| Role | Typical instance | Adapters |
|---|---|---|
| NOHYPER | `ubuntu_24_04_container.n1-655-pro` | cavalry, gpio, iav |
| HVM | `ubuntu_24_04.n1-655-pro` | none of those |

## scripts/show_app.sh

Show **edge-app** manifest interface names. Those names are the left side
of `--adapter=intfname:assigngrp`.

```bash
./scripts/show_app.sh ubuntu_24_04_container
```

If `cavalry` / `gpio0` / `iav` are missing here, `set_adapters.sh` cannot
attach them. Adding interfaces to the bundle (`edge-app update --manifest`
plus `restart_instance.sh --refresh`) is a separate step.

## scripts/set_adapters.sh

Replace adapter attachments on a **running** instance without dropping
network instances.

`zcli edge-app-instance update --adapter=` replaces the whole
`interfaces` list. This wrapper reads the current instance, keeps every
`netinstname` row, and sends `--network-instance=` plus the new
`--adapter=` list together.

```bash
./scripts/set_adapters.sh ubuntu_24_04_container.n1-655-pro \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio --adapter=iav:iav \
  --allow-visorc --dry-run
./scripts/set_adapters.sh NAME --adapter=… --allow-visorc --restart
./scripts/set_adapters.sh ubuntu_24_04.n1-655-pro --clear-adapters --restart
```

- `--adapter=` **replaces** adapter attachments. Re-specify all you want.
- `--clear-adapters` removes adapters and keeps networks (HVM).
- `cavalry` / `gpio` / `iav` require `--allow-visorc` (NOHYPER only).
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
