#!/bin/sh
#
# tools/bringup/scripts/run_sensor_bringup.sh
#
# Earlier runner. It drives whatever modules and userspace are already on the
# target. That tree stalled vin0_idsp_sof / vin0_idsp_last_pixel at 7.
# The 2026-10-01 checkpoint that streamed 900 frames at 30 fps is
# stage_fresh_bringup.sh plus run_sensor_bringup_fresh.sh. Use those.
#
# Copyright (C) 2026, Ambarella International LLC
#

set -eu

TARGET="${1:-n1-655-pro}"
LUA_SCRIPT="${2:-/persist/scripts/n1_655_vin0_1080p_linear.lua}"

echo "================================================================================"
echo " Starting Deterministic Sensor Bringup on: $TARGET"
echo "================================================================================"

ssh "$TARGET" LUA_SCRIPT="$LUA_SCRIPT" LOAD_DSPLOG="${LOAD_DSPLOG:-0}" 'sh -s' << 'EOF'
set -eu

stop_dsplog_capture() {
    if [ -x /persist/bin/test_cap_cr_v2 ]; then
        echo "=== CR CAPTURE DUMP ==="
        /persist/bin/test_cap_cr_v2 -d -o /tmp 2>&1 || true
        ls -la /tmp/*.bin /tmp/*.y /tmp/*.uv 2>/dev/null || true
    fi
    [ -n "${DSPLOG_PID:-}" ] || return 0
    killall dsplog_cap 2>/dev/null || true
    sleep 1
    DSPLOG_PID=""
    echo "=== ORC FIRMWARE LOG (parsed) ==="
    /persist/bin/dsplog_cap -i /tmp/dsplog_crash.bin -f /tmp/dsplog_crash.txt -d /persist/firmware 2>&1 || true
    ls -la /tmp/*dsplog*.txt 2>/dev/null || true
    for txt in /tmp/*dsplog*.txt; do
        [ -f "$txt" ] || continue
        echo "--- Last 25 lines of $txt ---"
        tail -n 25 "$txt" 2>/dev/null || true
    done
}

trap 'stop_dsplog_capture; echo "=== DSP STATE ==="; cat /proc/ambarella/dsp_state 2>/dev/null || true; echo "=== KERNEL / DSP DMESG LOG ==="; dmesg | tail -n 60' EXIT

echo "=== 0. Clearing dmesg buffer for clean run capture ==="
dmesg -c > /dev/null

echo "=== 1. Pointing firmware search path to /persist/firmware ==="
echo -n "/persist/firmware" > /sys/module/firmware_class/parameters/path
mkdir -p /usr/lib/firmware
ln -sf /persist/firmware/lcd_r9611 /usr/lib/firmware/lcd_r9611 2>/dev/null || true

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
# Cooper leaves every ambcma size parameter unset, but EVE's ambcma cannot do
# that: with dsp_buf_size omitted, init_bufs() oopses during probe and
# cma_alloc() then fails with "prealloc mmb hasn't been allocated", taking down
# dsp (-12) and iav (-14). 0x40000000 is 1 GiB of the 1664 MiB iav@0 partition,
# so it does not starve BSB/overlay the way the Cooper-vs-EVE report assumed
# (that report was written against an older 0x68000000 setting).
load_mod "ambcma" "ama_enable=1 dsp_buf_size=0x40000000"
load_mod "cavalry" "virt_user_window_mb=2048"
load_mod "msg"
load_mod "ambnl"
load_mod "dsp"
load_mod "amba_opti_print"
load_mod "imgproc"
load_mod "iav"
# dsplog captures the ORC firmware log ring in IAV_PART_DSP_LOG. It needs
# iav_register_dsplog_ops from iav.ko, so it must load after iav. Without it
# dsplog_cap returns 0 bytes and DSP-side assertions look silent.
# dsplog panics the board when ambcma auto-partitions (DSP private region grows
# from 1 GiB to the full 1664 MiB and dsplog_init memsets a log region derived
# from the old layout). It is diagnostic-only, so keep it opt-in.
[ "${LOAD_DSPLOG:-0}" = "1" ] && load_mod "dsplog"
# amba_otp is mandatory: dsp_monitor_service aborts without it, and the DSP
# then fails profile activation with an OrcVIN assert at orcvin_boot.c.
# It must be built from drivers/private/amba_otp/sec_v2 with -DAMBA_SOC_N1_655.
load_mod "amba_otp"
# lcd_r9611 drives MIPI DSI display for VOUT0 and preview canvas.
load_mod "lcd_r9611" "i2c_addr_map=1 i2c_connected=0"

echo "=== 3. Creating character device nodes from dynamic major numbers ==="
IAV_MAJOR=$(awk '$2=="iav_ucode" {print $1}' /proc/devices)
CAV_MAJOR=$(awk '$2=="cavalry" {print $1}' /proc/devices)
# amba_otp.ko registers its chrdev region under the name "otp", not "amba_otp".
OTP_MAJOR=$(awk '$2=="otp" {print $1}' /proc/devices)

echo "Discovered dynamic majors: IAV=$IAV_MAJOR CAV=$CAV_MAJOR OTP=$OTP_MAJOR"
[ -n "$IAV_MAJOR" ] && rm -f /dev/iav /dev/ucode && mknod /dev/iav c "$IAV_MAJOR" 1 && mknod /dev/ucode c "$IAV_MAJOR" 0 && chmod 666 /dev/iav /dev/ucode
[ -n "$CAV_MAJOR" ] && rm -f /dev/cavalry && mknod /dev/cavalry c "$CAV_MAJOR" 0 && chmod 666 /dev/cavalry
[ -n "$OTP_MAJOR" ] && rm -f /dev/amba_otp && mknod /dev/amba_otp c "$OTP_MAJOR" 0 && chmod 600 /dev/amba_otp

echo "=== 4. Uploading microcode ===" 
/persist/bin/load_ucode /persist/firmware

echo "=== 5. Launching DSP monitor service (after ucode, before IDLE) ==="
ROOTFS=$(ls -d /persist/clear/volumes/*.container/rootfs 2>/dev/null | head -n 1)
LD_SO=""
LIB_PATH=""
if [ -f "/persist/lib/ld-linux-aarch64.so.1" ]; then
    LD_SO="/persist/lib/ld-linux-aarch64.so.1"
    LIB_PATH="/persist/lib"
elif [ -n "$ROOTFS" ] && [ -d "$ROOTFS" ]; then
    LD_SO="$ROOTFS/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
    LIB_PATH="$ROOTFS/usr/lib/aarch64-linux-gnu:$ROOTFS/lib/aarch64-linux-gnu:$ROOTFS/usr/lib:$ROOTFS/lib:/persist/lib"
fi

if [ -n "$LD_SO" ] && [ -f "$LD_SO" ]; then
    killall -9 dsp_monitor_service 2>/dev/null || true
    "$LD_SO" --library-path "$LIB_PATH" /persist/bin/dsp_monitor_service > /tmp/dsp_monitor.log 2>&1 &
    DSP_MON_PID=$!
    echo "dsp_monitor_service started (PID $DSP_MON_PID), waiting 3s for chip ID write..."
    sleep 3
    echo "--- dsp_monitor_service log ---"
    cat /tmp/dsp_monitor.log 2>/dev/null || true
    echo "-------------------------------"
else
    echo "WARNING: No ld-linux found, dsp_monitor_service skipped."
fi

echo "=== 6. Entering DSP IDLE state ==="
/persist/bin/test_encode --idle --nopreview || true

echo "=== 6b. Verifying chip ID after IDLE ==="
CHIP_INFO=$(/persist/bin/test_encode --show-chip-info 2>&1 || true)
echo "$CHIP_INFO"
if ! echo "$CHIP_INFO" | grep -q "CHIP Version : N1_655"; then
    echo "WARNING: Chip version is not N1_655, enforcing set_debug_chip_id..."
    /persist/bin/set_debug_chip_id
    /persist/bin/test_encode --show-chip-info 2>&1 || true
fi


echo "=== 7. Preparing 3A Tuning Database & Paths ==="
mkdir -p /usr/share/ambarella
ln -sf /persist/share/ambarella/idsp /usr/share/ambarella/idsp 2>/dev/null || true
ln -sf /persist/share/ambarella/lua_scripts /usr/share/ambarella/lua_scripts 2>/dev/null || true
ln -sf /persist/firmware/default_binary.bin /usr/share/ambarella/idsp/default_binary.bin 2>/dev/null || true
killall -9 test_aaa_service 2>/dev/null || true

echo "=== 8. Asserting Camera Power-Over-Coax (POC) GPIOs (92, 93, 98, 99) ==="
for g in 92 93 98 99; do
    [ -d "/sys/class/gpio/gpio$g" ] || echo $g > /sys/class/gpio/export 2>/dev/null || true
    echo high > /sys/class/gpio/gpio$g/direction 2>/dev/null || echo 1 > /sys/class/gpio/gpio$g/value
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

# echo "=== 10b. Forcing MAX96712 Continuous MIPI Clock Mode (Omitted in Trial 1) ==="
# /persist/bin/check_locks --set-cont-clk || true

echo "=== 10c. Starting continuous ORC firmware log capture ==="
# Must start after the DSP has booted into IDLE, otherwise dsplog_cap aborts
# with "Invalid chip_arch [0]". The OrcVIN boot assertion fires during the
# CMD_DSP_SET_PROFILE / mode 2 entry driven by step 11 below.
if [ "${LOAD_DSPLOG:-0}" = "1" ]; then
    rm -f /tmp/*dsplog*.bin /tmp/*dsplog*.txt 2>/dev/null || true
    # -o specifies the output binary for background multi-core capture;
    # -i and -f are used when parsing captured binary into text.
    /persist/bin/dsplog_cap -m all -l 4 -r 0 -o /tmp/dsplog_crash.bin > /tmp/dsplog_cap.log 2>&1 &
    DSPLOG_PID=$!
    sleep 2
    echo "dsplog_cap started (PID $DSPLOG_PID)"
    head -n 5 /tmp/dsplog_cap.log 2>/dev/null || true
else
    echo "dsplog capture disabled (set LOAD_DSPLOG=1 to enable)"
fi

echo "=== 10d. Starting 3A Tuning Service (before preview entry) ==="
killall -9 test_aaa_service 2>/dev/null || true
nohup /persist/bin/test_aaa_service -a < /dev/null > /persist/aaa.log 2>&1 &
sleep 2
cat /persist/aaa.log 2>/dev/null || true

echo "=== 11. Applying Lua Resource Configuration ($LUA_SCRIPT) & Entering Preview ==="
VOUT_CFG_ARG=""
if [ -f "/persist/scripts/vout0_dsi.lua" ] && grep -q '"prev"' "$LUA_SCRIPT" 2>/dev/null; then
    VOUT_CFG_ARG="--vout-cfg /persist/scripts/vout0_dsi.lua"
    echo "Preview canvas detected in Lua, passing $VOUT_CFG_ARG"
fi
/persist/bin/test_encode --resource-cfg "$LUA_SCRIPT" $VOUT_CFG_ARG

echo "=== 12. Verifying 3A Tuning Service Status ==="
cat /persist/aaa.log 2>/dev/null || true

echo "=== 13. Checking Streaming Status & Interrupt Counters ==="
sleep 1
cat /proc/interrupts | grep -E "vin|vdsp"

echo "=== 14. Executing Video Encoding Test (150 frames) ==="
/persist/bin/test_encode -A -h 1080p -b 0 -e -d 150 || true

echo "=== 15. Post-Test Interrupt Counters & Final Status ==="
cat /proc/interrupts | grep -E "vin|vdsp"

EOF

echo "================================================================================"
echo " Bringup script completed on: $TARGET"
echo "================================================================================"
