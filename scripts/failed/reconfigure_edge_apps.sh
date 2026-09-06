#!/bin/sh
# FAILED in-place / orchestrated reconfigure. Do not run against gmwtus.
# See scripts/failed/README.md. Create a new edge-app and instance instead
# (scripts/create_app.sh, scripts/create_instance.sh).

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
echo "scripts/failed/reconfigure_edge_apps.sh: do not run; create a new container instead" >&2
exit 1
APPLY=0
VERSION=
HVM=
SOURCE_APP=
NEW_APP=
NEW_INST=
EDGE_NODE=
NETWORK=
ADAPTERS="cavalry:cavalry gpio0:gpio iav:iav"
DIRECT_IFS="cavalry gpio0 iav"

usage() {
	echo "usage: scripts/failed/reconfigure_edge_apps.sh (do not run; see scripts/failed/README.md)" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/reconfigure_edge_apps.sh: set ZCLI_TOKEN (do not commit it)" >&2
	exit 1
fi

while [ "$#" -gt 0 ]; do
	case "$1" in
	-h|--help)
		usage
		exit 0
		;;
	--apply)
		APPLY=1
		shift
		;;
	--dry-run)
		APPLY=0
		shift
		;;
	--hvm=*)
		HVM=${1#--hvm=}
		shift
		;;
	--source-app=*)
		SOURCE_APP=${1#--source-app=}
		shift
		;;
	--new-app=*)
		NEW_APP=${1#--new-app=}
		shift
		;;
	--new-instance=*)
		NEW_INST=${1#--new-instance=}
		shift
		;;
	--edge-node=*)
		EDGE_NODE=${1#--edge-node=}
		shift
		;;
	--network=*)
		NETWORK=${1#--network=}
		shift
		;;
	--version=*)
		VERSION=${1#--version=}
		shift
		;;
	--hvm|--source-app|--new-app|--new-instance|--edge-node|--network|--version)
		opt=$1
		if [ "$#" -lt 2 ]; then
			echo "scripts/reconfigure_edge_apps.sh: $opt needs a value" >&2
			exit 1
		fi
		case "$opt" in
		--hvm) HVM=$2 ;;
		--source-app) SOURCE_APP=$2 ;;
		--new-app) NEW_APP=$2 ;;
		--new-instance) NEW_INST=$2 ;;
		--edge-node) EDGE_NODE=$2 ;;
		--network) NETWORK=$2 ;;
		--version) VERSION=$2 ;;
		esac
		shift 2
		;;
	--nohyper=*|--nohyper|--hvm-app=*|--hvm-app|--nohyper-app=*|--nohyper-app=*)
		echo "scripts/reconfigure_edge_apps.sh: in-place update flags are gone; gmwtus cannot add interfaces to ubuntu_24_04-container while instances exist. Use --source-app --new-app --new-instance --edge-node --network" >&2
		usage
		exit 1
		;;
	-*)
		echo "scripts/reconfigure_edge_apps.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		echo "scripts/reconfigure_edge_apps.sh: unexpected argument: $1" >&2
		usage
		exit 1
		;;
	esac
done

if [ -z "$HVM" ] || [ -z "$SOURCE_APP" ] || [ -z "$NEW_APP" ] || [ -z "$NEW_INST" ] || [ -z "$EDGE_NODE" ] || [ -z "$NETWORK" ]; then
	usage
	exit 1
fi

if [ -z "$VERSION" ]; then
	echo "scripts/reconfigure_edge_apps.sh: --version= is required" >&2
	exit 1
fi

if [ "$NEW_APP" = "$SOURCE_APP" ]; then
	echo "scripts/reconfigure_edge_apps.sh: --new-app must differ from --source-app" >&2
	exit 1
fi

echo "scripts/reconfigure_edge_apps.sh: HVM=$HVM NEW=$NEW_INST APP=$NEW_APP" >&2
if [ "$APPLY" -eq 0 ]; then
	echo "scripts/reconfigure_edge_apps.sh: dry-run (pass --apply to create on the controller)" >&2
fi

echo "===== existing =====" >&2
"$ROOT/scripts/show_instances.sh" "$HVM"
"$ROOT/scripts/show_instances.sh" --edge-app="$SOURCE_APP"
"$ROOT/scripts/show_app.sh" "$SOURCE_APP"
echo "scripts/reconfigure_edge_apps.sh: leave those instance records; start Halted ones when you want them running" >&2

echo "===== clone source bundle locally =====" >&2
"$ROOT/scripts/pull_app.sh" "$SOURCE_APP"
"$ROOT/scripts/clone_app.sh" "$SOURCE_APP" "$NEW_APP"
# shellcheck disable=SC2086
set --
for n in $DIRECT_IFS; do
	set -- "$@" --if="$n"
done
"$ROOT/scripts/add_app_direct.sh" "$NEW_APP" "$@"

if [ "$APPLY" -eq 0 ]; then
	echo "===== would apply =====" >&2
	"$ROOT/scripts/create_app.sh" "$NEW_APP" --version="$VERSION" --dry-run
	# shellcheck disable=SC2086
	set --
	for spec in $ADAPTERS; do
		set -- "$@" --adapter="$spec"
	done
	"$ROOT/scripts/create_instance.sh" "$NEW_INST" \
		--edge-app="$NEW_APP" \
		--edge-node="$EDGE_NODE" \
		--network-instance="$NETWORK" \
		"$@" --allow-visorc --dry-run
	echo "  (HVM --clear-adapters --restart only if VisORC is attached)" >&2
	echo "scripts/reconfigure_edge_apps.sh: nothing created on the controller" >&2
	exit 0
fi

echo "===== create edge-app =====" >&2
"$ROOT/scripts/create_app.sh" "$NEW_APP" --version="$VERSION"

echo "===== create instance =====" >&2
# shellcheck disable=SC2086
set --
for spec in $ADAPTERS; do
	set -- "$@" --adapter="$spec"
done
"$ROOT/scripts/create_instance.sh" "$NEW_INST" \
	--edge-app="$NEW_APP" \
	--edge-node="$EDGE_NODE" \
	--network-instance="$NETWORK" \
	"$@" --allow-visorc

echo "===== HVM: clear VisORC if present =====" >&2
hvm_detail=$("$ROOT/scripts/show_instances.sh" "$HVM")
if printf '%s\n' "$hvm_detail" | grep -q '	adapter	\(cavalry\|gpio\|iav\)'; then
	"$ROOT/scripts/set_adapters.sh" "$HVM" --clear-adapters --restart
else
	echo "scripts/reconfigure_edge_apps.sh: $HVM has no VisORC adapters" >&2
fi

echo "===== re-check =====" >&2
"$ROOT/scripts/show_app.sh" "$NEW_APP"
"$ROOT/scripts/show_instances.sh" "$HVM"
"$ROOT/scripts/show_instances.sh" "$NEW_INST"
echo "scripts/reconfigure_edge_apps.sh: done" >&2
