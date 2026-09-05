#!/bin/sh
# Restart, stop, start, or refresh an existing edge-app instance.
# ZCLI_TOKEN must already be in the environment. This script does not set it.
#
#   ./scripts/restart_instance.sh NAME
#   ./scripts/restart_instance.sh NAME --stop
#   ./scripts/restart_instance.sh NAME --start
#   ./scripts/restart_instance.sh NAME --refresh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"
ACTION=restart
DRY_RUN=0
NAME=

usage() {
	echo "usage: scripts/restart_instance.sh INSTANCE [--restart|--stop|--start|--refresh] [--dry-run]" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/restart_instance.sh: set ZCLI_TOKEN (do not commit it)" >&2
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
		ACTION=restart
		shift
		;;
	--stop)
		ACTION=stop
		shift
		;;
	--start)
		ACTION=start
		shift
		;;
	--refresh)
		ACTION=refresh
		shift
		;;
	-*)
		echo "scripts/restart_instance.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -n "$NAME" ]; then
			echo "scripts/restart_instance.sh: only one instance name" >&2
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

echo "scripts/restart_instance.sh: $ACTION $NAME" >&2
if [ "$DRY_RUN" -eq 1 ]; then
	echo "  scripts/zcli -- edge-app-instance $ACTION $NAME" >&2
	exit 0
fi

"$ZCLI" -- edge-app-instance "$ACTION" "$NAME" || {
	echo "scripts/restart_instance.sh: $ACTION failed" >&2
	exit 1
}
