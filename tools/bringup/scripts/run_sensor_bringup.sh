#!/bin/sh
#
# tools/bringup/scripts/run_sensor_bringup.sh
#
# Unified, deterministic end-to-end sensor bringup and video pipeline test runner.
# Executes cleanly from boot without trial-and-error insmod/rmmod cycles.
#
# Copyright (C) 2026, Ambarella International LLC
#

set -eu

TARGET="${1:-n1-655-devkit}"
LUA_SCRIPT="${2:-/persist/scripts/n1_655_vin0_1080p_linear.lua}"

echo "================================================================================"
echo " Starting Deterministic Sensor Bringup on: $TARGET"
echo "================================================================================"

ssh "$TARGET" LUA_SCRIPT="$LUA_SCRIPT" 'sh -s' << 'EOF'
set -eu

trap 'echo "=== KERNEL / DSP DMESG LOG ==="; dmesg | tail -n 60' EXIT

echo "=== 0. Clearing dmesg buffer for clean run capture ==="
dmesg -c > /dev/null

echo "=== 1. Pointing firmware search path to /persist/firmware ==="
echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path

load_mod() {
    mod="$1"
    args="${2:-}"
    ko="/persist/modules/${mod}.ko"
    if ! lsmod | grep -q "^${mod} "; then
        echo "Loading $ko $args..."
        if [ -n "$args" ]; then
            insmod "$ko" $args
        else
            insmod "$ko"
        fi
    else
        echo "Module $mod is already loaded."
    fi
}

echo "=== 2. Inserting base kernel video stack ==="
load_mod "hw_timer"
load_mod "ambcma" "ama_enable=1 dsp_buf_size=0x68000000"
load_mod "cavalry" "virt_user_window_mb=2048"
load_mod "msg"
load_mod "ambnl"
load_mod "dsp"
load_mod "amba_opti_print"
load_mod "imgproc"
load_mod "iav"
load_mod "amba_otp" || true

echo "=== 3. Creating character device nodes from dynamic major numbers ==="
IAV_MAJOR=$(awk '$2=="iav_ucode" {print $1}' /proc/devices)
CAV_MAJOR=$(awk '$2=="cavalry" {print $1}' /proc/devices)
OTP_MAJOR=$(awk '$2=="amba_otp" {print $1}' /proc/devices)

echo "Discovered dynamic majors: IAV=$IAV_MAJOR CAV=$CAV_MAJOR OTP=$OTP_MAJOR"
[ -n "$IAV_MAJOR" ] && rm -f /dev/iav /dev/ucode && mknod /dev/iav c "$IAV_MAJOR" 1 && mknod /dev/ucode c "$IAV_MAJOR" 0 && chmod 666 /dev/iav /dev/ucode
[ -n "$CAV_MAJOR" ] && rm -f /dev/cavalry && mknod /dev/cavalry c "$CAV_MAJOR" 0 && chmod 666 /dev/cavalry
[ -n "$OTP_MAJOR" ] && rm -f /dev/amba_otp && mknod /dev/amba_otp c "$OTP_MAJOR" 0 && chmod 600 /dev/amba_otp

echo "=== 4. Uploading microcode ===" 
/persist/bin/load_ucode /persist/firmware

echo "=== 5. Launching DSP monitor service (after ucode, before IDLE) ==="
ROOTFS=$(ls -d /persist/clear/volumes/*.container/rootfs 2>/dev/null | head -n 1)
if [ -n "$ROOTFS" ] && [ -d "$ROOTFS" ]; then
    LD_SO="$ROOTFS/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
    LIB_PATH="$ROOTFS/usr/lib/aarch64-linux-gnu:$ROOTFS/lib/aarch64-linux-gnu:$ROOTFS/usr/lib:$ROOTFS/lib:/persist/lib"
    killall -9 dsp_monitor_service 2>/dev/null || true
    "$LD_SO" --library-path "$LIB_PATH" /persist/bin/dsp_monitor_service > /tmp/dsp_monitor.log 2>&1 &
    DSP_MON_PID=$!
    echo "dsp_monitor_service started (PID $DSP_MON_PID), waiting 3s for chip ID write..."
    sleep 3
    echo "--- dsp_monitor_service log ---"
    cat /tmp/dsp_monitor.log 2>/dev/null || true
    echo "-------------------------------"
else
    echo "WARNING: No container rootfs found, dsp_monitor_service skipped."
fi

echo "=== 6. Entering DSP IDLE state ==="
/persist/bin/test_encode --idle --nopreview

echo "=== 6b. Verifying chip ID after IDLE ==="
/persist/bin/test_encode --show-chip-info 2>&1 || true


echo "=== 7. Initializing 3A Tuning Database & Service (Early Netlink Port 27 Binding) ==="
mkdir -p /usr/share/ambarella
ln -sf /persist/share/ambarella/idsp /usr/share/ambarella/idsp 2>/dev/null || true
ln -sf /persist/share/ambarella/lua_scripts /usr/share/ambarella/lua_scripts 2>/dev/null || true
ln -sf /persist/firmware/default_binary.bin /usr/share/ambarella/idsp/default_binary.bin 2>/dev/null || true
killall -9 test_aaa_service 2>/dev/null || true
nohup /persist/bin/test_aaa_service -a < /dev/null > /persist/aaa.log 2>&1 &
sleep 2
cat /persist/aaa.log

echo "=== 8. Asserting Camera Power-Over-Coax (POC) GPIOs (92, 93, 98, 99) ==="
for g in 92 93 98 99; do
    echo $g > /sys/class/gpio/export 2>/dev/null || true
    echo out > /sys/class/gpio/gpio$g/direction
    echo 1 > /sys/class/gpio/gpio$g/value
done
echo "Waiting 4 seconds for camera serializers & oscillators to stabilize..."
sleep 4

echo "=== 9. Inserting SerDes & Sensor drivers (Port A / Bridge 0 id=0x01 brg_id=0x1) ==="
load_mod "vio_monitor"
load_mod "ambrg"
load_mod "max96712" "id=0x01 dts_addr=1 use_max20087=0"
load_mod "os08a10_mipi_brg" "brg_id=0x1"

echo "=== 10. Verifying Sensor Probe ==="
/persist/bin/test_encode --show-vsrc-info

echo "=== 10b. Forcing MAX96712 Continuous MIPI Clock Mode (Reg 0x08A0 = 0x84) ==="
/persist/bin/check_locks --set-cont-clk

echo "=== 11. Applying Lua Resource Configuration ($LUA_SCRIPT) & Entering Preview ==="
/persist/bin/test_encode --resource-cfg "$LUA_SCRIPT"

echo "=== 12. Checking Streaming Status & Interrupt Counters ==="
sleep 1
cat /proc/interrupts | grep -E "vin|vdsp"

echo "=== 13. Executing Video Encoding Test (150 frames) ==="
/persist/bin/test_encode -A -h 1080p -e -d 150
EOF

echo "================================================================================"
echo " Bringup script completed on: $TARGET"
echo "================================================================================"
