#!/bin/sh
# FAILED in-place recipe. Do not run against gmwtus.
# edge-app update cannot add interfaces while appInstCount > 0
# (Halted still counts). See scripts/failed/README.md.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
echo "scripts/failed/push_app.sh: do not run; in-place bundle update cannot add interfaces" >&2
exit 1
ZCLI="$ROOT/scripts/zcli"
APPS="$ROOT/apps"
DRY_RUN=0
NAME=
VERSION=

usage() {
	echo "usage: scripts/failed/push_app.sh EDGE-APP --version=VER [--dry-run]" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/push_app.sh: set ZCLI_TOKEN (do not commit it)" >&2
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
	--version=*)
		VERSION=${1#--version=}
		shift
		;;
	--version)
		if [ "$#" -lt 2 ]; then
			echo "scripts/push_app.sh: --version needs a value" >&2
			exit 1
		fi
		VERSION=$2
		shift 2
		;;
	-*)
		echo "scripts/push_app.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -n "$NAME" ]; then
			echo "scripts/push_app.sh: only one edge-app name" >&2
			exit 1
		fi
		NAME=$1
		shift
		;;
	esac
done

if [ -z "$NAME" ] || [ -z "$VERSION" ]; then
	usage
	exit 1
fi

src="$APPS/$NAME.json"
if [ ! -f "$src" ]; then
	echo "scripts/push_app.sh: missing $src (run pull_app.sh / add_app_direct.sh first)" >&2
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "scripts/push_app.sh: need python3" >&2
	exit 1
fi
python3 "$ROOT/scripts/app_manifest.py" sanitize-file "$src" || {
	echo "scripts/push_app.sh: failed to sanitize $src" >&2
	exit 1
}

raw=$("$ZCLI" -- --format=json edge-app show "$NAME" --detail) || {
	echo "scripts/push_app.sh: failed to show $NAME" >&2
	exit 1
}
printf '%s\n' "$raw" | python3 "$ROOT/scripts/app_manifest.py" check-new-ifs "$src" || {
	echo "scripts/push_app.sh: refusing to push new interfaces" >&2
	exit 1
}

echo "scripts/push_app.sh: $NAME version $VERSION" >&2
if [ "$DRY_RUN" -eq 1 ]; then
	echo "  scripts/zcli -- edge-app update $NAME --manifest=/home/zcli/apps/$NAME.json --version=$VERSION" >&2
	exit 0
fi

"$ZCLI" -- edge-app update "$NAME" \
	--manifest="/home/zcli/apps/$NAME.json" \
	--version="$VERSION" || {
	echo "scripts/push_app.sh: update failed" >&2
	exit 1
}
