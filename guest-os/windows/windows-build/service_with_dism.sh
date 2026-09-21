#!/usr/bin/env bash
#
# Service an offline Windows ARM64 disk with genuine Microsoft DISM and bcdboot.
#
# The servicing environment is the Microsoft Windows Setup image from the
# original ARM64 ISO.  It boots briefly under TCG, sees the target through
# its inbox NVMe driver, stages the signed VirtIO packages, writes the UEFI
# boot files and system BCD store with bcdboot, and shuts down.
#
# bcdboot must create the store.  An offline-patched BCD is rejected by
# spbcd.dll during the specialize pass ("File is not system store",
# STATUS_FILE_INVALID), which aborts Windows Setup.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
TMP_DIR="${ROOT_DIR}/build/tmp/dism-service"
TARGET_RAW="${1:-}"

# Candidate Windows ARM64 ISO media paths
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

VIRTIO_ISO="${ROOT_DIR}/build/iso/virtio-win.iso"

if [ -z "$TARGET_RAW" ] || [ ! -f "$TARGET_RAW" ]; then
    echo "Usage: $0 <windows-disk.raw>" >&2
    exit 2
fi

for image in "$WIN_ISO" "$VIRTIO_ISO"; do
    if [ -z "$image" ] || [ ! -f "$image" ]; then
        echo "Error: required image not found: $image" >&2
        exit 1
    fi
done

AAVMF_CODE=""
for candidate in \
    /usr/share/AAVMF/AAVMF_CODE.no-secboot.fd \
    /usr/share/AAVMF/AAVMF_CODE.fd \
    /usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
    if [ -f "$candidate" ]; then
        AAVMF_CODE="$candidate"
        break
    fi
done

AAVMF_VARS=""
for candidate in \
    /usr/share/AAVMF/AAVMF_VARS.fd \
    /usr/share/AAVMF/AAVMF_VARS.ms.fd; do
    if [ -f "$candidate" ]; then
        AAVMF_VARS="$candidate"
        break
    fi
done

if [ -z "$AAVMF_CODE" ] || [ -z "$AAVMF_VARS" ]; then
    echo "Error: AAVMF firmware is not installed." >&2
    exit 1
fi

rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR/pe-files/sources"

BOOT_WIM="$TMP_DIR/boot.wim"
STARTNET="$TMP_DIR/startnet.cmd"
WINPESHL="$TMP_DIR/winpeshl.ini"
PE_ESP="$TMP_DIR/esp.img"
PE_DISK="$TMP_DIR/dism-service.img"
STATUS_DISK="$TMP_DIR/dism-status.img"
STATUS_FS="$TMP_DIR/dism-status.fat"
VARS_COPY="$TMP_DIR/vars.fd"

echo "[*] Extracting pristine Microsoft Windows Setup image..."
7z e -so "$WIN_ISO" sources/boot.wim > "$BOOT_WIM"

cat > "$STARTNET" <<'EOF'
@echo off
echo [*] Windows PE Servicing Environment Starting...
wpeinit

echo [*] Scanning for DISM status storage volume...
set LOG_DRV=
for /l %%i in (1,1,30) do (
    if not defined LOG_DRV (
        for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
            if not defined LOG_DRV if exist %%d:\dism-service.tag set LOG_DRV=%%d:
        )
        if not defined LOG_DRV (
            >X:\dp-scan.txt echo rescan
            diskpart /s X:\dp-scan.txt >nul 2>&1
            ping -n 2 127.0.0.1 >nul 2>&1
        )
    )
)

if not defined LOG_DRV (
    echo [ERROR] Status volume not found, shutting down.
    wpeutil shutdown
    exit /b 10
)

set LOG=%LOG_DRV%\dism-status.txt
echo STARTED>%LOG%
echo [*] Status volume mounted at %LOG_DRV%

set WIN_DRV=
for /l %%i in (1,1,15) do (
    if not defined WIN_DRV (
        for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
            if not defined WIN_DRV if exist %%d:\Windows\System32\Config\SYSTEM set WIN_DRV=%%d:
        )
        if not defined WIN_DRV ping -n 2 127.0.0.1 >nul 2>&1
    )
)
if not defined WIN_DRV (
    echo ERROR: Windows volume not found>>%LOG%
    wpeutil shutdown
    exit /b 11
)
echo Windows volume: %WIN_DRV%>>%LOG%
echo [*] Target Windows volume found at %WIN_DRV%

set VIRTIO_DRV=
for /l %%i in (1,1,15) do (
    if not defined VIRTIO_DRV (
        for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
            if not defined VIRTIO_DRV if exist %%d:\viostor\w11\ARM64\viostor.inf set VIRTIO_DRV=%%d:
        )
        if not defined VIRTIO_DRV ping -n 2 127.0.0.1 >nul 2>&1
    )
)
if not defined VIRTIO_DRV (
    echo ERROR: VirtIO driver media not found>>%LOG%
    wpeutil shutdown
    exit /b 12
)
echo VirtIO media: %VIRTIO_DRV%>>%LOG%
echo [*] VirtIO driver media found at %VIRTIO_DRV%

echo [*] Staging VirtIO ARM64 packages with DISM in one-shot batch pass...
echo Staging VirtIO ARM64 drivers with DISM...>>%LOG%
dism.exe /Image:%WIN_DRV%\ /ScratchDir:X:\Windows\Temp /Add-Driver /Driver:"%VIRTIO_DRV%\viostor\w11\ARM64\viostor.inf" /Driver:"%VIRTIO_DRV%\NetKVM\w11\ARM64\netkvm.inf" /Driver:"%VIRTIO_DRV%\viogpudo\w11\ARM64\viogpudo.inf" /Driver:"%VIRTIO_DRV%\Balloon\w11\ARM64\balloon.inf" /Driver:"%VIRTIO_DRV%\pvpanic\w11\ARM64\pvpanic.inf" /Driver:"%VIRTIO_DRV%\vioserial\w11\ARM64\vioser.inf"
if errorlevel 1 (
    echo [ERROR] DISM returned error code %errorlevel%>>%LOG%
) else (
    echo [SUCCESS] DISM completed successfully>>%LOG%
)

echo [*] Assigning drive letter P: to UEFI System Partition...
set WIN_LETTER=%WIN_DRV:~0,1%
>X:\dp-esp.txt echo select volume %WIN_LETTER%
>>X:\dp-esp.txt echo select partition 1
>>X:\dp-esp.txt echo assign letter=P noerr
>>X:\dp-esp.txt echo list volume
diskpart /s X:\dp-esp.txt>%LOG_DRV%\diskpart.txt 2>&1
type %LOG_DRV%\diskpart.txt>>%LOG%
if not exist P:\ (
    echo ERROR: target ESP not assigned>>%LOG%
    wpeutil shutdown
    exit /b 13
)

echo [*] Writing genuine UEFI boot files with bcdboot...
echo Writing UEFI boot files with bcdboot>>%LOG%
bcdboot.exe %WIN_DRV%\Windows /s P: /f UEFI>%LOG_DRV%\bcdboot.txt 2>&1
type %LOG_DRV%\bcdboot.txt>>%LOG%

echo COMPLETE>>%LOG%
echo [*] Servicing complete! Shutting down guest...
wpeutil shutdown
exit /b 0
EOF
unix2dos "$STARTNET" >/dev/null 2>&1 || sed -i -e 's/$/\r/' "$STARTNET"

cat > "$WINPESHL" <<'EOF'
[LaunchApps]
%SystemRoot%\System32\cmd.exe, /c %SystemRoot%\System32\startnet.cmd
EOF
unix2dos "$WINPESHL" >/dev/null 2>&1 || sed -i -e 's/$/\r/' "$WINPESHL"

echo "[*] Configuring Microsoft Windows Setup indices 1 and 2 for one-shot servicing..."
docker run --rm \
    -v "${ROOT_DIR}:/work" \
    win11-builder:latest \
    bash -c '
        set -e
        # Update both index 1 (WinPE) and index 2 (Windows Setup)
        for idx in 1 2; do
            if wiminfo /work/build/tmp/dism-service/boot.wim "$idx" >/dev/null 2>&1; then
                wimupdate /work/build/tmp/dism-service/boot.wim "$idx" \
                    --command="add /work/build/tmp/dism-service/startnet.cmd /Windows/System32/startnet.cmd" || true
                wimupdate /work/build/tmp/dism-service/boot.wim "$idx" \
                    --command="add /work/build/tmp/dism-service/winpeshl.ini /Windows/System32/winpeshl.ini" || true
            fi
        done
        wiminfo /work/build/tmp/dism-service/boot.wim 2 --boot || true
    '

echo "[*] Building disposable UEFI WinPE servicing disk..."
7z x -y -o"$TMP_DIR/pe-files" "$WIN_ISO" efi/ boot/ >/dev/null
cp "$BOOT_WIM" "$TMP_DIR/pe-files/sources/boot.wim"
touch "$TMP_DIR/dism-service.tag"

truncate -s 1022M "$PE_ESP"
mkfs.vfat -F 32 -n "DISMSVC" "$PE_ESP" >/dev/null
mcopy -o -i "$PE_ESP" -s "$TMP_DIR/pe-files/efi" ::
mcopy -o -i "$PE_ESP" -s "$TMP_DIR/pe-files/boot" ::
mcopy -o -i "$PE_ESP" -s "$TMP_DIR/pe-files/sources" ::

truncate -s 1024M "$PE_DISK"
parted -s "$PE_DISK" mklabel gpt
parted -s "$PE_DISK" mkpart ESP fat32 1MiB 1023MiB
parted -s "$PE_DISK" set 1 esp on
dd if="$PE_ESP" of="$PE_DISK" bs=1M seek=1 count=1022 conv=notrunc status=none

truncate -s 30M "$STATUS_FS"
mkfs.vfat -F 16 -n "DISMSTATUS" "$STATUS_FS" >/dev/null
mcopy -o -i "$STATUS_FS" "$TMP_DIR/dism-service.tag" ::
truncate -s 32M "$STATUS_DISK"
parted -s "$STATUS_DISK" mklabel gpt
parted -s "$STATUS_DISK" mkpart Status fat16 1MiB 31MiB
dd if="$STATUS_FS" of="$STATUS_DISK" bs=1M seek=1 count=30 conv=notrunc status=none

cp "$AAVMF_VARS" "$VARS_COPY"

echo "[*] Booting genuine ARM64 WinPE under TCG for DISM servicing..."
set +e
timeout --signal=TERM 15m \
    qemu-system-aarch64 \
        -M virt,gic-version=max \
        -cpu max,pauth-impdef=on \
        -accel tcg,thread=multi \
        -smp 8 \
        -m 4096 \
        -drive if=pflash,format=raw,readonly=on,file="$AAVMF_CODE" \
        -drive if=pflash,format=raw,file="$VARS_COPY" \
        -device ramfb \
        -device qemu-xhci,id=usb \
        -drive file="$PE_DISK",if=none,id=pe,format=raw \
        -device nvme,serial=PEBOOT,drive=pe,bootindex=1 \
        -drive file="$VIRTIO_ISO",if=none,id=virtio,format=raw,media=cdrom,readonly=on \
        -device usb-storage,bus=usb.0,drive=virtio \
        -drive file="$STATUS_DISK",if=none,id=status,format=raw \
        -device nvme,serial=DISMSTATUS,drive=status \
        -drive file="$TARGET_RAW",if=none,id=target,format=raw \
        -device nvme,serial=DISMSERVICE,drive=target,bootindex=2 \
        -display none \
        -serial null \
        -no-reboot < /dev/null
QEMU_STATUS=$?
set -e

echo "[*] WinPE servicing result:"
dd if="$STATUS_DISK" of="$STATUS_FS" bs=1M skip=1 count=30 status=none
if ! STATUS_TEXT="$(mtype -i "$STATUS_FS" ::dism-status.txt)"; then
    echo "Error: WinPE did not produce a DISM status file (QEMU status $QEMU_STATUS)." >&2
    exit 1
fi
printf '%s\n' "$STATUS_TEXT"

ERROR_COUNT="$(printf '%s\n' "$STATUS_TEXT" | grep -c '^Error:' || true)"
if ! printf '%s\n' "$STATUS_TEXT" | grep -qiE 'DISM completed successfully|The operation completed successfully' || [ "$ERROR_COUNT" -ne 0 ]; then
    echo "Error: DISM servicing failed (QEMU status $QEMU_STATUS)." >&2
    exit 1
fi

if ! printf '%s\n' "$STATUS_TEXT" | grep -q 'Boot files successfully created'; then
    echo "Error: bcdboot did not create the UEFI boot files (QEMU status $QEMU_STATUS)." >&2
    exit 1
fi

echo "[*] Signed VirtIO packages staged successfully by Microsoft DISM."
echo "[*] UEFI boot files and system BCD store created by Microsoft bcdboot."
