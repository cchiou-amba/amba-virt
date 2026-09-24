#!/proc/boot/sh
#
# qnx-serial-console.sh
#
# Dedicated serial console supervisor for QNX 8.0 HVM.
# Manages terminal line discipline and automatic session respawn.
#
# Copyright (C) 2026, Ambarella International LLC
#

DEV="${1:-/dev/ser3}"

while [ ! -e "${DEV}" ]; do
    sleep 1
done

while true; do
    if [ -e "${DEV}" ]; then
        stty +opost +onlcr +icrnl +echo +echoe +icanon <"${DEV}" 2>/dev/null || true
        /system/bin/login -f root <"${DEV}" >"${DEV}" 2>&1
    fi
    sleep 1
done
