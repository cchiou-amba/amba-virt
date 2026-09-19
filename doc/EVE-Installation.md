# EVE Installation Guide for Ambarella N1-655

This document details the complete end-to-end installation procedure for Edge
Virtualization Engine (EVE) on Ambarella N1-655 platforms arriving with blank or
unformatted eMMC flash storage, assuming a functional bootloader.

Related documents:
- [Architecture.md](Architecture.md) (Platform architecture and I/O virtualization)
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (Hardware models and Cloud inventory)
- [EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md) (OTA firmware updates)

---

## 1. Scope & System Overview

This installation procedure deploys EVE-OS onto internal eMMC storage. Upon
completion, the board operates autonomously with:
- Das U-Boot bootloader residing persistently in hardware boot partition 0
  (`/dev/mmcblk0boot0`).
- Persistent MAC address stored in the bootloader environment partition.
- Unified Extensible Firmware Interface (UEFI) execution path utilizing
  standard ARM64 `BOOTAA64.EFI` (GRUB 2).
- Native device tree preservation passing hardware peripheral configurations
  directly to Dom0 LinuxKit kernel.
- EVE partition scheme on user data eMMC (`/dev/mmcblk0`), including EFI System
  (partition 1), system rootfs image (`IMGA`, partition 2), device configuration
  (`CONFIG`, partition 4), and dynamically expanded persistent storage (`/persist`,
  partition 3).

### Supported Target Platforms

| Hardware Model | Ambarella SoC | System Architecture | Memory | Primary Storage | Hardware Validation Status |
|---|---|---|---|---|---|
| **N1-655-Cooper-Devkit** | CV3AD655 | 4× ARM Cortex-A78 | 32 GB LPDDR5 | eMMC 5.1 (User Data Area) | Hardware-Validated |
| **N1-655-Cooper-Pro** | CV3AD655 | 4× ARM Cortex-A78 | 32 GB LPDDR5 | eMMC 5.1 (User Data Area) | Target Compatible (Validation Separate) |

> [!NOTE]
> This end-to-end installation procedure has been hardware-validated on the
> **N1-655-Cooper-Devkit** platform only. While the N1-655-Cooper-Pro shares the
> CV3AD655 SoC architecture, Cooper Pro deployment and validation are conducted
> under a separate dedicated procedure. Do not claim or assume Cooper Pro hardware
> validation from this guide.

---

## 2. Host Prerequisites & Setup

The build and staging environment requires an x86_64 or ARM64 Linux workstation
equipped with cross-compilation toolchains and standard utilities.

### 2.1 Toolchain & Package Installation

On Debian or Ubuntu systems:

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    bison \
    flex \
    device-tree-compiler \
    libssl-dev \
    tftpd-hpa \
    gdisk \
    curl \
    git
```

### 2.2 Repository Checkout with Submodules

Clone the repository recursively to populate required third-party and kernel
trees:

```bash
git clone --recursive <repository-url> amba-virt
cd amba-virt
git submodule update --init --recursive
```

---

## 3. Building Bootloader & EVE-OS Artifacts

All firmware and operating system components build through standardized
top-level wrapper targets.

### 3.1 Build U-Boot Bootloader

Compile the bootloader binary for Ambarella N1-655:

```bash
make u-boot
```

- **Output Binary**: `build/bin/u-boot.bin`
- **Maximum Allocated Size**: 3 MiB (`0x300000` bytes)

Package the bootloader container if required for packaging pipelines:

```bash
make u-boot-pkg
```

- **Output Container**: `build/firmware/bld.img`

### 3.2 Build EVE-OS Image

Compile the EVE-OS image using the containerized LinuxKit toolchain:

```bash
make eve
```

Upon build completion, the deployable raw disk image is located under the
distribution directory:
- **Raw Disk Image**: `eve/dist/arm64/current/live.raw`
- **Approximate Size**: ~591 MiB (~619 MB)

---

## 4. Network Staging & Bootloader Setup

### 4.1 Stage Artifacts on TFTP Server

Copy the compiled bootloader binary and disk image to your TFTP server root
directory (typically `/var/lib/tftpboot`):

```bash
sudo cp build/bin/u-boot.bin /var/lib/tftpboot/u-boot.bin
sudo cp eve/dist/arm64/current/live.raw /var/lib/tftpboot/live.raw
sudo chmod 644 /var/lib/tftpboot/u-boot.bin /var/lib/tftpboot/live.raw
```

Verify the staged file sizes and checksums:

```bash
ls -lh /var/lib/tftpboot/u-boot.bin /var/lib/tftpboot/live.raw
sha256sum /var/lib/tftpboot/u-boot.bin /var/lib/tftpboot/live.raw
```

### 4.2 Connect Serial Console & Stop Autoboot

Connect a terminal emulator to the board's primary UART console (115200 baud,
8N1, no hardware flow control). Power on or reset the board.

When the countdown banner appears, press any key to enter the bootloader prompt:

```text
Hit any key to stop autoboot:  0
=>
```

### 4.3 Configure Bootloader Networking

Acquire an IP lease from your local network via DHCP and designate the TFTP
server:

```text
=> setenv autoload no
=> dhcp
=> setenv serverip <server-ip>
```

Verify that the bootloader can reach the TFTP server by pinging the host or
verifying DHCP output (`printenv ipaddr netmask gatewayip serverip`).

---

## 5. Provisioning Persistent Hardware Identity (MAC Address)

Ambarella N1-655 bootloaders store persistent network identifiers in the
dedicated environment partition on hardware boot partition 0 (`boot0`).

### 5.1 Query Current MAC Address

```text
=> mac show
Active ethaddr: <mac-address>
```

### 5.2 Set Board-Specific MAC Address

Assign the unique hardware MAC address assigned to your board:

```text
=> mac set <mac-address>
```

> [!IMPORTANT]
> The `mac set` command validates that the address format is valid and non-multicast,
> and immediately commits it to persistent eMMC boot0 storage. This identity
> survives firmware updates, OS reinstallations, and power cycles.

If an uninitialized board boots without a provisioned MAC address, the bootloader
automatically generates a locally administered address (LAA) on first startup and
commits it to persistent storage.

---

## 6. Installing Persistent Bootloader to eMMC Hardware Boot Partition 0 (boot0)

To achieve autonomous execution across power cycles, the compiled Das U-Boot
binary (`u-boot.bin`) must be written persistently to the dedicated hardware boot
partition 0 (`boot0` / `/dev/mmcblk0boot0`).

On Ambarella N1-655 platforms, `boot0` spans 4 MiB (8,192 sectors of 512 bytes):
- Sector `0x000`–`0x0ff` (0–128 KB): Bootstrap loader (`bst`)
- Sector `0x100`–`0x18ff` (128 KB–3 MiB): Bootloader (`bld` / `u-boot.bin`)
- Sector `0x1900`–`0x19ff` (3.125–3.25 MiB): RTOS container (`rtos`)
- Sector `0x1a00`–`0x1aff` (3.25–3.375 MiB): Persistent U-Boot environment (`env`)

> [!IMPORTANT]
> Writing outside the allocated `bld` sector range risks corrupting the primary
> bootstrap (`bst`) or persistent environment (`env`). Follow the backup, exact-size
> calculation, and readback verification procedure strictly.

### 6.1 Download `u-boot.bin` into DRAM Buffer

From the U-Boot console (`=>`), download the raw bootloader image into DRAM:

```text
=> tftp 0x10000000 u-boot.bin
```

Verify that the transfer succeeded and record the transferred byte count:
```text
Bytes transferred = <payload-bytes> (<payload-bytes-hex> hex)
```

> [!CAUTION]
> The bootloader binary must not exceed the 3 MiB (`0x300000` bytes) allocation.
> If `<payload-bytes>` exceeds 3,145,728 bytes (`0x300000` hex), STOP immediately;
> do not attempt to flash.

### 6.2 Switch to Hardware Boot Partition 0

Select MMC device 0, hardware boot partition 1 (`boot0`):

```text
=> mmc dev 0 1
switch to partitions #1, OK
mmc0(part 1) is current device
```

### 6.3 Backup Existing `boot0` to DRAM & Record CRC

Preserve the entire 4 MiB `boot0` partition (`0x2000` sectors of 512 bytes) in DRAM
at `0x30000000` and compute its CRC32 checksum:

```text
=> mmc read 0x30000000 0 0x2000
MMC read: dev # 0, block # 0, count 8192 ... 8192 blocks read: OK

=> crc32 0x30000000 0x400000
crc32 for 30000000 ... 303fffff ==> <original-boot0-crc32>
```

Record `<original-boot0-crc32>`.

> [!WARNING]
> This DRAM backup is volatile and will be lost if the board is reset or power-cycled.
> In the event of any write or comparison discrepancy, restore `boot0` immediately
> before issuing any reset or reboot command.

### 6.4 Compute Payload Sector Count

Calculate the number of 512-byte sectors required for the payload:

$$\text{Payload Sectors (Hex)} = \left\lceil \frac{\text{Bytes Transferred}}{512} \right\rceil$$

*(In hexadecimal: divide `<payload-bytes-hex>` by `0x200`, rounding up to the next integer sector).*

### 6.5 Write `u-boot.bin` into the BLD Slot

Write the payload into `boot0` starting strictly at sector `0x100` (128 KB offset):

```text
=> mmc write 0x10000000 0x100 <payload-sectors-hex>
MMC write: dev # 0, block # 256, count <payload-sectors> ... <payload-sectors> blocks written: OK
```

### 6.6 Read Back and Verify Bit-for-Bit Equality

Read back the flashed sectors to an alternate DRAM buffer (`0x20000000`) and compare
the exact byte count with the source payload in DRAM:

```text
=> mmc read 0x20000000 0x100 <payload-sectors-hex>
MMC read: dev # 0, block # 256, count <payload-sectors> ... <payload-sectors> blocks read: OK

=> cmp.b 0x10000000 0x20000000 <payload-bytes-hex>
Total of <payload-bytes> byte(s) were the same
```

Verification requires an exact byte-for-byte match (`Total of <payload-bytes> byte(s) were the same`).

### 6.7 Recovery Procedure on Write or Compare Failure

If any error occurs during write, readback, or comparison, immediately restore the
complete original `boot0` image from the DRAM backup before resetting the board:

```text
# Restore original full boot0 partition
=> mmc write 0x30000000 0 0x2000
MMC write: dev # 0, block # 0, count 8192 ... 8192 blocks written: OK

# Read back restored boot0 to verify integrity
=> mmc read 0x20000000 0 0x2000
MMC read: dev # 0, block # 0, count 8192 ... 8192 blocks read: OK

# Verify CRC matches the original recorded CRC
=> crc32 0x20000000 0x400000
crc32 for 20000000 ... 203fffff ==> <original-boot0-crc32>
```

Verify that the CRC matches `<original-boot0-crc32>` before taking any further action.

### 6.8 Return to User Data Area

Once write and comparison verify successfully, switch back to the eMMC user data
partition (device 0, partition 0):

```text
=> mmc dev 0 0
switch to partitions #0, OK
mmc0(part 0) is current device
```

Verify that the persistent environment remains intact:
```text
=> printenv ethaddr bootcmd
```

---

## 7. Installing `live.raw` to eMMC User Storage

### 7.1 Download `live.raw` into DRAM Buffer

Load the raw disk image from the TFTP server into the system load address:

```text
=> tftp 0x10000000 live.raw
```

The console outputs the byte count upon transfer completion:
```text
Bytes transferred = <bytes-transferred> (<bytes-hex> hex)
```

### 7.2 Compute Sector Block Count

eMMC storage uses 512-byte (`0x200` hex) sectors. Calculate the exact block count:

$$\text{Block Count (Hex)} = \left\lceil \frac{\text{Bytes Transferred}}{512} \right\rceil$$

*(In hexadecimal: divide `<bytes-hex>` by `0x200`, rounding up to the next integer sector).*

### 7.3 Write Image to eMMC User Block 0

Ensure the device is set to the eMMC user data partition (device 0, partition 0)
and write the loaded image:

```text
=> mmc dev 0 0
switch to partitions #0, OK
mmc0(part 0) is current device

=> mmc write 0x10000000 0 <block-count-hex>
```

Wait until the write operation confirms all blocks were written:
```text
MMC write: dev # 0, block # 0, count <blocks> ... <blocks> blocks written: OK
```

### 7.4 Verify Partition Layout & EFI Filesystem

Confirm that the GPT partition table was initialized properly:

```text
=> part list mmc 0
```

**Expected Partitions:**
```text
Partition Map for MMC device 0  --   Partition Type: EFI

Part    Start LBA       End LBA         Name
  1     0x00000800      0x000207ff      "EFI System"
  2     0x00023000      0x00122fff      "IMGA"
  4     0x00020800      0x00022fff      "CONFIG"
```

> [!NOTE]
> The initial raw disk image populates Partition 1 (`EFI System`), Partition 2 (`IMGA`),
> and Partition 4 (`CONFIG`). Do not expect Partition 3 (`/persist`) or an `IMGB` partition
> at this stage. Partition 3 is dynamically created and formatted across the remaining
> available eMMC capacity during first boot by the EVE `storage-init` service. Standby
> partition `IMGB` is allocated subsequently during controller-driven OTA updates.

Verify that the UEFI bootloader exists on Partition 1:

```text
=> ls mmc 0:1 /EFI/BOOT/
   1032192   BOOTAA64.EFI
     24381   grub.cfg
```

---

## 8. Configuring Persistent Boot Command & First Boot

### 8.1 Configure Boot Environment

Program the persistent UEFI boot command sequence into bootloader storage:

```text
=> setenv kernel_addr_r 0x10000000
=> setenv bootcmd_efi 'mmc dev 0 0; load mmc 0:1 ${kernel_addr_r} /EFI/BOOT/BOOTAA64.EFI && bootefi ${kernel_addr_r}'
=> setenv bootcmd 'run bootcmd_efi'
=> saveenv
```

Verify the saved environment:

```text
=> printenv bootcmd bootcmd_efi kernel_addr_r
```

### 8.2 Boot EVE-OS

Initiate the boot sequence:

```text
=> run bootcmd
```

---

## 9. First Boot Expectations & System Verification

### 9.1 First Boot Timeline

During first boot, EVE-OS performs critical one-time provisioning:
1. **UEFI / GRUB Execution (0–5 seconds)**: GRUB loads and transfers control to
   the LinuxKit kernel.
2. **Silent Early Kernel Boot (5–40 seconds)**: Dom0 platform tweaks suppress
   unnecessary kernel verbosity (`quiet loglevel=3`). The console remains silent
   while the kernel initializes hardware and secondary CPU cores.
3. **Storage Expansion (40–70 seconds)**: The `storage-init` service formats
   the remaining unallocated capacity on `/dev/mmcblk0` as Partition 3 (`/persist`),
   allocating the remaining available eMMC capacity as persistent data volume.
4. **Service Startup (70–90 seconds)**: System services (containerd shims,
   pillar, monitor, newlogd) initialize.
5. **Console Prompt (90–120 seconds)**: The LinuxKit getty login banner appears:

```text
              Edge Virtualization Engine
linuxkit-<mac-suffix> login: root (automatic login)

EVE is Edge Virtualization Engine

Take a look around and don't forget to use eve(1).
linuxkit-<mac-suffix>:~#
```

### 9.2 Initial Observational Verification (Serial Console & Host Network)

Initial node verification is strictly observational. Do not execute interactive Linux
shell commands or configuration scripts over the serial console.

1. **Serial Console Observation**:
   Verify that the boot sequence completes cleanly to the automatic root login
   prompt without kernel panics, CPU initialization timeouts, or GRUB syntax errors.
   Once the prompt `linuxkit-<mac-suffix>:~#` appears, early boot is complete.

2. **Host Network Reachability & Identity Check**:
   From an administrative workstation connected to the same local network subnet,
   verify network reachability and confirm that the active ARP entry matches the
   provisioned hardware MAC address:

   ```bash
   ping -c 3 <node-ip>
   arp -an | grep <node-ip>
   ```

   **Expected Output:**
   - 0% packet loss on ping.
   - The ARP table associates `<node-ip>` with the provisioned `<mac-address>`.

### 9.3 Post-Onboarding Remote Management Health Checks

Once the node has been onboarded to a controller or when accessing the system via an
authorized remote management shell, perform comprehensive system health verification:

1. **Verify Symmetric Multiprocessing (All 4 Cores Online)**:
   ```bash
   cat /sys/devices/system/cpu/online
   # Expected output: 0-3

   nproc --all
   # Expected output: 4
   ```

2. **Verify Network Identity**:
   ```bash
   cat /sys/class/net/eth0/address
   # Must match your assigned <mac-address>
   ```

3. **Verify Persistent Storage Allocation**:
   ```bash
   df -h /persist
   # Verifies that /persist is mounted and utilizes remaining available eMMC capacity
   ```

4. **Verify EVE Daemon Status**:
   ```bash
   eve status
   ```

---

## 10. Troubleshooting

| Symptom | Probable Cause | Corrective Action |
|---|---|---|
| `Card did not respond to voltage select` during boot | Slot enumeration probe on unpopulated SD card slot 1 | Normal hardware probing behavior; U-Boot automatically falls through to eMMC slot 0. |
| `** Unrecognized filesystem type **` on LBA 0 | `live.raw` was written to wrong block or truncated | Verify `block-count-hex` calculation; ensure write starts at block `0`. |
| Console silent for > 60s after `Booting Boot...` | Normal production quiet boot | Wait at least 90–120s for `storage-init` to format `/persist` and launch getty. |
| Network does not obtain DHCP address in EVE | PHY link negotiation delay or switch port portfast disabled | Check physical Ethernet link LED; ensure Ethernet cable is in primary RJ45 port. |

---

## 11. Next Steps: Controller Onboarding & Application Provisioning

Once the base operating system is installed and verified healthy, the edge node is
ready for controller enrollment. Controller onboarding (registering the node with
ZEDEDA Cloud or an open-source EVE controller) and edge application container
deployment are covered in:
- [Architecture.md](Architecture.md)
- [EVE-EdgeApp-Provision.md](EVE-EdgeApp-Provision.md)
- [EVE-Create-NOHYPER-EdgeApp-Instance.md](EVE-Create-NOHYPER-EdgeApp-Instance.md)
- [EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md)
