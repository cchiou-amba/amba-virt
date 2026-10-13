#!/bin/sh
# tools/bringup/camera_boot_order.sh
#
# Read-only check that an amba-virt-server run configured the camera windows before it reported
# ready. Prints one JSON record and exits 0 only when the windows were configured before ready
# and the DSP and bitstream windows have the sizes the camera pipeline is expected to give.
#
#   tools/bringup/camera_boot_order.sh <ssh host alias> [expected-dsp-bytes] [expected-bsb-bytes]
#
# The "ready" time is the daemon log's "worker <pid> is ready" line; the windows time is
# `amba-virt-ctl camera-status` ready_ms. Both are CLOCK_MONOTONIC milliseconds.
#
# Copyright (C) 2026, Ambarella International LLC

set -eu

NODE=${1:?usage: camera_boot_order.sh <ssh host alias> [dsp-bytes] [bsb-bytes]}
WANT_DSP=${2:-1073741824}
WANT_BSB=${3:-16777216}

IMAGE=$(ssh "$NODE" 'cat /run/eve-release')
READY_MS=$(ssh "$NODE" "sed -n 's/^\[\([0-9]*\)\] worker [0-9]* is ready\$/\1/p' /run/amba-virt/server-daemon.log" | tail -n 1)
STATUS=$(ssh "$NODE" '/hostfs/usr/bin/amba-virt-ctl camera-status')

field() {
	printf '%s\n' "$STATUS" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

WINDOWS=$(field windows)
DSP=$(field dsp)
BSB=$(field bsb)
WIN_MS=$(field ready_ms)

BEFORE=false
if [ -n "$READY_MS" ] && [ "$WINDOWS" = ready ] && [ "${WIN_MS:-0}" -gt 0 ] && [ "$WIN_MS" -lt "$READY_MS" ]; then
	BEFORE=true
fi

printf '{"node":"%s","image":"%s","ready_ms":%s,"windows":"%s","windows_ready_ms":%s,"windows_before_ready":%s,"dsp":%s,"bsb":%s}\n' \
	"$NODE" "$IMAGE" "${READY_MS:-0}" "${WINDOWS:-none}" "${WIN_MS:-0}" "$BEFORE" "${DSP:-0}" "${BSB:-0}"

[ "$BEFORE" = true ] && [ "${DSP:-0}" = "$WANT_DSP" ] && [ "${BSB:-0}" = "$WANT_BSB" ]
