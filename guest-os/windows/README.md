# Windows 11 IoT Enterprise LTSC ARM64 HVM Guest on Ambarella N1-655

This directory contains the automated unattended build pipeline and runtime configuration for running **Windows 11 IoT Enterprise LTSC on ARM64** as a lean, high-reliability Hardware Virtual Machine (HVM) edge application under EVE-OS on Ambarella N1-655 platforms.

---

## 1. Overview & Architecture

Unlike Linux and QNX guests which boot kernel images directly or from minimal ramdisks, Windows 11 IoT Enterprise on ARM64 boots strictly via **UEFI firmware (AAVMF / EDK2)** presenting **ACPI tables**. Storage and networking operate over high-performance paravirtualized VirtIO devices.

```text
+-------------------------------------------------------------------------------+
| Ambarella SoC | EVE-OS Hypervisor (EL2)                                       |
|                                                                               |
|   +-----------------------------------------------------------------------+   |
|   |            QEMU (machine: virt, gic-version=host)                     |   |
|   +-----------------------------------------------------------------------+   |
|     |         |             |                 |                 |             |
|     v         v             v                 v                 v             |
|  +-----+  +-------+  +--------------+  +--------------+  +--------------+     |
|  |AAVMF|  | swtpm |  |virtio-blk-pci|  |virtio-net-pci|  |virtio-gpu-pci|     |
|  | EDK2|  | (vTPM |  |  (Storage)   |  |  (Network)   |  |  (Display)   |     |
|  |(UEFI|  |  2.0) |  |              |  |              |  |              |     |
|  +-----+  +-------+  +--------------+  +--------------+  +--------------+     |
+-----|---------:--------------:-----------------:----------------:-------------+
      |         :              :                 :                :
      v         :              :                 :                :
+---------------+--------------:-----------------:----------------:-------------+
| Windows 11 IoT Enterprise LTSC ARM64 HVM Guest (EL1 / EL0)     :             |
|               :              :                 :                :             |
|   +---------------------------------------+    :                :             |
|   | \EFI\Microsoft\Boot\bootmgfw.efi       |    :                :             |
|   | (Windows Boot Manager)                |    :                :             |
|   +---------------------------------------+    :                :             |
|                       |                        :                :             |
|                       v                        :                :             |
|   +---------------------------------------+    :                :             |
|   | ntoskrnl.exe (Windows Kernel)         |<...+                :             |
|   +---------------------------------------+                     :             |
|        |                  |             |                       :             |
|        v                  v             v                       :             |
|   +-----------+     +-----------+ +-----------+                 :             |
|   |viostor.sys|     |netkvm.sys | |viogpu.sys |                 :             |
|   |(Block Drv)|     |(Net Driver| |(GPU Driver|<................+             |
|   +-----------+     +-----------+ +-----------+                               |
|         ^                 ^                                                   |
|         :                 +.....................................:             |
|         +....................................:                                |
|                                                                               |
|   +---------------------------------------+                                   |
|   | TermService (Remote Desktop :3389)    |                                   |
|   +---------------------------------------+                                   |
|                                                                               |
|   +-----------------------------------------------------------------------+   |
|   | Edge Appliance Features: UWF (Flash Wear Protection) + Shell Launcher |   |
|   +-----------------------------------------------------------------------+   |
+-------------------------------------------------------------------------------+
```

---

## 2. Resource Specifications (Lean Edge Appliance Profile)

| Resource | Value | Sizing Rationale |
|---|---|---|
| **vCPUs** | 2 | Responsive UI rendering and AI inference pipelines without starving host cores. |
| **RAM** | 2048 – 3072 MiB (2–3 GiB) | ~750–850 MB idle footprint; leaves >2.2 GB for Edge AI models (ONNX/YOLO). |
| **Virtual Disk** | 20 GiB | CompactOS + disabled hibernation drops clean install to ~5.5 GB. |
| **vTPM 2.0** | Enabled (`disableVTPM: false`) | Satisfies Windows 11 hardware integrity checks via `swtpm`. |
| **Display & Remote Access** | RDP (Port 3389) + VNC | High frame-rate Remote Desktop + emergency VNC console. |
| **Default User** | `windows` / `windows` | AutoLogon enabled for instant console & VNC display. |
| **Appliance Protection** | Unified Write Filter (UWF) | Redirects disk writes to RAM overlay; prevents NAND wear & sudden power-cut corruption. |

---

## 3. Directory Layout

```text
guest-os/windows/
├── README.md                           # This document
└── windows-build/                      # Cloud image pipeline
    ├── Autounattend.xml                # Unattended specialize/OOBE answer file
    ├── firstboot.cmd                   # First-logon RDP and firewall setup
    ├── optimize.ps1                    # Lean optimization & UWF/Shell script
    ├── Dockerfile.builder              # wimlib/hivex/ntfs-3g servicing image
    ├── build.sh                        # GPT disk assembly + WinPE DISM/bcdboot
    ├── service_with_dism.sh            # One-shot WinPE: Add-Driver and bcdboot
    └── output/                         # Build artifacts (gitignored)
        ├── win11-disk.raw
        └── dist/
            └── windows-11-arm64-cloudimg.qcow2
```

---

## 4. Building the Cloud Image

### Prerequisites
Place the following ISO media in the repository's `build/iso/` directory:
1. `Windows_11_IoT_Enterprise_LTSC_ARM64.iso` (or `Win11_25H2_English_Arm64_v2.iso`)
2. `virtio-win.iso` (Fedora VirtIO Windows drivers)

### Executing the Build
From repository root:
```bash
# Execute via Makefile
make guest-windows

# Or directly:
./guest-os/windows/windows-build/build.sh
```

The script will:
1. Detect and inspect `install.wim` to select the IoT Enterprise LTSC edition index.
2. Apply the image to an NTFS volume with `wimapply --strict-acls`.
3. Inject `unattend.xml`, first-logon scripts, optimization utilities, and VirtIO driver media.
4. Boot a short WinPE pass under TCG to run Microsoft DISM (`/Add-Driver`) and `bcdboot` against the real ESP.
5. Compress the result to `windows-11-arm64-cloudimg.qcow2`.

---

## 5. Edge Appliance Features (UWF & Shell Launcher)

The guest image includes built-in PowerShell functions located in `C:\Windows\Setup\Scripts\optimize.ps1`:

### A. Enable Unified Write Filter (UWF)
Locks down the root partition `C:` into a read-only state using a volatile RAM overlay, making the system 100% resilient against abrupt power cuts and preventing NAND flash wear:
```powershell
# From an administrative PowerShell prompt:
. C:\Windows\Setup\Scripts\optimize.ps1
Enable-UnifiedWriteFilter -OverlaySizeMB 512
Restart-Computer
```

### B. Custom Shell Launcher (Kiosk Mode)
Replaces Windows Explorer with a direct full-screen launch of your Edge AI application:
```powershell
. C:\Windows\Setup\Scripts\optimize.ps1
Enable-ShellLauncher -AppPath "C:\Program Files\EdgeAI\App.exe"
```

---

## 6. Related documentation

- [guest-os/README.md](../README.md) — HVM guest architecture and compilation guide
- [doc/Guest-OS-Cross-Compilation.md](../../doc/Guest-OS-Cross-Compilation.md) — Guest cross-compilation pipeline

---

## 7. Deployment with `zcli`

Once the compressed image is staged in `output/dist/`:

```bash
# 1. Uplink image to datastore (e.g. LocalHTTP)
./scripts/zcli -- image uplink windows-11-arm64-cloudimg \
    --image-sha="<IMAGE_SHA>" \
    --image-size="<IMAGE_SIZE>"

# 2. Push edge application manifest
./scripts/push_app.sh apps/windows_11_arm64.json

# 3. Create instance on target node
./scripts/create_instance.sh windows_11_arm64 n1-655-devkit
```

