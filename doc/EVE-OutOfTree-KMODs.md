# EVE-OS Out-of-Tree Kernel Modules (KMODs): Architecture & Dual Modes

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

- **Date**: 2026-09-11
- **Platform**: Ambarella N1-655 (`cv3ad655`) / EVE-OS (kernel 6.1.112)
- **Related Documents**: [EVE-BaseOS-AmbarellaDrivers.md](EVE-BaseOS-AmbarellaDrivers.md), [Architecture.md](Architecture.md), [CavalryVirtualization.md](CavalryVirtualization.md)

> [!IMPORTANT]
> **Qualified build path (2026-10-10): development mode only.**
> Use `make set-mode-development`, require `make mode` to report
> `DEVELOPMENT` and `kernel-gcc`, then run `make all`. This builds the kernel
> first, signs the host-built modules with the persistent key whose
> certificate is embedded in that kernel, and packs them into the
> `eve/pkg/amba-virt` image layer.
>
> Production mode (`kernel-ambarella` / `Dockerfile.ambarella`) is retained
> below as an unqualified design description. It currently fails from a clean
> baseline and has never produced a qualified image. Do not invoke it, repair
> it opportunistically, or use it as evidence that a module is deployable.

---

## 1. Overview & Problem Statement

Ambarella edge platforms rely on proprietary hardware accelerators and virtualization transport layers that require specialized kernel drivers and microcode:

1. **`cavalry.ko`**: The Ambarella Vector Processor (NVP/VP) accelerator driver managing reserved CMA carveouts (`0x100000000 - 0x3ffffffff`, 12 GB), microcode staging (`0x1fc00000 - 0x1fffffff`, 4 MiB), command/message queues, doorbells, and IRQs (41/42).
2. **`cavalry.bin`**: Hardware microcode executed directly by the on-chip Vector Processor scheduler.
3. **`amba_virt.ko`**: POSIX shared memory / IVSHMEM transport driver providing zero-copy IPC between KVM HVM virtual machines and `amba-virt-server` in EVE Dom0.
4. **Proprietary & NDA Modules**: Out-of-tree drivers with commercial or NDA restrictions (`cavalry` for VisORC vector processor, `ambvideo` / `iav` for video DSP, `amba_otp` for security fuses, and `pwr_gpu` / `pvrsrvkm.ko` for Imagination PowerVR Rogue GPU) are decoupled from public Git tracking and ignored in `drivers/.gitignore`. The top-level `Makefile` dynamically compiles them when present in the workspace and cleanly skips them when absent.

EVE-OS enforces strict security constraints:
- **Module Signature Enforcement**: `CONFIG_MODULE_SIG_FORCE=y` requires every `.ko` module to carry a valid cryptographic signature matching a root/trusted X.509 certificate compiled into the kernel.
- **Read-Only Root Filesystem**: The operating system root is a read-only SquashFS image (`rootfs.img`). `/lib/modules` and `/lib/firmware` cannot be modified at runtime, so out-of-tree modules have to be in the image.

The build system exposes two operational modes. Only development mode is
currently qualified. The production column below describes an intended
architecture, not a runnable release procedure:

```text
+------------------------------------+      +-----------------------------------------+
| Development Mode (Image Layer)     |      | Production Mode (Hermetic Single-Image) |
+------------------------------------+      +-----------------------------------------+
| Driver Source Code                 |      | Driver Source Code                      |
| (cavalry, amba_virt, camera)       |      | (cavalry, amba_virt)                    |
|                 |                  |      |                    |                    |
|                 v                  |      |                    v                    |
| Host Build Target                  |      | Docker BuildKit External Contexts       |
| (make drivers)                     |      | (--build-context)                       |
|        |                           |      |                    |                    |
|        v                           |      |                    v                    |
| Host sign-file (sha256)            |      | Ephemeral Single-Use Key (kbuild)       |
| (via signing_key.pem)              |      |             /              \            |
|        |                           |      |            v                v           |
|        v                           |      | Kernel extra/ Dir    Firmware Dir       |
| eve/pkg/amba-virt layer            |      | (/lib/modules/extra) (/lib/firmware)    |
| (make amba-virt-image)             |      |            |                            |
| /lib/modules/<rel>/extra/*.ko      |      |            v                            |
|        |                           |      | depmod (modules.alias from DTS)         |
|        v                           |      |            |                            |
| amba-virt-server (Dom0 init)       |      |            v                            |
| finit_module per modules.conf      |      | udev Early Userspace (kmod load)        |
+------------------------------------+      +-----------------------------------------+
```

---

## 2. Boot-Time Insertion Mechanism: How `cavalry.ko` Gets Loaded

A core difference between Development and Production modes is how `cavalry.ko` is discovered and loaded during the boot sequence.

### 2.1 Production Mode Design (Unqualified; Do Not Use)

In the intended Production Mode, module insertion would be automated by early userspace:

```text
+--------+       +------------+       +-----------+       +---------------+       +------------+       +-------------+
| Linux  |       |Device Tree |       | EVE udev  |       | kmod /        |       | cavalry.ko |       | Firmware    |
| Kernel |       |sub_sched0  |       |  daemon   |       | modprobe      |       |            |       | /lib/firmw. |
+--------+       +------------+       +-----------+       +---------------+       +------------+       +-------------+
    |                  |                    |                     |                     |                     |
    | 1. Probes HW     |                    |                     |                     |                     |
    |----------------->|                    |                     |                     |                     |
    |                  |                    |                     |                     |                     |
    | 2. Discovers "ambarella,sub-scheduler"|                     |                     |                     |
    |<-----------------|                    |                     |                     |                     |
    |                                       |                     |                     |                     |
    | 3. Emits uevent (MODALIAS=of:N...)    |                     |                     |                     |
    |-------------------------------------->|                     |                     |                     |
    |                                       |                     |                     |                     |
    |                                       | 4. Rule triggers (kmod load $MODALIAS)    |                     |
    |                                       |-------------------->|                     |                     |
    |                                       |                     |                     |                     |
    |                                       |                     | 5. Loads /lib/.../extra/cavalry.ko        |
    |                                       |                     |-------------------->|                     |
    |                                       |                     |                     |                     |
    | 6. Calls request_firmware(&fw, "cavalry.bin", dev)          |                     |                     |
    |<----------------------------------------------------------------------------------|                     |
    |                                                                                                         |
    | 7. Reads /lib/firmware/cavalry.bin (sealed in rootfs)                                                   |
    |-------------------------------------------------------------------------------------------------------->|
    |                                                                                                         |
    |                                                             |                     | 8. Starts VP ucode  |
    |                                                             |                     |    & creates        |
    |                                                             |                     |    /dev/cavalry     |
```

1. **Device Tree Binding**: The kernel parses the `sub_scheduler0` node (`compatible = "ambarella,sub-scheduler"`).
2. **Uevent Generation**: The kernel emits a device-add uevent containing `MODALIAS`.
3. **Udev Match**: `/etc/udev/rules.d/80-drivers.rules` matches the modalias:
   ```udev
   ACTION=="add", ENV{MODALIAS}!="", RUN{builtin}+="kmod load $env{MODALIAS}"
   ```
4. **Modprobe Resolution**: `kmod load` consults `/lib/modules/$(uname -r)/modules.alias`, which maps the device compatible string directly to `cavalry.ko`.
5. **Firmware Resolution**: `request_firmware()` searches `/lib/firmware/cavalry.bin`, which is sealed into `rootfs.img`.
6. **No manual intervention or scripts required.**

---

### 2.2 Development Mode: Loaded by `amba-virt-server`

In Development Mode the signed modules are in the `eve/pkg/amba-virt` layer
under `/lib/modules/<release>/extra/`. The layer does not run `depmod`.
`amba-virt-server`, started by `/etc/init.d/021-amba-virt-server` before the
EVE services, loads them by path with `finit_module`:

1. `modules.conf` in order: `ambcma.ko` with its parameters, `cavalry.ko`,
   `amba_virt.ko`. A module already loaded with the same parameters is
   accepted; different parameters stop the boot with "already loaded with
   parameters".
2. With `camera.conf` present, the camera pipeline loads
   `early-modules.conf`, powers the camera, loads `late-modules.conf`, and
   sets `/sys/module/firmware_class/parameters/path` to
   `/usr/lib/amba-virt/firmware` for the DSP microcode.
3. `cavalry.ko` calls `request_firmware("cavalry.bin")`, which the kernel
   finds in `/lib/firmware/cavalry.bin` in the same layer.

Details and readiness: [AmbaVirtServer.md](AmbaVirtServer.md) §14.2 and
[EVE-BaseOS-AmbarellaDrivers.md](EVE-BaseOS-AmbarellaDrivers.md) §2.

The earlier development flow staged `cavalry.ko` and `cavalry.bin` in
`/persist/modules` and `/persist/firmware` and loaded them from
`/persist/bin/load-ambarella-drivers.sh`. It is retired and deleted.

---

## 3. Detailed Mode Comparison

| Architectural Property | Development Mode | Production Mode |
|---|---|---|
| **Qualification Status** | Qualified build path | Unqualified design; do not use |
| **Primary Purpose** | Rapid driver debugging & code iteration | Sealed, zero-trust appliance deployment |
| **Driver Storage** | `/lib/modules/<ver>/extra/*.ko` (SquashFS, `eve/pkg/amba-virt` layer) | `/lib/modules/<ver>/extra/*.ko` (SquashFS) |
| **Microcode Storage** | `/lib/firmware/cavalry.bin`; DSP files in `/usr/lib/amba-virt/firmware` | `/lib/firmware/cavalry.bin` |
| **Firmware Search** | `/lib/firmware`, plus sysfs `path` set to `/usr/lib/amba-virt/firmware` by the camera pipeline | Standard kernel search in `/lib/firmware` |
| **Signing Key Type** | Persistent local key (`certs/signing_key.pem`) | Ephemeral key dynamically created in Docker |
| **Signing Key Lifetime**| Persisted on development host | Destroyed immediately when Docker build finishes |
| **Signing Execution** | Host script (`scripts/sign-file`) | `kbuild modules_install` inside Docker |
| **Boot-Time Trigger** | `amba-virt-server` (`finit_module` per `modules.conf`) | Automatic `udev` (`kmod load $MODALIAS`) |
| **Iteration Turnaround**| Image rebuild -> OTA -> reboot; manual `/persist/amba-virt-dev/` loop for experiments | **~30 minutes** (full OS build -> OTA -> reboot) |
| **Reboot Required?** | **Yes** for product changes (OTA rootfs update) | **Yes** (OTA rootfs update & cold boot) |
| **OTA Impact** | Modules updated atomically with `rootfs.img` | Modules updated atomically with `rootfs.img` |
| **TPM PCR 13 Impact** | Part of `rootfs.img`, like production | Measured into TPM PCR 13 via SquashFS |

---

## 4. Development Mode Deep Dive

### 4.1 Prerequisites & Kernel Signature Alignment

Because `CONFIG_MODULE_SIG_FORCE=y` is enforced by the kernel:
1. The kernel build must incorporate the public certificate of the development key into its builtin system keyring (`system_trusted_keyring`).
2. The private key (`signing_key.pem`) is retained on the developer workstation under `eve-kernel/certs/` or `build/certs/` (ignored by `.gitignore`).
3. If the kernel is rebuilt, modules compiled on the host **must** match the exact kernel version magic string (e.g. `6.1.112-linuxkit-310c92224386-...`).

### 4.2 Building Out-of-Tree on the Host

The top-level `make drivers` target automates compilation and cryptographic signing:

```bash
# Compile and sign all available out-of-tree drivers against extracted headers
make drivers
```

Under the hood:
1. Locates the extracted kernel headers (`build/usr/src/linux-headers-...` extracted via `make eve-kernel-headers`).
2. Invokes kbuild out-of-tree:
   ```bash
   make -C "$KDIR" M="$SRC_DIR" modules ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
   ```
3. Uses `sign-file` to append a CMS cryptographic signature to each ELF binary (`amba_virt.ko`, `amba_pci_platform.ko`, and `cavalry.ko` if present):
   ```bash
   $KDIR/scripts/sign-file sha256 build/certs/signing_key.pem build/certs/signing_key.x509 amba_virt.ko
   ```
4. Verifies the module signature footer with `modinfo`:
   ```text
   signer:         Build time autogenerated kernel key
   sig_key:        41:67:4C:36:67:77:67:7B:59:9C:49:8D:F8:D1:16:C4:13:6D:F8:DF
   sig_hashalgo:   sha256
   signature:      ...
   ```

### 4.3 Packing the Modules into the Image

`make amba-virt-image` copies the signed modules from `build/modules/` into
`eve/pkg/amba-virt/rootfs/lib/modules/<release>/extra/`, taking `<release>`
from the `vermagic` of `amba_virt.ko`, then builds the layer. `make eve`
depends on it, so the modules, the kernel that trusts their signer, and the
server always ship in one image.

The former path (Debian package installed with `apt` in a NOHYPER container,
then streamed to a Dom0 `amba-virt-backend` over TCP port 5556 and stored in
`/persist/modules`) is retired and deleted.

---

## 5. Production Mode Design Record (Unqualified)

### 5.1 Zero-Trust Ephemeral Signing

The intended production design keeps private keys off the developer's
workstation. The following is not an executable runbook and has not been
qualified:

1. When `make set-mode-production && make eve` is invoked, Docker BuildKit mounts the driver sources into the container using external contexts:
   ```dockerfile
   # syntax=docker/dockerfile:1.6
   FROM scratch AS cavalry
   FROM scratch AS amba-virt
   ...
   ```
2. The kernel build generates a unique, single-use 4096-bit RSA key pair during compilation (`certs/signing_key.pem`).
3. `kbuild` compiles the out-of-tree drivers alongside in-tree modules.
4. During `make modules_install`, all modules are signed with the ephemeral key.
5. The public X.509 certificate is embedded into the kernel binary (`/boot/kernel`).
6. The container finishes, and the private key is **permanently destroyed**.
7. Result: The root filesystem image (`rootfs.img`) is sealed and hermetic. No attacker or third party can sign rogue kernel modules post-build.


---

## 6. Boot-Time Driver Loading Architecture

### 6.1 Early Boot

1. LinuxKit init runs `/etc/init.d/021-amba-virt-server`.
2. The server loads the modules and starts the camera pipeline as in §2.2,
   then reports ready once `/dev/amba_virt_shm*` exists.
3. EVE starts the HVM guests afterwards. No Ambarella device is assigned to
   an app.

`/etc/init.d/000-mod-params` no longer loads `amba_virt.ko`;
`020-amba-virt-backend` and the `storage-init` hook are gone.

---

## 7. Troubleshooting & Common Pitfalls

### Pitfall 1: Firmware Missing or Inaccessible
- **Symptom**: `cavalry sub_scheduler0: Direct firmware load for cavalry.bin failed with error -2`.
- **Root Cause**: `cavalry.bin` is missing from `/lib/firmware` in the image, or the DSP files are missing from `/usr/lib/amba-virt/firmware`.
- **Fix**: Rebuild with `make amba-virt-image` and check that `eve/pkg/amba-virt/rootfs/lib/firmware/cavalry.bin` exists before `make eve`.

### Pitfall 2: Module Verification Failed (`Key was rejected by service`)
- **Symptom**: `insmod: can't insert 'cavalry.ko': Permission denied` (dmesg: `Loading of unsigned module is rejected`).
- **Root Cause**: The module was compiled without signing, or was signed with a key whose public certificate is not embedded in `/boot/kernel`.
- **Fix**: Re-sign with the active key matching the kernel using `scripts/sign-file sha256 certs/signing_key.pem certs/signing_key.x509 cavalry.ko`.

### Pitfall 3: Invalid Module Format (`version magic mismatch`)
- **Symptom**: `insmod: can't insert 'cavalry.ko': invalid module format` (dmesg: `version magic '...ac6b5c0e15b0...' should be '...310c92224386...'`).
- **Root Cause**: The running kernel on the target board was updated (e.g. via OTA or partition flip), but the modules were compiled against an older commit.
- **Fix**: Rebuild with `make all` so the kernel, the modules, and the layer come from one build.

### Pitfall 4: Module Already Loaded with Different Parameters
- **Symptom**: `server.log` reports "already loaded with parameters" and the worker does not become ready.
- **Root Cause**: A module from `modules.conf` was loaded earlier (by hand, or by another service) with parameters different from the configured ones.
- **Fix**: Unload it, or cold boot, and let the server load it.

---

## 8. Host Diagnostic & Validation Modules (`drivers/diag/`)

For hypervisor-level verification and hardware validation on the EVE Dom0 host, standalone diagnostic drivers are maintained under `drivers/diag/`:

```text
drivers/diag/
├── Kbuild
├── Makefile
├── diag_gdma.c         # Ambarella native GDMA validation suite
└── diag_stage2_pte.c   # KVM hypervisor Stage-2 PTE introspection tool
```

### 8.1 Building Diagnostic Modules
The suite can be built and signed individually or alongside standard out-of-tree drivers:
```bash
# Build and sign diag modules into build/modules/
make diag
```
This generates:
- `build/modules/diag_gdma.ko`
- `build/modules/diag_stage2_pte.ko`

### 8.2 Native GDMA Hardware Validation (`diag_gdma.ko`)
Verifies hardware DMA engine operations directly against physical memory (`CAVALRY_MEM_USER` at `0x100000000`):
```bash
# Load module and trigger validation
insmod diag_gdma.ko run=1

# Output in dmesg confirms linear, reverse, pitch, and boundary copy tests:
# [ 9217.408679] diag_gdma: 10/10 cases passed at USER phys 0x0000000100000000
rmmod diag_gdma
```

### 8.3 KVM Stage-2 Hypervisor Page Table Inspector (`diag_stage2_pte.ko`)
Inspects Stage-2 translation for a guest VM process (QEMU) by dynamically resolving `kvm_pgtable_get_leaf` via kprobe:
```bash
# Target the guest QEMU process PID and GPA (e.g., ivshmem BAR2 at 0x8000000000)
insmod diag_stage2_pte.ko target_pid=<qemu-pid> target_gpa=0x8000000000

# Output in dmesg confirms physical HPA mapping and memory attributes:
# Stage-2 PTE Dump for PID 31651 GPA 0x8000000000:
#   Raw PTE:      0x00400001000007d7 (Level 3, VALID)
#   Target HPA:   0x0100000000
#   MemAttr[5:2]: 0x5 -> Normal-NC (MT_S2_FWB_NORMAL_NC / MT_S2_NORMAL_NC)
#   S2AP[7:6]:    0x3 (Read/Write)
#   SH[9:8]:      0x3 (Inner Shareable)
#   AF[10]:       1 (Access Flag)
rmmod diag_stage2_pte
```

