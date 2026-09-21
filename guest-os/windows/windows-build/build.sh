#!/usr/bin/env bash
#
# build.sh - High-speed containerized builder for Windows 11 ARM64 HVM on EVE-OS
# Author: Ambarella Edge Virtualization
#
# Builds a production-ready Windows 11 ARM64 cloud image using native WIM
# extraction followed by a short Microsoft WinPE servicing boot that stages the
# VirtIO packages with DISM and writes the UEFI boot files with bcdboot.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"
DIST_DIR="${OUTPUT_DIR}/dist"
TMP_DIR="${ROOT_DIR}/build/tmp"

# Candidate Windows ARM64 ISO media paths (in priority order)
CANDIDATE_ISOS=(
    "${WIN_ISO:-}"
    "${ROOT_DIR}/build/iso/Windows_11_IoT_Enterprise_LTSC_ARM64.iso"
    "${ROOT_DIR}/build/iso/Win11_IoT_Enterprise_LTSC_ARM64.iso"
    "${ROOT_DIR}/build/iso/Win11_IoT_Enterprise_Arm64.iso"
    "${ROOT_DIR}/build/iso/Win11_25H2_English_Arm64_v2.iso"
    "${ROOT_DIR}/build/iso/Win11_24H2_English_Arm64.iso"
)

WIN_ISO=""
for candidate in "${CANDIDATE_ISOS[@]}"; do
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        WIN_ISO="$candidate"
        break
    fi
done

export WIN_ISO
VIRTIO_ISO="${ROOT_DIR}/build/iso/virtio-win.iso"
INSTALL_WIM="${TMP_DIR}/install.wim"
AUTOUNATTEND_XML="${SCRIPT_DIR}/Autounattend.xml"
OPTIMIZE_PS1="${SCRIPT_DIR}/optimize.ps1"
FINAL_RAW="${OUTPUT_DIR}/win11-disk.raw"
FINAL_QCOW2="${DIST_DIR}/windows-11-arm64-cloudimg.qcow2"

echo "=========================================================="
echo " Building Lean Windows 11 ARM64 HVM Cloud Image for EVE-OS"
echo " (Windows 11 IoT Enterprise LTSC / Direct Assembly & DISM)"
echo "=========================================================="

# 1. Prerequisite verification
mkdir -p "$OUTPUT_DIR" "$DIST_DIR" "$TMP_DIR"

if [ -z "$WIN_ISO" ] || [ ! -f "$WIN_ISO" ]; then
    echo "Error: Windows 11 ARM64 ISO not found."
    echo "Please place Windows_11_IoT_Enterprise_LTSC_ARM64.iso or Win11_25H2_English_Arm64_v2.iso in build/iso/"
    exit 1
fi
echo "[*] Using Windows ISO: $(basename "$WIN_ISO")"

if [ ! -f "$VIRTIO_ISO" ]; then
    echo "Error: VirtIO ISO not found at $VIRTIO_ISO"
    exit 1
fi

ISO_STAMP="${TMP_DIR}/.iso_source"
if [ ! -f "$INSTALL_WIM" ] || [ ! -f "$ISO_STAMP" ] || [ "$(cat "$ISO_STAMP" 2>/dev/null)" != "$WIN_ISO" ] || [ "$WIN_ISO" -nt "$INSTALL_WIM" ]; then
    echo "[*] Extracting install.wim from $(basename "$WIN_ISO")..."
    rm -f "$INSTALL_WIM" "$ISO_STAMP"
    rm -rf "${TMP_DIR}/wim_extract_tmp"
    7z e -y -o"${TMP_DIR}/wim_extract_tmp" "$WIN_ISO" sources/install.wim
    mv "${TMP_DIR}/wim_extract_tmp/install.wim" "$INSTALL_WIM"
    rm -rf "${TMP_DIR}/wim_extract_tmp"
    echo "$WIN_ISO" > "$ISO_STAMP"
fi

# Ensure Docker builder image is available
if ! docker image inspect win11-builder:latest >/dev/null 2>&1; then
    echo "[*] Building local win11-builder container..."
    docker build -t win11-builder:latest -f "${SCRIPT_DIR}/Dockerfile.builder" "${SCRIPT_DIR}"
fi

# 2. Create and Populate Target Disk (20 GiB raw)
echo "[*] Creating 20 GiB GPT Target Disk..."
rm -f "$FINAL_RAW" "${TMP_DIR}/windows.ntfs"
truncate -s 20G "$FINAL_RAW"
parted -s "$FINAL_RAW" mklabel gpt
# Sector 2048 to 534527 = 260 MiB (System ESP)
parted -s "$FINAL_RAW" mkpart System fat32 2048s 534527s
parted -s "$FINAL_RAW" set 1 esp on
# Sector 534528 to 567295 = 16 MiB (MSR)
parted -s "$FINAL_RAW" mkpart MSR 261MiB 277MiB
parted -s "$FINAL_RAW" set 2 msftres on
# Sector 567296 to 41940991 = ~19.7 GiB (Windows NTFS)
parted -s "$FINAL_RAW" mkpart Windows ntfs 567296s 41940991s

# Write 32-bit MBR Disk Signature (offset 440)
echo "[*] Injecting MBR Disk Signature into Sector 0..."
printf '\x56\x49\x4f\x53' | dd of="$FINAL_RAW" bs=1 seek=440 count=4 conv=notrunc

# Format Partition 1 as an empty ESP. Microsoft bcdboot populates it with the
# signed boot files and a genuine system BCD store during WinPE servicing.
ESP_IMG="${TMP_DIR}/esp.img"
echo "[*] Formatting empty 260 MiB UEFI System Partition..."
rm -f "$ESP_IMG"
truncate -s 260M "$ESP_IMG"
mkfs.vfat -F 32 -n "System" "$ESP_IMG"
dd if="$ESP_IMG" of="$FINAL_RAW" bs=1M seek=1 count=260 conv=notrunc
rm -f "$ESP_IMG"

# Format Partition 3 as NTFS in container, extract WIM, inject scripts & drivers
NTFS_SECTORS=$(( 41940991 - 567296 + 1 ))
NTFS_BYTES=$(( NTFS_SECTORS * 512 ))

echo "[*] Formatting NTFS Partition (${NTFS_SECTORS} sectors, $(( NTFS_BYTES / 1024 / 1024 )) MiB)..."
truncate -s "$NTFS_BYTES" "${TMP_DIR}/windows.ntfs"

docker run --rm --privileged -v "${ROOT_DIR}:/work" win11-builder:latest bash -c '
set -euo pipefail
mkfs.ntfs -F -f -C -L "Windows" -p 567296 -H 255 -S 63 -s 512 /work/build/tmp/windows.ntfs

WIM_PATH="/work/build/tmp/install.wim"
TARGET_INDEX=1
if wiminfo "$WIM_PATH" | grep -qi "IoT Enterprise"; then
    TARGET_INDEX=$(wiminfo "$WIM_PATH" | grep -B1 -i "Name:.*IoT Enterprise" | grep "^Index:" | head -n1 | tr -s " " | cut -d" " -f2)
    echo "[*] Detected Windows IoT Enterprise image at Index $TARGET_INDEX"
elif wiminfo "$WIM_PATH" | grep -qi "Pro"; then
    TARGET_INDEX=$(wiminfo "$WIM_PATH" | grep -B1 -i "Name:.*Pro" | grep "^Index:" | head -n1 | tr -s " " | cut -d" " -f2)
    echo "[*] Detected Windows Pro image at Index $TARGET_INDEX"
else
    echo "[*] Using default image Index $TARGET_INDEX"
fi

echo "[*] Applying Windows image (Index $TARGET_INDEX) directly to unmounted NTFS..."
wimapply "$WIM_PATH" "$TARGET_INDEX" /work/build/tmp/windows.ntfs \
    --include-invalid-names --strict-acls

mkdir -p /mnt/ntfs
ntfs-3g /work/build/tmp/windows.ntfs /mnt/ntfs

echo "[*] Injecting unattend.xml, firstboot, and lean optimization scripts..."
mkdir -p /mnt/ntfs/Windows/Panther /mnt/ntfs/Windows/Setup/Scripts /mnt/ntfs/Windows/System32/sysprep
cp /work/guest-os/windows/windows-build/Autounattend.xml /mnt/ntfs/Windows/Panther/unattend.xml
cp /work/guest-os/windows/windows-build/Autounattend.xml /mnt/ntfs/Windows/System32/sysprep/unattend.xml
cp /work/guest-os/windows/windows-build/optimize.ps1 /mnt/ntfs/Windows/Setup/Scripts/optimize.ps1
cp /work/guest-os/windows/windows-build/firstboot.cmd /mnt/ntfs/Windows/Setup/Scripts/firstboot.cmd
mkdir -p "/mnt/ntfs/ProgramData/Microsoft/Windows/Start Menu/Programs/StartUp"
cp /work/guest-os/windows/windows-build/firstboot.cmd "/mnt/ntfs/ProgramData/Microsoft/Windows/Start Menu/Programs/StartUp/firstboot.cmd"

echo "[*] Extracting VirtIO ARM64 drivers and guest tools..."
mkdir -p /mnt/ntfs/Drivers/VirtIO
7z x -y -o/mnt/ntfs/Drivers/VirtIO /work/build/iso/virtio-win.iso \
    viostor/w11/ARM64/* \
    NetKVM/w11/ARM64/* \
    viogpudo/w11/ARM64/* \
    Balloon/w11/ARM64/* \
    pvpanic/w11/ARM64/* \
    vioserial/w11/ARM64/* \
    virtio-win-guest-tools.exe
cp /mnt/ntfs/Drivers/VirtIO/virtio-win-guest-tools.exe /mnt/ntfs/Windows/Setup/Scripts/ 2>/dev/null || true

sync
umount /mnt/ntfs
'

echo "[*] Assembling NTFS partition into 20 GiB raw disk image..."
dd if="${TMP_DIR}/windows.ntfs" of="$FINAL_RAW" bs=1M seek=277 conv=notrunc status=progress
rm -f "${TMP_DIR}/windows.ntfs"

# 4. Service the image with genuine Microsoft tooling in a one-shot WinPE boot.
# DISM stages the signed VirtIO packages and bcdboot writes the UEFI boot files
# and a real system BCD store. Nothing here hand-edits Microsoft state: no
# synthesized DriverDatabase entries, no offline BCD patching, and no clearing
# of the pristine first-boot setup state that drives windeploy.exe.
"${SCRIPT_DIR}/service_with_dism.sh" "$FINAL_RAW"

# 5. Compress and package the final cloud image
echo "[*] Converting and compressing to production cloud image: $FINAL_QCOW2..."
qemu-img convert -c -f raw -O qcow2 "$FINAL_RAW" "$FINAL_QCOW2"

# 6. Checksums and Summary
IMG_SIZE=$(stat -c %s "$FINAL_QCOW2")
IMG_SHA=$(sha256sum "$FINAL_QCOW2" | awk '{print $1}')

echo ""
echo "=========================================================="
echo " Windows 11 ARM64 HVM Cloud Image Ready!"
echo "=========================================================="
echo " Master Image : $FINAL_RAW"
echo " Cloud Image  : $FINAL_QCOW2"
echo " Image Size   : $IMG_SIZE bytes ($(( IMG_SIZE / 1024 / 1024 )) MiB)"
echo " SHA-256      : $IMG_SHA"
echo "=========================================================="
echo ""
echo "Ready for uplink to ZedControl datastore."
