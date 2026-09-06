#!/bin/sh
# List edge-app instances or show one instance's adapters and networks.
# ZCLI_TOKEN must already be in the environment. This script does not set it.
#
#   ./scripts/show_instances.sh
#   ./scripts/show_instances.sh --edge-node=NAME
#   ./scripts/show_instances.sh --edge-app=NAME
#   ./scripts/show_instances.sh ubuntu_24_04_container.n1-655-pro

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"
EDGE_NODE=
EDGE_APP=
RAW_JSON=0
NAME=

usage() {
	echo "usage: scripts/show_instances.sh [--edge-node=NAME] [--edge-app=NAME] [--format=json] [INSTANCE]" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/show_instances.sh: set ZCLI_TOKEN (do not commit it)" >&2
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
	--edge-node=*)
		EDGE_NODE=${1#--edge-node=}
		shift
		;;
	--edge-node)
		if [ "$#" -lt 2 ]; then
			echo "scripts/show_instances.sh: --edge-node needs a name" >&2
			exit 1
		fi
		EDGE_NODE=$2
		shift 2
		;;
	--edge-app=*)
		EDGE_APP=${1#--edge-app=}
		shift
		;;
	--edge-app)
		if [ "$#" -lt 2 ]; then
			echo "scripts/show_instances.sh: --edge-app needs a name" >&2
			exit 1
		fi
		EDGE_APP=$2
		shift 2
		;;
	-*)
		echo "scripts/show_instances.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -n "$NAME" ]; then
			echo "scripts/show_instances.sh: only one instance name" >&2
			exit 1
		fi
		NAME=$1
		shift
		;;
	esac
done

have_jq() { command -v jq >/dev/null 2>&1; }
have_py() { command -v python3 >/dev/null 2>&1; }

if ! have_jq && ! have_py; then
	echo "scripts/show_instances.sh: need jq or python3" >&2
	exit 1
fi

if [ -n "$NAME" ]; then
	raw=$("$ZCLI" -- --format=json edge-app-instance show "$NAME" --detail) || {
		echo "scripts/show_instances.sh: failed to show $NAME" >&2
		exit 1
	}
else
	set -- edge-app-instance show
	if [ -n "$EDGE_NODE" ]; then
		set -- "$@" --edge-node="$EDGE_NODE"
	fi
	if [ -n "$EDGE_APP" ]; then
		set -- "$@" --edge-app="$EDGE_APP"
	fi
	raw=$("$ZCLI" -- --format=json "$@") || {
		echo "scripts/show_instances.sh: failed to list instances" >&2
		exit 1
	}
fi

if [ "$RAW_JSON" -eq 1 ]; then
	printf '%s\n' "$raw"
	exit 0
fi

if [ -n "$NAME" ]; then
	if have_jq; then
		printf '%s\n' "$raw" | jq -r '
			.config as $c
			| "name\t\($c.name // "")",
			  "edge-app\t\($c.appName // "")",
			  "edge-node\t\($c.deviceName // "")",
			  "admin\t\($c.__adminState // $c.activate // "")",
			  "",
			  "intfname\tkind\tattached",
			  (
				($c.interfaces // [])[]
				| . as $i
				| if ($i.netinstname // "") != "" then
					"\($i.intfname)\tnetwork\t\($i.netinstname)"
				  elif (($i.io.name // "") != "") then
					"\($i.intfname)\tadapter\t\($i.io.name)"
				  else
					"\($i.intfname)\tother\t"
				  end
			  )
		'
	else
		printf '%s\n' "$raw" | python3 -c '
import json, sys
data = json.load(sys.stdin)
cfg = data.get("config") or data
print("name\t{}".format(cfg.get("name") or ""))
print("edge-app\t{}".format(cfg.get("appName") or ""))
print("edge-node\t{}".format(cfg.get("deviceName") or ""))
print("admin\t{}".format(cfg.get("__adminState") or cfg.get("activate") or ""))
print()
print("intfname\tkind\tattached")
for i in cfg.get("interfaces") or []:
    intf = i.get("intfname") or ""
    net = i.get("netinstname") or ""
    adp = (i.get("io") or {}).get("name") or ""
    if net:
        print("{}\tnetwork\t{}".format(intf, net))
    elif adp:
        print("{}\tadapter\t{}".format(intf, adp))
    else:
        print("{}\tother\t".format(intf))
'
	fi
	exit 0
fi

if have_jq; then
	printf '%s\n' "$raw" | jq -r '
		(["name","edge-app","edge-node","run-state"] | @tsv),
		(
			(if type == "array" then .
			 elif has("list") then (.list // [])
			 else [.] end)
			[]
			| [
				.name // "",
				.appName // "",
				.deviceName // "",
				.__runState // .runState // ""
			  ]
			| @tsv
		)
	'
else
	printf '%s\n' "$raw" | python3 -c '
import json, sys
data = json.load(sys.stdin)
if isinstance(data, list):
    rows = data
elif isinstance(data, dict) and "list" in data:
    rows = data.get("list") or []
else:
    rows = [data]
print("name\tedge-app\tedge-node\trun-state")
for r in rows:
    print("{}\t{}\t{}\t{}".format(
        r.get("name") or "",
        r.get("appName") or "",
        r.get("deviceName") or "",
        r.get("__runState") or r.get("runState") or "",
    ))
'
fi
