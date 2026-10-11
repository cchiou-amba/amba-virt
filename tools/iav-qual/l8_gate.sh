#!/bin/bash
#
# l8_gate.sh - the Level 8 adversarial run (plan_iav_virtualization, task 4.3a), one board.
#
#   l8_gate.sh <board> <attacker-guest-host> <victim-guest-host>
#
# The victim guest runs the qualification suite twice against the real server: once alone (the
# baseline) and once while the attacker guest runs the raw-vsock client (iav-attack). Observed from
# Dom0: the server worker's pid, thread count, open descriptors and memory before and after, the
# daemon's "is ready" count (a restart adds one), and the kernel log for oopses. Every line printed
# starts with a tag so the raw log can be read without the script.
#
# The checks are printed as "CHECK <name> PASS|FAIL <detail>" and the script exits 1 if any fails.
#
# Copyright (C) 2026, Ambarella International LLC.

set -u

B=$1
ATT=$2
VIC=$3
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
FAIL=0

check() {
	# check <name> <0|1 ok> <detail>
	if [ "$2" = 1 ]; then
		echo "CHECK $1 PASS $3"
	else
		echo "CHECK $1 FAIL $3"
		FAIL=1
	fi
}

snap() {
	# snap <label>: print the server worker facts as "SNAP <label> k=v ..."
	ssh "$B" '
	p=$(grep "is ready" /run/amba-virt/server-daemon.log | tail -1 | sed "s/.*worker \([0-9]*\) is ready/\1/")
	t=$(grep "^Threads" /proc/$p/status | tr -dc 0-9)
	r=$(grep "^VmRSS" /proc/$p/status | tr -dc 0-9)
	f=$(ls /proc/$p/fd | wc -l)
	n=$(grep -c "is ready" /run/amba-virt/server-daemon.log)
	o=$(dmesg | grep -c "Oops\|Unable to handle\|BUG:\|Call trace")
	u=$(cut -d. -f1 /proc/uptime)
	echo "pid=$p threads=$t rss_kb=$r fds=$f ready_lines=$n kernel_faults=$o uptime=$u"' |
		sed "s/^/SNAP $1 /"
}

val() {
	# val <snap line> <key>
	echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"
}

stream_ok() {
	# stream_ok <url>: the stream carries rising sequence numbers
	local a
	a=$(curl -s -m 8 "$1" | grep -ao '"seq":[0-9]*' | sed -n '1p;$p' | tr -d '"seq:' | tr '\n' ' ')
	echo "STREAM $1 $a"
	echo "$a" | awk '{exit !($2>$1)}'
}

ip_of() {
	ssh -G "$1" | sed -n 's/^hostname //p'
}

# 1. Stage the binaries: the suite built into the demo, and the attack client.
scp -q "$ROOT/guest-os/apps/amba-virt-demo/amba_virt_demo-musl" "$VIC:/tmp/iavq" || exit 9
scp -q "$ROOT/guest-os/client/iav-attack" "$ATT:/tmp/iav-attack" || exit 9
ssh "$VIC" 'chmod +x /tmp/iavq; rm -f /tmp/l8_base.jsonl /tmp/l8_att.jsonl'
ssh "$ATT" 'chmod +x /tmp/iav-attack; rm -f /tmp/l8_attack.out'

S0=$(snap before)
echo "$S0"
PID0=$(val "$S0" pid)
THR0=$(val "$S0" threads)
FD0=$(val "$S0" fds)
RSS0=$(val "$S0" rss_kb)
RDY0=$(val "$S0" ready_lines)
KF0=$(val "$S0" kernel_faults)
UP0=$(val "$S0" uptime)

# 2. Victim baseline, alone.
echo "RUN victim baseline"
ssh "$VIC" "cd /tmp; sudo /tmp/iavq --iav-qual --backend real --mode tests --results /tmp/l8_base.jsonl --board $B" \
	> /tmp/l8_base.out 2>&1
echo "victim baseline exit=$?"
tail -n 3 /tmp/l8_base.out
ssh "$VIC" 'sudo cat /tmp/l8_base.jsonl' > /tmp/l8_base.jsonl

# 3. Attack, with the victim running the same suite at the same time.
echo "RUN attacker and victim"
ssh "$ATT" 'cd /tmp; sudo timeout 900 /tmp/iav-attack > /tmp/l8_attack.out 2>&1; echo $? > /tmp/l8_attack.rc' &
ATTPID=$!
sleep 3
ssh "$VIC" "cd /tmp; sudo /tmp/iavq --iav-qual --backend real --mode tests --results /tmp/l8_att.jsonl --board $B" \
	> /tmp/l8_att.out 2>&1
echo "victim under attack exit=$?"
tail -n 3 /tmp/l8_att.out
wait $ATTPID
echo "ATTACKER OUTPUT (tail)"
ssh "$ATT" 'tail -n 25 /tmp/l8_attack.out; echo "attacker exit=$(cat /tmp/l8_attack.rc)"'
ARC=$(ssh "$ATT" 'cat /tmp/l8_attack.rc')
ssh "$VIC" 'sudo cat /tmp/l8_att.jsonl' > /tmp/l8_att.jsonl

# Negative control: with L8_CONTROL=restart the worker is restarted here, as a crash would, so
# the checks on the same worker and on the "is ready" count must fail.
if [ "${L8_CONTROL:-}" = restart ]; then
	echo "CONTROL restarting the server worker"
	ssh "$B" 'p=$(cat /run/amba-virt/server.pid); kill -USR1 $p; sleep 8; kill -USR2 $p'
	for i in $(seq 1 40); do
		ssh "$B" 'tail -1 /run/amba-virt/server-daemon.log' | grep -q "is ready" && break
		sleep 5
	done
fi

# 4. The server after the attack, once the guest's resources have been released.
sleep 20
S1=$(snap after)
echo "$S1"

check attacker_exit_0 "$([ "$ARC" = 0 ] && echo 1 || echo 0)" "exit=$ARC"
check server_same_worker "$([ "$(val "$S1" pid)" = "$PID0" ] && echo 1 || echo 0)" "before=$PID0 after=$(val "$S1" pid)"
check server_not_restarted "$([ "$(val "$S1" ready_lines)" = "$RDY0" ] && echo 1 || echo 0)" "ready_lines before=$RDY0 after=$(val "$S1" ready_lines)"
check node_not_rebooted "$([ "$(val "$S1" uptime)" -gt "$UP0" ] && echo 1 || echo 0)" "uptime before=$UP0 after=$(val "$S1" uptime)"
# The count includes lines present since boot (the hw_timer warnings), so the check is that the
# attack added none.
check kernel_faults_unchanged "$([ "$(val "$S1" kernel_faults)" = "$KF0" ] && echo 1 || echo 0)" "before=$KF0 after=$(val "$S1" kernel_faults)"
check threads_baseline "$([ "$(val "$S1" threads)" = "$THR0" ] && echo 1 || echo 0)" "before=$THR0 after=$(val "$S1" threads)"
check fds_baseline "$([ "$(val "$S1" fds)" = "$FD0" ] && echo 1 || echo 0)" "before=$FD0 after=$(val "$S1" fds)"
# Memory: within 20 percent of the baseline (the figure is this gate's own, fixed at the seal).
RSS1=$(val "$S1" rss_kb)
check rss_within_20pct "$([ $((RSS1 * 100)) -le $((RSS0 * 120)) ] && echo 1 || echo 0)" "before_kb=$RSS0 after_kb=$RSS1"

# 5. The victim's results under attack equal its baseline, test by test.
python3 -I - /tmp/l8_base.jsonl /tmp/l8_att.jsonl <<'EOF'
import json, sys
def load(p):
    d = {}
    for line in open(p):
        line = line.strip()
        if line:
            r = json.loads(line)
            d[r["group"] + "." + r["test"]] = r["status"]
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
diff = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
print("VICTIM tests baseline=%d attacked=%d differences=%d" % (len(a), len(b), len(diff)))
for k in diff:
    print("VICTIM DIFF %s baseline=%s attacked=%s" % (k, a.get(k), b.get(k)))
sys.exit(1 if (diff or not a) else 0)
EOF
check victim_results_identical "$([ $? = 0 ] && echo 1 || echo 0)" "see VICTIM lines"

# 6. Both demos on the board still stream frames.
# The guests' streams are forwarded on the board's address: Ubuntu on 8080, Alpine on 8081.
IPB=$(ip_of "$B")
OKS=1
stream_ok "http://$IPB:8080/live/stream" || OKS=0
stream_ok "http://$IPB:8081/live/stream" || OKS=0
check both_demo_streams_advance "$OKS" "see STREAM lines"

exit $FAIL
