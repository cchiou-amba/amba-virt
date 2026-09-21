#!/bin/sh
# /persist/bin/load-ambarella-drivers.sh
#
# Runtime dynamic module and firmware loader for Ambarella out-of-tree drivers
# on EVE-OS. Executed on the target edge node from /persist/bin/.
#
# Usage:
#   /persist/bin/load-ambarella-drivers.sh
#   /persist/bin/load-ambarella-drivers.sh --reload
#   /persist/bin/load-ambarella-drivers.sh --status
#

# ==============================================================================
# Module Enable / Disable Configuration (1 = enabled, 0 = disabled)
# Edit or comment out variables below to control boot-time / runtime loading.
# Environment variables take precedence if set in environment (e.g. ENABLE_AMBA_VIRT=1).
# ==============================================================================

# Core Timers & Memory Management
#ENABLE_HW_TIMER=${ENABLE_HW_TIMER:-1}
#ENABLE_AMBCMA=${ENABLE_AMBCMA:-1}
#ARGS_AMBCMA=${ARGS_AMBCMA:-""}

# IPC & DSP Video Processing Subsystem
#ENABLE_MSG=${ENABLE_MSG:-1}
#ENABLE_AMBNL=${ENABLE_AMBNL:-1}
#ENABLE_DSP=${ENABLE_DSP:-1}
#ENABLE_AMBA_OPTI_PRINT=${ENABLE_AMBA_OPTI_PRINT:-1}
#ENABLE_IMGPROC=${ENABLE_IMGPROC:-1}
#ENABLE_IAV=${ENABLE_IAV:-1}
#ENABLE_DSPLOG=${ENABLE_DSPLOG:-1}

# Camera / Bridge / Deserializer Subsystem
#ENABLE_VIO_MONITOR=${ENABLE_VIO_MONITOR:-0}
#ENABLE_AMBRG=${ENABLE_AMBRG:-0}
#ENABLE_MAX96712=${ENABLE_MAX96712:-0}
#ARGS_MAX96712=${ARGS_MAX96712:-"id=0x08040201 dts_addr=1 use_max20087=0"}
#ENABLE_OS08A10_MIPI_BRG=${ENABLE_OS08A10_MIPI_BRG:-0}
#ARGS_OS08A10_MIPI_BRG=${ARGS_OS08A10_MIPI_BRG:-"brg_id=0x2"}

# NPU / Virtualization / GPU Subsystem
# (amba_virt provides /dev/amba_virt_shm required by Ubuntu and Alpine HVM guests)
#ENABLE_CAVALRY=${ENABLE_CAVALRY:-1}
#ARGS_CAVALRY=${ARGS_CAVALRY:-"virt_user_window_mb=2048"}
ENABLE_AMBA_VIRT=${ENABLE_AMBA_VIRT:-1}
ARGS_AMBA_VIRT=${ARGS_AMBA_VIRT:-"shm_phys=0x100000000 shm_size=0x80000000"}
#ENABLE_AMBA_PCI_PLATFORM=${ENABLE_AMBA_PCI_PLATFORM:-1}
#ENABLE_PVRSRVKM=${ENABLE_PVRSRVKM:-1}

# Diagnostics & GDMA Test Modules
#ENABLE_TEST_GDMA=${ENABLE_TEST_GDMA:-0}
#ENABLE_DIAG_GDMA=${ENABLE_DIAG_GDMA:-0}
#ENABLE_DIAG_STAGE2_PTE=${ENABLE_DIAG_STAGE2_PTE:-0}

# Security / OTP (Disabled by default to avoid hang on Cooper N1-655 without ATF SMC)
#ENABLE_AMBA_OTP=${ENABLE_AMBA_OTP:-0}

# ==============================================================================
# Execution Logic
# ==============================================================================

RELOAD=0
STATUS=0

for arg in "$@"; do
    case "$arg" in
    -r|--reload)
        RELOAD=1
        ;;
    -s|--status)
        STATUS=1
        ;;
    -h|--help)
        echo "Usage: $0 [--reload] [--status]"
        exit 0
        ;;
    esac
done

PERSIST_DIR="/persist"
MODULES_DIR="$PERSIST_DIR/modules"
FIRMWARE_DIR="$PERSIST_DIR/firmware"

# Show loaded status if requested
if [ "$STATUS" -eq 1 ]; then
    echo "=== Ambarella Kernel Modules Status ==="
    printf "%-22s %-10s %-10s %s\n" "Module" "Config" "Loaded" "Extra Args"
    echo "----------------------------------------------------------------------"
    check_status() {
        mod="$1"
        cfg="$2"
        args="${3:-}"
        if lsmod | grep -q "^${mod} "; then
            loaded="YES"
        else
            loaded="NO"
        fi
        if [ "$cfg" = "1" ]; then
            cfg_str="ENABLED"
        else
            cfg_str="disabled"
        fi
        printf "%-22s %-10s %-10s %s\n" "$mod" "$cfg_str" "$loaded" "$args"
    }
    check_status "hw_timer"          "$ENABLE_HW_TIMER"          ""
    check_status "ambcma"            "$ENABLE_AMBCMA"            "$ARGS_AMBCMA"
    check_status "msg"               "$ENABLE_MSG"               ""
    check_status "ambnl"             "$ENABLE_AMBNL"             ""
    check_status "dsp"               "$ENABLE_DSP"               ""
    check_status "amba_opti_print"   "$ENABLE_AMBA_OPTI_PRINT"   ""
    check_status "imgproc"           "$ENABLE_IMGPROC"           ""
    check_status "iav"               "$ENABLE_IAV"               ""
    check_status "dsplog"            "$ENABLE_DSPLOG"            ""
    check_status "vio_monitor"       "$ENABLE_VIO_MONITOR"       ""
    check_status "ambrg"             "$ENABLE_AMBRG"             ""
    check_status "max96712"          "$ENABLE_MAX96712"          "$ARGS_MAX96712"
    check_status "os08a10_mipi_brg"  "$ENABLE_OS08A10_MIPI_BRG"  "$ARGS_OS08A10_MIPI_BRG"
    check_status "cavalry"           "$ENABLE_CAVALRY"           "$ARGS_CAVALRY"
    check_status "amba_virt"         "$ENABLE_AMBA_VIRT"         "$ARGS_AMBA_VIRT"
    check_status "amba_pci_platform" "$ENABLE_AMBA_PCI_PLATFORM" ""
    check_status "pvrsrvkm"          "$ENABLE_PVRSRVKM"          ""
    check_status "testGDMA"          "$ENABLE_TEST_GDMA"         ""
    check_status "diag_gdma"         "$ENABLE_DIAG_GDMA"         ""
    check_status "diag_stage2_pte"   "$ENABLE_DIAG_STAGE2_PTE"   ""
    check_status "amba_otp"          "$ENABLE_AMBA_OTP"          ""
    exit 0
fi

# 1. Configure kernel firmware search path if firmware files exist
if [ -d "$FIRMWARE_DIR" ] && [ -n "$(ls -A "$FIRMWARE_DIR" 2>/dev/null)" ]; then
    if [ -f /sys/module/firmware_class/parameters/path ]; then
        echo -n "$FIRMWARE_DIR" > /sys/module/firmware_class/parameters/path
    fi
fi

# Helper functions for individual unload and load
unload_mod() {
    mod="$1"
    if lsmod | grep -q "^${mod} "; then
        echo "Unloading $mod..."
        rmmod "$mod" 2>/dev/null || echo "Warning: failed to unload $mod" >&2
    fi
}

load_mod() {
    mod="$1"
    enabled="$2"
    args="${3:-}"
    ko="$MODULES_DIR/${mod}.ko"

    if [ "$enabled" != "1" ]; then
        return 0
    fi

    if [ ! -f "$ko" ]; then
        echo "Note: $ko not present in $MODULES_DIR, skipping"
        return 0
    fi

    if lsmod | grep -q "^${mod} "; then
        echo "Module $mod already loaded"
    else
        echo "Inserting $ko ${args}..."
        if [ -n "$args" ]; then
            # shellcheck disable=SC2086
            insmod "$ko" $args 2>&1 || echo "Warning: failed to insert $ko" >&2
        else
            insmod "$ko" 2>&1 || echo "Warning: failed to insert $ko" >&2
        fi
    fi
}

# 2. Driver unloading in reverse dependency order if --reload requested
if [ "$RELOAD" -eq 1 ]; then
    unload_mod "amba_otp"
    unload_mod "diag_stage2_pte"
    unload_mod "diag_gdma"
    unload_mod "testGDMA"
    unload_mod "pvrsrvkm"
    unload_mod "amba_pci_platform"
    unload_mod "amba_virt"
    unload_mod "cavalry"
    unload_mod "os08a10_mipi_brg"
    unload_mod "max96712"
    unload_mod "ambrg"
    unload_mod "vio_monitor"
    unload_mod "dsplog"
    unload_mod "iav"
    unload_mod "imgproc"
    unload_mod "amba_opti_print"
    unload_mod "dsp"
    unload_mod "ambnl"
    unload_mod "msg"
    unload_mod "ambcma"
    unload_mod "hw_timer"
fi

# 3. Driver loading in forward dependency order (strictly individual, no wildcards)
if [ -d "$MODULES_DIR" ]; then
    load_mod "hw_timer"          "$ENABLE_HW_TIMER"
    load_mod "ambcma"            "$ENABLE_AMBCMA"            "$ARGS_AMBCMA"
    load_mod "msg"               "$ENABLE_MSG"
    load_mod "ambnl"             "$ENABLE_AMBNL"
    load_mod "dsp"               "$ENABLE_DSP"
    load_mod "amba_opti_print"   "$ENABLE_AMBA_OPTI_PRINT"
    load_mod "imgproc"           "$ENABLE_IMGPROC"
    load_mod "iav"               "$ENABLE_IAV"
    load_mod "dsplog"            "$ENABLE_DSPLOG"
    load_mod "vio_monitor"       "$ENABLE_VIO_MONITOR"
    load_mod "ambrg"             "$ENABLE_AMBRG"
    load_mod "max96712"          "$ENABLE_MAX96712"          "$ARGS_MAX96712"
    load_mod "os08a10_mipi_brg"  "$ENABLE_OS08A10_MIPI_BRG"  "$ARGS_OS08A10_MIPI_BRG"
    load_mod "cavalry"           "$ENABLE_CAVALRY"           "$ARGS_CAVALRY"
    load_mod "amba_virt"         "$ENABLE_AMBA_VIRT"         "$ARGS_AMBA_VIRT"
    load_mod "amba_pci_platform" "$ENABLE_AMBA_PCI_PLATFORM"
    load_mod "pvrsrvkm"          "$ENABLE_PVRSRVKM"
    load_mod "testGDMA"          "$ENABLE_TEST_GDMA"
    load_mod "diag_gdma"         "$ENABLE_DIAG_GDMA"
    load_mod "diag_stage2_pte"   "$ENABLE_DIAG_STAGE2_PTE"
    load_mod "amba_otp"          "$ENABLE_AMBA_OTP"
fi
