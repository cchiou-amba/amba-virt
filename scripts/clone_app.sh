#!/bin/sh
# Copy apps/<SRC>.json to apps/<DST>.json and set ACE name to DST.
# Does not talk to the controller.
#
#   ./scripts/clone_app.sh ubuntu_24_04-container ubuntu_24_04-container-visorc

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
APPS="$ROOT/apps"

usage() {
	echo "usage: scripts/clone_app.sh SRC-APP DST-APP" >&2
}

SRC=
DST=
while [ "$#" -gt 0 ]; do
	case "$1" in
	-h|--help)
		usage
		exit 0
		;;
	-*)
		echo "scripts/clone_app.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -z "$SRC" ]; then
			SRC=$1
		elif [ -z "$DST" ]; then
			DST=$1
		else
			echo "scripts/clone_app.sh: extra argument: $1" >&2
			exit 1
		fi
		shift
		;;
	esac
done

if [ -z "$SRC" ] || [ -z "$DST" ]; then
	usage
	exit 1
fi

src="$APPS/$SRC.json"
if [ ! -f "$src" ]; then
	echo "scripts/clone_app.sh: missing $src (run pull_app.sh first)" >&2
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "scripts/clone_app.sh: need python3" >&2
	exit 1
fi

python3 "$ROOT/scripts/app_manifest.py" clone "$src" "$DST"
echo "scripts/clone_app.sh: wrote $APPS/$DST.json" >&2
