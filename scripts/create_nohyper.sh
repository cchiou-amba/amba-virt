#!/bin/sh
# Create a NOHYPER edge-app instance on an Ambarella node with all
# assignable Ambarella model adapters (cavalry, gpio, iav, USB).
# Leverages scripts/zcli.
# ZCLI_TOKEN must already be in the environment.
#
#   ./scripts/create_nohyper.sh n1-655-devkit
#   ./scripts/create_nohyper.sh n1-655-pro
#   ./scripts/create_nohyper.sh --edge-node=n1-655-devkit --dry-run
#   ./scripts/create_nohyper.sh n1-655-devkit --name=my_container

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"

DRY_RUN=0
EDGE_NODE=
EDGE_APP="ubuntu_24_04-container"
INSTANCE_NAME=
NETWORK_INSTANCE=
ADAPTERS="cavalry:cavalry gpio0:gpio0 iav:iav USB:USB"

usage() {
	cat << 'EOF' >&2
usage: scripts/create_nohyper.sh [NODE] [OPTIONS]

Create a NOHYPER edge application instance on an Ambarella edge node with
all Ambarella model adapters attached (cavalry, gpio, iav, USB).

Arguments:
  NODE                   Edge node name (e.g. n1-655-devkit, n1-655-pro)

Options:
  --edge-node=NODE       Edge node name (alternative to positional argument)
  --name=NAME            Instance name (default: ubuntu_24_04_container.<node>)
  --edge-app=APP         Edge-app bundle name (default: ubuntu_24_04-container)
  --network=NET          Network instance (default: eth0:defaultLocal-<node>)
  --adapter=INTF:GRP     Override or specify custom adapters (may be repeated)
  --dry-run              Print the zcli command without executing
  -h, --help             Show this help message
EOF
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/create_nohyper.sh: set ZCLI_TOKEN (do not commit it)" >&2
	exit 1
fi

CUSTOM_ADAPTERS=""

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
	--edge-node=*)
		EDGE_NODE=${1#--edge-node=}
		shift
		;;
	--edge-node)
		if [ "$#" -lt 2 ]; then
			echo "scripts/create_nohyper.sh: --edge-node needs a value" >&2
			exit 1
		fi
		EDGE_NODE=$2
		shift 2
		;;
	--name=*)
		INSTANCE_NAME=${1#--name=}
		shift
		;;
	--name)
		if [ "$#" -lt 2 ]; then
			echo "scripts/create_nohyper.sh: --name needs a value" >&2
			exit 1
		fi
		INSTANCE_NAME=$2
		shift 2
		;;
	--edge-app=*)
		EDGE_APP=${1#--edge-app=}
		shift
		;;
	--edge-app)
		if [ "$#" -lt 2 ]; then
			echo "scripts/create_nohyper.sh: --edge-app needs a value" >&2
			exit 1
		fi
		EDGE_APP=$2
		shift 2
		;;
	--network=*|--network-instance=*)
		val=${1#*=}
		NETWORK_INSTANCE=$val
		shift
		;;
	--network|--network-instance)
		if [ "$#" -lt 2 ]; then
			echo "scripts/create_nohyper.sh: $1 needs a value" >&2
			exit 1
		fi
		NETWORK_INSTANCE=$2
		shift 2
		;;
	--adapter=*)
		CUSTOM_ADAPTERS="$CUSTOM_ADAPTERS ${1#--adapter=}"
		shift
		;;
	--adapter)
		if [ "$#" -lt 2 ]; then
			echo "scripts/create_nohyper.sh: --adapter needs a value" >&2
			exit 1
		fi
		CUSTOM_ADAPTERS="$CUSTOM_ADAPTERS $2"
		shift 2
		;;
	-*)
		echo "scripts/create_nohyper.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -z "$EDGE_NODE" ]; then
			EDGE_NODE=$1
		elif [ -z "$INSTANCE_NAME" ]; then
			INSTANCE_NAME=$1
		else
			echo "scripts/create_nohyper.sh: unexpected argument: $1" >&2
			usage
			exit 1
		fi
		shift
		;;
	esac
done

if [ -z "$EDGE_NODE" ]; then
	echo "scripts/create_nohyper.sh: edge node must be specified (e.g. n1-655-devkit or n1-655-pro)" >&2
	usage
	exit 1
fi

if [ -n "$CUSTOM_ADAPTERS" ]; then
	ADAPTERS=$CUSTOM_ADAPTERS
fi

if [ -z "$INSTANCE_NAME" ]; then
	app_prefix=$(printf '%s\n' "$EDGE_APP" | tr '-' '_')
	INSTANCE_NAME="${app_prefix}.${EDGE_NODE}"
fi

if [ -z "$NETWORK_INSTANCE" ]; then
	NETWORK_INSTANCE="eth0:defaultLocal-${EDGE_NODE}"
elif [ "${NETWORK_INSTANCE#*:}" = "$NETWORK_INSTANCE" ]; then
	NETWORK_INSTANCE="eth0:${NETWORK_INSTANCE}"
fi

# Build arguments for zcli edge-app-instance create
set -- edge-app-instance create "$INSTANCE_NAME" \
	--edge-app="$EDGE_APP" \
	--edge-node="$EDGE_NODE" \
	--network-instance="$NETWORK_INSTANCE"

for adp in $ADAPTERS; do
	set -- "$@" --adapter="$adp"
done

echo "scripts/create_nohyper.sh: creating instance '$INSTANCE_NAME' on node '$EDGE_NODE'" >&2
echo "  Edge-app: $EDGE_APP" >&2
echo "  Network:  $NETWORK_INSTANCE" >&2
echo "  Adapters: $ADAPTERS" >&2

if [ "$DRY_RUN" -eq 1 ]; then
	echo "scripts/create_nohyper.sh: would run:" >&2
	printf '  scripts/zcli --' >&2
	for arg in "$@"; do
		printf ' %s' "$arg" >&2
	done
	printf '\n' >&2
	exit 0
fi

"$ZCLI" -- "$@" || {
	echo "scripts/create_nohyper.sh: failed to create $INSTANCE_NAME" >&2
	exit 1
}

echo "scripts/create_nohyper.sh: successfully created $INSTANCE_NAME" >&2
