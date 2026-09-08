# Creating NOHYPER EdgeApp Instances on Ambarella Models

Current deploy recipe: [EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md)
(HVM + NOHYPER, `amba_virt`, cloud-init). This page is the older
`create_nohyper.sh` path (no `amba_virt`).

Related documents:
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (hardware models and adapter inventory)
- [ZedControl-scripts.md](ZedControl-scripts.md) (catalog of zcli wrappers)
- [Architecture.md](Architecture.md) (system architecture and transport design)

---

## 1. Ambarella Hardware Adapter Mapping

Ambarella hardware models (`N1-655-Cooper-Devkit` and `N1-655-Cooper-Pro`) publish the following assignable adapters in their `ioMemberList`:

| Interface Name (`intfname`) | Model Adapter Name (`io.name`) | Physical Device / Chardev | Description |
|---|---|---|---|
| `eth0` | `eth0` | `eth0` | Local network instance (`defaultLocal-<node>`). Management usage on host. |
| `cavalry` | `cavalry` | `/dev/cavalry`, `/dev/cavalry_profile` | VisORC NPU accelerator chardevs |
| `gpio0` | `gpio0` | `/dev/gpiochip0` | Ambarella SoC GPIO lines |
| `iav` | `iav` | `/dev/iav` | Ambarella DSP / Video input chardev |
| `USB` | `USB` | USB Host Controller | USB peripheral controller |

> [!NOTE]
> When assigning adapters via `zcli edge-app-instance create --adapter=INTF:ADP`, the parameter syntax is `<manifest-intfname>:<model-adapter-name>`. For the GPIO controller, the model adapter name is `gpio0` (`--adapter=gpio0:gpio0`).

---

## 2. Prerequisites

1. **ZEDEDA Cloud Authentication**:
   Ensure `$ZCLI_TOKEN` is exported in your environment:
   ```bash
   export ZCLI_TOKEN="<your-token>"
   ```

2. **Verify Target Node**:
   Confirm that the target node is `Online` and that the desired NOHYPER container is missing:
   ```bash
   ./scripts/show_instances.sh --edge-node=n1-655-devkit
   ```
   *(Expected: Only the HVM instance `ubuntu_24_04.n1-655-devkit` is listed; the container is missing).*

3. **Verify Edge Application Bundle**:
   Confirm that `ubuntu_24_04-container` is published in ZEDEDA Cloud with all hardware interfaces:
   ```bash
   ./scripts/show_app.sh ubuntu_24_04-container
   ```
   *(Must list: `eth0`, `cavalry`, `gpio0`, `iav`, `USB`). If missing, see [Appendix: Edge-App Bundle Setup](#appendix-edge-app-bundle-setup).*

---

## 3. Step-by-Step: Create the Running Container

### Step 1: Create the Container Instance Using `create_nohyper.sh`

Supply the target edge node name to [`scripts/create_nohyper.sh`](../scripts/create_nohyper.sh):

#### For Ambarella Cooper Devkit (`n1-655-devkit`):
```bash
./scripts/create_nohyper.sh n1-655-devkit
```

#### For Ambarella Cooper Pro (`n1-655-pro`):
```bash
./scripts/create_nohyper.sh n1-655-pro
```

The script automatically:
- Names the instance `ubuntu_24_04_container.<node>` (e.g. `ubuntu_24_04_container.n1-655-devkit`).
- Attaches the local network instance `eth0:defaultLocal-<node>`.
- Attaches all Ambarella hardware adapters:
  - `--adapter=cavalry:cavalry`
  - `--adapter=gpio0:gpio0`
  - `--adapter=iav:iav`
  - `--adapter=USB:USB`
- Submits the creation request to ZEDEDA Cloud via `scripts/zcli`.

#### Optional Flags:
- `--dry-run`: Preview the exact `zcli` command without executing:
  ```bash
  ./scripts/create_nohyper.sh n1-655-devkit --dry-run
  ```
- `--name=NAME`: Override the instance name:
  ```bash
  ./scripts/create_nohyper.sh n1-655-devkit --name=my_custom_container
  ```
- `--network=NET`: Override the network instance:
  ```bash
  ./scripts/create_nohyper.sh n1-655-devkit --network=eth0:custom-network
  ```

---

### Step 2: Monitor Deployment to Running State (`Online`)

Once created, the controller begins deploying the container image to the edge node. The instance transitions through:
`Init` &rarr; `Downloading` &rarr; `Booting` &rarr; `Online`.

Monitor the instance status:

```bash
./scripts/show_instances.sh ubuntu_24_04_container.n1-655-devkit
```

Wait until `run-state` shows `Online`:

```bash
./scripts/show_instances.sh
```

Expected output:
```console
name                                  edge-app                 edge-node      run-state
ubuntu_24_04.n1-655-devkit            ubuntu_24_04             n1-655-devkit  Online
ubuntu_24_04.n1-655-pro               ubuntu_24_04             n1-655-pro     Online
ubuntu_24_04_container.n1-655-devkit  ubuntu_24_04-container   n1-655-devkit  Online
ubuntu_24_04_container.n1-655-pro     ubuntu_24_04-container   n1-655-pro     Online
```

If an instance is ever stopped or halted, bring it back online with:
```bash
./scripts/restart_instance.sh ubuntu_24_04_container.n1-655-devkit --start
```

---

### Step 3: Verify Hardware Attachments & Devices

#### 1. Verify Attached Adapters on Controller
Confirm that all 4 Ambarella adapters plus the network are attached to the instance:

```bash
./scripts/show_instances.sh ubuntu_24_04_container.n1-655-devkit
```

Expected output:
```console
name	ubuntu_24_04_container.n1-655-devkit
edge-app	ubuntu_24_04-container
edge-node	n1-655-devkit
admin	true

intfname	kind	attached
cavalry	adapter	cavalry
gpio0	adapter	gpio0
iav	adapter	iav
USB	adapter	USB
eth0	network	defaultLocal-n1-655-devkit
```

#### 2. Verify Chardevs Inside the Container
Log into the running container (via EdgeView console or SSH on mapped port 4222):

```bash
# Verify Ambarella chardevs:
ls -l /dev/cavalry /dev/cavalry_profile /dev/gpiochip0 /dev/iav

# Verify USB controller:
lsusb
```

Expected devices:
- `/dev/cavalry` and `/dev/cavalry_profile`: VisORC NPU chardevs
- `/dev/gpiochip0`: Ambarella GPIO character device
- `/dev/iav`: Ambarella DSP video driver chardev
- USB devices visible under `lsusb`

#### 3. Confirm HVM Isolation
Check that the HVM guest (`ubuntu_24_04.n1-655-devkit`) remains clean and does not own VisORC/Cavalry:

```bash
./scripts/show_instances.sh ubuntu_24_04.n1-655-devkit
```
*(No adapters must be listed for the HVM).*

---

## 4. Manual Equivalent via `scripts/zcli`

If creating instances without the helper script, run `scripts/zcli` directly:

```bash
# On n1-655-devkit:
./scripts/zcli -- edge-app-instance create ubuntu_24_04_container.n1-655-devkit \
  --edge-app=ubuntu_24_04-container \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=cavalry:cavalry \
  --adapter=gpio0:gpio0 \
  --adapter=iav:iav \
  --adapter=USB:USB

# On n1-655-pro:
./scripts/zcli -- edge-app-instance create ubuntu_24_04_container.n1-655-pro \
  --edge-app=ubuntu_24_04-container \
  --edge-node=n1-655-pro \
  --network-instance=eth0:defaultLocal-n1-655-pro \
  --adapter=cavalry:cavalry \
  --adapter=gpio0:gpio0 \
  --adapter=iav:iav \
  --adapter=USB:USB
```

---

## Appendix: Edge-App Bundle Setup

If the `ubuntu_24_04-container` bundle in ZEDEDA Cloud is missing any of the direct-attach interface definitions (`cavalry`, `gpio0`, `iav`, `USB`), prepare the local manifest in `apps/ubuntu_24_04-container.json`:

```json
  "interfaces": [
    {
      "name": "eth0",
      "directattach": false,
      "acls": [ ... ]
    },
    {
      "name": "cavalry",
      "directattach": true,
      "acls": []
    },
    {
      "name": "gpio0",
      "directattach": true,
      "acls": []
    },
    {
      "name": "iav",
      "directattach": true,
      "acls": []
    },
    {
      "name": "USB",
      "directattach": true,
      "acls": []
    }
  ]
```

Push the updated manifest to ZEDEDA Cloud:

```bash
./scripts/push_app.sh ubuntu_24_04-container
```

*(Note: Adding interfaces to an edge application requires that no instances are currently deployed, i.e., `appInstCount == 0`).*
