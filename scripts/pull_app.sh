#!/bin/sh
# Pull an edge-app manifest JSON from ZEDEDA Cloud into apps/.
# ZCLI_TOKEN must already be in the environment. This script does not set it.
#
#   ./scripts/pull_app.sh ubuntu_24_04-container
#   ./scripts/pull_app.sh ubuntu_24_04 --dry-run

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"
APPS="$ROOT/apps"
DRY_RUN=0
NAME=

usage() {
	echo "usage: scripts/pull_app.sh [--dry-run] EDGE-APP" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/pull_app.sh: set ZCLI_TOKEN (do not commit it)" >&2
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
	-*)
		echo "scripts/pull_app.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -n "$NAME" ]; then
			echo "scripts/pull_app.sh: only one edge-app name" >&2
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

case "$NAME" in
""|*/*|*..*|*" "*|*"	"*)
	echo "scripts/pull_app.sh: unsafe edge-app name: $NAME" >&2
	exit 1
	;;
esac

have_jq() { command -v jq >/dev/null 2>&1; }
have_py() { command -v python3 >/dev/null 2>&1; }

if ! have_py; then
	echo "scripts/pull_app.sh: need python3 (strips show --detail UI fields)" >&2
	exit 1
fi

raw=$("$ZCLI" -- --format=json edge-app show "$NAME" --detail) || {
	echo "scripts/pull_app.sh: failed to show $NAME" >&2
	exit 1
}

if have_jq; then
	man=$(printf '%s\n' "$raw" | jq '
		(if has("config") then .config else . end) as $c
		| ($c.manifestJSON // $c.manifest)
		| if . == null then error("missing manifestJSON")
		  elif type == "string" then fromjson
		  else .
		  end
	') || {
		echo "scripts/pull_app.sh: failed to extract manifest" >&2
		exit 1
	}
else
	man=$(printf '%s\n' "$raw" | python3 -c '
import json, sys
data = json.load(sys.stdin)
cfg = data["config"] if isinstance(data, dict) and "config" in data else data
man = cfg.get("manifestJSON")
if man is None:
    man = cfg.get("manifest")
if man is None:
    sys.stderr.write("scripts/pull_app.sh: missing manifestJSON\n")
    sys.exit(1)
if isinstance(man, str):
    man = json.loads(man)
json.dump(man, sys.stdout, indent=2)
sys.stdout.write("\n")
') || {
		echo "scripts/pull_app.sh: failed to extract manifest" >&2
		exit 1
	}
fi

# show --detail JSON is not round-trippable; drop __* / null / imagestatus.
man=$(printf '%s\n' "$man" | python3 "$ROOT/scripts/app_manifest.py" sanitize) || {
	echo "scripts/pull_app.sh: failed to sanitize manifest" >&2
	exit 1
}

dest="$APPS/$NAME.json"
if [ "$DRY_RUN" -eq 1 ]; then
	echo "scripts/pull_app.sh: would write $dest" >&2
	printf '%s\n' "$man"
	exit 0
fi

mkdir -p "$APPS"
tmp=$(mktemp "$APPS/$NAME.json.tmp.XXXXXX")
printf '%s\n' "$man" >"$tmp"
mv "$tmp" "$dest"
echo "scripts/pull_app.sh: wrote $dest" >&2
