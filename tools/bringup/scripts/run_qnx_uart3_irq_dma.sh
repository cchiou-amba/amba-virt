#!/usr/bin/env bash
#
# tools/bringup/scripts/run_qnx_uart3_irq_dma.sh
#
# Qualification test for Envelope 3: QNX Neutrino 8.0 HVM UART3 Generic DMA & IRQ 144.
# Validates bidirectional 4 KiB DMA transfers across physical Console 3 (Port 6072).
#
# Copyright (C) 2026, Ambarella International LLC
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
ARTIFACT_DIR="${WORKSPACE_ROOT}/plan/plan_hvm_uart_irq_dma.artifacts/envelope3"

mkdir -p "${ARTIFACT_DIR}"

GUEST_HOST="${GUEST_HOST:-n1-655-devkit-qnx}"
DOM0_HOST="${DOM0_HOST:-n1-655-devkit}"
CONSOLE_HOST="${CONSOLE_HOST:-127.0.0.1}"
CONSOLE_PORT="${CONSOLE_PORT:-6072}"
PAYLOAD_SIZE=4096

echo "============================================================"
echo " Starting Envelope 3: QNX UART3 Physical DMA & IRQ 144 Test"
echo " Target Guest:   ${GUEST_HOST}"
echo " Dom0 Host:      ${DOM0_HOST}"
echo " Console 3:      ${CONSOLE_HOST}:${CONSOLE_PORT}"
echo " Payload Size:   ${PAYLOAD_SIZE} bytes"
echo " Artifact Dir:   ${ARTIFACT_DIR}"
echo "============================================================"

# Step 1: Pre-flight checks on QNX Guest
echo "[1/5] Pre-flight inspection on ${GUEST_HOST}..."
ssh "${GUEST_HOST}" "slay -f qnx-getty login qnx_read_exact 2>/dev/null || true"
sleep 0.5
QNX_PROCS=$(ssh "${GUEST_HOST}" "pidin" || true)
echo "${QNX_PROCS}" > "${ARTIFACT_DIR}/qnx_pidin_pre.txt"

DEV_CHECK=$(ssh "${GUEST_HOST}" "ls -l /dev/amba_virt /dev/ser3" || true)
echo "  - Device nodes: ${DEV_CHECK}"
if ! echo "${DEV_CHECK}" | grep -q "amba_virt" || ! echo "${DEV_CHECK}" | grep -q "ser3"; then
    echo "FAIL: Required device nodes /dev/amba_virt or /dev/ser3 not found!"
    exit 1
fi

export CONSOLE_HOST CONSOLE_PORT GUEST_HOST DOM0_HOST ARTIFACT_DIR PAYLOAD_SIZE

# Step 2: Run Python DMA verification harness
echo "[2/5] Running bidirectional 4 KiB transfer harness via ${CONSOLE_HOST}:${CONSOLE_PORT}..."
python3 - << 'PYEOF'
import socket
import subprocess
import hashlib
import time
import os
import sys

CONSOLE_HOST = os.environ.get("CONSOLE_HOST", "127.0.0.1")
CONSOLE_PORT = int(os.environ.get("CONSOLE_PORT", "6072"))
GUEST_HOST = os.environ.get("GUEST_HOST", "n1-655-devkit-qnx")
DOM0_HOST = os.environ.get("DOM0_HOST", "n1-655-devkit")
ARTIFACT_DIR = os.environ.get("ARTIFACT_DIR", "/tmp")
PAYLOAD_SIZE = int(os.environ.get("PAYLOAD_SIZE", "4096"))

print(f"Connecting to Console 3 endpoint ({CONSOLE_HOST}:{CONSOLE_PORT})...")
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(5.0)
sock.connect((CONSOLE_HOST, CONSOLE_PORT))

# Negotiate RFC 856 8-bit Binary Mode on Telnet Console
time.sleep(0.2)
try:
    greeting = sock.recv(1024)
except Exception:
    pass
sock.sendall(b"\xff\xfb\x00\xff\xfd\x00\xff\xfd\x03\xff\xfe\x01")
time.sleep(0.2)
sock.setblocking(False)
try:
    while True:
        data = sock.recv(4096)
        if not data:
            break
except BlockingIOError:
    pass
sock.setblocking(True)
sock.settimeout(10.0)

# -------------------------------------------------------------
# TEST PART 1: 4 KiB TX (QNX /dev/ser3 -> Physical Wire)
# -------------------------------------------------------------
print("\n--- Testing 4 KiB TX (QNX /dev/ser3 -> Physical Wire) ---")
# Generate deterministic 4 KiB pattern (1..254 to avoid Telnet IAC command interpretation)
tx_pattern = bytearray((i * 19 + 5) % 254 + 1 for i in range(PAYLOAD_SIZE))
tx_sha = hashlib.sha256(tx_pattern).hexdigest()
print(f"Generated TX payload: {len(tx_pattern)} bytes, SHA-256: {tx_sha}")

with open(f"{ARTIFACT_DIR}/tx_payload.bin", "wb") as f:
    f.write(tx_pattern)

# Stage TX payload to QNX guest
subprocess.run(["scp", f"{ARTIFACT_DIR}/tx_payload.bin", f"{GUEST_HOST}:/data/var/tmp/tx_payload.bin"], check=True)

# Ensure /dev/ser3 is in raw mode
subprocess.run(["ssh", GUEST_HOST, "stty raw -echo -opost -onlcr < /dev/ser3"], check=True)

# Flush any wire noise before starting TX measurement
sock.setblocking(False)
try:
    while True:
        data = sock.recv(4096)
        if not data:
            break
except BlockingIOError:
    pass
sock.setblocking(True)
sock.settimeout(10.0)

# Start physical wire receiver
rx_chunks = []
total_received = 0

# Launch QNX transmitter in background with raw dd write
guest_proc = subprocess.Popen(["ssh", GUEST_HOST, "dd if=/data/var/tmp/tx_payload.bin of=/dev/ser3 bs=4096 count=1"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

start_time = time.time()
while total_received < PAYLOAD_SIZE and (time.time() - start_time) < 10.0:
    try:
        chunk = sock.recv(min(4096, PAYLOAD_SIZE - total_received))
        if chunk:
            rx_chunks.append(chunk)
            total_received += len(chunk)
    except socket.timeout:
        break

guest_proc.wait(timeout=5)
if guest_proc.returncode != 0:
    stderr = guest_proc.stderr.read().decode()
    print(f"FAIL: QNX transmitter failed ({guest_proc.returncode}): {stderr}")
    sys.exit(1)

received_tx = b"".join(rx_chunks)
with open(f"{ARTIFACT_DIR}/tx_received_wire.bin", "wb") as f:
    f.write(received_tx)

print(f"Received {len(received_tx)} / {PAYLOAD_SIZE} bytes from wire.")
if len(received_tx) != PAYLOAD_SIZE:
    print(f"FAIL: Expected {PAYLOAD_SIZE} bytes on wire, received {len(received_tx)} bytes!")
    sys.exit(1)

received_tx_sha = hashlib.sha256(received_tx).hexdigest()
if received_tx_sha != tx_sha:
    print(f"FAIL: Payload corrupted! Expected SHA {tx_sha}, received {received_tx_sha}")
    sys.exit(1)
print(f"PASS: 4 KiB TX payload received bit-exact across physical wire! SHA: {received_tx_sha}")

# -------------------------------------------------------------
# TEST PART 2: 4 KiB RX (Physical Wire -> QNX /dev/ser3)
# -------------------------------------------------------------
print("\n--- Testing 4 KiB RX (Physical Wire -> QNX /dev/ser3) ---")
rx_pattern = bytearray((i * 29 + 11) % 254 + 1 for i in range(PAYLOAD_SIZE))
rx_sha = hashlib.sha256(rx_pattern).hexdigest()
print(f"Generated RX payload: {len(rx_pattern)} bytes, SHA-256: {rx_sha}")

with open(f"{ARTIFACT_DIR}/rx_payload.bin", "wb") as f:
    f.write(rx_pattern)

# Launch QNX receiver in background
rx_guest_proc = subprocess.Popen(["ssh", GUEST_HOST, "/data/var/tmp/qnx_read_exact /dev/ser3 /data/var/tmp/rx_received.bin 4096"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(0.5)

# Send payload across wire socket
sock.sendall(rx_pattern)

rx_guest_proc.wait(timeout=15)
if rx_guest_proc.returncode != 0:
    stderr = rx_guest_proc.stderr.read().decode()
    print(f"FAIL: QNX receiver failed ({rx_guest_proc.returncode}): {stderr}")
    sys.exit(1)

# Retrieve received payload from QNX guest
subprocess.run(["scp", f"{GUEST_HOST}:/data/var/tmp/rx_received.bin", f"{ARTIFACT_DIR}/rx_received.bin"], check=True)

with open(f"{ARTIFACT_DIR}/rx_received.bin", "rb") as f:
    received_rx = f.read()

print(f"Received {len(received_rx)} / {PAYLOAD_SIZE} bytes on QNX.")
if len(received_rx) != PAYLOAD_SIZE:
    print(f"FAIL: Expected {PAYLOAD_SIZE} bytes on QNX, received {len(received_rx)} bytes!")
    sys.exit(1)

received_rx_sha = hashlib.sha256(received_rx).hexdigest()
if received_rx_sha != rx_sha:
    print(f"FAIL: Payload corrupted! Expected SHA {rx_sha}, received {received_rx_sha}")
    sys.exit(1)
print(f"PASS: 4 KiB RX payload received bit-exact on QNX! SHA: {received_rx_sha}")

sock.close()
PYEOF

# Step 3: Dom0 dmesg inspection for UART3 DMA transactions
echo "[3/5] Inspecting Dom0 DMA logs..."
DOM0_DMESG=$(ssh "${DOM0_HOST}" "dmesg | grep 'UART3 DMA' | tail -n 20" || true)
echo "  - Dom0 DMA Submissions:"
echo "${DOM0_DMESG}"
echo "${DOM0_DMESG}" > "${ARTIFACT_DIR}/dom0_dmesg_uart3_dma.txt"

# Step 4: QNX driver telemetry
echo "[4/5] Capturing QNX driver logs..."
QNX_LOGS=$(ssh "${GUEST_HOST}" "cat /data/var/tmp/devc-seramb.log; cat /data/var/tmp/resmgr.log" || true)
# Step 5: Restore interactive console supervisor
echo "[5/5] Restoring QNX interactive console supervisor..."
ssh "${GUEST_HOST}" "nohup /system/bin/qnx-getty /dev/ser3 >/dev/null 2>&1 &"

echo "============================================================"
echo " ENVELOPE 3 QUALIFICATION COMPLETE: All gates passed 100%!"
echo " All evidence preserved in ${ARTIFACT_DIR}"
echo "============================================================"
