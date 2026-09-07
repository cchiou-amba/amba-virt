#!/bin/sh
# Power cycle (remote reboot) or prepare poweroff for an Ambarella edge node.
# Leverages scripts/zcli.
# ZCLI_TOKEN must already be in the environment.
#
#   ./scripts/power_cycle_node.sh n1-655-devkit
#   ./scripts/power_cycle_node.sh n1-655-pro
#   ./scripts/power_cycle_node.sh n1-655-devkit --wait
#   ./scripts/power_cycle_node.sh n1-655-devkit --status
#   ./scripts/power_cycle_node.sh n1-655-devkit --poweroff
#   ./scripts/power_cycle_node.sh n1-655-devkit --dry-run

set -eu

PROG=$(basename "$0")
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"

DRY_RUN=0
WAIT=0
ACTION="reboot"
TIMEOUT=300
POLL_INTERVAL=10
EDGE_NODE=

usage() {
	cat << EOF >&2
usage: scripts/$PROG [NODE] [OPTIONS]

Power cycle (remote reboot) or manage power state for an Ambarella edge node.

Arguments:
  NODE                   Edge node name (e.g. n1-655-devkit, n1-655-pro)

Options:
  --edge-node=NODE       Edge node name (alternative to positional argument)
  --reboot               (Default) Remotely reboot the edge node
  --poweroff             Prepare edge node for power off (stops applications)
  --prepare-poweroff     Alias for --poweroff
  --status               Show current edge node status and run-state
  --wait                 Wait for the edge node to reboot and return Online
  --timeout=SEC          Max seconds to wait when --wait is enabled (default: 300)
  --poll-interval=SEC    Polling interval in seconds for --wait (default: 10)
  --dry-run              Print the zcli command without executing
  -h, --help             Show this help message

Examples:
  ./scripts/$PROG n1-655-devkit
  ./scripts/$PROG n1-655-devkit --wait
  ./scripts/$PROG n1-655-pro --status
  ./scripts/$PROG n1-655-devkit --poweroff
  ./scripts/$PROG n1-655-devkit --dry-run
EOF
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/$PROG: set ZCLI_TOKEN (do not commit it)" >&2
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
	--wait)
		WAIT=1
		shift
		;;
	--reboot)
		ACTION="reboot"
		shift
		;;
	--poweroff|--prepare-poweroff)
		ACTION="prepare-poweroff"
		shift
		;;
	--status)
		ACTION="status"
		shift
		;;
	--timeout=*)
		TIMEOUT=${1#--timeout=}
		shift
		;;
	--timeout)
		if [ "$#" -lt 2 ]; then
			echo "scripts/$PROG: --timeout needs a value" >&2
			exit 1
		fi
		TIMEOUT=$2
		shift 2
		;;
	--poll-interval=*)
		POLL_INTERVAL=${1#--poll-interval=}
		shift
		;;
	--poll-interval)
		if [ "$#" -lt 2 ]; then
			echo "scripts/$PROG: --poll-interval needs a value" >&2
			exit 1
		fi
		POLL_INTERVAL=$2
		shift 2
		;;
	--edge-node=*)
		EDGE_NODE=${1#--edge-node=}
		shift
		;;
	--edge-node|-n)
		if [ "$#" -lt 2 ]; then
			echo "scripts/$PROG: $1 needs a node name" >&2
			exit 1
		fi
		EDGE_NODE=$2
		shift 2
		;;
	-*)
		echo "scripts/$PROG: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -z "$EDGE_NODE" ]; then
			EDGE_NODE=$1
		else
			echo "scripts/$PROG: unexpected argument: $1" >&2
			usage
			exit 1
		fi
		shift
		;;
	esac
done

if [ -z "$EDGE_NODE" ]; then
	echo "scripts/$PROG: edge node must be specified (e.g. n1-655-devkit or n1-655-pro)" >&2
	usage
	if [ "$DRY_RUN" -eq 0 ]; then
		echo "" >&2
		echo "Available edge nodes on controller:" >&2
		"$ZCLI" -- edge-node show || true
	fi
	exit 1
fi

if [ "$ACTION" = "status" ]; then
	echo "scripts/$PROG: querying status for edge node '$EDGE_NODE'..." >&2
	if [ "$DRY_RUN" -eq 1 ]; then
		echo "  scripts/zcli -- edge-node show $EDGE_NODE" >&2
		exit 0
	fi
	exec "$ZCLI" -- edge-node show "$EDGE_NODE"
fi

if [ "$ACTION" = "prepare-poweroff" ]; then
	echo "scripts/$PROG: preparing edge node '$EDGE_NODE' for power off..." >&2
	if [ "$DRY_RUN" -eq 1 ]; then
		echo "  scripts/zcli -- edge-node prepare-poweroff $EDGE_NODE -f" >&2
		exit 0
	fi
	"$ZCLI" -- edge-node prepare-poweroff "$EDGE_NODE" -f || {
		echo "scripts/$PROG: prepare-poweroff failed for $EDGE_NODE" >&2
		exit 1
	}
	echo "scripts/$PROG: prepare-poweroff request sent to $EDGE_NODE successfully" >&2
	exit 0
fi

# ACTION = reboot
echo "scripts/$PROG: issuing remote reboot for edge node '$EDGE_NODE'..." >&2

if [ "$DRY_RUN" -eq 1 ]; then
	echo "  scripts/zcli -- edge-node reboot $EDGE_NODE -f" >&2
	if [ "$WAIT" -eq 1 ]; then
		echo "  (would wait up to ${TIMEOUT}s polling every ${POLL_INTERVAL}s for node to return Online)" >&2
	fi
	exit 0
fi

"$ZCLI" -- edge-node reboot "$EDGE_NODE" -f || {
	echo "scripts/$PROG: failed to reboot $EDGE_NODE" >&2
	exit 1
}

echo "scripts/$PROG: reboot request successfully dispatched for $EDGE_NODE" >&2

if [ "$WAIT" -eq 0 ]; then
	echo "scripts/$PROG: to monitor reboot status, run:" >&2
	echo "  ./scripts/$PROG $EDGE_NODE --status" >&2
	echo "  ./scripts/$PROG $EDGE_NODE --wait" >&2
	exit 0
fi

echo "scripts/$PROG: waiting for '$EDGE_NODE' to reboot and return Online (timeout: ${TIMEOUT}s)..." >&2
start_time=$(date +%s)
transitioned=0

# Small pause before first poll to allow controller event dispatch
sleep 5

while true; do
	current_time=$(date +%s)
	elapsed=$((current_time - start_time))

	if [ "$elapsed" -ge "$TIMEOUT" ]; then
		echo "scripts/$PROG: timed out waiting for $EDGE_NODE to return Online (${elapsed}s elapsed)" >&2
		exit 1
	fi

	raw_show=$("$ZCLI" -- edge-node show "$EDGE_NODE" 2>/dev/null) || true
	run_state=$(printf '%s\n' "$raw_show" | grep -E '^Run State:' | head -n 1 | sed 's/^Run State:[[:space:]]*//' | tr -d '\r')

	if [ -n "$run_state" ]; then
		printf '[%02dm%02ds] Node: %s, Run State: %s\n' "$((elapsed / 60))" "$((elapsed % 60))" "$EDGE_NODE" "$run_state" >&2
		if [ "$run_state" != "Online" ]; then
			transitioned=1
		elif [ "$transitioned" -eq 1 ] || [ "$elapsed" -ge 20 ]; then
			echo "scripts/$PROG: $EDGE_NODE is Online and operational!" >&2
			exit 0
		fi
	else
		printf '[%02dm%02ds] Querying node status...\n' "$((elapsed / 60))" "$((elapsed % 60))" >&2
	fi

	sleep "$POLL_INTERVAL"
done
