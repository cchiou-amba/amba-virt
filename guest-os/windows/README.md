# Windows 11 ARM64 HVM Guest on Ambarella N1-655

This directory contains the automated unattended build pipeline and runtime configuration for running **Windows 11 on ARM64** as a lean Hardware Virtual Machine (HVM) edge application under EVE-OS on Ambarella N1-655 platforms.

---

## 1. Overview & Architecture

Unlike Linux and QNX guests which boot kernel images directly or from minimal ramdisks, Windows 11 on ARM64 boots strictly via **UEFI firmware (AAVMF / EDK2)** presenting **ACPI tables**. Storage and networking operate over high-performance paravirtualized VirtIO devices.

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
| Windows 11 ARM64 HVM Guest (EL1 / EL0)         :                :             |
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
+-------------------------------------------------------------------------------+
```

---

## 2. Resource Specifications (Lean Demo Profile)

| Resource | Value | Sizing Rationale |
|---|---|---|
| **vCPUs** | 2 | Responsive desktop UI rendering without starving host cores. |
| **RAM** | 3072 MiB (3 GiB) | ~1.1 GB idle footprint; leaves ~1.9 GB for demo applications. |
| **Virtual Disk** | 20 GiB | CompactOS + disabled hibernation drops clean install to ~8 GB. |
| **vTPM 2.0** | Enabled (`disableVTPM: false`) | Satisfies Windows 11 hardware integrity checks via `swtpm`. |
| **Display & Remote Access** | RDP (Port 3389) + VNC | High frame-rate Remote Desktop + emergency VNC console. |
| **Default User** | `windows` / `windows` | AutoLogon enabled for instant console & VNC display. |

---

## 3. Directory Layout

```text
guest-os/windows/
├── README.md                           # This document
└── windows-build/                      # Cloud image pipeline
    ├── Autounattend.xml                # Unattended specialize/OOBE answer file
    ├── firstboot.cmd                   # First-logon RDP and firewall setup
    ├── optimize.ps1                    # Lean optimization script
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
1. `Win11_25H2_English_Arm64_v2.iso` (Official Microsoft Windows 11 ARM64 media)
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
1. Apply `install.wim` to an NTFS volume with `wimapply --strict-acls`.
2. Inject `unattend.xml`, first-logon scripts, and VirtIO driver media.
3. Boot a short WinPE pass under TCG to run Microsoft DISM (`/Add-Driver`)
   and `bcdboot` against the real ESP.
4. Compress the result to `windows-11-arm64-cloudimg.qcow2`.

---

## 5. Related documentation

- [`automation/doc/WindowsEdgeAppHVM.md`](../../automation/doc/WindowsEdgeAppHVM.md) — cloud image, edge-app, and RDP deployment
- [`automation/doc/WindowsPortingDetails.md`](../../automation/doc/WindowsPortingDetails.md) — boot, DISM, BCD, and specialize log

---

## 6. Deployment with `zcli`

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
