#!/bin/bash
# Lean Ubuntu 24.04 AArch64 HVM Image Builder
# Ambarella EVE Platform
# QEMU / TCG Full-System Emulation Pipeline

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
CACHE_DIR="$SCRIPT_DIR/cache"
WORK_DIR="$SCRIPT_DIR/output/work"
DIST_DIR="$SCRIPT_DIR/output/dist"
ASSETS_DIR="$SCRIPT_DIR/assets"
LOCK_FILE="$SCRIPT_DIR/base-image.lock"
BOOTSTRAP_HOST="$SCRIPT_DIR/bootstrap-host.sh"
QEMU_RUNNER="$SCRIPT_DIR/qemu_runner.py"

CLEAN=0
DRY_RUN=0

usage() {
    cat << 'EOF'
usage: build.sh [OPTIONS]

Build a lean Ubuntu 24.04 AArch64 QCOW2 image for Ambarella EVE HVM via QEMU/TCG.

Options:
  --clean     Remove cache/ and output/ directories before building
  --dry-run   Validate dependencies, lock file, and configuration without building image
  -h, --help  Show this help message
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "build.sh: unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            echo "build.sh: unexpected positional argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [ "$CLEAN" -eq 1 ]; then
    echo "=== Cleaning build cache and output directories ==="
    rm -rf "$CACHE_DIR" "$SCRIPT_DIR/output"
fi

echo "=== Verifying and bootstrapping host dependencies ==="
if [ "$DRY_RUN" -eq 1 ]; then
    "$BOOTSTRAP_HOST" --dry-run
else
    "$BOOTSTRAP_HOST"
fi

echo "=== Verifying required host utilities ==="
REQUIRED_TOOLS=(
    qemu-system-aarch64
    qemu-img
    genisoimage
    python3
    sshpass
    sha256sum
    curl
)

for tool in "${REQUIRED_TOOLS[@]}"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "build.sh: error: required tool '$tool' not found in PATH" >&2
        exit 1
    fi
done

# Resolve AArch64 UEFI firmware from qemu-efi-aarch64 package metadata
UEFI_CODE=$(dpkg -L qemu-efi-aarch64 2>/dev/null | grep -E '/AAVMF_CODE\.fd$' | head -n 1 || true)
UEFI_VARS=$(dpkg -L qemu-efi-aarch64 2>/dev/null | grep -E '/AAVMF_VARS\.fd$' | head -n 1 || true)

if [ -z "$UEFI_CODE" ] || [ ! -f "$UEFI_CODE" ]; then
    echo "build.sh: error: unable to locate AArch64 UEFI code image in qemu-efi-aarch64 package" >&2
    exit 1
fi

if [ -z "$UEFI_VARS" ] || [ ! -f "$UEFI_VARS" ]; then
    echo "build.sh: error: unable to locate AArch64 UEFI vars image in qemu-efi-aarch64 package" >&2
    exit 1
fi

echo "Resolved UEFI Code: $UEFI_CODE"
echo "Resolved UEFI Vars: $UEFI_VARS"

if [ ! -f "$LOCK_FILE" ]; then
    echo "build.sh: lock file '$LOCK_FILE' not found" >&2
    exit 1
fi

# Source lock file
# shellcheck source=/dev/null
source "$LOCK_FILE"

REQUIRED_ASSETS=(
    01-ambarella.yaml
    50-console.cfg
    grow-rootfs.service
    grow-rootfs.sh
    journald.conf
    meta-data
    provision-image.sh
    ssh-hostkeys.service
    sshd.conf
    user-data.in
)

for asset in "${REQUIRED_ASSETS[@]}"; do
    if [ ! -f "$ASSETS_DIR/$asset" ]; then
        echo "build.sh: error: missing required asset '$ASSETS_DIR/$asset'" >&2
        exit 1
    fi
done

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[DRY-RUN] Host tools, UEFI firmware, lock configuration, and assets verified successfully."
    echo "[DRY-RUN] Base Image: $BASE_IMAGE_URL"
    echo "[DRY-RUN] Expected Size: $BASE_IMAGE_SIZE bytes"
    echo "[DRY-RUN] Expected SHA-256: $BASE_IMAGE_SHA256"
    exit 0
fi

mkdir -p "$CACHE_DIR" "$WORK_DIR" "$DIST_DIR"

BASE_IMAGE_PATH="$CACHE_DIR/$BASE_IMAGE_NAME"
BASE_MANIFEST_PATH="$CACHE_DIR/$BASE_MANIFEST_NAME"

# 1. Download and verify Canonical base image and manifest
echo "=== Verifying base image in cache ==="
if [ -f "$BASE_IMAGE_PATH" ]; then
    CACHED_SHA=$(sha256sum "$BASE_IMAGE_PATH" | awk '{print $1}')
    if [ "$CACHED_SHA" != "$BASE_IMAGE_SHA256" ]; then
        echo "Cached base image checksum mismatch. Re-downloading..."
        rm -f "$BASE_IMAGE_PATH"
    fi
fi

if [ ! -f "$BASE_IMAGE_PATH" ]; then
    echo "Downloading Canonical base image: $BASE_IMAGE_URL..."
    curl -fL -o "$BASE_IMAGE_PATH.tmp" "$BASE_IMAGE_URL"
    ACTUAL_SIZE=$(stat -c %s "$BASE_IMAGE_PATH.tmp")
    if [ "$ACTUAL_SIZE" -ne "$BASE_IMAGE_SIZE" ]; then
        echo "build.sh: downloaded image size mismatch: expected $BASE_IMAGE_SIZE, got $ACTUAL_SIZE" >&2
        rm -f "$BASE_IMAGE_PATH.tmp"
        exit 1
    fi
    ACTUAL_SHA=$(sha256sum "$BASE_IMAGE_PATH.tmp" | awk '{print $1}')
    if [ "$ACTUAL_SHA" != "$BASE_IMAGE_SHA256" ]; then
        echo "build.sh: downloaded image checksum mismatch: expected $BASE_IMAGE_SHA256, got $ACTUAL_SHA" >&2
        rm -f "$BASE_IMAGE_PATH.tmp"
        exit 1
    fi
    mv "$BASE_IMAGE_PATH.tmp" "$BASE_IMAGE_PATH"
fi

if [ ! -f "$BASE_MANIFEST_PATH" ]; then
    echo "Downloading Canonical manifest: $BASE_MANIFEST_URL..."
    curl -fL -o "$BASE_MANIFEST_PATH" "$BASE_MANIFEST_URL"
fi

# 2. Prepare working image (never modify cached base image in place)
WORK_IMAGE="$WORK_DIR/working-ubuntu-24.04-arm64.qcow2"
echo "=== Preparing working copy in $WORK_IMAGE ==="
rm -f "$WORK_IMAGE"
qemu-img convert -O qcow2 "$BASE_IMAGE_PATH" "$WORK_IMAGE"

# Resize working image to 8 GiB virtual capacity before the bake
echo "=== Resizing working disk virtual capacity to 8 GiB ==="
qemu-img resize "$WORK_IMAGE" 8G

# 3. Assemble NoCloud CIDATA seed ISO
echo "=== Assembling NoCloud CIDATA seed ISO ==="
USER_DATA_OUT="$WORK_DIR/user-data"
META_DATA_OUT="$WORK_DIR/meta-data"
CIDATA_ISO="$WORK_DIR/cidata.iso"

cp -f "$ASSETS_DIR/meta-data" "$META_DATA_OUT"

GROW_SH_B64=$(base64 -w 0 "$ASSETS_DIR/grow-rootfs.sh")
GROW_SVC_B64=$(base64 -w 0 "$ASSETS_DIR/grow-rootfs.service")
SSH_SVC_B64=$(base64 -w 0 "$ASSETS_DIR/ssh-hostkeys.service")
NETPLAN_B64=$(base64 -w 0 "$ASSETS_DIR/01-ambarella.yaml")
PROV_SH_B64=$(base64 -w 0 "$ASSETS_DIR/provision-image.sh")

sed \
    -e "s|@GROW_ROOTFS_SH_B64@|$GROW_SH_B64|g" \
    -e "s|@GROW_ROOTFS_SERVICE_B64@|$GROW_SVC_B64|g" \
    -e "s|@SSH_HOSTKEYS_SERVICE_B64@|$SSH_SVC_B64|g" \
    -e "s|@NETPLAN_YAML_B64@|$NETPLAN_B64|g" \
    -e "s|@PROVISION_IMAGE_SH_B64@|$PROV_SH_B64|g" \
    "$ASSETS_DIR/user-data.in" > "$USER_DATA_OUT"

rm -f "$CIDATA_ISO"
genisoimage -output "$CIDATA_ISO" -volid cidata -joliet -rock "$USER_DATA_OUT" "$META_DATA_OUT"

# 4. Execute QEMU/TCG bake phase
BAKE_SERIAL_LOG="$WORK_DIR/bake-serial.log"
python3 "$QEMU_RUNNER" \
    --mode bake \
    --disk "$WORK_IMAGE" \
    --cidata "$CIDATA_ISO" \
    --uefi-code "$UEFI_CODE" \
    --uefi-vars "$UEFI_VARS" \
    --serial-log "$BAKE_SERIAL_LOG" \
    --work-dir "$WORK_DIR" \
    --timeout 10800

# 5. Execute seedless verification boot phase (no CIDATA attached)
VERIFY_SERIAL_LOG="$WORK_DIR/verify-serial.log"
python3 "$QEMU_RUNNER" \
    --mode verify \
    --disk "$WORK_IMAGE" \
    --uefi-code "$UEFI_CODE" \
    --uefi-vars "$UEFI_VARS" \
    --serial-log "$VERIFY_SERIAL_LOG" \
    --work-dir "$WORK_DIR" \
    --timeout 1200

# 6. Compress and emit final verified artifacts
FINAL_IMAGE="$DIST_DIR/ubuntu-24.04-arm64-cloudimg.qcow2"
FINAL_SHA_FILE="$DIST_DIR/ubuntu-24.04-arm64-cloudimg.qcow2.sha256"
MANIFEST_FILE="$DIST_DIR/build-manifest.json"

echo "=== Converting and compressing final QCOW2 ==="
rm -f "$FINAL_IMAGE"
qemu-img convert -O qcow2 -c "$WORK_IMAGE" "$FINAL_IMAGE"

echo "=== Verifying final QCOW2 integrity ==="
qemu-img check "$FINAL_IMAGE"

FINAL_SIZE=$(stat -c %s "$FINAL_IMAGE")
FINAL_SHA=$(sha256sum "$FINAL_IMAGE" | awk '{print $1}')
echo "$FINAL_SHA  ubuntu-24.04-arm64-cloudimg.qcow2" > "$FINAL_SHA_FILE"

cat << EOF > "$MANIFEST_FILE"
{
  "artifact": "ubuntu-24.04-arm64-cloudimg.qcow2",
  "sha256": "$FINAL_SHA",
  "size_bytes": $FINAL_SIZE,
  "base_image": {
    "name": "$BASE_IMAGE_NAME",
    "source_url": "$BASE_IMAGE_URL",
    "sha256": "$BASE_IMAGE_SHA256",
    "size_bytes": $BASE_IMAGE_SIZE
  },
  "canonical_manifest": {
    "name": "$BASE_MANIFEST_NAME",
    "source_url": "$BASE_MANIFEST_URL",
    "sha256": "$BASE_MANIFEST_SHA256",
    "size_bytes": $BASE_MANIFEST_SIZE
  },
  "build_date": "$(date -u +'%Y-%m-%dT%H:%M:%SZ')",
  "build_method": "qemu-tcg-nocloud"
}
EOF

echo "=== Image build completed successfully ==="
echo "Final Image : $FINAL_IMAGE"
echo "SHA-256     : $FINAL_SHA"
echo "Size        : $FINAL_SIZE bytes"
echo "Manifest    : $MANIFEST_FILE"
