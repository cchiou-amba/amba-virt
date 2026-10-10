# Ambarella Out-of-Tree Drivers & Firmware in the EVE Image

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

This document describes how the Ambarella out-of-tree kernel modules
(`ambcma.ko`, `cavalry.ko`, `amba_virt.ko`, and the camera modules), their
firmware, and `amba-virt-server` reach EVE Dom0 on the N1-655 boards
(`n1-655-devkit` and `n1-655-pro`): as the `eve/pkg/amba-virt` layer of the
signed EVE root filesystem. Nothing amba-virt is staged in `/persist`.

Related documents:
- [AmbaVirtServer.md](AmbaVirtServer.md) (server process tree, boot order, readiness)
- [EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md) (BaseOS OTA upgrade architecture and `/persist` preservation)
- [EVE-OutOfTree-KMODs.md](EVE-OutOfTree-KMODs.md) (compile modes and module signing)
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (Hardware models and adapter inventory)
- [Architecture.md](Architecture.md) (System architecture and transport design)

> [!IMPORTANT]
> **Build status (2026-10-10): only development mode is qualified.**
> Run `make set-mode-development`, verify `make mode` reports
> `DEVELOPMENT` and `kernel-gcc`, then run `make all`. This is the path that
> produced the images qualified on the N1-655 boards: the kernel embeds the
> persistent development certificate, host `make drivers` signs modules with
> the matching `build/certs/signing_key.pem`, and `make amba-virt-image`
> packs them into the image layer.
>
> The production-mode path (`make set-mode-production`, `kernel-ambarella`,
> `eve-kernel/Dockerfile.ambarella`) is a design target, not a qualified
> recipe. It currently fails from a clean baseline. Do not use it to build,
> qualify, or release an image.

---

## 1. The `eve/pkg/amba-virt` Image Layer

EVE Dom0 runs from an immutable SquashFS root filesystem (`IMGA` / `IMGB`).
The amba-virt payload is a LinuxKit `init` layer of that filesystem, so it is
signed, measured, and replaced atomically with the rest of EVE by the A/B
update.

`make amba-virt-image` regenerates `eve/pkg/amba-virt/rootfs/` (gitignored)
and builds the layer:

```text
/etc/init.d/021-amba-virt-server -> /usr/bin/amba-virt-server
/etc/amba-virt/modules.conf          ambcma.ko (with parameters), cavalry.ko, amba_virt.ko
/etc/amba-virt/early-modules.conf    camera modules loaded before camera power
/etc/amba-virt/late-modules.conf     camera modules loaded after camera power
/etc/amba-virt/camera.conf           camera pipeline configuration (pipeline on)
/lib/modules/<release>/extra/*.ko    the signed modules those lists name
/lib/firmware/cavalry.bin            Cavalry VisORC microcode
/usr/bin/amba-virt-server, amba-virt-ctl, amba-virt-aaa, dsp_monitor_service
/usr/lib/amba-virt/lib/              private glibc and camera libraries
/usr/lib/amba-virt/firmware/         DSP microcode for the camera pipeline
/usr/share/ambarella/                Lua scripts and IDSP assets
/usr/share/amba-virt/PREBUILTS       origin and checksum of each prebuilt file
```

The layer is tagged from a digest of the staged rootfs and reaches the EVE
`rootfs.yml` as `AMBAVIRT_TAG` when `make eve` runs.

## 2. Boot Order and Module Loading

1. LinuxKit init runs `/etc/init.d/021-amba-virt-server` before the EVE
   system services (pillar, `domainmgr`) start.
2. The server loads `modules.conf` in order with `finit_module`, from
   `/lib/modules/$(uname -r)/extra/`. A module already loaded with the same
   parameters is accepted; one loaded with different parameters fails the
   boot.
3. With `camera.conf` present, the camera pipeline loads
   `early-modules.conf`, powers the camera, loads `late-modules.conf`, and
   points the kernel firmware search path
   (`/sys/module/firmware_class/parameters/path`) at
   `/usr/lib/amba-virt/firmware`. `cavalry.ko` finds `cavalry.bin` in
   `/lib/firmware`.
4. The server reports ready only after `/dev/amba_virt_shm*` exists, so
   every Ambarella node is present before EVE starts any guest. No device is
   assigned to an app.

`/etc/init.d/000-mod-params` no longer loads `amba_virt.ko`, and the
`storage-init` hook and `/persist/bin/load-ambarella-drivers.sh` are gone.

## 2.1 What `/persist` Holds

| Location | Role |
|---|---|
| `/persist/etc/amba-virt/policies.json` | ACL policy file of `amba-virt-server` (read fallback `/etc/amba-virt/policies.json` in the image) |
| `/persist/amba-virt-dev/` | Optional, manual development tree; never read by the production boot ([AmbaVirtServer.md](AmbaVirtServer.md) §14.3) |
| `/persist/etc/`, `/persist/status/`, and the rest | EVE state, untouched |

---

## 3. Platform Device Tree & Memory Carveout Requirements

### 3.1 Mandatory Reserved Memory Carveouts
Both `ambcma.ko` and `cavalry.ko` enforce strict hardware memory carveout checks during module initialization. If the active device tree lacks these nodes, `ambcma.ko` immediately aborts with `-ENODEV` (`#iav_error# ambarella_get_cma_pool_info: /reserved-memory/cavalry@0 not found`), preventing `/dev/iav` and `/dev/cavalry` from being created.

U-Boot detects the DRAM size from the controller and LPDDR5 MR8, then writes `/memory`, `/chosen/sys-dram-size`, and `cavalry@0` before GRUB starts. On the N1-655 devkit and pro boards that size is 32 GiB. The checked-in DTB carries the same 32 GiB default. GRUB must forward U-Boot's tree. A `set_global devicetree` assignment replaces it.

The ipcam-mini reserved-memory contract is:

| Memory / Carveout Node | Physical Address Range | Size | Allocation Policy | Functional Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `/memory` | `0x0000000000`–`0x07ffffffff` | 32 GiB | `device_type = "memory"` | Detected DRAM (`reg = <0x0 0x0 0x8 0x0>`) |
| `/chosen/sys-dram-size` | `0x00000008 0x00000000` | 32 GiB | `u64` property | Size `ambcma.ko` validates |
| `/reserved-memory/virtio_reserved@2000000` | `0x0002000000`–`0x00021fffff` | 2 MiB | `no-map;` | VirtIO device configuration window |
| `/reserved-memory/cavalry@2` | `0x1fc00000`–`0x1fffffff` | 4 MiB | `no-map;` | Cavalry VisORC ucode staging buffer |
| `/reserved-memory/cavalry@1` | none | 0 | `no-map;` | Node and phandle kept; this profile has no shared-pool `reg` |
| `/reserved-memory/disp@0` | `0x20000000`–`0x27ffffff` | 128 MiB | `no-map;` | Display buffer |
| `/reserved-memory/iav@1` | `0x28000000`–`0x3fffffff` | 384 MiB | `no-map;` | `IDSP_SHARED` |
| `/reserved-memory/iav@0` | `0x40000000`–`0xffffffff` | 3072 MiB | `no-map;` | `IDSP_PRIVATE`, contiguous with `iav@1` and ending at 4 GiB |
| `/reserved-memory/linux,cma` | Linux high, above `0x400000000` | 512 MiB | `reusable;` | Kernel CMA. Linux low is smaller than this pool |
| `/reserved-memory/cavalry@0` | `0x100000000`–`0x3ffffffff` | 12 GiB | `no-map;` | Cavalry private pool. Linux RAM continues from `0x400000000` to the detected DRAM top |

`iav@1` and `iav@0` form one 3456 MiB window, `0x28000000`–`0x100000000`. `disp@0` and `cavalry@2` sit directly below it. `cavalry@0` starts on the 4 GiB line, so it does not overlap that 32-bit window. The Dom0 server loaded `ambcma`, `dsp`, `iav`, and `cavalry` against this map in the 2026-10-10 cold-boot gates on both boards, with camera capture and Cavalry inference passing.

In addition, the device tree declares the `sub_scheduler0` platform device for Cavalry, `hwtimer` for IAV hardware timing, `vinbrg0..3` for SerDes bridge I2C buses, and specifies `enable-method = "spin-table"` under `/cpus/cpu@*` for SMP CPU activation.

### 3.2 The 32-Bit DSP Microcode Boundary & Address Alignment

The Ambarella DSP microcode engines (`orccode.bin`, `orcidsp0.bin`, `orcidsp1.bin`, `orcvin0.bin`, `orcvin1.bin`) execute strictly within an internal **32-bit physical address space** (`< 0x100000000` / 4 GiB). All pointer arithmetic, partition tables, and bounds checks in microcode (e.g. `dsp_mempar.c`, `orcvin_boot.c`) require that the IDSP shared and DSP private regions reside in a single contiguous top-memory window.

This establishes a critical architectural rule: **All memory managed by `ambcma.ko` for DSP buffers must reside strictly below the 4 GiB physical address boundary and align contiguously with `iav@1`.**

On the ipcam-mini map:
1. `/reserved-memory/iav@1` starts at `0x28000000` with size `0x18000000` (384 MiB).
2. `/reserved-memory/iav@0` starts at `0x40000000` with size `0xc0000000` (3072 MiB) and ends at `0x100000000`.
3. `dsp_buf_size` has to fit inside that `iav@0` node and remain below 4 GiB. The old 1664 MiB value belonged to a different map. The image loads `ambcma.ko` with `dsp_buf_size=0x40000000` (1 GiB), the value the 2026-10-10 board gates ran with.
4. `cavalry@0` (12 GiB) starts at `0x100000000`. Linux RAM continues above it, from `0x400000000` to the detected top. The EFI stub may place kernel code in that Linux-high range. That placement is outside the DSP window.

### 3.3 U-Boot SMP Relocation Top (`/u-boot_cfg/reloc-top`)

On 32 GiB Ambarella N1-655 platforms, U-Boot dynamically relocates itself near the top of usable physical RAM. In `arch/arm/mach-ambarella/common.c`, `board_get_usable_ram_top()` queries the control DTB for `/u-boot_cfg/reloc-top`.

1. **Failure Mode (Missing `reloc-top`)**: If `/u-boot_cfg` is absent from the control DTB, U-Boot relocates to the top of 32 GiB (`0x7fff3db90`). The spin-table trampoline (`secondary_cortex_jump`) is placed at this 35-bit physical address. When Linux attempts to release secondary CPUs (`CPU1..3`), the PC-relative instructions (`adr`) fail to reach the high-memory spin table. The secondary cores never wake, Linux reports `CPU1..3: failed to come online` at 5-second intervals, and the board resets via hardware watchdog after 15 seconds.
2. **Resolution**: The control DTB sets `reloc-top` to `0x13000000`, below `cavalry@2`. `fdt_update_cpux()` writes `cpu-release-addr` to the runtime address of `secondary_cortex_jump`. The old `0x80000000` top falls inside `iav@0` of this map.

### 3.4 Partition 4 (`CONFIG`) Device Tree Deployment & GRUB Override

Do not set `devicetree` in `/config/grub.cfg` or the CONFIG-partition `grub.cfg`. That assignment makes GRUB replace the tree U-Boot patched, including the detected DRAM size and the runtime spin-table addresses.

On a booted Dom0 shell, the tree matches the contract when:

```bash
nproc --all
od -An -tx1 /proc/device-tree/memory/reg
od -An -tx1 /proc/device-tree/chosen/sys-dram-size
od -An -tx1 /proc/device-tree/reserved-memory/iav@0/reg
od -An -tx1 /proc/device-tree/reserved-memory/iav@1/reg
od -An -tx1 /proc/device-tree/reserved-memory/cavalry@0/reg
od -An -tx1 /proc/device-tree/reserved-memory/cavalry@2/reg
test ! -e /proc/device-tree/reserved-memory/cavalry@1/reg && echo cavalry@1-has-no-reg
test ! -e /proc/device-tree/reserved-memory/dma32_reserved && echo dma32-absent
grep -E 'Kernel code|1fc00000-3ffffffff' /proc/iomem
```

`nproc --all` prints `4`. `/memory` and `sys-dram-size` are 32 GiB on these boards. Kernel code is outside `0x1fc00000`–`0x400000000`. Placement in Linux high is accepted.

---

## 4. Hardware Initialization Sequence

When the server loads `cavalry.ko`:
1. It matches the `sub_scheduler0` platform device declared in `arch/arm64/boot/dts/ambarella/n1_655.dts`.
2. It parses and claims the reserved memory carveouts established at boot:
   - `cavalry_reserved` (User/CVMEM pool: `0x100000000 - 0x3ffffffff`, 12 GB)
   - `cavalry_ucode` (Microcode staging: `0x1fc00000`–`0x1fffffff`, 4 MiB)
3. It loads `cavalry.bin` into ucode memory through the firmware loader.
4. It initializes interrupts (IRQ 41/42) and VP clocks, starting the Vector Processor ucode.
5. It creates device nodes `/dev/cavalry` and `/dev/cavalry_profile`.

When the server loads `amba_virt.ko`, it creates `/dev/amba_virt` (the host
end of the HVM transport) and the `/dev/amba_virt_shm*` backing nodes.

Expected kernel output highlights:
```text
cavalry_show_version: Cavalry Linux Driver Version: 3.0.x
cavalry_parse_reserved_mem: CVMEM(1:ucode) RANGE: [0x1fc00000 - 0x1fffffff]
cavalry_parse_reserved_mem: CVMEM(0:user)  RANGE: [0x100000000 - 0x3ffffffff]
show_ucode_version: Cavalry Ucode Version = N1_655-Ver.206-...
visorc_start: Cavalry Ucode is started.
```

On the node, as root:
```bash
grep 'is ready' /run/amba-virt/server-daemon.log
ls -l /dev/cavalry /dev/amba_virt /dev/amba_virt_shm*
dmesg | grep -E 'cavalry|amba_virt' | tail -n 25
```

---

## 5. Development & Deployment Workflow

```bash
make set-mode-development && make mode   # must report development / kernel-gcc
make all                                 # kernel, drivers, amba-virt-image, eve
./scripts/pub_eve_datastore.sh <datastore-dir>
```

Then update the node over the air ([EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md)).
Driver, firmware, and server changes ship only this way. For a quick manual
experiment, see the `/persist/amba-virt-dev/` loop in
[AmbaVirtServer.md](AmbaVirtServer.md) §14.3; it does not survive as product.

---

## 6. BaseOS Upgrade & Lifecycle Considerations

### Kernel ABI Compatibility
- Module `vermagic` must match the kernel release, compiler, and `LOCALVERSION`
  of the image kernel. `make amba-virt-image` installs the modules under the
  release read from `amba_virt.ko`'s own `vermagic`.
- Modules and kernel always change together because both are in the same
  image; there is no stale module tree on `/persist` to fall out of step.

### Board Reboot and Power Cycles
Software warm reboot has been fixed in updated U-Boot firmware. When performing an OTA update or issuing a reboot, monitor the SoC serial console:
- If `BootFrom:PAHTA` appears within **10 seconds**, the warm reboot succeeded and U-Boot/GRUB will boot automatically.
- If `BootFrom:PAHTA` does not appear within 10 seconds, the board is stuck and requires an MCU power cycle:
```bash
# Via MCP tool:
embdevenv_mcu_power(target="<target>", action="reboot")
```
The server starts by itself on every boot; no manual load step follows.

---

## 7. Retired Workflows [Historical]

> [!NOTE]
> Early bringup used `/persist/bin/load-ambarella-drivers.sh` invoked from
> `storage-init`, then `scripts/deploy_and_insmod.sh`. A later design ran
> `amba-virt-server` in a NOHYPER container and loaded modules through a Dom0
> `amba-virt-backend` over TCP port 5556, with modules and firmware stored in
> `/persist/modules` and `/persist/firmware`. All of these are retired and
> deleted.

---

## 9. Build Workflow Status

EVE-OS enforces kernel module signature verification
(`CONFIG_MODULE_SIG_FORCE=y`). The Makefile exposes two modes, but only
development mode has produced the images and signed modules qualified on the
N1-655 boards.

### 9.1 Mode Comparison

| Property | Development Mode (Qualified) | Production Mode (Unqualified) |
|---|---|---|
| **Status** | The only supported build path | Design target; do not use |
| **Kernel Target** | `kernel-gcc` | `kernel-ambarella` |
| **Driver Location** | `/lib/modules/<ver>/extra/` in the `eve/pkg/amba-virt` layer of `rootfs.img` | `/lib/modules/<ver>/extra/` (baked in `rootfs.img`) |
| **Driver Build** | Host filesystem via `make drivers` | Inside Docker container via `Dockerfile.ambarella` |
| **Signing Key** | Local persistent key (`eve-kernel/certs/signing_key.pem`) | Ephemeral key generated dynamically inside Docker |
| **Signing Action** | Host script runs `scripts/sign-file sha256 <key> <cert> <.ko>` | Kernel `modules_install` automatically signs inside Docker |
| **Private Key Lifetime** | Persisted on host (`.gitignore`d) for repeated signing | Destroyed immediately when Docker container exits |
| **Evidence** | Produced the current `*-gcc-kvm-arm64` images and matching signed modules | No qualified image |

### 9.2 Developer Guide: Querying & Switching Between Modes

The active compile mode is persisted in the repository root `.mode` file and managed directly via the top-level `Makefile`.

#### Querying Current Mode

Run `make mode` at any time to inspect the active configuration:

```bash
make mode
```

This displays whether the build is configured for `development` or `production`, the target kernel Docker tag, driver storage locations, signing key types, and iteration commands.

#### Qualified Development Build

1. **Select development mode and verify it**:
   ```bash
   make set-mode-development
   make mode
   ```
   Stop unless the output says `EVE Compile Mode: development` and
   `Kernel: kernel-gcc`.

2. **Build Development Kernel & BaseOS**:
   ```bash
   make all
   ```
   Compiles `kernel-gcc`, extracts headers and signing keys to `build/`, compiles out-of-tree drivers, stages the `eve/pkg/amba-virt` layer (`make amba-virt-image`), and builds the development BaseOS installer.

3. **Verify identity before publication**:
   - `eve/dist/arm64/current/installer/eve_version` ends in
     `-gcc-kvm-arm64`.
   - Every module to be loaded reports the expected kernel vermagic.
   - Every module signer matches the certificate embedded in that kernel.

#### Production Mode: Do Not Use

`make set-mode-production` changes the top-level `make eve` dependency from
`kernel-gcc` to `kernel-ambarella`; it does not merely change packaging.
`Dockerfile.ambarella` is currently unqualified and does not build from its
committed baseline. No release, qualification, or troubleshooting procedure
may switch to this mode until a separate plan builds it cleanly and qualifies
the resulting image on hardware.


