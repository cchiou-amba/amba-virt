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
RELOAD=0

while [ "$#" -gt 0 ]; do
    case "$1" in
    --restart-app=*)
        RESTART_APP="${1#--restart-app=}"
        shift
        ;;
    --reload)
        RELOAD=1
        shift
        ;;
    -h|--help)
        echo "Usage: scripts/deploy_and_insmod.sh [target-node] [--restart-app=APP_NAME] [--reload]"
        exit 0
        ;;
    *)
        echo "deploy_and_insmod.sh: unknown option: $1" >&2
        exit 1
        ;;
    esac
done

BUILD_MOD_DIR="$ROOT/build/modules"
BUILD_FW_DIR="$ROOT/build/firmware"
LOADER_SCRIPT="$ROOT/scripts/load-ambarella-drivers.sh"

echo "Target Node: $TARGET_NODE"
echo "Creating /persist/modules, /persist/firmware, /persist/bin on $TARGET_NODE..."
ssh -o BatchMode=yes "$TARGET_NODE" "mkdir -p /persist/modules /persist/firmware /persist/bin"

# 1. Stage all available kernel modules
echo "Staging all kernel modules to $TARGET_NODE:/persist/modules/..."
if [ -d "$BUILD_MOD_DIR" ] && [ -n "$(ls -A "$BUILD_MOD_DIR"/*.ko 2>/dev/null)" ]; then
    scp "$BUILD_MOD_DIR"/*.ko "$TARGET_NODE:/persist/modules/"
elif [ -n "$DRIVERS_DIR" ]; then
    for ko in $(find "$DRIVERS_DIR" -name "*.ko"); do
        scp "$ko" "$TARGET_NODE:/persist/modules/"
    done
fi

# 2. Stage firmware binaries
if [ -d "$BUILD_FW_DIR" ] && [ -n "$(ls -A "$BUILD_FW_DIR" 2>/dev/null)" ]; then
    echo "Staging firmware binaries from $BUILD_FW_DIR..."
    scp "$BUILD_FW_DIR"/*.bin "$TARGET_NODE:/persist/firmware/"
elif [ -n "$DRIVERS_DIR" ] && [ -f "$DRIVERS_DIR/cavalry/firmware/cavalry.bin" ]; then
    echo "Staging cavalry.bin..."
    scp "$DRIVERS_DIR/cavalry/firmware/cavalry.bin" "$TARGET_NODE:/persist/firmware/"
fi

# 3. Deploy and configure /persist/bin/load-ambarella-drivers.sh
if [ -f "$LOADER_SCRIPT" ]; then
    echo "Installing $LOADER_SCRIPT to $TARGET_NODE:/persist/bin/..."
    scp "$LOADER_SCRIPT" "$TARGET_NODE:/persist/bin/load-ambarella-drivers.sh"
    ssh -o BatchMode=yes "$TARGET_NODE" "chmod +x /persist/bin/load-ambarella-drivers.sh"
fi

# 4. Execute the dynamic driver loader on the target node
LOAD_ARGS=""
if [ "$RELOAD" -eq 1 ]; then
    LOAD_ARGS="--reload"
fi

echo "Executing driver loader on $TARGET_NODE..."
ssh -o BatchMode=yes "$TARGET_NODE" "/persist/bin/load-ambarella-drivers.sh $LOAD_ARGS"

if [ -n "$RESTART_APP" ]; then
    echo "Restarting application $RESTART_APP to refresh device bindings..."
    ssh -o BatchMode=yes "$TARGET_NODE" "eve app restart $RESTART_APP"
fi

echo "Verifying driver status on $TARGET_NODE:"
ssh -o BatchMode=yes "$TARGET_NODE" "
    lsmod | grep -E 'cavalry|amba_virt|amba_otp|iav|pvrsrvkm' || true
    ls -l /dev/cavalry /dev/amba_virt /dev/amba_otp /dev/iav 2>/dev/null || true
"
echo "Done."
