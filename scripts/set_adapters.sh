#!/bin/sh
# Replace I/O adapters on an existing edge-app instance; keep networks.
# ZCLI_TOKEN must already be in the environment. This script does not set it.
#
#   ./scripts/set_adapters.sh NAME --adapter=cavalry:cavalry --adapter=gpio0:gpio
#   ./scripts/set_adapters.sh NAME --clear-adapters
#   ./scripts/set_adapters.sh NAME --adapter=… --dry-run
#   ./scripts/set_adapters.sh NAME --adapter=… --restart

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"
DRY_RUN=0
DO_RESTART=0
CLEAR=0
ALLOW_VISORC=0
NAME=
ADAPTERS=

usage() {
	echo "usage: scripts/set_adapters.sh INSTANCE (--clear-adapters | --adapter=INTF:GRP...) [--allow-visorc] [--dry-run] [--restart]" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/set_adapters.sh: set ZCLI_TOKEN (do not commit it)" >&2
	exit 1
fi

while [ "$#" -gt 0 ]; do
	case "$1" in
	-h|--help)
		usage
		exit 0
		;;
	--dry-run)
		DRY_RUN=1
		shift
		;;
	--restart)
		DO_RESTART=1
		shift
		;;
	--clear-adapters)
		CLEAR=1
		shift
		;;
	--allow-visorc)
		ALLOW_VISORC=1
		shift
		;;
	--adapter=*)
		ADAPTERS="$ADAPTERS ${1#--adapter=}"
		shift
		;;
	--adapter)
		if [ "$#" -lt 2 ]; then
			echo "scripts/set_adapters.sh: --adapter needs intfname:assigngrp" >&2
			exit 1
		fi
		ADAPTERS="$ADAPTERS $2"
		shift 2
		;;
	-*)
		echo "scripts/set_adapters.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -n "$NAME" ]; then
			echo "scripts/set_adapters.sh: only one instance name" >&2
			exit 1
		fi
		NAME=$1
		shift
		;;
	esac
done

if [ -z "$NAME" ]; then
	usage
	exit 1
fi

if [ "$CLEAR" -eq 1 ] && [ -n "$ADAPTERS" ]; then
	echo "scripts/set_adapters.sh: use --clear-adapters or --adapter, not both" >&2
	exit 1
fi

if [ "$CLEAR" -eq 0 ] && [ -z "$ADAPTERS" ]; then
	echo "scripts/set_adapters.sh: need --adapter= or --clear-adapters" >&2
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "scripts/set_adapters.sh: need python3" >&2
	exit 1
fi

# shellcheck disable=SC2086
set -- $ADAPTERS
for spec in "$@"; do
	case "$spec" in
	*:*)
		;;
	*)
		echo "scripts/set_adapters.sh: adapter must be intfname:assigngrp ($spec)" >&2
		exit 1
		;;
	esac
	grp=${spec#*:}
	case "$grp" in
	cavalry|gpio|iav)
		if [ "$ALLOW_VISORC" -eq 0 ]; then
			echo "scripts/set_adapters.sh: $grp is VisORC; pass --allow-visorc for NOHYPER" >&2
			exit 1
		fi
		;;
	esac
done

raw=$("$ZCLI" -- --format=json edge-app-instance show "$NAME" --detail) || {
	echo "scripts/set_adapters.sh: failed to show $NAME" >&2
	exit 1
}

nets=$(printf '%s\n' "$raw" | python3 -c '
import json, sys
data = json.load(sys.stdin)
cfg = data.get("config") or data
for i in cfg.get("interfaces") or []:
    intf = i.get("intfname") or ""
    net = i.get("netinstname") or ""
    if intf and net:
        print("{}:{}".format(intf, net))
')

# Build: zcli edge-app-instance update NAME [--network-instance=...] [--adapter=...]
set -- edge-app-instance update "$NAME"
# shellcheck disable=SC2086
for net in $nets; do
	set -- "$@" --network-instance="$net"
done
if [ "$CLEAR" -eq 0 ]; then
	# shellcheck disable=SC2086
	for spec in $ADAPTERS; do
		set -- "$@" --adapter="$spec"
	done
fi

# zcli update with no --adapter and no --network-instance is a no-op.
if [ "$#" -eq 3 ] && [ "$CLEAR" -eq 1 ]; then
	echo "scripts/set_adapters.sh: $NAME has no networks; zcli cannot push an empty interface list" >&2
	exit 1
fi

echo "scripts/set_adapters.sh: $NAME" >&2
if [ "$DRY_RUN" -eq 1 ]; then
	echo "scripts/set_adapters.sh: would run:" >&2
	printf '  scripts/zcli --' >&2
	for arg in "$@"; do
		printf ' %s' "$arg" >&2
	done
	printf '\n' >&2
	if [ "$DO_RESTART" -eq 1 ]; then
		echo "  scripts/zcli -- edge-app-instance restart $NAME" >&2
	fi
	exit 0
fi

"$ZCLI" -- "$@" || {
	echo "scripts/set_adapters.sh: update failed" >&2
	exit 1
}

if [ "$DO_RESTART" -eq 1 ]; then
	echo "scripts/set_adapters.sh: restart $NAME" >&2
	"$ZCLI" -- edge-app-instance restart "$NAME" || {
		echo "scripts/set_adapters.sh: restart failed" >&2
		exit 1
	}
fi
