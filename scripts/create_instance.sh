#!/bin/sh
# Create an edge-app instance with networks and adapters.
# Adapter intfnames must already exist on the edge-app template.
# ZCLI_TOKEN must already be in the environment.
#
#   ./scripts/create_instance.sh ubuntu_24_04_container_visorc.n1-655-devkit \
#     --edge-app=ubuntu_24_04-container-visorc \
#     --edge-node=n1-655-devkit \
#     --network-instance=eth0:defaultLocal-n1-655-devkit \
#     --adapter=cavalry:cavalry --adapter=gpio0:gpio --adapter=iav:iav \
#     --allow-visorc --dry-run

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"
DRY_RUN=0
ALLOW_VISORC=0
NAME=
EDGE_APP=
EDGE_NODE=
NETWORKS=
ADAPTERS=

usage() {
	echo "usage: scripts/create_instance.sh INSTANCE --edge-app=APP --edge-node=NODE --network-instance=INTF:NI... [--adapter=INTF:GRP...] [--allow-visorc] [--dry-run]" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/create_instance.sh: set ZCLI_TOKEN (do not commit it)" >&2
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
	--allow-visorc)
		ALLOW_VISORC=1
		shift
		;;
	--edge-app=*)
		EDGE_APP=${1#--edge-app=}
		shift
		;;
	--edge-node=*)
		EDGE_NODE=${1#--edge-node=}
		shift
		;;
	--network-instance=*)
		NETWORKS="$NETWORKS ${1#--network-instance=}"
		shift
		;;
	--adapter=*)
		ADAPTERS="$ADAPTERS ${1#--adapter=}"
		shift
		;;
	--edge-app|--edge-node|--network-instance|--adapter)
		opt=$1
		if [ "$#" -lt 2 ]; then
			echo "scripts/create_instance.sh: $opt needs a value" >&2
			exit 1
		fi
		case "$opt" in
		--edge-app) EDGE_APP=$2 ;;
		--edge-node) EDGE_NODE=$2 ;;
		--network-instance) NETWORKS="$NETWORKS $2" ;;
		--adapter) ADAPTERS="$ADAPTERS $2" ;;
		esac
		shift 2
		;;
	-*)
		echo "scripts/create_instance.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -n "$NAME" ]; then
			echo "scripts/create_instance.sh: only one instance name" >&2
			exit 1
		fi
		NAME=$1
		shift
		;;
	esac
done

if [ -z "$NAME" ] || [ -z "$EDGE_APP" ] || [ -z "$EDGE_NODE" ] || [ -z "$NETWORKS" ]; then
	usage
	exit 1
fi

# shellcheck disable=SC2086
set -- $ADAPTERS
for spec in "$@"; do
	case "$spec" in
	*:*)
		;;
	*)
		echo "scripts/create_instance.sh: adapter must be intfname:assigngrp ($spec)" >&2
		exit 1
		;;
	esac
	grp=${spec#*:}
	case "$grp" in
	cavalry|gpio|iav)
		if [ "$ALLOW_VISORC" -eq 0 ]; then
			echo "scripts/create_instance.sh: $grp is VisORC; pass --allow-visorc for NOHYPER" >&2
			exit 1
		fi
		;;
	esac
done

set -- edge-app-instance create "$NAME" \
	--edge-app="$EDGE_APP" \
	--edge-node="$EDGE_NODE"
# shellcheck disable=SC2086
for net in $NETWORKS; do
	set -- "$@" --network-instance="$net"
done
# shellcheck disable=SC2086
for spec in $ADAPTERS; do
	set -- "$@" --adapter="$spec"
done

echo "scripts/create_instance.sh: $NAME" >&2
if [ "$DRY_RUN" -eq 1 ]; then
	echo "scripts/create_instance.sh: would run:" >&2
	printf '  scripts/zcli --' >&2
	for arg in "$@"; do
		printf ' %s' "$arg" >&2
	done
	printf '\n' >&2
	exit 0
fi

"$ZCLI" -- "$@" || {
	echo "scripts/create_instance.sh: create failed" >&2
	exit 1
}
