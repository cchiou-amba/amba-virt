#!/bin/sh
# /persist/bin/load-ambarella-drivers.sh
#
# Runtime dynamic module and firmware loader for Ambarella out-of-tree drivers
# on EVE-OS. Executed on the target edge node from /persist/bin/.
#
# Usage:
#   /persist/bin/load-ambarella-drivers.sh
#   /persist/bin/load-ambarella-drivers.sh --reload
#

RELOAD=0
for arg in "$@"; do
    case "$arg" in
    -r|--reload)
        RELOAD=1
        ;;
    -h|--help)
        echo "Usage: $0 [--reload]"
        exit 0
        ;;
    esac
done

PERSIST_DIR="/persist"
MODULES_DIR="$PERSIST_DIR/modules"
FIRMWARE_DIR="$PERSIST_DIR/firmware"

# 1. Configure kernel firmware search path if firmware files exist
if [ -d "$FIRMWARE_DIR" ] && [ -n "$(ls -A "$FIRMWARE_DIR" 2>/dev/null)" ]; then
    if [ -f /sys/module/firmware_class/parameters/path ]; then
        echo -n "$FIRMWARE_DIR" > /sys/module/firmware_class/parameters/path
    fi
fi

# 2. Driver unloading in reverse dependency order if --reload requested
# Note: amba_otp is commented out because direct OTP access without ATF SMC causes soft lockups
UNLOAD_ORDER="pvrsrvkm amba_pci_platform amba_virt cavalry dsplog iav imgproc amba_opti_print dsp ambnl msg ambcma hw_timer"
# UNLOAD_ORDER="amba_otp $UNLOAD_ORDER"

if [ "$RELOAD" -eq 1 ] && [ -d "$MODULES_DIR" ]; then
    for mod in $UNLOAD_ORDER; do
        if lsmod | grep -q "^${mod} "; then
            echo "Unloading $mod..."
            rmmod "$mod" 2>/dev/null || true
        fi
    done
fi

# 3. Driver loading in forward dependency order from /persist/modules/
# Note: amba_otp is commented out per project instructions to avoid hang on Cooper N1-655
LOAD_ORDER="hw_timer ambcma msg ambnl dsp amba_opti_print imgproc iav dsplog cavalry amba_virt amba_pci_platform pvrsrvkm"
# LOAD_ORDER="$LOAD_ORDER amba_otp"

# Modules explicitly excluded from loading
EXCLUDED_MODULES="amba_otp"

if [ -d "$MODULES_DIR" ]; then
    LOADED_LIST=""
    # First pass: load known drivers according to dependency order
    for mod in $LOAD_ORDER; do
        ko="$MODULES_DIR/${mod}.ko"
        if [ -f "$ko" ]; then
            LOADED_LIST="$LOADED_LIST $mod"
            if ! lsmod | grep -q "^${mod} "; then
                echo "Inserting $ko..."
                insmod "$ko" 2>&1 || echo "Warning: failed to insert $ko" >&2
            else
                echo "Module $mod already loaded"
            fi
        fi
    done

    # Second pass: load any additional .ko files found in /persist/modules/
    for ko in "$MODULES_DIR"/*.ko; do
        [ -f "$ko" ] || continue
        base=$(basename "$ko" .ko)
        case " $EXCLUDED_MODULES " in
            *" $base "*)
                echo "Skipping excluded module $base"
                continue
                ;;
        esac
        case " $LOADED_LIST " in
            *" $base "*)
                # Already processed
                ;;
            *)
                if ! lsmod | grep -q "^${base} "; then
                    echo "Inserting extra module $ko..."
                    insmod "$ko" 2>&1 || echo "Warning: failed to insert $ko" >&2
                fi
                ;;
        esac
    done
fi
