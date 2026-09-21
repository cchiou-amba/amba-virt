#!/usr/bin/env bash
#
# build.sh - Automated build script for QNX 8.0 HVM disk image for EVE-OS
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================================="
echo " Building QNX 8.0 HVM Disk Image for Ambarella / EVE-OS"
echo "=========================================================="

# 1. Environment check
if [ -z "$QNX_TARGET" ] || [ -z "$QNX_HOST" ]; then
    echo "[!] Sourcing QNX SDP 8.0 environment..."
    if [ -f "$HOME/qnx/qnx800/qnxsdp-env.sh" ]; then
        source "$HOME/qnx/qnx800/qnxsdp-env.sh"
    elif [ -f "$HOME/qnx800/qnxsdp-env.sh" ]; then
        source "$HOME/qnx800/qnxsdp-env.sh"
    else
        echo "Error: QNX SDP 8.0 environment not found. Please source qnxsdp-env.sh."
        exit 1
    fi
fi

# 2. Check / Apply SDP patches
if [ -f "${SCRIPT_DIR}/patches/apply_sdp_patches.py" ]; then
    echo "[*] Checking SDP patches..."
    python3 "${SCRIPT_DIR}/patches/apply_sdp_patches.py"
fi

# 3. Clean previous build if requested
if [ "$1" == "--clean" ]; then
    echo "[*] Cleaning output directory..."
    rm -rf output
fi

# 4. Locate mkqnximage
MKQNXIMAGE=""
for cand in \
    "${QNX_HOST}/../../common/bin/mkqnximage" \
    "${QNX_HOST}/../common/bin/mkqnximage" \
    "${QNX_HOST}/../../common/mkqnximage/mkqnximage" \
    "${QNX_TARGET}/../host/common/bin/mkqnximage" \
    "$(which mkqnximage 2>/dev/null || true)"; do
    if [ -n "${cand}" ] && [ -x "${cand}" ]; then
        MKQNXIMAGE="${cand}"
        break
    fi
done

if [ -z "$MKQNXIMAGE" ] || [ ! -x "$MKQNXIMAGE" ]; then
    echo "Error: mkqnximage binary not found in QNX SDP installation."
    exit 1
fi

# 5. Run mkqnximage
echo "[*] Running mkqnximage..."
"$MKQNXIMAGE"

# 6. Convert raw disk to compressed QCOW2
if [ -f "${SCRIPT_DIR}/output/disk-qemu" ]; then
    echo "[*] Converting raw disk to compressed QCOW2..."
    mkdir -p "${SCRIPT_DIR}/output/dist"
    QCOW2_OUT="${SCRIPT_DIR}/output/dist/qnx-8.0-arm64-cloudimg.qcow2"
    qemu-img convert -c -f raw -O qcow2 "${SCRIPT_DIR}/output/disk-qemu" "$QCOW2_OUT"
    
    IMG_SIZE=$(stat -c %s "$QCOW2_OUT")
    IMG_SHA=$(sha256sum "$QCOW2_OUT" | awk '{print $1}')
    
    echo ""
    echo "=========================================================="
    echo " QNX 8.0 HVM Build Complete!"
    echo "=========================================================="
    echo " Raw Disk Image : ${SCRIPT_DIR}/output/disk-qemu"
    echo " QCOW2 Image    : ${QCOW2_OUT}"
    echo " Image Size     : ${IMG_SIZE} bytes"
    echo " SHA-256        : ${IMG_SHA}"
    echo "=========================================================="
    echo ""
    echo "To uplink to ZedControl datastore (e.g. LocalHTTP):"
    echo "  ../amba-virt/scripts/zcli -- image uplink qnx-8.0-arm64-cloudimg \\"
    echo "    --image-sha=\"${IMG_SHA}\" \\"
    echo "    --image-size=\"${IMG_SIZE}\""
fi
