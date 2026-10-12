#!/bin/bash
#
# dev_server.sh - run the candidate server (the qualification build) on a board for guest runs.
#
#   dev_server.sh <board> start [<primary-slice> [<camera.conf on the board>]]
#                                                   stage the binary in the development tree and start it; the
#                                                   optional camera.conf (a path on the board, for example in
#                                                   /tmp) selects another pipeline resource script
#   dev_server.sh <board> stop                      stop it; the sentinel resumes the production worker
#
# The candidate holds the production worker, serves the guests without the Dom0 frame tap (the driver allows one
# reader of frame descriptors) and starts stream 1 itself. The binary lives only in /persist/amba-virt-dev, the
# development tree of doc/AmbaVirtServer.md section 14.3, which `stop` leaves in place and `clean` removes.
# Nothing else is written to /persist.
#
# Copyright (C) 2026, Ambarella International LLC.

set -u
B=$1
CMD=$2
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
BIN=$ROOT/build/iav-qual/aarch64/amba-virt-server-qual
DIR=/persist/amba-virt-dev

wait_ready() {
	# the production worker is up and settled again
	local i
	for i in $(seq 1 60); do
		if ssh "$B" 'tail -n 1 /run/amba-virt/server-daemon.log' 2>/dev/null | grep -q 'is ready'; then
			return 0
		fi
		sleep 3
	done
	return 1
}

case $CMD in
start)
	SLICE=${3:-0}
	CONF=${4:-}
	CARG=""
	[ -n "$CONF" ] && CARG="-C $CONF"
	[ -x "$BIN" ] || { echo "missing $BIN; run make -C drivers/amba_virt/tools amba-virt-server-qual" >&2; exit 2; }
	ssh "$B" "mkdir -p $DIR" || exit 1
	scp -q "$BIN" "$B:$DIR/amba-virt-server-qual" || exit 1
	ssh "$B" "chmod +x $DIR/amba-virt-server-qual; sha256sum $DIR/amba-virt-server-qual" | cut -c1-64
	ssh "$B" "nsenter -t 1 -m -- sh -c 'nohup $DIR/amba-virt-server-qual --iav-serve -P $SLICE $CARG > /run/amba-virt/dev-serve.log 2>&1 &'"
	for i in $(seq 1 60); do
		if ssh "$B" 'grep -q "listening for TCP client bridges" /run/amba-virt/dev-serve.log' 2>/dev/null; then
			echo "candidate server is serving (primary slice $SLICE)"
			ssh "$B" 'grep -E "iav_broker" /run/amba-virt/dev-serve.log | head -n 2'
			exit 0
		fi
		sleep 3
	done
	echo "the candidate server did not start; see /run/amba-virt/dev-serve.log" >&2
	exit 1
	;;
stop)
	ssh "$B" 'p=$(cat /run/amba-virt/serve.pid 2>/dev/null); [ -n "$p" ] && kill -TERM $p; for i in $(seq 1 40); do kill -0 $p 2>/dev/null || exit 0; sleep 1; done; exit 1'
	wait_ready && echo "production worker is ready again" || { echo "production worker did not come back" >&2; exit 1; }
	;;
clean)
	ssh "$B" "rm -rf $DIR; ls /persist | grep -c amba-virt-dev"
	;;
*)
	echo "usage: $0 <board> start [slice] | stop | clean" >&2
	exit 2
	;;
esac
