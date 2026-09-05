#!/bin/sh
# Show edge-app manifest interface names (left side of --adapter=).
# ZCLI_TOKEN must already be in the environment. This script does not set it.
#
#   ./scripts/show_app.sh ubuntu_24_04_container

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"
RAW_JSON=0
NAME=

usage() {
	echo "usage: scripts/show_app.sh [--format=json] EDGE-APP" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/show_app.sh: set ZCLI_TOKEN (do not commit it)" >&2
	exit 1
fi

while [ "$#" -gt 0 ]; do
	case "$1" in
	-h|--help)
		usage
		exit 0
		;;
	--format=json)
		RAW_JSON=1
		shift
		;;
	-*)
		echo "scripts/show_app.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -n "$NAME" ]; then
			echo "scripts/show_app.sh: only one edge-app name" >&2
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

have_jq() { command -v jq >/dev/null 2>&1; }
have_py() { command -v python3 >/dev/null 2>&1; }

if ! have_jq && ! have_py; then
	echo "scripts/show_app.sh: need jq or python3" >&2
	exit 1
fi

raw=$("$ZCLI" -- --format=json edge-app show "$NAME" --detail) || {
	echo "scripts/show_app.sh: failed to show $NAME" >&2
	exit 1
}

if [ "$RAW_JSON" -eq 1 ]; then
	printf '%s\n' "$raw"
	exit 0
fi

if have_jq; then
	printf '%s\n' "$raw" | jq -r '
		(if has("config") then .config else . end) as $c
		| ($c.manifestJSON // $c.manifest // {}) as $m
		| "name\t\($c.name // "")",
		  "title\t\($c.title // "")",
		  "",
		  "intfname\ttype",
		  (
			($m.interfaces // [])[]
			| "\(.name // "")\t\(.type // .ztype // "")"
		  )
	'
else
	printf '%s\n' "$raw" | python3 -c '
import json, sys
data = json.load(sys.stdin)
cfg = data["config"] if isinstance(data, dict) and "config" in data else data
man = cfg.get("manifestJSON") or cfg.get("manifest") or {}
if isinstance(man, str):
    man = json.loads(man)
print("name\t{}".format(cfg.get("name") or ""))
print("title\t{}".format(cfg.get("title") or ""))
print()
print("intfname\ttype")
for i in man.get("interfaces") or []:
    print("{}\t{}".format(i.get("name") or "", i.get("type") or i.get("ztype") or ""))
'
fi
