# Ambarella Out-of-Tree Drivers & Firmware Management in EVE-OS

This document details the architecture and operational workflow for managing Ambarella proprietary kernel modules (`cavalry.ko`, `amba_virt.ko`) and hardware microcode (`cavalry.bin`) using the persistent storage (`/persist`) layout on EVE edge nodes (`n1-655-devkit` and `n1-655-pro`).

Related documents:
- [EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md) (BaseOS OTA upgrade architecture and `/persist` preservation)
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (Hardware models and adapter inventory)
- [Architecture.md](Architecture.md) (System architecture and transport design)

---

## 1. Motivation & Architecture Decoupling

### The Immutable Rootfs Constraint
EVE-OS Dom0 utilizes an immutable, read-only SquashFS root filesystem (`IMGA` / `IMGB`). System directories such as `/lib/modules` and `/lib/firmware` are strictly read-only at runtime.

Historically, vendor drivers were built into the EVE kernel image by embedding build directives inside `eve-kernel/Dockerfile.gcc` and pulling source repositories via `.repo` manifests. While functional, this tightly coupled model presents several drawbacks:
1. **Upstream LF-Edge EVE Integration**: The upstream `lf-edge/eve-kernel` repository cannot depend on internal or proprietary out-of-tree repositories (`cavalry_drv`, `firmware_cavalry`, `amba-virt`). Building `Dockerfile.gcc` in an upstream environment would fail without access to internal Ambarella IP.
2. **Development Turnaround**: Modifying, debugging, or updating a kernel driver required rebuilding the entire EVE BaseOS image, re-uploading to the controller/datastore, and performing an OTA update cycle followed by a 5+ minute node reboot.
3. **Repository Cleanliness**: Embedding proprietary drivers directly into `eve-kernel` creates untracked directories (`ambarella/`) that complicate git hygiene.

### The `/persist` Solution
EVE maintains a dedicated read-write ext4 partition mounted at `/persist` that:
- Is **completely preserved** across BaseOS updates, A/B switches, and node reboots.
- Provides read-write space accessible via the Dom0 SSH debug container.
- Can host out-of-tree kernel modules and microcode binaries independently of the BaseOS rootfs.

```
/ (SquashFS - Read-Only)
├── lib/
│   ├── modules/<kernel-ver>/   (In-tree kernel modules only)
│   └── firmware/               (Standard platform firmware)
│
/persist (ext4 - Read-Write, Persistent across OTA & Reboot)
├── modules/                    (Out-of-tree .ko drivers)
│   ├── amba_virt.ko
│   ├── amba_pci_platform.ko
│   ├── dsplog.ko               (optional, built when ambvideo present)
│   └── cavalry.ko              (optional proprietary module)
├── firmware/                   (Hardware microcode binaries)
│   └── cavalry.bin             (optional proprietary microcode)
└── bin/                        (Runtime loader scripts)
    └── load-ambarella-drivers.sh
```

---

## 2. Directory Layout & Roles

| Location | Role | Typical Files | Access Mode |
|---|---|---|---|
| `/persist/modules/` | Houses out-of-tree kernel modules compiled for the active kernel ABI | `amba_virt.ko`, `amba_pci_platform.ko`, `dsplog.ko`, plus optional proprietary modules (`cavalry.ko`, `iav.ko`, `dsp.ko`, `pvrsrvkm.ko`, `amba_otp.ko`) | Read / Write |
| `/persist/firmware/` | Houses proprietary hardware microcode and DSP firmware | `cavalry.bin`, `orccode.bin`, etc. | Read / Write |
| `/persist/status/` | EVE system state, microservice checkpoints, app configs | Managed by EVE | Read / Write |

---

## 3. Platform Device Tree & Memory Carveout Requirements

### 3.1 Mandatory Reserved Memory Carveouts
Both `ambcma.ko` and `cavalry.ko` enforce strict hardware memory carveout checks during module initialization. If the active device tree lacks these nodes, `ambcma.ko` immediately aborts with `-ENODEV` (`#iav_error# ambarella_get_cma_pool_info: /reserved-memory/cavalry@0 not found`), preventing `/dev/iav` and `/dev/cavalry` from being created.

The verified EVE platform device tree must declare the following reserved memory nodes under `/reserved-memory`:

| Node | Physical Address Range | Size | Allocation Policy | Purpose |
|---|---|---|---|---|
| `/reserved-memory/iav@0` | `0x00a3000000`–`0x00ffffffff` | 1488 MiB | `no-map;` | Primary IAV & DSP private buffer window (`dsp_buf_size=0x40000000` allocates 1024 MiB) |
| `/reserved-memory/iav@1` | `0x0028000000`–`0x003fffffff` | 384 MiB | `no-map;` | IAV shared memory pool (pyramid layers, stats, overlays) |
| `/reserved-memory/cavalry@0` | `0x0100000000`–`0x03ffffffff` | 12 GiB | `no-map;` | Cavalry User / CVMEM execution pool (`virt_user_window_mb=2048`) |
| `/reserved-memory/cavalry@1` | `0x0026000000`–`0x0027ffffff` | 32 MiB | `no-map;` | Cavalry shared DMA pool |
| `/reserved-memory/cavalry@2` | `0x0025c00000`–`0x0025ffffff` | 4 MiB | `no-map;` | Cavalry ucode staging buffer |

In addition, the device tree must declare the `sub_scheduler0` platform device for Cavalry, `hwtimer` for IAV hardware timing, `vinbrg0..3` for SerDes bridge I2C buses, and specify `enable-method = "spin-table"` under `/cpus/cpu@*` for SMP CPU activation.

### 3.2 The 32-Bit DSP Microcode Boundary & Address Wraparound

The Ambarella DSP microcode engines (`orccode.bin`, `orcidsp0.bin`, `orcidsp1.bin`) execute within an internal **32-bit physical address space** (`< 0x100000000` / 4 GiB). All pointer arithmetic, partition tables, and bounds checks in microcode (e.g. `dsp_mempar.c`) utilize 32-bit unsigned registers.

This establishes a critical architectural rule: **All memory managed by `ambcma.ko` for DSP buffers must reside strictly below the 4 GiB physical address boundary.**

#### The 32-Bit Overflow Failure Mode
If `/reserved-memory/iav@0` is placed too high (for example, starting at `0xbd000000` as in stock Cooper U-Boot):
1. When `ambcma.ko` initializes with `dsp_buf_size=0x30000000` (768 MiB) or `0x40000000` (1024 MiB), the DSP DRAM buffer begins at `dram_base = 0xd1f59000`.
2. Adding `dsp_buf_size` yields `0x101f59000` (4127 MiB). Because the microcode operates in 32-bit arithmetic, the upper bit is truncated, wrapping the end address to `end = 0x01f59000` (32.8 MiB).
3. The microcode allocator performs the bounds check: `if (next_off > end) { assert("MM:: no more dram"); }`.
4. On the very first sub-buffer allocation, `next_off` (`0xd1f82980`) is compared against `end` (`0x01f59000`). Because `0xd1f82980 > 0x01f59000`, the bounds check fails immediately, triggering a false out-of-memory kernel panic during `IAV_IOC_ENABLE_PREVIEW`:
   ```text
   #iav_error# dsp_check_assertion(770): DSP assertion happened!
   [IDSP1:0] Assertion failure at line:1057 of .../dsp_mempar.c module 1
   MM:: no more dram, size_aligned=512 next_off=0xd1f82980 dram_base=0xd1f59000, end=0x01f59000
   ```

#### The Phase 1 Resolution
To guarantee the DSP buffer never crosses 4 GiB, `/reserved-memory/iav@0` is anchored at `0x00a3000000` with size `0x5d000000` (1488 MiB). Because `0x00a3000000 + 0x5d000000 = 0x100000000`, the pool spans exactly up to the 4 GiB boundary without overflowing. `cavalry@0` (12 GiB) starts at `0x0100000000`, completely non-overlapping in 64-bit space.

### 3.3 U-Boot SMP Relocation Top (`/u-boot_cfg/reloc-top`)

On 32 GiB Ambarella N1-655 platforms, U-Boot dynamically relocates itself near the top of usable physical RAM. In `arch/arm/mach-ambarella/common.c`, `board_get_usable_ram_top()` queries the control DTB for `/u-boot_cfg/reloc-top`.

1. **Failure Mode (Missing `reloc-top`)**: If `/u-boot_cfg` is absent from the control DTB, U-Boot relocates to the top of 32 GiB (`0x7fff3db90`). The spin-table trampoline (`secondary_cortex_jump`) is placed at this 35-bit physical address. When Linux attempts to release secondary CPUs (`CPU1..3`), the PC-relative instructions (`adr`) fail to reach the high-memory spin table. The secondary cores never wake, Linux reports `CPU1..3: failed to come online` at 5-second intervals, and the board resets via hardware watchdog after 15 seconds.
2. **Resolution**: Any control DTB compiled into or used by U-Boot must include:
   ```dts
   u-boot_cfg {
       #address-cells = <0x01>;
       #size-cells = <0x00>;
       reloc-top = <0x80000000>;
   };
   ```
   This restricts U-Boot relocation to the 2 GiB boundary (`0x7ff37b90`), ensuring secondary CPU spin tables remain accessible.

### 3.4 Partition 4 (`CONFIG`) Device Tree Deployment & GRUB Override

EVE provides a persistent mechanism to deliver custom device trees via the FAT configuration partition (`CONFIG`, partition 4 / `/dev/mmcblk0p4`).

> [!WARNING]
> When GRUB loads a device tree via `set_global devicetree "($config_part)/eve.dtb"`, it passes the file directly to the Linux kernel, **bypassing U-Boot's dynamic `spin_table_update_dt()` runtime patching**.
> Therefore, the static DTB deployed to Partition 4 must explicitly include the pre-calculated per-CPU strided `cpu-release-addr` properties:
> - `cpu@0`: `0x00 0x7ff38460`
> - `cpu@1`: `0x00 0x7ff38468`
> - `cpu@2`: `0x00 0x7ff38470`
> - `cpu@3`: `0x00 0x7ff38478`
> Without these strided addresses, the secondary CPUs will fail to come online under Linux.

To deploy the verified device tree:

```bash
# 1. Mount the persistent CONFIG partition on the edge node
mkdir -p /tmp/cfgmnt
mount /dev/mmcblk0p4 /tmp/cfgmnt

# 2. Stage the verified DevKit DTB as eve.dtb
cp /path/to/build/eve_iav.dtb /tmp/cfgmnt/eve.dtb

# 3. Configure grub.cfg override to load eve.dtb
cat << 'EOF' > /tmp/cfgmnt/grub.cfg
set_global devicetree "($config_part)/eve.dtb"
EOF

# 4. Unmount and warm reboot
umount /tmp/cfgmnt
reboot
```

Upon reboot, GRUB reads `($config_part)/grub.cfg` and loads `eve.dtb`. Verify active carveouts and SMP in Dom0:

```bash
nproc --all
# Expected output: 4
cat /proc/device-tree/model
# Expected output: n1-655 cooper devkit
ls -la /proc/device-tree/reserved-memory/
# Confirms iav@0, iav@1, cavalry@0..2 are present
```

---

## 4. Technical Mechanics

### 4.1 Dynamic Firmware Search Redirection
When `cavalry.ko` probes the device tree platform device (`sub_scheduler0`), it invokes `request_firmware(&fw, "cavalry.bin", dev)`. The kernel firmware subsystem searches standard paths (`/lib/firmware`) by default.

Because `/lib/firmware` is read-only SquashFS, new or updated firmware binaries cannot be copied there directly. However, the Linux kernel `firmware_class` parameter `/sys/module/firmware_class/parameters/path` is writable (`-rw-r--r--`) at runtime.

By pointing this sysfs parameter to `/persist/firmware`, the kernel's firmware loader checks `/persist/firmware/cavalry.bin` before falling back to `/lib/firmware`:

```bash
echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path
```

### 4.2 Out-of-Tree Module Insertion
With the firmware search path redirected, the kernel drivers can be dynamically inserted using `insmod`:

```bash
# 1. Insert cavalry driver (binds reserved memory and boots microcode)
insmod /persist/modules/cavalry.ko

# 2. Insert amba_virt transport driver
insmod /persist/modules/amba_virt.ko
```

### 4.3 Hardware Initialization Sequence
When `cavalry.ko` is inserted:
1. It matches the `sub_scheduler0` platform device declared in `arch/arm64/boot/dts/ambarella/n1_655.dts`.
2. It parses and claims the reserved memory carveouts established at boot:
   - `cavalry_reserved` (User/CVMEM pool: `0x100000000 - 0x3ffffffff`, 12 GB)
   - `cavalry_ucode` (Microcode staging: `0x25c00000 - 0x25ffffff`, 4 MB)
3. It loads `/persist/firmware/cavalry.bin` into ucode memory via the redirected firmware path.
4. It initializes interrupts (IRQ 41/42) and VP clocks, starting the Vector Processor ucode.
5. It creates device nodes `/dev/cavalry` and `/dev/cavalry_profile`.

When `amba_virt.ko` is inserted:
1. It creates `/dev/amba_virt` for inter-workload communication (HVM VM <-> NOHYPE container).

---

## 5. Edge Application Lifecycle & Device Injection

In EVE-OS, containerized edge applications (such as `ubuntu_24_04_container`) access host devices via EVE's adapter assignment model:
- `Ifname "/dev/cavalry"`
- `Ifname "/dev/cavalry_profile"`
- `Ifname "/dev/amba_virt"`

### The Container Startup Race Condition
When EVE boots, `domainmgr` iterates over assigned adapters and injects the corresponding device major/minor numbers and cgroup permissions into the container's OCI runtime specification (`oci.go`).

If EVE boots **before** `/persist/modules/cavalry.ko` or `/persist/modules/amba_virt.ko` have been inserted:
1. `domainmgr` attempts to inspect `/dev/cavalry` and `/dev/amba_virt`.
2. The device nodes do not exist yet; `domainmgr` logs an error and skips device attachment.
3. The container starts **without** `/dev/cavalry` or `/dev/amba_virt` in its device whitelist.

### Post-Insertion Container Refresh
To grant the running container access after inserting the modules, restart the application instance:

```bash
# Via EVE local CLI inside debug container:
eve app restart ubuntu_24_04_container

# OR remotely via zcli:
./scripts/zcli -- edge-node app-restart <node-name> --app=ubuntu_24_04_container
```

Upon restart, `domainmgr` detects the existing `/dev/cavalry` and `/dev/amba_virt` character devices, creates the device nodes in the container's devfs, and updates the device cgroup whitelist.

---

## 6. Development & Deployment Workflow

### Step 1: Build Modules Out-of-Tree on the Host
Compile the out-of-tree drivers against the extracted kernel headers using the top-level build target:

```bash
# Extract headers and signing keys from LinuxKit cache (if not already extracted)
make eve-kernel-headers

# Compile and cryptographically sign all out-of-tree drivers
make drivers

# Or use the standalone helper script:
./scripts/build_kmod_out_of_tree.sh
```
This automatically compiles `amba_virt.ko`, `amba_pci_platform.ko` (and proprietary modules such as `cavalry.ko` when present), signs each binary with `build/certs/signing_key.pem`, and stages them to `build/modules/`.

### Step 2: Stage Artifacts to the Edge Node
Create the directory structure on the target node and copy the compiled `.ko` files and firmware:

```bash
# Create persistent directories on node
ssh <target-node> "mkdir -p /persist/modules /persist/firmware"

# Transfer microcode
scp cavalry.bin <target-node>:/persist/firmware/cavalry.bin

# Transfer driver modules
scp cavalry.ko <target-node>:/persist/modules/cavalry.ko
scp amba_virt.ko <target-node>:/persist/modules/amba_virt.ko
```

### Step 3: Load Drivers & Verify
Execute the runtime setup commands via SSH:

```bash
ssh <target-node> "
  echo -n '/persist/firmware' > /sys/module/firmware_class/parameters/path
  insmod /persist/modules/cavalry.ko
  insmod /persist/modules/amba_virt.ko
"
```

Verify successful initialization in kernel logs:

```bash
ssh <target-node> "dmesg | tail -n 25; ls -l /dev/cavalry /dev/amba_virt"
```

Expected kernel output highlights:
```text
cavalry: loading out-of-tree module taints kernel.
cavalry_show_version: Cavalry Linux Driver Version: 3.0.x
cavalry_parse_reserved_mem: CVMEM(1:ucode) RANGE: [0x25c00000 - 0x25ffffff]
cavalry_parse_reserved_mem: CVMEM(0:user)  RANGE: [0x100000000 - 0x3ffffffff]
show_ucode_version: Cavalry Ucode Version = N1_655-Ver.206-...
visorc_start: Cavalry Ucode is started.
```

### Step 4: Refresh Edge Application
```bash
ssh <target-node> "eve app restart ubuntu_24_04_container"
```

Verify device presence inside the running container:
```bash
ssh <target-node> "eve app enter ubuntu_24_04_container ls -la /dev/cavalry /dev/amba_virt"
```

---

## 7. BaseOS Upgrade & Lifecycle Considerations

### Preservation Across Updates
- When performing a BaseOS update via `eveimage-update`, the A/B rootfs partitions (`IMGA`/`IMGB`) are updated.
- The `/persist` partition is untouched. `/persist/modules` and `/persist/firmware` remain intact across updates.

### Kernel ABI Compatibility
- Kernel modules compiled with `vermagic` must match the kernel version, compiler version, and `LOCALVERSION` of the active kernel.
- If an OTA update installs a kernel with an incremented version or altered build configuration, existing modules in `/persist/modules` will fail to insert (`Exec format error` or `Unknown symbol in module`).
- Whenever the BaseOS kernel version changes, recompile `cavalry.ko` and `amba_virt.ko` against the new kernel headers and update `/persist/modules/`.

### Board Reboot and Power Cycles
Software warm reboot has been fixed in updated U-Boot firmware. When performing an OTA update or issuing a reboot, monitor the SoC serial console:
- If `BootFrom:PAHTA` appears within **10 seconds**, the warm reboot succeeded and U-Boot/GRUB will boot automatically.
- If `BootFrom:PAHTA` does not appear within 10 seconds, the board is stuck and requires an MCU power cycle:
```bash
# Via MCP tool:
embdevenv_mcu_power(target="<target>", action="reboot")
```
Because `/persist` is non-volatile flash, staged files persist through reboots and cold power cycles. After the board boots up, running the insmod sequence reloads the drivers.

---

## 8. Integrated EVE `storage-init` Boot Hook
In modern EVE BaseOS builds, driver loading at boot time is fully automated via an early hook in EVE's `storage-init` service.

Upon mounting the `/persist` filesystem on system startup, `storage-init` automatically executes `/persist/bin/load-ambarella-drivers.sh` inside `/hostfs`:

```bash
#!/bin/sh
# /persist/bin/load-ambarella-drivers.sh (deployed by scripts/deploy_and_insmod.sh)

set -e

# 1. Configure kernel firmware search path to persistent storage
if [ -f /persist/firmware/cavalry.bin ]; then
    echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path
fi

# 2. Insert out-of-tree hardware drivers if present
if [ -f /persist/modules/cavalry.ko ] && ! lsmod | grep -q cavalry; then
    insmod /persist/modules/cavalry.ko
fi

# 3. Insert virtualization transport drivers
if [ -f /persist/modules/amba_pci_platform.ko ] && ! lsmod | grep -q amba_pci_platform; then
    insmod /persist/modules/amba_pci_platform.ko
fi

if [ -f /persist/modules/amba_virt.ko ] && ! lsmod | grep -q amba_virt; then
    insmod /persist/modules/amba_virt.ko
fi
```

Because `storage-init` executes before edge applications and hypervisor domains launch, character devices (`/dev/cavalry`, `/dev/amba_virt`) are created up front, eliminating the container startup race condition. Deploying drivers via `./scripts/deploy_and_insmod.sh <target-node>` automatically installs this loader into `/persist/bin/`.

---

## 9. Dual-Mode Build Workflow: Development vs. Production

EVE-OS enforces kernel module signature verification (`CONFIG_MODULE_SIG_FORCE=y`). To balance developer productivity (sub-5-second edit-compile-reload loops) with production security (hermetic, zero-trust appliance images), two build and execution modes are supported:

### 9.1 Mode Comparison

| Property | Development Mode | Production Mode |
|---|---|---|
| **Driver Location** | `/persist/modules/` (deployed live via SSH) | `/lib/modules/<ver>/extra/` (baked in `rootfs.img`) |
| **Driver Build** | Host filesystem via `scripts/build_kmod_out_of_tree.sh` | Inside Docker container via `Dockerfile.ambarella` |
| **Signing Key** | Local persistent key (`eve-kernel/certs/signing_key.pem`) | Ephemeral key generated dynamically inside Docker |
| **Signing Action** | Host script runs `scripts/sign-file sha256 <key> <cert> <.ko>` | Kernel `modules_install` automatically signs inside Docker |
| **Private Key Lifetime** | Persisted on host (`.gitignore`d) for repeated signing | Destroyed immediately when Docker container exits |
| **Iteration Turnaround** | **~3 seconds** (edit -> build -> deploy -> reload) | **~30 minutes** (full BaseOS build + OTA + reboot) |
| **Upstream Cleanliness** | Git status clean (`certs/` in `.gitignore`) | 100% clean upstream tree (sources mapped via `--build-context`) |

### 9.2 Developer Guide: Querying & Switching Between Modes

The active compile mode is persisted in the repository root `.mode` file and managed directly via the top-level `Makefile`.

#### Querying Current Mode

Run `make mode` at any time to inspect the active configuration:

```bash
make mode
```

This displays whether the build is configured for `development` or `production`, the target kernel Docker tag, driver storage locations, signing key types, and iteration commands.

#### Switching to Development Mode (Host Out-of-Tree Builds)

1. **Activate Development Mode**:
   ```bash
   make set-mode-development
   ```
   This writes `development` to `.mode` and invalidates any conflicting production build cache markers.

2. **Build Development Kernel & BaseOS**:
   ```bash
   make all
   ```
   Compiles `kernel-gcc`, extracts headers and signing keys to `build/`, compiles out-of-tree drivers, and builds the development BaseOS installer.

3. **Deploy Development Firmware to Target Node**:
   ```bash
   pub_eve_datastore.sh /path/to/eve-images/
   zcli edge-node eveimage-update <target-node> --image=<new-image-name>
   ```
   Monitor SoC console for `BootFrom:PAHTA` (reboots within 10 seconds; if stuck, power cycle via MCP `embdevenv_mcu_power(target="<target>", action="reboot")`).

4. **Rapidly Iterate on Driver Code**:
   Modify driver code on the host, then recompile and reload live in ~3 seconds:
   ```bash
   make drivers
   ./scripts/deploy_and_insmod.sh <target-node> --reload
   ```
   The module is recompiled, signed, transferred, and reloaded on the running board without rebooting.

#### Switching to Production Mode (In-Tree Hermetic Build)

1. **Activate Production Mode**:
   ```bash
   make set-mode-production
   ```
   This writes `production` to `.mode` and invalidates any conflicting development build cache markers.

2. **Build Hermetic Production BaseOS Image**:
   ```bash
   make eve
   ```
   Builds `kernel-ambarella` inside Docker BuildKit using driver contexts, generates an ephemeral 4096-bit RSA key to sign all modules during `modules_install`, bakes all drivers into `/lib/modules/<ver>/extra/` and firmware into `/lib/firmware/`, and embeds them into `rootfs.img`. The private signing key is destroyed when Docker finishes building, maintaining EVE's zero-trust security architecture.

3. **Deploy Production OTA Image**:
   ```bash
   pub_eve_datastore.sh /path/to/eve-images/
   zcli edge-node eveimage-update <target-node> --image=<new-production-image>
   ```
   Upon reboot, the Linux kernel automatically probes and loads all Ambarella drivers via `udev` without requiring any `/persist` scripts.


