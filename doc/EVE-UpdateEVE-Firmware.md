# Updating EVE Firmware / BaseOS on Ambarella Edge Nodes

This recipe documents the step-by-step process of building a custom EVE BaseOS firmware image on the host, locating the generated artifacts, registering the image in ZedControl, and performing an over-the-air (OTA) update on an active edge node (`n1-655-devkit` or `n1-655-pro`).

Related documents:
- [ZedControl-scripts.md](ZedControl-scripts.md) (catalog of `zcli` wrappers)
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (hardware models and adapter inventory)
- [Architecture.md](Architecture.md) (system architecture and transport design)

---

## 1. Overview: Dual-Partition Architecture & Edge App Preservation

EVE uses an **A/B dual-partitioning scheme** with priority boot support in GRUB for atomic, fail-safe updates:

1. **Standby Partition Write**: When an update is initiated, EVE's `baseosmgr` streams the new `rootfs.img` into the inactive (standby) rootfs slot (`IMGA` or `IMGB`). The running system partition is never modified in place.
2. **Reboot & Switch**: When triggered with `--activate`, GRUB updates boot priority to the newly written partition and the device reboots into the new kernel and system services.
3. **10-Minute Health Verification**: Upon booting the new image, EVE enters a 10-minute probationary testing window. If the node successfully connects to the controller (ZedControl) and all microservices start cleanly without crashing, the controller marks the new version as permanent/committed.
4. **Automatic Fallback**: If the new image fails to boot, crashes, or loses network connectivity, the watchdog / GRUB automatically falls back to the previous known-good partition.

### Are Existing Edge Apps and Data Preserved?

**Yes.** EVE strictly separates the BaseOS system partitions from application data:

| Partition | Mount Point | Contents | Touched During OTA Update? |
|---|---|---|---|
| **IMGA** (Rootfs A) | `/` (Active) | EVE Dom0 kernel, hypervisor, drivers, system microservices | **No** (Current running OS) |
| **IMGB** (Rootfs B) | *(Standby)* | Standby OS partition | **Yes** — new `rootfs.img` is written here |
| **P3 / PERSIST** | `/persist` | **All edge application rootfs layers, volume instances (`/persist/img`), container layers, snapshots, and persistent configs** | **NO** — completely untouched and preserved |

- **During Download**: Applications continue running without interruption while the new BaseOS is fetched.
- **During Reboot**: Application instances are gracefully stopped, the device restarts into the new kernel, and EVE automatically restarts all configured application instances.
- **After Boot**: All edge applications resume with their existing volumes, rootfs layers, and states intact.

> [!NOTE]
> Unlike an OTA update (`eveimage-update`), flashing a node from scratch using `live.raw` or the USB installer re-partitions the whole disk and **will** reformat `/persist` unless configured otherwise.

---

## 2. Prerequisites

1. **Host Build Tree**:
   The repositories are structured cleanly at the workspace root:
   - `Makefile`: Top-level build orchestration Makefile in `amba-virt/`.
   - `eve/`: LF-Edge EVE system source and tools.
   - `eve-kernel/`: Ambarella Linux kernel and device drivers.
   - `drivers/`: Out-of-tree Ambarella vendor drivers (`cavalry/`, `amba_otp/`, etc.).
   - `boot/`: Hardware bootloaders and ARM Trusted Firmware.

2. **ZedControl Access**:
   Ensure `$ZCLI_TOKEN` is exported in your environment for [`scripts/zcli`](../scripts/zcli):
   ```bash
   export ZCLI_TOKEN="<your-token>"
   ```

3. **Check Current Node Firmware**:
   Verify that the target edge node (`n1-655-devkit`) is `Online`:
   ```bash
   ./scripts/zcli -- edge-node show
   ```
   Note the current version reported under the `Active Image` column (for example, `0.0.0-master-55808d64-k-arm64-v6.1.112-generic-bd12ffefde1e-xlzhanga-dirty-gcc-kvm-arm64`).

---

## 3. Building EVE on the Host

Build the EVE image from the repository root:

```bash
make eve
```

### What this does:
1. Builds the Ambarella kernel with GCC (`make -C eve-kernel -f Makefile.eve kernel-gcc`).
2. Invokes LinuxKit and builder Docker containers to compile the EVE microservices and assemble packages for `ZARCH=arm64 HV=kvm`.
3. Even though Docker is used internally for toolchains, the output files are mounted directly onto the host filesystem.

---

## 4. Locating Build Artifacts

All output files are written to the host distribution directory:

```
eve/dist/arm64/current/
```
*(This is a symlink pointing to `eve/dist/arm64/<version-tag>`)*

### Key Files:

| File Path | Description | Typical Size |
|---|---|---|
| `installer/rootfs.img` | **The BaseOS OTA Payload**. SquashFS/ext4 root filesystem containing the dom0 kernel, hypervisor (KVM), device drivers, and EVE pillar microservices. This is the only file required for OTA firmware updates. | ~262 MB |
| `installer/eve_version` | Text file containing the exact EVE version string (e.g., `0.0.0-HEAD-2f545f1e-k-arm64-v6.1.112-generic-bd12ffefde1e-custom-gcc-kvm-arm64`). | 80 B |
| `live.raw` | Full bootable raw disk image. Used only when flashing a board from scratch (eMMC/NVMe). | ~591 MB |
| `live.qcow2` | QEMU/KVM virtual disk image. | ~265 MB |

Capture the version string for subsequent steps:
```bash
EVE_VER=$(cat eve/dist/arm64/current/installer/eve_version)
echo "Built EVE version: $EVE_VER"
```

---

## 5. Staging & Registering with `pub_eve_datastore.sh` (Recommended)

To automate staging the `rootfs.img` into a versioned subdirectory, computing checksums, and creating/uplinking the image in ZedControl, use [`scripts/pub_eve_datastore.sh`](../scripts/pub_eve_datastore.sh):

```bash
# Stage to destination and register in ZedControl:
./scripts/pub_eve_datastore.sh ~/public_html/eve-images/

# Optional: preview actions without executing:
./scripts/pub_eve_datastore.sh ~/public_html/eve-images/ --dry-run
```

The script automatically:
1. Detects the latest build artifacts and reads `eve_version`.
2. Creates the versioned subdirectory `~/public_html/eve-images/<EVE_VER>/`.
3. Copies `rootfs.img` into it and verifies the SHA-256 checksum.
4. Detects the active local HTTP datastore from ZedControl (or uses `--datastore=NAME`).
5. Calls `zcli image create` and `zcli image uplink` with exact byte size and checksum.
6. Prints the exact command to trigger the update on your edge node.

---

## 6. Manual Staging and Registration (Reference)

If performing the steps manually without the helper script:

1. **Copy `rootfs.img` to a Versioned Staging Directory**:
   ```bash
   EVE_VER=$(cat eve/dist/arm64/current/installer/eve_version)
   mkdir -p /path/to/public_html/eve-images/"$EVE_VER"
   cp eve/dist/arm64/current/installer/rootfs.img /path/to/public_html/eve-images/"$EVE_VER"/rootfs.img
   ```

2. **Compute Checksum and File Size**:
   ```bash
   IMAGE_FILE="eve/dist/arm64/current/installer/rootfs.img"
   IMAGE_SHA=$(sha256sum "$IMAGE_FILE" | awk '{print $1}')
   IMAGE_SIZE=$(stat -c %s "$IMAGE_FILE")
   ```

3. **Register and Uplink in ZedControl**:
   ```bash
   IMAGE_NAME="${EVE_VER}"
   IMAGE_URL="eve-images/${EVE_VER}/rootfs.img"

   ./scripts/zcli -- image create "$IMAGE_NAME" \
       --title="EVE $EVE_VER" \
       --type=Eve \
       --image-format=raw \
       --arch=ARM64 \
       --datastore-name=LocalHTTP \
       --image-url="$IMAGE_URL"

   ./scripts/zcli -- image uplink "$IMAGE_NAME" \
       --datastore-name=LocalHTTP \
       --image-url="$IMAGE_URL" \
       --image-sha="$IMAGE_SHA" \
       --image-size="$IMAGE_SIZE"

   ./scripts/zcli -- image show "$IMAGE_NAME"
   ```

---

## 7. Updating the Edge Node (`n1-655-devkit`)

```bash
EVE_VER=$(cat eve/dist/arm64/current/installer/eve_version)

# 1. Publish the new image to the node's candidate BaseOS config:
./scripts/zcli -- edge-node eveimage-update n1-655-devkit --image="${EVE_VER}"

# 2. Activate the update (triggers partition switch and device reboot):
./scripts/zcli -- edge-node eveimage-update n1-655-devkit --image="${EVE_VER}" --activate
```

> [!IMPORTANT]
> - **Image Name Matching**: In ZedControl, the image name **must exactly match** the internal version string (`$EVE_VER` from `eve_version` without an `eve-` prefix). During installation, EVE's `baseosmgr` verifies that the configured image name strictly equals the installed version (`shortVer`). If prefixed (e.g. `eve-0.0.0-...`), the node will fail with `checkInstalledVersion: image name not match. config eve-..., image ver ...`.
> - **Image Type**: In ZedControl, BaseOS firmware images **must** be created with `--type=Eve` (not `EvePrivate`), otherwise the controller will reject the assignment with `Image ... is not a base image`.
> - **Two-Step Publish and Activate**: Running `eveimage-update` with `--activate` directly on an image that is not yet associated with the node fails with `Conflict: Image is not present`. You must first publish the image (without `--activate`) to stage it into `Configured Next Eve Image`, and then run with `--activate` to apply it.

---

## 8. Monitoring and Verification

1. **Monitor Node State**:
   Watch the node transition through the update phases:
   ```bash
   ./scripts/zcli -- edge-node show n1-655-devkit --detail
   ```

   The node lifecycle during update:
   - **`Online`**: Controller sends update request (`BaseOsConfig`).
   - **`BaseOSUpdating`**: Node downloads `rootfs.img` from the datastore and writes to the standby partition. Running edge apps are unaffected.
   - **`Rebooting`**: Edge apps gracefully stop. GRUB switches active partition and restarts the SoC.
   - **`Online` (Testing)**: Node boots the new kernel, starts microservices, and restarts all edge apps under the 10-minute test window.
   - **Committed**: Controller confirms healthy connection and commits the new partition as permanent.

2. **Verify the Active Firmware Version**:
   Check the summary table:
   ```bash
   ./scripts/zcli -- edge-node show
   ```
   Or query attestation JSON details directly:
   ```bash
   ./scripts/zcli -- --format=json edge-node show n1-655-devkit --detail | jq '.attestation_details'
   ```
   Verify that `eveVersion` matches your newly built version string:
   ```json
   {
     "eveVersion": "0.0.0-HEAD-2f545f1e-k-arm64-v6.1.112-generic-bd12ffefde1e-custom-gcc-kvm-arm64",
     "firmwareVersion": "U-Boot-2020.10-rc2-00475-g2335072d46-dirty-06/24/2026"
   }
   ```

3. **Verify Edge App Instances**:
   Confirm that existing edge applications restarted successfully:
   ```bash
   ./scripts/show_instances.sh --edge-node=n1-655-devkit
   ```

## 6. Troubleshooting & Recovery from Upgrade Deadlocks

### Deadlock Symptom: Node Stuck in `Baseos_updating` and Apps in `Suspect`

If an edge node stays indefinitely in `Run State: Baseos_updating` while all edge applications report `Run State: Suspect`:
1. **Do NOT delete or reinstall the edge applications**: Their virtual disks and container layers on `/persist/clear/volumes/` remain intact and healthy. Reinstalling will not start them because the node's config parser is paused.
2. **Check the Node's Local State**:
   ```bash
   ssh <node-ip> "cat /run/baseosmgr/BaseOsStatus/*.json"
   ```
   If the output shows `"TooEarly": true`, `baseosmgr` has deferred installing the update.

### Why This Happens

1. **`zedagent` Configuration Suppression**: Whenever the configured target image on ZedControl differs from the running image version (`status.ShortVersion != cfg.BaseOsVersion`) and has `Activate: true`, `zedagent` enters an update-pending state. In this state, it intentionally bypasses `parseAppInstanceConfig()`, so `zedmanager` never receives instructions to launch the applications.
2. **Standby Partition Stalemate (`TooEarly: true`)**: If an abnormal reboot, crash, or manual power event interrupted the previous update transition, both rootfs partitions (`IMGA` and `IMGB`) may be marked `active` in `zboot`. When `baseosmgr` detects that the standby partition is marked `active`, it assumes testing is still underway on a fallback partition and refuses to overwrite it, flagging `TooEarly = true` and deferring indefinitely.
3. **Ambarella Hardware Warm-Reboot Handling**: In older firmware, the Ambarella N1-655 SoC did not cycle PMIC rails during software warm reboot (`ambarella,reboot` halted at the kernel restart notifier). **This has been fixed in recent U-Boot firmware.** On nodes running fixed firmware, warm reset completes cleanly and the bootloader prints `BootFrom:PAHTA` on the serial console within 10 seconds of reboot. If `BootFrom:PAHTA` does not appear within 10 seconds, the board is stuck in an older halting state and requires an MCU power cycle.

### Resolution Procedures

#### Method A: Zero-Reboot Recovery (Align Controller to Running Image)
If the node is running an operational image and you want to restore the edge applications immediately without downtime or reboots:
```bash
# 1. Check the image version currently running on the node
ssh <node-ip> "cat /run/eve-release"

# 2. Update ZedControl to match the running image
./scripts/zcli -- edge-node eveimage-update <node-name> \
  --image=<running-eve-image-name> \
  --activate -f
```
As soon as the controller target version matches `status.ShortVersion`, `zedagent` unblocks application config parsing, `zedmanager` mounts the existing volumes, and both applications transition from `Suspect` to `Online`.

#### Method B: Reset Standby Partition State to Complete Upgrade
If you wish to proceed with the pending update:
1. Inspect partition states on the node:
   ```bash
   ssh <node-ip> "eve exec pillar /usr/bin/zboot partstate IMGA -d; eve exec pillar /usr/bin/zboot partstate IMGB -d"
   ```
2. Reset the standby partition (e.g. `IMGB`) to `unused`:
   ```bash
   ssh <node-ip> "eve exec pillar /usr/bin/zboot set_partstate IMGB unused"
   ```
3. Restart `baseosmgr` so it immediately claims the partition:
   ```bash
   ssh <node-ip> "pkill -f baseosmgr"
   ```
4. Once `baseosmgr` completes downloading and writing the image, EVE will trigger a reboot.
5. **Monitor Serial Console for `BootFrom:PAHTA`**:
   - Monitor the SoC serial console during reboot:
     - `n1-655-pro`: `ttyCH9344USB0` (screen session `ttyCH9344USB00`)
     - `n1-655-devkit`: `ttyCH9344USB8` (screen session `ttyCH9344USB08`)
   - If the node has the fixed U-Boot firmware, `BootFrom:PAHTA` will appear within 10 seconds.
   - If `BootFrom:PAHTA` does not appear within 10 seconds, the board is stuck and requires an MCU power cycle:
     - `n1-655-pro`: `ttyCH9344USB3` (screen session `ttyCH9344USB03`)
     - `n1-655-devkit`: `ttyCH9344USB11` (screen session `ttyCH9344USB11`)
     ```text
     pwr off -y
     pwr on
     ```
     Wait **5+ minutes** for hardware memory training, early boot, and EVE pillar startup.

### Avoiding `domainmgr` Fatal Panic During Model Updates

When updating physical IO adapters in hardware models (`scripts/push_models.sh`):
> [!CAUTION]
> **Never modify or reorder hardware model IO adapters while applications are active.**  
> If an adapter (`iav`, `gpio0`, `amba_virt`) is modified in the model while assigned to an active container or VM, `domainmgr.releaseAdapters()` will fail to locate the bundle in `AssignableAdapters` and execute `log.Fatalf()`, crashing the node into a halted reboot state.  
> Always stop or undeploy direct-attached applications before updating hardware models on ZedControl.
