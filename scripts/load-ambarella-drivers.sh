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
ENABLE_HW_TIMER=${ENABLE_HW_TIMER:-1}
ENABLE_AMBCMA=${ENABLE_AMBCMA:-1}
ARGS_AMBCMA=${ARGS_AMBCMA:-"ama_enable=1 dsp_buf_size=0x40000000"}

# IPC & DSP Video Processing Subsystem
ENABLE_MSG=${ENABLE_MSG:-1}
ENABLE_AMBNL=${ENABLE_AMBNL:-1}
ARGS_AMBNL=${ARGS_AMBNL:-"amb_nl_cn=1"}
ENABLE_DSP=${ENABLE_DSP:-1}
ENABLE_AMBA_OPTI_PRINT=${ENABLE_AMBA_OPTI_PRINT:-1}
ENABLE_IMGPROC=${ENABLE_IMGPROC:-1}
ENABLE_IAV=${ENABLE_IAV:-1}
ENABLE_DSPLOG=${ENABLE_DSPLOG:-0}

# Camera / Bridge / Deserializer Subsystem
ENABLE_VIO_MONITOR=${ENABLE_VIO_MONITOR:-0}
ENABLE_AMBRG=${ENABLE_AMBRG:-0}
ENABLE_MAX96712=${ENABLE_MAX96712:-0}
ARGS_MAX96712=${ARGS_MAX96712:-"id=0x01 dts_addr=1 use_max20087=0 port_mode=0"}
ENABLE_OS08A10_MIPI_BRG=${ENABLE_OS08A10_MIPI_BRG:-0}
ARGS_OS08A10_MIPI_BRG=${ARGS_OS08A10_MIPI_BRG:-"brg_id=0x1"}

# NPU / Virtualization / GPU Subsystem
# (amba_virt provides /dev/amba_virt_shm required by Ubuntu and Alpine HVM guests)
ENABLE_CAVALRY=${ENABLE_CAVALRY:-1}
ARGS_CAVALRY=${ARGS_CAVALRY:-"virt_user_window_mb=2048"}
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
    check_status "ambnl"             "$ENABLE_AMBNL"             "$ARGS_AMBNL"
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
    load_mod "ambnl"             "$ENABLE_AMBNL"             "$ARGS_AMBNL"
    load_mod "dsp"               "$ENABLE_DSP"
    load_mod "amba_opti_print"   "$ENABLE_AMBA_OPTI_PRINT"
    load_mod "imgproc"           "$ENABLE_IMGPROC"
    load_mod "iav"               "$ENABLE_IAV"
    load_mod "dsplog"            "$ENABLE_DSPLOG"

    # Assert POC power GPIOs if SerDes or Sensor is enabled
    if [ "$ENABLE_MAX96712" = "1" ] || [ "$ENABLE_AMBRG" = "1" ] || [ "$ENABLE_OS08A10_MIPI_BRG" = "1" ]; then
        for g in 92 93 98 99; do
            echo $g > /sys/class/gpio/export 2>/dev/null || true
            echo out > /sys/class/gpio/gpio$g/direction 2>/dev/null || true
            echo 1 > /sys/class/gpio/gpio$g/value 2>/dev/null || true
        done
        sleep 2
    fi

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

# 4. Create character device nodes dynamically from /proc/devices
IAV_MAJOR=$(awk '$2=="iav_ucode" {print $1}' /proc/devices 2>/dev/null || true)
CAV_MAJOR=$(awk '$2=="cavalry" {print $1}' /proc/devices 2>/dev/null || true)
OTP_MAJOR=$(awk '$2=="amba_otp" {print $1}' /proc/devices 2>/dev/null || true)

[ -n "$IAV_MAJOR" ] && rm -f /dev/iav /dev/ucode && mknod /dev/iav c "$IAV_MAJOR" 1 && mknod /dev/ucode c "$IAV_MAJOR" 0 && chmod 666 /dev/iav /dev/ucode
[ -n "$CAV_MAJOR" ] && rm -f /dev/cavalry && mknod /dev/cavalry c "$CAV_MAJOR" 0 && chmod 666 /dev/cavalry
[ -n "$OTP_MAJOR" ] && rm -f /dev/amba_otp && mknod /dev/amba_otp c "$OTP_MAJOR" 0 && chmod 600 /dev/amba_otp

# 5. Start amba-virt-backend background supervisor if binary present
BACKEND_BIN=""
if [ -x "$PERSIST_DIR/bin/amba-virt-backend" ]; then
    BACKEND_BIN="$PERSIST_DIR/bin/amba-virt-backend"
elif [ -x "/usr/local/bin/amba-virt-backend" ]; then
    BACKEND_BIN="/usr/local/bin/amba-virt-backend"
fi

if [ -n "$BACKEND_BIN" ]; then
    mkdir -p "$PERSIST_DIR/log" "$PERSIST_DIR/etc"
    if ! pidof amba-virt-backend >/dev/null 2>&1; then
        echo "Starting amba-virt-backend supervisor with crash backoff..."
        (
            FAIL_COUNT=0
            FIRST_FAIL_TIME=0
            while true; do
                if [ -x "$BACKEND_BIN" ]; then
                    NOW=$(date +%s)
                    if [ "$FAIL_COUNT" -eq 0 ]; then
                        FIRST_FAIL_TIME=$NOW
                    fi
                    
                    "$BACKEND_BIN" --port 5556 --log-file "$PERSIST_DIR/log/amba-virt-backend.log" >> "$PERSIST_DIR/log/amba-virt-backend.log" 2>&1 || true
                    
                    NOW=$(date +%s)
                    FAIL_COUNT=$((FAIL_COUNT + 1))
                    
                    if [ $((NOW - FIRST_FAIL_TIME)) -le 60 ] && [ "$FAIL_COUNT" -ge 5 ]; then
                        echo "[$(date)] amba-virt-backend crashing repeatedly (5 times in 60s). Backing off 30s..." >> "$PERSIST_DIR/log/amba-virt-backend.log"
                        sleep 30
                        FAIL_COUNT=0
                    else
                        if [ $((NOW - FIRST_FAIL_TIME)) -gt 60 ]; then
                            FAIL_COUNT=1
                            FIRST_FAIL_TIME=$NOW
                        fi
                        sleep 2
                    fi
                else
                    sleep 5
                fi
            done
        ) &
    fi
fi

