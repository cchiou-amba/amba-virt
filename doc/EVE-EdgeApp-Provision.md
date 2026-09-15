# Provision Ubuntu 24.04 HVM and NOHYPER on EVE

From-scratch recipe for one Ambarella node: an Ubuntu **HVM** (KVM guest)
and a privileged Ubuntu **NOHYPER** container. Example node:
**`n1-655-devkit`**. Other nodes are the same commands with the node name and
`defaultLocal-<node>` swapped.

`$ZCLI_TOKEN` must already be in the environment. Wrappers never export it.
Catalog: [ZedControl-scripts.md](ZedControl-scripts.md). Models:
[EVE-Ambarella-Models.md](EVE-Ambarella-Models.md). Architecture:
[Architecture.md](Architecture.md).

This guide stops when login and adapter assignment work. Transport smoke
tests stay in [guest-os/client/README.md](../guest-os/client/README.md).
The pair this creates is the same transport Native-ivshmem already ran.

Do not bake NoCloud into the Ubuntu image before EVE sees it. EVE injects
CIDATA from the **instance** `--custom-configuration`, not from the
edge-app bundle. Putting the YAML only on the marketplace app is not
enough for `zcli`. On ARM `virt`, that ISO is a USB CD and cloud-init
often never sees it; the repeatable operator seed is
[Appendix H](#h-good-iso-guest-never-ran-cloud-init). Local QEMU image
hacks live next to this repo in `cloud-images/` (reference only).

## Names

| Role | Edge-app | Instance | Image |
|---|---|---|---|
| HVM | `ubuntu_24_04` | `ubuntu_24_04.n1-655-devkit` | `ubuntu-24.04-server-cloudimg-arm64` (QCOW2 from Ubuntu) |
| NOHYPER | `ubuntu_24_04-container` | `ubuntu_24_04_container.n1-655-devkit` | `ubuntu-24-04-container` (`library/ubuntu:24.04`) |

Network: `eth0:defaultLocal-n1-655-devkit`.

| App | Adapters at instance create |
|---|---|
| HVM | `amba_shm:amba_shm` |
| NOHYPER | `cavalry:cavalry`, `gpio0:gpio0`, `iav:iav`, `USB:USB`, `amba_virt:amba_virt` |

Do not attach VisORC to the HVM. `amba_shm` is an `IO_TYPE_OTHER` window
marker (empty `phyaddrs`); assigning it is what makes `kvm.go` emit
`ivshmem-plain`. `amba_virt` is `Ifname=/dev/amba_virt` on the host.

**`--adapter=INTF:NAME` takes `logicallabel`, not `assigngrp`.** GPIO is
`gpio0:gpio0`. Passing group `gpio` is rejected (`model does not have adapter`).

Put every interface on the **new** edge-app at create. gmwtus will not add
interfaces later if `appInstCount > 0` (Halted counts).
[EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md).

## One ivshmem, many drivers

One HVM/NOHYPER pair gets **one** `ivshmem-plain` BAR. Cavalry, DMA, SD/eMMC,
and later frontends share it. Do not add a second `amba_shm` per driver.
Several drivers **inside one guest** share offsets (`shm_off` / `shm_len` on
vsock). Several HVMs would need several windows:
[EVE-Multiple-HVM.md](EVE-Multiple-HVM.md) (deferred).

Control stays on vsock (CID 2, port **5555**, never 2000). Bulk is the BAR.
A later host allocator hands out non-overlapping slices; Cavalry takes most of
the pool by quota.

**16M is PoC-only.** It cannot hold Cavalry tensors or DVI. Host
`cavalry_reserved` is **12 GB AMA**; the VP DMA-reads those HPAs. ivshmem is
the **guest staging area** the proxy copies or token-rewrites into that AMA.
Do not map 12 GB as BAR2: PCI BAR size must be a power of two (12G is not),
and host `/dev/shm` is ~8.9 GB.

**Production `cbattr.shmsize`: `1G`.** Power of two. Guest RAM
(`resources.memory` 1 GiB) is a different number; the window is extra and must
be charged in full by `ivshmemVMMOverhead` or QEMU is OOM-killed.
[EVE-Native-ivshmem-Support.md](EVE-Native-ivshmem-Support.md)
§3.4.1. Changing `shmsize` after the instance exists does not resize a live BAR.

## SSH port-forwards

Same mapping the previous instances used. Convenient network access; `eve
console` / `eve enter` still work. Password is **`ubuntu` / `ubuntu`**.

| App | Host (edge node) | Guest | Command |
|---|---|---|---|
| HVM | TCP **2222** | 22 | `ssh ubuntu@<node-ip> -p 2222` |
| NOHYPER | TCP **4222** | 22 | `ssh ubuntu@<node-ip> -p 4222` |

`eth0` ACL `lport` / `portmapto.appPort` on the edge-app. ACLs are snapshotted
into the instance at create; updating the bundle afterwards does not change a
born-wrong port. Password is `ubuntu` / `ubuntu` from **instance**
cloud-init (`--custom-configuration`), not from the edge-app bundle alone.

Resources: 2 CPUs, `1048576` KiB RAM. HVM also `104857600` KiB disk, `HV_HVM`,
`disableVTPM: true`, `enablevnc: true`. NOHYPER is `HV_NOHYPER`.

## 0. Starting point

Node already onboarded and `Online`. BaseOS has the ivshmem patch
(`0.0.0-amba-ivshmem-…` on the example node).

```bash
./scripts/zcli -- edge-node show n1-655-devkit
./scripts/show_instances.sh --edge-node=n1-655-devkit
./scripts/zcli -- edge-app show
```

No instances on the node. `ubuntu_24_04` and `ubuntu_24_04-container` must
be **absent** from the library (this illustration creates them). If those
names are still present and have no instances:

```bash
./scripts/zcli -- edge-app delete ubuntu_24_04 -f
./scripts/zcli -- edge-app delete ubuntu_24_04-container -f
```

Confirm the model publishes `amba_shm` / `amba_virt` / cavalry / gpio / iav /
USB, and that `amba_shm` `cbattr.shmsize` is **1G**:

```bash
./scripts/zcli -- --format=json model show N1-655-Cooper-Devkit --detail \
  | jq -r '.ioMemberList[] | select(.logicallabel=="amba_shm") | .cbattr'
```

If it still says `16M`, edit [models/N1-655-Cooper-Devkit.json](../models/N1-655-Cooper-Devkit.json)
(and Pro) then:

```bash
./scripts/push_models.sh N1-655-Cooper-Devkit --dry-run
./scripts/push_models.sh N1-655-Cooper-Devkit
```

Do this **before** creating the HVM instance.

## 1. Datastores and images

Inspect:

```bash
./scripts/zcli -- datastore show
./scripts/zcli -- datastore show Ubuntu --detail
./scripts/zcli -- datastore show Docker --detail
./scripts/zcli -- image show
```

On gmwtus an older **`Ubuntu`** store is HTTP (`http://cloud-images.ubuntu.com`,
path `releases`). `datastore update` does not change its type. Create a
separate HTTPS store (this is **`UbuntuCloud`**, UUID
`d188e4e2-26ab-430d-aad5-0e02c64ef71e`):

```bash
./scripts/zcli -- datastore create UbuntuCloud \
  --dstype=HTTPS --fqdn=https://cloud-images.ubuntu.com --dpath=releases \
  --title=UbuntuCloud
./scripts/zcli -- datastore show UbuntuCloud --detail
```

If `UbuntuCloud` already exists, skip create.

**HVM image** — official noble ARM64 cloudimg, not a customized
`ubuntu-24_04`. Relative URL is under datastore path `releases`:

```bash
# SHA-256 and size from
# https://cloud-images.ubuntu.com/releases/noble/release/SHA256SUMS
# Refresh these when Ubuntu publishes a new release image.
IMG_SHA=afa139bac6f2629c1e1f2f8f34215f3a9ad9779801bcb945521ba1a45016743f
IMG_SIZE=619036160

./scripts/zcli -- image create ubuntu-24.04-server-cloudimg-arm64 \
  --datastore-name=UbuntuCloud --arch=ARM64 --image-format=qcow2 \
  --type=Application \
  --image-url=noble/release/ubuntu-24.04-server-cloudimg-arm64.img \
  --title=ubuntu-24.04-server-cloudimg-arm64

./scripts/zcli -- image uplink ubuntu-24.04-server-cloudimg-arm64 \
  --datastore-name=UbuntuCloud --image-sha="$IMG_SHA" --image-size="$IMG_SIZE"

./scripts/zcli -- image show ubuntu-24.04-server-cloudimg-arm64 --detail
```

Record `Image ID` from `--detail` for the edge-app JSON.

**NOHYPER image** — official Ubuntu container on Docker Hub. cloud-images
does not serve OCI images. Reuse `ubuntu-24-04-container` if it is already
`library/ubuntu:24.04` on datastore **`Docker`**. Otherwise:

```bash
./scripts/zcli -- image create ubuntu-24-04-container \
  --datastore-name=Docker --arch=ARM64 --image-format=container \
  --type=Application --image-url=library/ubuntu:24.04 \
  --title=ubuntu-24-04-container
```

```bash
./scripts/zcli -- image show ubuntu-24-04-container --detail
```

## 2. Cloud-init (`ubuntu` / `ubuntu`)

Two objects, both required:

1. **Edge-app** `configuration.customConfig`: `name: cloud-init`, `add:
   true`, **`override: false`**, plus `template`. That is “Allow Edge App
   deployments to set entire configuration” in the UI. It does **not**
   copy user-data onto instances created with `zcli`. `override: true`
   together with `template` on the marketplace app is rejected.
2. **Instance create** `--custom-configuration=<json>`. The JSON is the
   same `customConfig` object with `override: true` and `template` set to
   the base64 YAML. `create_instance.sh` extracts this from
   `apps/<edge-app>.json` and passes it. `edge-app-instance update` has no
   such flag; a missed instance must be deleted and recreated.

ZEDEDA: [Custom Configuration Edge Application](https://help.zededa.com/hc/en-us/articles/4440323189403-Custom-Configuration-Edge-Application).

Encode the YAML with:

```bash
python3 -c 'import base64,sys; print(base64.b64encode(sys.stdin.buffer.read()).decode())'
```

Do not copy [cloud-images/user-data](../../cloud-images/user-data) as-is
(personal SSH key, baked NoCloud seed).

An instance created without `--custom-configuration` still gets a CIDATA
ISO, but `user-data` is **empty**. That is worse than no CIDATA: NoCloud
wins as the datasource, the password is never set, and DHCP often never
completes (`RX=0`, `ssh -p 2222` → `No route to host`).
`edge-app-instance show` redacts `customConfig.template`. Confirm on the
node: CIDATA `user-data` must start with `#cloud-config` and be non-empty.

Failure catalog (empty ISO after reboot, Ubuntu 24.04 `chpasswd`,
`EVE_ECO_CMD`, `ignorepurge`, what `update` cannot do):
[Appendix: Cloud-init failure modes](#appendix-cloud-init-failure-modes).

### HVM

Full cloud-init. EVE mounts it as NoCloud (CIDATA). The official noble
cloudimg already has cloud-init and the `ubuntu` user. Ubuntu 24.04’s
cloud-init ignores the old `chpasswd.list` form, so use `chpasswd.users`
with `type: text`. Virtio-net shows up as `enp3s0`; pin DHCP with a
netplan match on `en*`. `linux-headers-generic` is for a later in-guest
`make build-hvm`, not used in this document.

```yaml
#cloud-config
hostname: ubuntu
ssh_pwauth: true
users:
  - name: ubuntu
    lock_passwd: false
    sudo: ALL=(ALL) NOPASSWD:ALL
    groups: sudo
    shell: /bin/bash
chpasswd:
  expire: false
  users:
    - name: ubuntu
      password: ubuntu
      type: text
packages:
  - build-essential
  - linux-headers-generic
  - openssh-server
write_files:
  - path: /etc/netplan/01-dhcp.yaml
    content: |
      network:
        version: 2
        ethernets:
          allnics:
            match:
              name: en*
            dhcp4: true
runcmd:
  - rm -f /etc/netplan/50-cloud-init.yaml
  - netplan generate
  - netplan apply
```

### NOHYPER

EVE does **not** run cloud-init inside the container. It only applies `runcmd`
(environment) and `write_files`
([EVE cloud-init](https://eve-os.readthedocs.io/docs/CLOUD-INIT/)). `runcmd`
sets `EVE_ECO_CMD=/etc/init.sh`, which **replaces** the image entrypoint
(`ubuntu:24.04` is `/bin/bash`). That must be on the **instance**
custom-configuration so it is the command on every start. If it is only
in the edge-app bundle, the first volume may get `write_files` but a
reboot falls back to `/bin/bash` and sshd never comes back (`Connection
refused` on 4222).

`init.sh` creates `ubuntu`, sets the password, starts `sshd`, then
`sleep infinity` so the container does not exit.

`eve enter` is a host nsenter (root, **no** login prompt). `su - ubuntu` and
`ssh -p 4222` use `ubuntu` / `ubuntu`. First boot runs `apt-get` (needs
network on `eth0`); wait until sshd is up before `-p 4222`.

```yaml
#cloud-config
runcmd:
  - EVE_ECO_CMD=/etc/init.sh
write_files:
  - path: /etc/init.sh
    permissions: '0755'
    owner: root:root
    content: |
      #!/bin/sh
      echo "`date` - /etc/init.sh starts" >> /var/log/init.sh.log
      export DEBIAN_FRONTEND=noninteractive
      apt-get update
      apt-get install -y --no-install-recommends openssh-server sudo passwd
      id ubuntu >/dev/null 2>&1 || useradd -m -s /bin/bash -G sudo ubuntu
      echo 'ubuntu:ubuntu' | chpasswd
      passwd -u ubuntu || true
      echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/90-ubuntu
      chmod 0440 /etc/sudoers.d/90-ubuntu
      mkdir -p /run/sshd /etc/ssh/sshd_config.d
      printf 'PasswordAuthentication yes\nKbdInteractiveAuthentication yes\n' \
        > /etc/ssh/sshd_config.d/00-passwordauth.conf
      ssh-keygen -A
      /usr/sbin/sshd
      echo "`date` - /etc/init.sh ends" >> /var/log/init.sh.log
      exec sleep infinity
```

## 3. Create the edge-apps

Write ACE JSON under `apps/` (gitignored). `create_app.sh` sanitizes and
calls `zcli edge-app create`. Substitute `IMAGE_ID` from `image show
--detail`. Datastore IDs on gmwtus: UbuntuCloud
`d188e4e2-26ab-430d-aad5-0e02c64ef71e`, Docker
`ed972124-19d0-4dfa-a89b-a29d892e0771`. Image
`ubuntu-24.04-server-cloudimg-arm64` is
`9c940ce7-6b6e-474e-8e56-67bc882bb849`.

`add_app_direct.sh` allows `amba_shm` on `HV_HVM` (empty `phyaddrs` window
marker). It still refuses `cavalry`.

### HVM `apps/ubuntu_24_04.json`

```json
{
  "acKind": "VMManifest",
  "acVersion": "1.2.0",
  "name": "ubuntu_24_04",
  "images": [
    {
      "imagename": "ubuntu-24.04-server-cloudimg-arm64",
      "imageid": "9c940ce7-6b6e-474e-8e56-67bc882bb849",
      "imageformat": "QCOW2",
      "maxsize": "10485760",
      "preserve": false,
      "target": "Disk",
      "drvtype": "HDD",
      "ignorepurge": true,
      "cleartext": true,
      "datastore": [
        { "id": "d188e4e2-26ab-430d-aad5-0e02c64ef71e", "name": "UbuntuCloud" }
      ]
    }
  ],
  "interfaces": [
    {
      "name": "eth0",
      "directattach": false,
      "acls": [
        {
          "matches": [{ "type": "ip", "value": "0.0.0.0/0" }],
          "actions": []
        },
        {
          "matches": [
            { "type": "protocol", "value": "tcp" },
            { "type": "lport", "value": "2222" },
            { "type": "ip", "value": "0.0.0.0/0" }
          ],
          "actions": [
            {
              "portmap": true,
              "portmapto": { "appPort": 22 }
            }
          ]
        }
      ]
    },
    {
      "name": "amba_shm",
      "directattach": true,
      "acls": []
    }
  ],
  "vmmode": "HV_HVM",
  "enablevnc": true,
  "resources": [
    { "name": "cpus", "value": "2" },
    { "name": "memory", "value": "1048576.00" },
    { "name": "storage", "value": "104857600.00" }
  ],
  "configuration": {
    "customConfig": {
      "name": "cloud-init",
      "add": true,
      "override": false,
      "allowStorageResize": false,
      "fieldDelimiter": "",
      "template": "<base64 of HVM #cloud-config>",
      "variableGroups": []
    }
  },
  "appType": "APP_TYPE_VM",
  "deploymentType": "DEPLOYMENT_TYPE_STAND_ALONE",
  "disableVTPM": true
}
```

```bash
./scripts/create_app.sh ubuntu_24_04 --version=1.0 --dry-run
./scripts/create_app.sh ubuntu_24_04 --version=1.0
./scripts/show_app.sh ubuntu_24_04
```

Expect `eth0` and `amba_shm`. `--version` is `userDefinedVersion`. ACE
`acVersion` stays `1.2.0`. Marketplace `customConfig.override` is
**`false`**; `create_instance.sh` sets `true` only on the instance JSON
([appendix](#appendix-cloud-init-failure-modes)).

### NOHYPER `apps/ubuntu_24_04-container.json`

```json
{
  "acKind": "PodManifest",
  "acVersion": "1.2.0",
  "name": "ubuntu_24_04-container",
  "images": [
    {
      "imagename": "ubuntu-24-04-container",
      "imageid": "d9a25a64-d542-44ff-ac37-11e2f5a4674a",
      "imageformat": "CONTAINER",
      "maxsize": "0",
      "cleartext": true,
      "datastore": [
        { "id": "ed972124-19d0-4dfa-a89b-a29d892e0771", "name": "Docker" }
      ]
    }
  ],
  "interfaces": [
    {
      "name": "eth0",
      "directattach": false,
      "acls": [
        {
          "matches": [{ "type": "ip", "value": "0.0.0.0/0" }],
          "actions": []
        },
        {
          "matches": [
            { "type": "protocol", "value": "tcp" },
            { "type": "lport", "value": "4222" },
            { "type": "ip", "value": "0.0.0.0/0" }
          ],
          "actions": [
            {
              "portmap": true,
              "portmapto": { "appPort": 22 }
            }
          ]
        }
      ]
    },
    { "name": "cavalry", "directattach": true, "acls": [] },
    { "name": "gpio0", "directattach": true, "acls": [] },
    { "name": "iav", "directattach": true, "acls": [] },
    { "name": "USB", "directattach": true, "acls": [] },
    { "name": "amba_virt", "directattach": true, "acls": [] }
  ],
  "vmmode": "HV_NOHYPER",
  "enablevnc": true,
  "resources": [
    { "name": "cpus", "value": "2" },
    { "name": "memory", "value": "1048576.00" }
  ],
  "configuration": {
    "customConfig": {
      "name": "cloud-init",
      "add": true,
      "override": false,
      "fieldDelimiter": "",
      "template": "<base64 of NOHYPER #cloud-config>",
      "variableGroups": []
    }
  },
  "appType": "APP_TYPE_CONTAINER",
  "deploymentType": "DEPLOYMENT_TYPE_STAND_ALONE"
}
```

Confirm `imageid` with `image show ubuntu-24-04-container --detail` if you
created a new record.

```bash
./scripts/create_app.sh ubuntu_24_04-container --version=1.0 --dry-run
./scripts/create_app.sh ubuntu_24_04-container --version=1.0
./scripts/show_app.sh ubuntu_24_04-container
```

Expect `eth0`, `cavalry`, `gpio0`, `iav`, `USB`, `amba_virt`.

Do not use `create_nohyper.sh` for this pair; it still defaults to an app
without `amba_virt`.

## 4. Create instances on n1-655-devkit

`create_instance.sh` writes `apps/.custom-config.<edge-app>.json` from the
local manifest and passes `--custom-configuration=/home/zcli/apps/...`.
Dry-run must show that flag. Override with
`--custom-configuration=apps/FILE.json` or skip with
`--no-custom-configuration` (do not skip for this pair).

```bash
./scripts/create_instance.sh ubuntu_24_04.n1-655-devkit \
  --edge-app=ubuntu_24_04 \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=amba_shm:amba_shm \
  --dry-run
./scripts/create_instance.sh ubuntu_24_04.n1-655-devkit \
  --edge-app=ubuntu_24_04 \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=amba_shm:amba_shm

./scripts/create_instance.sh ubuntu_24_04_container.n1-655-devkit \
  --edge-app=ubuntu_24_04-container \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio0 \
  --adapter=iav:iav --adapter=USB:USB --adapter=amba_virt:amba_virt \
  --allow-visorc --dry-run
./scripts/create_instance.sh ubuntu_24_04_container.n1-655-devkit \
  --edge-app=ubuntu_24_04-container \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=cavalry:cavalry --adapter=gpio0:gpio0 \
  --adapter=iav:iav --adapter=USB:USB --adapter=amba_virt:amba_virt \
  --allow-visorc
```

`--allow-visorc` is required for cavalry / iav. Wait until `Online`:

```bash
./scripts/show_instances.sh --edge-node=n1-655-devkit
./scripts/show_instances.sh ubuntu_24_04.n1-655-devkit
./scripts/show_instances.sh ubuntu_24_04_container.n1-655-devkit
```

An `assigngrp` is exclusive. On a blank node that is not an issue.

## 5. Verify (provisioning only)

Stop here. Do not `insmod` `amba_virt.ko` or run `ping` / `shm` in this
document.

**Controller**

```bash
./scripts/show_instances.sh ubuntu_24_04.n1-655-devkit
./scripts/show_instances.sh ubuntu_24_04_container.n1-655-devkit
```

HVM: `amba_shm` only (plus `eth0` network). No cavalry / gpio / iav.
NOHYPER: cavalry, gpio0, iav, USB, amba_virt, and `eth0` →
`defaultLocal-n1-655-devkit`.

**HVM**

```text
eve console
```

Login `ubuntu` / `ubuntu`. Then:

```bash
lspci -nn | grep -E '1af4:1110|1af4:1053'
```

Expect vsock `1af4:1053` and ivshmem `1af4:1110`. BAR 2 size **1 GiB**.

```bash
ssh ubuntu@<node-ip> -p 2222
```

If the ISO is non-empty but serial has no `Cloud-init` (USB CD race),
do not MCU-cycle. Seed the qcow2:
[Appendix H.1](#h1-on-disk-nocloud-seed-repeatable).

**NOHYPER**

```text
eve enter
```

Root shell (no password). Then:

```bash
su - ubuntu
ls -l /dev/cavalry /dev/cavalry_profile /dev/gpiochip0 /dev/iav /dev/amba_virt
```

`/dev/amba_virt` is injected from the model. No `mknod`. SSH:

```bash
ssh ubuntu@<node-ip> -p 4222
```

## Out of scope

Virtualization transport demonstration: `insmod amba_virt.ko`, `amba-virt-server`,
test client. That workflow is documented in
([guest-os/client/README.md](../guest-os/client/README.md)). A 1G window does not change those tests:
they mmap whatever `GET_INFO.shm_size` reports and write 256 bytes at offset 0.
The guest module must be built in the VM against that kernel.

## Appendix: Cloud-init failure modes

Lab notes from first-booting this pair on `n1-655-devkit` and
`n1-655-pro` (2026-09-08). The VM/container can be `Online` with
`amba_shm` / `amba_virt` assigned and QEMU still emitting
`ivshmem-plain` while login and SSH are dead. That is this appendix, not
a firmware miss.

### A. Two `customConfig` objects (do not mix the flags)

| Where | `add` | `override` | `template` |
|---|---|---|---|
| Marketplace **edge-app** (`apps/<name>.json`, `create_app.sh` / `push_app.sh`) | `true` | **`false`** | base64 YAML |
| **Instance** `--custom-configuration` JSON (`create_instance.sh` writes `apps/.custom-config.<app>.json`) | `true` | **`true`** | same YAML |

Putting the YAML only on the edge-app is not enough for
`zcli edge-app-instance create`. `zcli` does not copy the bundle
template onto the instance. Dry-run **must** show
`--custom-configuration=/home/zcli/apps/...`. The file has to live under
`apps/` because the zcli container mounts that directory read-only at
`/home/zcli/apps`. `--no-custom-configuration` is how you get an empty
ISO on purpose; do not skip it for this pair.

`create_instance.sh` / `app_manifest.py extract-custom-config` reads the
local manifest and **forces** `override: true` on the instance JSON. Do
not push that extracted file back as the marketplace app.

Controller reject if the **edge-app** has both `override: true` and a
non-empty `template`:

```text
invalid custom config, both override flag and template can't be set
```

`add: true` is the UI “Allow Edge App deployments to set entire
configuration”. It only *permits* an instance script. It does not supply
one.

### B. What you cannot fix in place

- `zcli edge-app-instance update` has **no** `--custom-configuration`.
  A born-wrong or emptied instance must be **deleted and recreated**.
- `edge-app-instance show` **redacts** `customConfig.template`. An empty
  field in `show` does not mean the node ISO is empty, and a full local
  JSON does not mean the controller persisted it. Always inspect CIDATA
  on the edge node.
- `edge-app-instance refresh --purge` cannot reset the HVM disk when the
  image has `"ignorepurge": true` (the `ubuntu-24.04-server-cloudimg-arm64`
  stanza in this guide). Delete + create is the wipe.
- Updating the edge-app bundle after the instance exists does not
  rewrite CIDATA or `EVE_ECO_CMD`. ACLs/portmaps are snapshotted at
  create too.

### C. Empty CIDATA is worse than none

EVE always builds a NoCloud ISO at
`/run/domainmgr/cloudinit/<app-uuid>.cidata` and attaches it (see
`file = "...cidata"` in `/run/domainmgr/xen/xen2.cfg`). With no
instance template, `user-data` is **0 bytes** and `meta-data` still has
`instance-id` / `local-hostname`. Ubuntu then selects NoCloud, never
sets the password, and often never finishes DHCP.

**Symptoms (HVM)**

| Check | Empty / missing cloud-init | Healthy first boot |
|---|---|---|
| `eve app console` | `ubuntu login:` then **Login incorrect** for `ubuntu`/`ubuntu` | login works |
| CIDATA `user-data` | 0 bytes | starts with `#cloud-config`, ~748 bytes for the YAML in §2 |
| Guest tap (`nbu1x2` on the example node) | **RX=0** | RX and TX both moving |
| `ip neigh` for the DNAT target | `10.1.0.x` **FAILED** | REACHABLE / STALE |
| `ssh -p 2222` | **No route to host** | password `ubuntu` works |
| Host DNAT | still `2222 → 10.1.0.x:22` | same (DNAT is not the bug) |

**Connection refused** on 2222 is a different bug (guest has an IP, sshd
is down). **No route to host** is “guest never spoke on the tap”.

Inspect on the EVE host (Devkit `root@<devkit-ip>`; Pro host `:22` may
be closed — use serial):

```bash
ls -l /run/domainmgr/cloudinit/
# <uuid>.cidata
mkdir -p /tmp/cidata && mount -o loop,ro /run/domainmgr/cloudinit/<uuid>.cidata /tmp/cidata
ls -la /tmp/cidata
wc -c /tmp/cidata/user-data
cat /tmp/cidata/user-data
cat /tmp/cidata/meta-data
umount /tmp/cidata
```

`user-data` must start with `#cloud-config` and be non-empty. Do not
trust `zcli show`.

### D. Reboot empties the instance template

Observed on `n1-655-devkit` HVM `ubuntu_24_04.n1-655-devkit`
(`35ee20e0-0ca0-4f93-9fba-e0424c2d07f3`):

1. `create_instance.sh` with `--custom-configuration` → CIDATA
   `user-data` **748 bytes**.
2. MCU power-cycle (these boards do **not** reset on a cloud/EVE
   reboot; only the MCU rail does).
3. EVE rebuilds the ISO from the instance config the controller
   sends after reboot. That config no longer has the template.
4. Same path, `user-data` **0 bytes**, `meta-data` only
   (`instance-id: 35ee20e0-…/1`). Console and DHCP die as in §C.

A later first-boot on `n1-655-pro` with the same recipe was healthy
(DHCP `10.1.0.130` on `enp3s0`, SSH `:2222`, in-guest
`user-data.txt` 748 bytes). Treat that as first-boot only: the
Devkit MCU cycle showed the controller does not persist the instance
template, so the next power cycle can empty CIDATA the same way. Do
not MCU-cycle a node to “make cloud-init retry”. Delete and recreate
instead, then verify CIDATA **before** any power cycle.

`edge-app-instance update` cannot put the template back. Recreate:

```bash
./scripts/zcli -- edge-app-instance delete ubuntu_24_04.n1-655-devkit -f
./scripts/create_instance.sh ubuntu_24_04.n1-655-devkit \
  --edge-app=ubuntu_24_04 \
  --edge-node=n1-655-devkit \
  --network-instance=eth0:defaultLocal-n1-655-devkit \
  --adapter=amba_shm:amba_shm
```

Leave the NOHYPER instance alone if it is still good. Wait until
`Online`, mount CIDATA, then try `ssh -p 2222`. `cloud-init status:
running` on the guest is normal while `packages:` (`build-essential`,
`linux-headers-generic`, `openssh-server`) install; SSH can already
work.

### E. Ubuntu 24.04 YAML traps (HVM)

Even with a non-empty ISO, these still yield **Login incorrect** and/or
no DHCP:

| Mistake | What happens |
|---|---|
| `chpasswd.list` (`ubuntu:ubuntu` under `list: \|`) | Noble cloud-init **ignores** it. Password never set. |
| No `chpasswd.users` + `type: text` | Same. Use the YAML in §2. |
| Assume the NIC is `eth0` | Virtio-net is `enp3s0`. Without the netplan `match: name: en*` + `runcmd` that drops `50-cloud-init.yaml`, DHCP often never completes (`RX=0`). |
| Bake NoCloud into the qcow2 | Fights EVE’s CIDATA. Do not copy [cloud-images/user-data](../../cloud-images/user-data) (personal key, local seed). |
| Old `ubuntu` user locked | Official noble cloudimg already has `ubuntu`. `lock_passwd: false` + `ssh_pwauth: true` are required. |

### F. NOHYPER is not cloud-init

EVE does not run cloud-init inside the container. It only honors
`write_files` and `runcmd` as **host-side** ECO metadata
([EVE cloud-init](https://eve-os.readthedocs.io/docs/CLOUD-INIT/)).

`runcmd: [EVE_ECO_CMD=/etc/init.sh]` must be on the **instance**
`--custom-configuration` so it is the container command **every start**.
`ubuntu:24.04` default is `/bin/bash`. `init.sh` installs sshd, sets
`ubuntu`/`ubuntu`, then `exec sleep infinity`.

| After MCU / EVE restart | Meaning |
|---|---|
| PID 1 is `/etc/init.sh` or `sleep infinity`, `/var/log/init.sh.log` has a **new** timestamp | `EVE_ECO_CMD` reapplied. SSH `:4222` should return. |
| PID 1 is `/bin/bash`, log timestamp is still first-create | Instance template was not sent this boot. Volume kept `/etc/init.sh` and a previously installed sshd; **the next reboot drops sshd** unless someone started it by hand. |
| `ssh -p 4222` **Connection refused** | DNAT is fine (`4222 → 10.1.0.x:22`); sshd is not running. |
| `eve enter` works, `su - ubuntu` works | Expected: `eve enter` is host nsenter, root, no login. That does not prove SSH. |

Same persistence hole as the HVM ISO: if the instance custom-config is
not in what EVE receives after reboot, ECO falls back to the image
entrypoint. Fix is delete + recreate with `create_instance.sh`, not
`update`.

`/dev/amba_virt`, `/dev/cavalry`, `/dev/gpiochip0` coming from the
model is independent of this. Missing `/dev/iav` for the `ubuntu` user
does not mean cloud-init failed; check as root via `eve enter`.

### G. Quick split

| What you see | Likely cause | Fix |
|---|---|---|
| HVM `Online`, console **Login incorrect**, `ssh -p 2222` **No route to host**, tap **RX=0** | Empty CIDATA, YAML traps (§E), **or** guest never mounted the ISO (§H) | Mount ISO on the **host**. If `user-data` is 0 bytes: delete + recreate. If YAML is present: grep guest serial for `Cloud-init` and `sda`/`sr0`. |
| HVM SSH works on first boot, dies after MCU | Controller did not persist instance template; ISO rebuilt empty | Recreate. Verify CIDATA before the next power cycle. |
| NOHYPER `:4222` **Connection refused**, PID 1 `/bin/bash` | `EVE_ECO_CMD` not on the instance this boot | Recreate with container `--custom-configuration`. |
| `ssh -p 2222` / `4222` **Connection refused** but neighbor is REACHABLE | sshd not running (packages still installing, or `init.sh` never ran) | Wait for `cloud-init` / `init.sh`; do not MCU-cycle. |
| `push_app.sh` / `edge-app create` rejects custom config | App JSON has `override: true` **and** `template` | Set app `override: false`. Keep `override: true` only on the instance JSON. |
| `refresh --purge` does nothing useful | `"ignorepurge": true` on the cloudimg | Delete the instance. |
| `zcli show` template looks empty | Redaction | Ignore it. Mount CIDATA. |
| Host CIDATA is 748 bytes / `#cloud-config`, guest serial has **no** `Cloud-init` | EVE attached NoCloud as USB CD; `cloud-init-generator` often runs before `sr0` exists | See §H. Instance restart may bind `usb-storage` but is the same race. Do not MCU. |

### H. Good ISO, guest never ran cloud-init

This is **not** the empty-CIDATA bug. Observed on the 2026-09-08
recreate of `ubuntu_24_04.n1-655-devkit`
(`22457359-76b4-4fd5-bdd1-2ed7415f8879`) after `--custom-configuration`
was passed correctly.

On the EVE host the ISO was fine: LABEL `cidata`, `user-data` **748
bytes**, same YAML as the working Pro first boot. Volume
`CreateTime` was a fresh copy of
`ubuntu-24.04-server-cloudimg-arm64` (not the previous dirty disk).
`xen2.cfg` attaches that ISO as `usb-storage` / `media = "cdrom"`
(`QEMU USB HARDDRIVE`), not virtio-scsi.

The guest serial then showed:

- `enp3s0: renamed from eth0` (virtio-net is present)
- USB device `QEMU USB HARDDRIVE` (`idVendor=46f4`)
- **No** `usb-storage` / `sda` / `sr0` — the ISO never became a block
  device
- **No** `Cloud-init` lines at all
- `systemd-networkd-wait-online` **FAILED**
- `ubuntu login:` then **Login incorrect** for `ubuntu`/`ubuntu`

`hostname ubuntu` on the getty banner is the **cloudimg default**. It
does not mean our YAML ran. The official image’s `ubuntu` user stays
locked until cloud-init sets the password, so console login fails even
though the hash in the ISO is `ubuntu`. Tap `RX=0` is the same: netplan
DHCP from §2 never applied.

The same EVE USB CIDATA path **did** work on `n1-655-pro` first boot
(guest `user-data.txt` 748 bytes, `enp3s0` DHCP, SSH `:2222`). Treat
Devkit vs Pro here as USB enumeration, not a missing
`--custom-configuration`.

**Check**

```bash
# host: ISO must be non-empty (this is the empty-CIDATA check)
blkid /run/domainmgr/cloudinit/<uuid>.cidata   # LABEL="cidata"
# guest serial: must mention Cloud-init *and* a cidata block device
eve app console   # then:  <uuid>.1.2/cons
# or grep app logs:
#   Cloud-init v.
#   sda / sr0 / LABEL=cidata
```

**Do not MCU-cycle** to retry this. That rebuilds CIDATA from the
controller and can empty `user-data` (§D). An instance restart (not
MCU) re-enumerates USB and **keeps** the 748-byte ISO; on this node
that produced `usb-storage` + `sr0` but still **no** `cloud-init`
units. `cloud-init-generator` / ds-identify runs before the USB CD is
visible, decides there is no datasource, and leaves the cloudimg
`ubuntu` user locked. Pro’s first boot won that race; Devkit did not.

```bash
./scripts/restart_instance.sh ubuntu_24_04.n1-655-devkit
```

If serial still has no `Cloud-init` after that, this is an EVE/QEMU
CIDATA attach problem (`usb-storage` CD vs a virtio CD present at
generator time), not a zcli template miss. More restarts are the same
race. Do not MCU-cycle. Use the on-disk seed below.

`kvm.go` `qemuDiskTemplate` uses `usb-storage` for `cdrom` when
`Machine == virt`. A BaseOS change to virtio CD is the lasting fix.
Until then the seed is the repeatable operator path.

### H.1 On-disk NoCloud seed (repeatable)

**What it is.** Same YAML as §2, written into the guest root as
`/var/lib/cloud/seed/nocloud/`. `ds-identify` finds that directory on
the virtio root disk before USB exists, so cloud-init runs even when
the CIDATA ISO loses the race. Also set the `ubuntu` hash and netplan
so login works if cloud-init is slow.

**What it is not.** It is not `zcli create` and SSH. There is no
controller flag for this. Do it **once per qcow2** after the instance
exists. Do not bake the seed into the Ubuntu cloudimg in the datastore
(that fights EVE and every other node).

| Event | Seed still there? |
|---|---|
| `restart_instance.sh --stop` / `--start` | **Yes.** It lives on the volume. |
| Instance restart / QEMU reboot | **Yes.** |
| MCU power-cycle | **Yes** (volume kept). Host CIDATA may still go empty (§D); the seed does not care. |
| Delete + `create_instance.sh` | **No.** New volume from the cloudimg. Inject again. |
| `refresh --purge` | Usually **no** (and this image has `ignorepurge: true` anyway). |

Use this when host CIDATA `user-data` is non-empty **and** guest serial
has no `Cloud-init`. If `user-data` is 0 bytes, recreate with
`--custom-configuration` first (§C–D), then seed if USB still loses.

Worked on `ubuntu_24_04.n1-655-devkit`
(`22457359-76b4-4fd5-bdd1-2ed7415f8879`) 2026-09-08: after seed,
`enp3s0` `10.1.0.129`, `ssh -p 2222 ubuntu@<devkit-ip>`.

#### 1. Stop the HVM (QEMU must not hold the qcow2)

```bash
./scripts/restart_instance.sh ubuntu_24_04.n1-655-devkit --stop
```

Wait until `eve app list` says **HALTED** and `pgrep qemu-system` has
no line for that UUID. `Run State: Halted` on the controller can lag.
Do not nbd-mount a live qcow2.

#### 2. Connect nbd from the EVE debug shell

The debug shell is musl. `qemu-nbd` is the pillar binary and needs that
tree on `LD_LIBRARY_PATH`. Create `/var/lock` or qemu-nbd cannot bind
its socket.

```bash
mkdir -p /var/lock /run/lock
ROOT=/hostfs/containers/onboot/004-pillar-onboot/lower
export LD_LIBRARY_PATH="$ROOT/usr/lib:$ROOT/lib"
NBD="$ROOT/usr/bin/qemu-nbd"

# FileLocation from DomainStatus / xen2.cfg, not the app UUID.
QCOW=/persist/clear/volumes/<volume-uuid>#0.qcow2
"$NBD" --connect=/dev/nbd0 "$QCOW"
sleep 2
blkid /dev/nbd0p1    # LABEL="cloudimg-rootfs" TYPE="ext4"
mkdir -p /tmp/hvm-root
mount /dev/nbd0p1 /tmp/hvm-root
```

Noble ARM64 cloudimg layout: `p1` root, `p15` UEFI, `p16` BOOT. Mount
**p1**.

#### 3. Write seed, netplan, sshd, password

Put the §2 YAML on the debug host as `/tmp/hvm-fix-seed/user-data` (copy
from the CIDATA ISO if it is still 748 bytes, or decode
`apps/.custom-config.ubuntu_24_04.json`). Alongside it:

```text
/tmp/hvm-fix-seed/user-data          # §2 #cloud-config
/tmp/hvm-fix-seed/meta-data          # instance-id: nocloud-seed-1
/tmp/hvm-fix-seed/network-config     # netplan match en* dhcp4
/tmp/hvm-fix-seed/01-dhcp.yaml       # same netplan for /etc/netplan
/tmp/hvm-fix-seed/00-passwordauth.conf
/tmp/hvm-fix-seed/90-ubuntu          # sudoers
/tmp/hvm-fix-seed/99-nocloud.cfg     # datasource_list: [ NoCloud, None ]
```

`meta-data`:

```text
instance-id: nocloud-seed-1
local-hostname: ubuntu
```

`network-config` and `01-dhcp.yaml`:

```yaml
network:
  version: 2
  ethernets:
    allnics:
      match:
        name: en*
      dhcp4: true
```

```bash
MNT=/tmp/hvm-root
SRC=/tmp/hvm-fix-seed
mkdir -p "$MNT/var/lib/cloud/seed/nocloud" \
  "$MNT/etc/cloud/cloud.cfg.d" "$MNT/etc/netplan" \
  "$MNT/etc/ssh/sshd_config.d" "$MNT/etc/sudoers.d"
cp "$SRC/user-data" "$SRC/meta-data" "$SRC/network-config" \
  "$MNT/var/lib/cloud/seed/nocloud/"
cp "$SRC/99-nocloud.cfg" "$MNT/etc/cloud/cloud.cfg.d/"
cp "$SRC/01-dhcp.yaml" "$MNT/etc/netplan/"
chmod 600 "$MNT/etc/netplan/01-dhcp.yaml"
rm -f "$MNT/etc/netplan/50-cloud-init.yaml"
cp "$SRC/00-passwordauth.conf" "$MNT/etc/ssh/sshd_config.d/"
cp "$SRC/90-ubuntu" "$MNT/etc/sudoers.d/"
chmod 440 "$MNT/etc/sudoers.d/90-ubuntu"
rm -rf "$MNT/var/lib/cloud/instance" "$MNT/var/lib/cloud/instances" \
  "$MNT/var/lib/cloud/sem"
rm -f "$MNT/etc/cloud/cloud-init.disabled"
```

`00-passwordauth.conf` is `PasswordAuthentication yes` plus
`KbdInteractiveAuthentication yes`. `90-ubuntu` is
`ubuntu ALL=(ALL) NOPASSWD:ALL`. `99-nocloud.cfg` is
`datasource_list: [ NoCloud, None ]`.

Do **not** `chroot … chpasswd`: PAM fails on this mount
(`pam_chauthtok` / Authentication token manipulation error). Write the
sha512 hash from the YAML `users.passwd` field into `/etc/shadow`
without printing the file:

```bash
HASH=$(sed -n 's/.*passwd: "\(\$6\$[^"]*\)".*/\1/p' "$SRC/user-data")
# HASH must be non-empty (sha512 of ubuntu from the §2 template)
awk -F: -v h="$HASH" 'BEGIN{OFS=":"} $1=="ubuntu"{$2=h} {print}' \
  "$MNT/etc/shadow" > "$MNT/etc/shadow.new"
mv "$MNT/etc/shadow.new" "$MNT/etc/shadow"
chmod 640 "$MNT/etc/shadow"
```

If the YAML has no `passwd:` hash, generate one on the workstation
(`mkpasswd -m sha-512 ubuntu`) and put it in `users.passwd` / this
`HASH`. Confirm without dumping the hash:

```bash
awk -F: '$1=="ubuntu"{print ($2 ~ /^\$6\$/ ? "ubuntu_hash_ok" : "ubuntu_hash_bad")}' \
  "$MNT/etc/shadow"
```

Clearing `/var/lib/cloud/instance*` makes this look like first boot so
cloud-init applies the seed (packages, ssh_pwauth). The hash is so
console/SSH work even while `packages:` is still running.

#### 4. Detach and start

```bash
sync
umount /tmp/hvm-root
"$NBD" -d /dev/nbd0
```

On the workstation:

```bash
./scripts/restart_instance.sh ubuntu_24_04.n1-655-devkit --start
```

Wait until `eve app list` is **RUNNING** / controller `Online`. Guest
serial should show `cloud-init[…]`. Neighbor for the DNAT target
becomes **REACHABLE** (tap RX no longer 0). `ssh -p 2222` may be
**Connection refused** for a minute while sshd starts, then:

```bash
ssh ubuntu@<node-ip> -p 2222
```

`cloud-init status: running` is normal until `build-essential` /
headers finish. Password is `ubuntu` / `ubuntu`.

#### 5. Next node / next HVM

Same steps, new `QCOW` path and instance name. Skip if that guest’s
serial already has `Cloud-init` (USB race won, as on `n1-655-pro` first
boot). After delete+create, always check serial before injecting.
