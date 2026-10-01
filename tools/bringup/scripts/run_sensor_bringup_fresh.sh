#!/bin/sh
#
# tools/bringup/scripts/run_sensor_bringup_fresh.sh
#
# Target-side Port A (VIN0, OS08A10 behind MAX96712 id=0x01) bringup that
# loads only from its own stage directory, produced by stage_fresh_bringup.sh.
# Sequence follows doc/EVE-Physical-Camera-Bringup.md. Must run on a freshly
# cold-booted board: any preloaded DSP/IAV/Cavalry module or daemon is fatal.
#
# Usage (on target): /persist/fresh-bringup/run.sh
#   LOAD_DSPLOG=1  also insert dsplog.ko after iav.ko
#
# Copyright (C) 2026, Ambarella International LLC
#

set -u

STAGE=$(cd "$(dirname "$0")" && pwd)
OUT="$STAGE/out-$(date +%Y%m%d-%H%M%S)"
LD_SO="$STAGE/lib/ld-linux-aarch64.so.1"
LUA="$STAGE/share/ambarella/lua_scripts/n1_655_vin0_1080p_linear.lua"
VOUT_LUA="$STAGE/share/ambarella/lua_scripts/vout0_dsi.lua"

MODS="hw_timer ambcma cavalry msg ambnl dsp amba_opti_print imgproc iav dsplog \
amba_otp lcd_r9611 vio_monitor ambrg max96712 os08a10_mipi_brg"

mkdir -p "$OUT"
exec > "$OUT/run.log" 2>&1

die() {
    echo "FATAL: $*"
    snapshot final
    exit 1
}

run() {
    prog="$1"; shift
    "$LD_SO" --library-path "$STAGE/lib" "$STAGE/bin/$prog" "$@"
}

snapshot() {
    tag="$1"
    grep -E 'vin|vdsp|idsp|vout' /proc/interrupts > "$OUT/irq-$tag.txt" 2>/dev/null
    cat /proc/ambarella/dsp_state > "$OUT/dsp_state-$tag.txt" 2>/dev/null
    dmesg > "$OUT/dmesg-$tag.txt"
    echo "--- interrupts ($tag) ---"
    cat "$OUT/irq-$tag.txt"
}

ins() {
    mod="$1"; shift
    echo "insmod $mod $*"
    insmod "$STAGE/modules/$mod.ko" "$@" || die "insmod $mod failed"
}

echo "=== 0. Preflight ==="
echo "stage: $STAGE"
uname -a
[ "$(uname -r)" = "$(cat "$STAGE/KERNEL_RELEASE")" ] || die "kernel release mismatch"
(cd "$STAGE" && md5sum -c -s MANIFEST.md5) || die "stage manifest mismatch"
for m in $MODS; do
    grep -q "^$m " /proc/modules && die "module $m already loaded before bringup"
done
ps -o pid,args | grep -E 'dsp_monitor_service|test_aaa_service|dsplog_cap' |
    grep -v grep && die "Ambarella daemon already running"
[ -e /usr/share/ambarella ] && die "/usr/share/ambarella already exists"
cat /proc/uptime
for n in memory/reg chosen/sys-dram-size; do
    printf '%s: ' "$n"; od -An -tx1 "/proc/device-tree/$n" | tr -s ' ' | tr -d '\n'; echo
done
for n in /proc/device-tree/reserved-memory/iav* /proc/device-tree/reserved-memory/cavalry*; do
    printf '%s: ' "${n##*/}"; od -An -tx1 "$n/reg" 2>/dev/null | tr -s ' ' | tr -d '\n'; echo
done
dmesg -c > "$OUT/dmesg-boot.txt"

echo "=== 1. Firmware path ==="
echo -n "$STAGE/firmware" > /sys/module/firmware_class/parameters/path

echo "=== 2. Core video drivers ==="
ins hw_timer
ins ambcma ama_enable=1 dsp_buf_size=0x40000000
ins cavalry
ins msg
ins ambnl
ins dsp
ins amba_opti_print
ins imgproc
ins iav
[ "${LOAD_DSPLOG:-0}" = "1" ] && ins dsplog
ins amba_otp
ins lcd_r9611 i2c_addr_map=1 i2c_connected=0

echo "=== 3. Device nodes ==="
mk() {
    node="$1"; name="$2"; minor="$3"; mode="$4"
    [ -c "$node" ] && return 0
    major=$(awk -v n="$name" '$2 == n { print $1 }' /proc/devices)
    [ -n "$major" ] || die "no chrdev $name for $node"
    mknod "$node" c "$major" "$minor" && chmod "$mode" "$node"
}
mk /dev/ucode iav_ucode 0 666
mk /dev/iav iav_ucode 1 666
mk /dev/cavalry cavalry 0 666
mk /dev/amba_otp otp 0 600
ls -l /dev/ucode /dev/iav /dev/cavalry /dev/amba_otp

echo "=== 4. DSP monitor service ==="
# Processes run under the staged ld.so, so pidof cannot find them by name.
run dsp_monitor_service > "$OUT/dsp_monitor.log" 2>&1 &
DSP_MON_PID=$!
sleep 3
cat "$OUT/dsp_monitor.log"
kill -0 "$DSP_MON_PID" 2>/dev/null || die "dsp_monitor_service exited"

echo "=== 5. Microcode ==="
run load_ucode "$STAGE/firmware" || die "load_ucode failed"

echo "=== 6. IDLE ==="
run test_encode --idle --nopreview
run test_encode --show-chip-info > "$OUT/chip.txt" 2>&1
cat "$OUT/chip.txt"
grep -q "N1_655" "$OUT/chip.txt" || die "chip is not reported as N1_655"

echo "=== 7. 3A data paths ==="
mkdir -p /usr/share/ambarella
ln -s "$STAGE/share/ambarella/idsp" /usr/share/ambarella/idsp
ln -s "$STAGE/share/ambarella/lua_scripts" /usr/share/ambarella/lua_scripts

echo "=== 7b. 3A service (before camera power and sensor, Entry 5) ==="
# Started later, ambnl drops the port 27 message with "no active user".
# script(1) gives it a pty so its stdio is line-buffered into the log.
script -q -f -E never -c "$LD_SO --library-path $STAGE/lib $STAGE/bin/test_aaa_service -a" \
    "$OUT/aaa.log" < /dev/null > /dev/null 2>&1 &
AAA_PID=$!
sleep 2
cat "$OUT/aaa.log"
kill -0 "$AAA_PID" 2>/dev/null || die "test_aaa_service exited"

echo "=== 8. Camera POC GPIOs 92 93 98 99 ==="
for g in 92 93 98 99; do
    [ -d "/sys/class/gpio/gpio$g" ] || echo "$g" > /sys/class/gpio/export
    echo high > "/sys/class/gpio/gpio$g/direction"
    printf 'gpio%s=%s\n' "$g" "$(cat /sys/class/gpio/gpio$g/value)"
done
sleep 4

echo "=== 9. SerDes and sensor (Port A) ==="
ins vio_monitor
ins ambrg
ins max96712 id=0x01 dts_addr=1 use_max20087=0
ins os08a10_mipi_brg brg_id=0x1
run test_encode --show-vsrc-info | tee "$OUT/vsrc.txt"
grep -q "os08a10.*active" "$OUT/vsrc.txt" || die "os08a10 not active on VIN0"

kill -0 "$AAA_PID" 2>/dev/null || die "test_aaa_service exited"
snapshot pre-preview

echo "=== 11. Resource config and preview ==="
# Cooper's N1-655 default 3A profile is 10 (ROBOT); its rootfs ships os08a10
# tuning only under idsp/robot, so the IPC default falls back to generic tables.
APP_IMG_PROFILE=${APP_IMG_PROFILE:-10}
run test_encode --resource-cfg "$LUA" --vout-cfg "$VOUT_LUA" \
    --app-img-profile "$APP_IMG_PROFILE"
echo "resource-cfg rc=$?"
snapshot preview-t0
sleep 5
snapshot preview-t5
echo "--- aaa.log after preview ---"
cat "$OUT/aaa.log"
for s in "AAA prepare done" "ADJ parameter version" "AEB parameter version"; do
    grep -aq "$s" "$OUT/aaa.log" || die "3A handshake missing: $s"
done
dmesg | grep -E "no active user|empty ISO cfg" && die "3A message dropped by kernel"
grep -aE "Can't find file:.*os08a10\.rgb\.(linear\.liso\.adj|aeb)_param" "$OUT/aaa.log" &&
    die "3A fell back to default ADJ/AEB tables"

ENC_FRAMES=${ENC_FRAMES:-900}
echo "=== 12. Encode $ENC_FRAMES frames and capture the bitstream ==="
# test_stream must be running before the stream is enabled, or it misses
# the session. -f is a filename prefix; -s is the statistics interval.
run test_stream -f "$OUT/video" -s "$ENC_FRAMES" > "$OUT/test_stream.log" 2>&1 &
STREAM_PID=$!
sleep 1
kill -0 "$STREAM_PID" 2>/dev/null || die "test_stream exited before encode"
run test_encode -A -h 1080p -b 0 -e -d "$ENC_FRAMES"
echo "encode rc=$?"
i=0
while kill -0 "$STREAM_PID" 2>/dev/null && [ "$i" -lt $((ENC_FRAMES / 30 + 15)) ]; do
    sleep 1
    i=$((i + 1))
done
kill "$STREAM_PID" 2>/dev/null || true
wait "$STREAM_PID" 2>/dev/null || true
ls -la "$OUT"/video* 2>/dev/null || echo "no bitstream file"
cat "$OUT/test_stream.log"
snapshot final
cat "$OUT/dsp_state-final.txt"
echo "--- aaa.log tail ---"
tail -n 30 "$OUT/aaa.log"
echo "--- dmesg tail ---"
tail -n 80 "$OUT/dmesg-final.txt"
echo "DONE out=$OUT"
