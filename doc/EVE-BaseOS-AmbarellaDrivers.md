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
│   ├── cavalry.ko
│   └── amba_virt.ko
└── firmware/                   (Proprietary microcode binaries)
    └── cavalry.bin
```

---

## 2. Directory Layout & Roles

| Location | Role | Typical Files | Access Mode |
|---|---|---|---|
| `/persist/modules/` | Houses out-of-tree kernel modules compiled for the active kernel ABI | `cavalry.ko`, `amba_virt.ko` | Read / Write |
| `/persist/firmware/` | Houses proprietary hardware microcode and DSP firmware | `cavalry.bin` | Read / Write |
| `/persist/status/` | EVE system state, microservice checkpoints, app configs | Managed by EVE | Read / Write |

---

## 3. Technical Mechanics

### 3.1 Dynamic Firmware Search Redirection
When `cavalry.ko` probes the device tree platform device (`sub_scheduler0`), it invokes `request_firmware(&fw, "cavalry.bin", dev)`. The kernel firmware subsystem searches standard paths (`/lib/firmware`) by default.

Because `/lib/firmware` is read-only SquashFS, new or updated firmware binaries cannot be copied there directly. However, the Linux kernel `firmware_class` parameter `/sys/module/firmware_class/parameters/path` is writable (`-rw-r--r--`) at runtime.

By pointing this sysfs parameter to `/persist/firmware`, the kernel's firmware loader checks `/persist/firmware/cavalry.bin` before falling back to `/lib/firmware`:

```bash
echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path
```

### 3.2 Out-of-Tree Module Insertion
With the firmware search path redirected, the kernel drivers can be dynamically inserted using `insmod`:

```bash
# 1. Insert cavalry driver (binds reserved memory and boots microcode)
insmod /persist/modules/cavalry.ko

# 2. Insert amba_virt transport driver
insmod /persist/modules/amba_virt.ko
```

### 3.3 Hardware Initialization Sequence
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

## 4. Edge Application Lifecycle & Device Injection

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

## 5. Development & Deployment Workflow

### Step 1: Build Modules Out-of-Tree on the Host
Compile the modules against the target kernel build tree (`kernel-out` or target headers):

```bash
# Example for amba_virt
make -C /path/to/kernel-out M=drivers/amba_virt modules

# Example for cavalry_drv
make -C /path/to/kernel-out M=/path/to/cavalry_drv \
     AMBARELLA_DRV_CFLAGS="-I/path/to/cavalry_include -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655" \
     modules
```

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

## 6. BaseOS Upgrade & Lifecycle Considerations

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
```text
pwr off -y
pwr on
```
Because `/persist` is non-volatile flash, staged files persist through reboots and cold power cycles. After the board boots up, running the insmod sequence reloads the drivers.

---

## 7. Automated Boot Helper (Optional)

During active development, dynamic loading can be automated using a lightweight boot check script stored in `/persist/bin/load-ambarella-drivers.sh`:

```bash
#!/bin/sh
# /persist/bin/load-ambarella-drivers.sh

set -e

if [ -f /persist/firmware/cavalry.bin ]; then
    echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path
fi

if [ -f /persist/modules/cavalry.ko ] && ! lsmod | grep -q cavalry; then
    insmod /persist/modules/cavalry.ko
fi

if [ -f /persist/modules/amba_virt.ko ] && ! lsmod | grep -q amba_virt; then
    insmod /persist/modules/amba_virt.ko
fi
```

This ensures that developers can rapidly iterate on kernel drivers without modifying or rebuilding the base EVE operating system.

---

## 8. Dual-Mode Build Workflow: Development vs. Production

EVE-OS enforces kernel module signature verification (`CONFIG_MODULE_SIG_FORCE=y`). To balance developer productivity (sub-5-second edit-compile-reload loops) with production security (hermetic, zero-trust appliance images), two build and execution modes are supported:

### 8.1 Mode Comparison

| Property | Development Mode | Production Mode |
|---|---|---|
| **Driver Location** | `/persist/modules/` (deployed live via SSH) | `/lib/modules/<ver>/extra/` (baked in `rootfs.img`) |
| **Driver Build** | Host filesystem via `scripts/build_kmod_out_of_tree.sh` | Inside Docker container via `Dockerfile.ambarella` |
| **Signing Key** | Local persistent key (`eve-kernel/certs/signing_key.pem`) | Ephemeral key generated dynamically inside Docker |
| **Signing Action** | Host script runs `scripts/sign-file sha256 <key> <cert> <.ko>` | Kernel `modules_install` automatically signs inside Docker |
| **Private Key Lifetime** | Persisted on host (`.gitignore`d) for repeated signing | Destroyed immediately when Docker container exits |
| **Iteration Turnaround** | **~3 seconds** (edit -> build -> deploy -> reload) | **~30 minutes** (full BaseOS build + OTA + reboot) |
| **Upstream Cleanliness** | Git status clean (`certs/` in `.gitignore`) | 100% clean upstream tree (sources mapped via `--build-context`) |

### 8.2 Developer Guide: Querying & Switching Between Modes

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
   Monitor SoC console for `BootFrom:PAHTA` (reboots within 10 seconds; if stuck, cycle power rails via MCU serial console with `pwr off -y` / `pwr on`).

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


