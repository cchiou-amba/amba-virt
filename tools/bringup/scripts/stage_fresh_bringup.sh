#!/bin/sh
#
# tools/bringup/scripts/stage_fresh_bringup.sh
#
# Assemble a self-contained sensor bringup stage from trusted inputs only:
#   - DSP/IAV and camera-chain modules built from pristine Cooper SDK sources
#     (build/pristine-dsp_v6, build/pristine-chain), signed for the EVE kernel
#   - cavalry.ko and amba_otp.ko from build/modules (sources match Cooper)
#   - microcode, userspace, libraries and Lua from the Cooper golden rootfs
#     (build/cooper-golden/root)
# Nothing is taken from the target's existing /persist tree.
#
# Usage: stage_fresh_bringup.sh [target-host] [remote-dir]
#
# Copyright (C) 2026, Ambarella International LLC
#

set -eu

TARGET="${1:-n1-655-devkit}"
REMOTE="${2:-/persist/fresh-bringup}"

TOP=$(cd "$(dirname "$0")/../../.." && pwd)
KREL=6.1.112-linuxkit-eb5e281e5e4d-custom
KDIR="$TOP/build/usr/src/linux-headers-$KREL"
GOLD="$TOP/build/cooper-golden/root"
STAGE="$TOP/build/stage-fresh"

SIGN="$KDIR/scripts/sign-file"
KEY="$TOP/build/certs/signing_key.pem"
CERT="$TOP/build/certs/signing_key.x509"

rm -rf "$STAGE"
mkdir -p "$STAGE/modules" "$STAGE/firmware" "$STAGE/bin" "$STAGE/lib" \
    "$STAGE/share/ambarella"

for ko in \
    "$TOP"/build/pristine-dsp_v6/*.ko \
    "$TOP"/build/pristine-chain/*/*.ko \
    "$TOP/build/modules/cavalry.ko" \
    "$TOP/build/modules/amba_otp.ko"; do
    cp "$ko" "$STAGE/modules/"
done
for ko in "$STAGE"/modules/*.ko; do
    "$SIGN" sha256 "$KEY" "$CERT" "$ko"
    vm=$(modinfo -F vermagic "$ko")
    case "$vm" in
        "$KREL "*) ;;
        *) echo "vermagic mismatch: $ko: $vm" >&2; exit 1 ;;
    esac
done

# Mirror Cooper's flat /lib/firmware layout for load_ucode and request_firmware.
for f in orccode.bin orcidsp0.bin orcidsp1.bin orcvin0.bin orcvin1.bin \
    orcme0.bin orcmdxf0.bin default_binary.bin cavalry.bin; do
    cp "$GOLD/lib/firmware/$f" "$STAGE/firmware/"
done
cp -a "$GOLD/lib/firmware/lcd_r9611" "$STAGE/firmware/"
mkdir -p "$STAGE/firmware/ambarella/n1_655/dsp"
cp -a "$GOLD/lib/firmware/ambarella/n1_655/dsp/260920_075008" \
    "$STAGE/firmware/ambarella/n1_655/dsp/"
ln -s 260920_075008 "$STAGE/firmware/ambarella/n1_655/dsp/current"

for b in load_ucode dsp_monitor_service test_encode test_aaa_service \
    dsplog_cap test_cap_cr_v2 test_stream; do
    cp "$GOLD/usr/bin/$b" "$STAGE/bin/"
done

cp -L "$GOLD/usr/lib/ld-linux-aarch64.so.1" "$STAGE/lib/"
for l in libamba-mcl.so.4 libamba-utils.so.0 libcavalry_mem.so.0 \
    libdatatx.so.1 libdspmonitor.so.1 libimg_flow_n1_655.so.2 libnnctrl.so.3; do
    cp -L "$GOLD/usr/lib/$l" "$STAGE/lib/"
done
for l in libc.so.6 libm.so.6 liblua-5.4.so; do
    cp -L "$GOLD/usr/lib64/$l" "$STAGE/lib/"
done

cp -a "$GOLD/usr/share/ambarella/idsp" "$GOLD/usr/share/ambarella/lua_scripts" \
    "$STAGE/share/ambarella/"

cp "$TOP/tools/bringup/scripts/run_sensor_bringup_fresh.sh" "$STAGE/run.sh"
chmod +x "$STAGE/run.sh"
echo "$KREL" > "$STAGE/KERNEL_RELEASE"

# Every NEEDED entry of every staged ELF must resolve inside lib/.
missing=0
for f in "$STAGE"/bin/* "$STAGE"/lib/*; do
    for n in $(aarch64-linux-gnu-readelf -d "$f" 2>/dev/null |
        sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        [ -e "$STAGE/lib/$n" ] || { echo "unresolved: $f needs $n" >&2; missing=1; }
    done
done
[ "$missing" = 0 ] || exit 1

(cd "$STAGE" && find . -type f ! -name MANIFEST.md5 | sort | xargs md5sum > MANIFEST.md5)
echo "Staged $(wc -l < "$STAGE/MANIFEST.md5") files in $STAGE"

[ "$TARGET" = "-" ] && exit 0

ssh "$TARGET" "rm -rf '$REMOTE' && mkdir -p '$REMOTE'"
tar -C "$STAGE" -cf - . | ssh "$TARGET" "tar -C '$REMOTE' -xf - && sync"
# The board is cold-reset by MCU power cut; unsynced ext4 data comes back empty.
ssh "$TARGET" "sync && echo 3 > /proc/sys/vm/drop_caches && cd '$REMOTE' && md5sum -c -s MANIFEST.md5 && echo 'Manifest verified on $TARGET:$REMOTE'"
