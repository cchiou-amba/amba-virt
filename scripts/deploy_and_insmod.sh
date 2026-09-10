#!/bin/sh
# Deploy out-of-tree Ambarella drivers and firmware to an EVE edge node (/persist),
# configure the kernel firmware path, and insert modules via SSH.
#
#   ./scripts/deploy_and_insmod.sh [target-node]
#   ./scripts/deploy_and_insmod.sh n1-655-pro --restart-app=ubuntu_24_04_container

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
EVE_DIR=$(CDPATH= cd -- "$ROOT/../eve" 2>/dev/null && pwd || true)

TARGET_NODE="${1:-n1-655-pro}"
if [ "$#" -gt 0 ] && [ "${1#-}" = "$1" ]; then
    shift
fi

RESTART_APP=""

while [ "$#" -gt 0 ]; do
    case "$1" in
    --restart-app=*)
        RESTART_APP="${1#--restart-app=}"
        shift
        ;;
    -h|--help)
        echo "Usage: scripts/deploy_and_insmod.sh [target-node] [--restart-app=APP_NAME]"
        exit 0
        ;;
    *)
        echo "deploy_and_insmod.sh: unknown option: $1" >&2
        exit 1
        ;;
    esac
done

AMBA_VIRT_KO="$ROOT/build/kmod/amba_virt.ko"
CAVALRY_KO=""
CAVALRY_BIN=""

if [ -n "$EVE_DIR" ] && [ -d "$EVE_DIR/cavalry" ]; then
    if [ -f "$EVE_DIR/cavalry/cavalry_v3/cavalry.ko" ]; then
        CAVALRY_KO="$EVE_DIR/cavalry/cavalry_v3/cavalry.ko"
    else
        CAVALRY_KO="$EVE_DIR/cavalry/driver/cavalry_v3/cavalry.ko"
    fi
    CAVALRY_BIN="$EVE_DIR/cavalry/firmware/cavalry_v3/n1_655/cavalry.bin"
fi

echo "Target Node: $TARGET_NODE"
echo "Creating /persist/modules and /persist/firmware on $TARGET_NODE..."
ssh -o BatchMode=yes "$TARGET_NODE" "mkdir -p /persist/modules /persist/firmware /persist/bin"

if [ -f "$CAVALRY_BIN" ]; then
    echo "Staging Cavalry firmware binary..."
    scp "$CAVALRY_BIN" "$TARGET_NODE:/persist/firmware/cavalry.bin"
fi

if [ -f "$CAVALRY_KO" ]; then
    echo "Staging cavalry.ko..."
    scp "$CAVALRY_KO" "$TARGET_NODE:/persist/modules/cavalry.ko"
fi

if [ -f "$AMBA_VIRT_KO" ]; then
    echo "Staging amba_virt.ko..."
    scp "$AMBA_VIRT_KO" "$TARGET_NODE:/persist/modules/amba_virt.ko"
fi

echo "Configuring runtime firmware search path and inserting modules..."
ssh -o BatchMode=yes "$TARGET_NODE" "
    if [ -f /persist/firmware/cavalry.bin ]; then
        echo -n '/persist/firmware' > /sys/module/firmware_class/parameters/path
    fi
    if [ -f /persist/modules/cavalry.ko ] && ! lsmod | grep -q '^cavalry '; then
        echo 'Inserting cavalry.ko...'
        insmod /persist/modules/cavalry.ko
    fi
    if [ -f /persist/modules/amba_virt.ko ] && ! lsmod | grep -q '^amba_virt '; then
        echo 'Inserting amba_virt.ko...'
        insmod /persist/modules/amba_virt.ko
    fi
"

if [ -n "$RESTART_APP" ]; then
    echo "Restarting application $RESTART_APP to refresh device bindings..."
    ssh -o BatchMode=yes "$TARGET_NODE" "eve app restart $RESTART_APP"
fi

echo "Verifying driver status on $TARGET_NODE:"
ssh -o BatchMode=yes "$TARGET_NODE" "
    lsmod | grep -E 'cavalry|amba_virt' || true
    ls -l /dev/cavalry /dev/amba_virt 2>/dev/null || true
"
echo "Done."
