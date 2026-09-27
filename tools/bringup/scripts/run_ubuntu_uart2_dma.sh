#!/usr/bin/env bash
#
# tools/bringup/scripts/run_ubuntu_uart2_dma.sh
#
# Qualification test for Envelope 2: Ubuntu HVM UART2 Generic DMA.
# Validates bidirectional 4 KiB DMA transfers across physical Console 2 (Port 6071).
#
# Copyright (C) 2026, Ambarella International LLC
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
ARTIFACT_DIR="${WORKSPACE_ROOT}/plan/plan_hvm_uart_irq_dma.artifacts/envelope2"

mkdir -p "${ARTIFACT_DIR}"

GUEST_HOST="${GUEST_HOST:-n1-655-devkit-ubuntu}"
DOM0_HOST="${DOM0_HOST:-n1-655-devkit}"
CONSOLE_HOST="${CONSOLE_HOST:-127.0.0.1}"
CONSOLE_PORT="${CONSOLE_PORT:-6071}"
PAYLOAD_SIZE=4096

echo "============================================================"
echo " Starting Envelope 2: Ubuntu UART2 Physical DMA Qualification"
echo " Target Guest:   ${GUEST_HOST}"
echo " Dom0 Host:      ${DOM0_HOST}"
echo " Console 2:      ${CONSOLE_HOST}:${CONSOLE_PORT}"
echo " Payload Size:   ${PAYLOAD_SIZE} bytes"
echo " Artifact Dir:   ${ARTIFACT_DIR}"
echo "============================================================"

# Step 1: Pre-flight checks on Guest
echo "[1/6] Pre-flight inspection on ${GUEST_HOST}..."
PLATDEV_CHECK=$(ssh "${GUEST_HOST}" "ls -d /sys/bus/platform/devices/AMBA0001:00 2>/dev/null || true")
if [ -z "${PLATDEV_CHECK}" ]; then
    echo "FAIL: AMBA0001:00 platform device not found in guest sysfs!"
    exit 1
fi
echo "  - Platform device AMBA0001:00 verified: ${PLATDEV_CHECK}"

PCI_ALIAS_CHECK=$(ssh "${GUEST_HOST}" "modinfo amba_uart | grep -i pci || true")
if [ -n "${PCI_ALIAS_CHECK}" ]; then
    echo "FAIL: amba_uart.ko contains forbidden PCI alias: ${PCI_ALIAS_CHECK}"
    exit 1
fi
echo "  - amba_uart.ko verified: zero PCI aliases"

BAR_CHECK=$(ssh "${GUEST_HOST}" "lspci -s 00:07.0 -v | grep -i 'size=16M' || true")
if [ -z "${BAR_CHECK}" ]; then
    echo "FAIL: 16 MiB DMA32 BAR not found on guest 00:07.0!"
    exit 1
fi
echo "  - 16 MiB DMA32 lease window on 00:07.0 verified"

# Step 2: Ensure amba_uart has DMA active on guest
echo "[2/6] Configuring ttyAMBA0 in raw mode..."
ssh "${GUEST_HOST}" "sudo systemctl stop serial-getty@ttyAMBA0 2>/dev/null || true"
ssh "${GUEST_HOST}" "sudo fuser -k /dev/ttyAMBA0 2>/dev/null || true"
ssh "${GUEST_HOST}" "nohup sudo bash -c 'exec 3<>/dev/ttyAMBA0; stty -F /dev/ttyAMBA0 115200 raw -echo -opost -onlcr; sleep 120' >/dev/null 2>&1 &"
sleep 1
GUEST_STATUS=$(ssh "${GUEST_HOST}" "sudo cat /proc/tty/driver/amba_uart")
echo "  - /proc/tty/driver/amba_uart: ${GUEST_STATUS}"
if ! echo "${GUEST_STATUS}" | grep -q "dma:active"; then
    echo "FAIL: amba_uart driver does not report dma:active!"
    exit 1
fi
echo "${GUEST_STATUS}" > "${ARTIFACT_DIR}/proc_tty_driver_amba_uart_pre.txt"

export CONSOLE_HOST CONSOLE_PORT GUEST_HOST DOM0_HOST ARTIFACT_DIR

# Step 3: Run Python DMA verification harness
echo "[3/6] Running bidirectional 4 KiB transfer harness via ${CONSOLE_HOST}:${CONSOLE_PORT}..."
python3 - << 'PYEOF'
import socket
import subprocess
import hashlib
import time
import os
import sys
import re
import select

CONSOLE_HOST = os.environ.get("CONSOLE_HOST", "127.0.0.1")
CONSOLE_PORT = int(os.environ.get("CONSOLE_PORT", "6071"))
GUEST_HOST = os.environ.get("GUEST_HOST", "n1-655-devkit-ubuntu")
DOM0_HOST = os.environ.get("DOM0_HOST", "n1-655-devkit")
ARTIFACT_DIR = os.environ.get("ARTIFACT_DIR", "/tmp")
PAYLOAD_SIZE = 4096

def run_ssh(host, cmd):
    res = subprocess.run(["ssh", host, cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"SSH command to {host} failed ({res.returncode}): {res.stderr.strip()}")
    return res.stdout

print(f"Connecting to Console 2 endpoint ({CONSOLE_HOST}:{CONSOLE_PORT})...")
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
# TEST PART 1: 4 KiB TX (Guest -> Physical Wire)
# -------------------------------------------------------------
print("\n--- Testing 4 KiB TX (Guest ttyAMBA0 -> Physical Wire) ---")
# Generate deterministic 4 KiB pattern (1..255 to avoid Telnet IAC command interpretation)
tx_pattern = bytearray((i * 17 + 3) % 255 + 1 for i in range(PAYLOAD_SIZE))
tx_sha = hashlib.sha256(tx_pattern).hexdigest()
print(f"Generated TX payload: {len(tx_pattern)} bytes, SHA-256: {tx_sha}")

with open(f"{ARTIFACT_DIR}/tx_payload.bin", "wb") as f:
    f.write(tx_pattern)

# Stage TX payload to guest
subprocess.run(["scp", f"{ARTIFACT_DIR}/tx_payload.bin", f"{GUEST_HOST}:/tmp/tx_payload.bin"], check=True)

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

# Launch guest transmitter in background with guaranteed complete unbuffered write and FIFO drain
guest_proc = subprocess.Popen(["ssh", GUEST_HOST, "sudo python3 -c 'import os, time, termios, tty; fd = os.open(\"/dev/ttyAMBA0\", os.O_RDWR | os.O_NOCTTY); tty.setraw(fd); data = open(\"/tmp/tx_payload.bin\", \"rb\").read(); os.write(fd, data); termios.tcdrain(fd); time.sleep(0.5); os.close(fd)'"],
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
    print(f"FAIL: Guest transmitter failed ({guest_proc.returncode}): {stderr}")
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
# TEST PART 2: 4 KiB RX (Physical Wire -> Guest)
# -------------------------------------------------------------
print("\n--- Testing 4 KiB RX (Physical Wire -> Guest ttyAMBA0) ---")
rx_pattern = bytearray((i * 31 + 7) % 255 + 1 for i in range(PAYLOAD_SIZE))
rx_sha = hashlib.sha256(rx_pattern).hexdigest()
print(f"Generated RX payload: {len(rx_pattern)} bytes, SHA-256: {rx_sha}")

with open(f"{ARTIFACT_DIR}/rx_payload.bin", "wb") as f:
    f.write(rx_pattern)

# Setup SSH ControlMasters
guest_sock = f"/tmp/ssh_mux_{GUEST_HOST}.sock"
dom0_sock = f"/tmp/ssh_mux_{DOM0_HOST}.sock"
guest_rx_proc = None
dom0_dmesg_proc = None

def cleanup():
    # Kill any running dd on guest
    try:
        subprocess.run(["ssh", "-S", guest_sock, GUEST_HOST,
                        "sudo pkill -9 -f 'dd if=/dev/ttyAMBA0' 2>/dev/null || true"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
    except Exception:
        pass
    if guest_rx_proc and guest_rx_proc.poll() is None:
        guest_rx_proc.terminate()
    if dom0_dmesg_proc and dom0_dmesg_proc.poll() is None:
        dom0_dmesg_proc.terminate()
    # Close SSH masters
    try:
        subprocess.run(["ssh", "-S", guest_sock, "-O", "exit", GUEST_HOST],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2)
    except Exception:
        pass
    try:
        subprocess.run(["ssh", "-S", dom0_sock, "-O", "exit", DOM0_HOST],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2)
    except Exception:
        pass

for s in [guest_sock, dom0_sock]:
    if os.path.exists(s):
        try:
            os.remove(s)
        except OSError:
            pass

print("Establishing SSH ControlMasters to Guest and Dom0...")
subprocess.run(["ssh", "-M", "-S", guest_sock, "-f", "-N", GUEST_HOST], check=True)
subprocess.run(["ssh", "-M", "-S", dom0_sock, "-f", "-N", DOM0_HOST], check=True)

try:
    # Warm up and test latency
    for _ in range(2):
        subprocess.run(["ssh", "-S", guest_sock, GUEST_HOST, "true"], check=True)
        subprocess.run(["ssh", "-S", dom0_sock, DOM0_HOST, "true"], check=True)

    t0 = time.time()
    subprocess.run(["ssh", "-S", guest_sock, GUEST_HOST, "true"], check=True)
    guest_lat = (time.time() - t0) * 1000.0
    print(f"  - Guest ControlMaster latency: {guest_lat:.1f} ms")
    if guest_lat > 100.0:
        print(f"FAIL: Guest SSH ControlMaster latency exceeds 100 ms ({guest_lat:.1f} ms)!")
        sys.exit(1)

    t0 = time.time()
    subprocess.run(["ssh", "-S", dom0_sock, DOM0_HOST, "true"], check=True)
    dom0_lat = (time.time() - t0) * 1000.0
    print(f"  - Dom0 ControlMaster latency: {dom0_lat:.1f} ms")
    if dom0_lat > 100.0:
        print(f"FAIL: Dom0 SSH ControlMaster latency exceeds 100 ms ({dom0_lat:.1f} ms)!")
        sys.exit(1)

    # Drain any stale characters and wait for DMA RX idle
    for _ in range(30):
        res_idle = subprocess.run(["ssh", "-S", guest_sock, GUEST_HOST,
                                   "cat /sys/bus/platform/drivers/amba_uart/dma_rx 2>/dev/null || echo 0"],
                                  stdout=subprocess.PIPE, text=True)
        if res_idle.stdout.strip() == "0":
            break
        time.sleep(0.1)

    subprocess.run(["ssh", "-S", guest_sock, GUEST_HOST,
                    "sudo dd if=/dev/ttyAMBA0 of=/dev/null bs=4096 count=1 iflag=nonblock status=none 2>/dev/null || true"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Start guest receiver on guest master
    print("Starting dd receiver on guest...")
    guest_rx_proc = subprocess.Popen(["ssh", "-S", guest_sock, GUEST_HOST,
                                      f"sudo dd if=/dev/ttyAMBA0 of=/tmp/rx_received.bin bs={PAYLOAD_SIZE} count=1 iflag=fullblock status=none"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # Confirm dd is blocked in n_tty_read before arming
    dd_ready = False
    wchan = "unknown"
    for _ in range(60):
        res_w = subprocess.run(["ssh", "-S", guest_sock, GUEST_HOST,
                                "for p in $(pgrep -f 'dd if=/dev/ttyAMBA0'); do sudo cat /proc/$p/wchan 2>/dev/null; done"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        wchan = res_w.stdout.strip()
        if "n_tty_read" in wchan or "wait_woken" in wchan:
            dd_ready = True
            break
        time.sleep(0.05)

    if not dd_ready:
        print(f"FAIL: Guest dd did not block in n_tty_read (wchan='{wchan}')")
        sys.exit(1)
    print(f"  - Confirmed guest dd blocked in tty read (wchan: {wchan})")

    # Sample Dom0 printk clock timestamp immediately before arming
    sample_t0 = time.time()
    res_uptime = subprocess.run(["ssh", "-S", dom0_sock, DOM0_HOST,
                                 "echo 'rx_arm_sync' > /dev/kmsg; dmesg --color=never | grep 'rx_arm_sync' | tail -n 1"],
                                stdout=subprocess.PIPE, text=True, check=True)
    sample_t1 = time.time()
    m_arm = re.search(r'\[\s*([0-9]+\.[0-9]+)\]', res_uptime.stdout)
    arm_dom0_uptime = float(m_arm.group(1)) if m_arm else 0.0
    sample_rtt_ms = (sample_t1 - sample_t0) * 1000.0
    min_allowed_gap_ms = -sample_rtt_ms / 2.0
    print(f"  - Sampled Dom0 printk uptime: {arm_dom0_uptime:.6f} s (RTT: {sample_rtt_ms:.1f} ms)")

    # Start live line-buffered dmesg -w stream on Dom0 master using pseudo-terminal allocation
    print("Starting live Dom0 dmesg -w stream (line-buffered via pty)...")
    dom0_dmesg_proc = subprocess.Popen(["ssh", "-tt", "-S", dom0_sock, DOM0_HOST, "dmesg --color=never -w"],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    time.sleep(0.1)

    # Arm guest dma_rx asynchronously (ignore exit code as send gate)
    print("Arming guest DMA RX...")
    guest_arm_proc = subprocess.Popen(["ssh", "-S", guest_sock, GUEST_HOST,
                                       f"echo {PAYLOAD_SIZE} | sudo tee /sys/bus/platform/drivers/amba_uart/dma_rx"],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # Block on open Dom0 stream until fresh "UART2 DMA submitted" with "dreq=14" appears
    print("Waiting for fresh Dom0 DMA RX submission event...")
    rx_submitted = False
    matched_line = ""
    printk_uptime = 0.0
    import re
    import select

    t_arm_wait = time.time()
    while time.time() - t_arm_wait < 8.0:
        r, _, _ = select.select([dom0_dmesg_proc.stdout], [], [], 0.1)
        if not r:
            continue
        line = dom0_dmesg_proc.stdout.readline()
        if not line:
            break
        # Strip ANSI escape codes if present
        clean_line = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', line)
        if "amba_virt_dma:" in clean_line and "UART2 DMA submitted" in clean_line and "dreq=14" in clean_line:
            m = re.search(r'\[\s*([0-9]+\.[0-9]+)\]', clean_line)
            if m:
                line_uptime = float(m.group(1))
                if line_uptime < arm_dom0_uptime:
                    # Stale descriptor line from a previous run; ignore and continue reading
                    continue
                printk_uptime = line_uptime
            else:
                continue

            # Record sendall_time immediately BEFORE calling sock.sendall()
            sendall_time = time.time()
            # Transmit full payload to physical wire with RFC 854 IAC escaping for telnet serial endpoint
            telnet_wire_payload = bytes(rx_pattern).replace(b"\xff", b"\xff\xff")
            sock.sendall(telnet_wire_payload)
            rx_submitted = True
            matched_line = clean_line.strip()
            print(f"  - Dom0 RX descriptor submitted: {matched_line}")
            break

    if not rx_submitted:
        print("FAIL: Dom0 dmesg did not report fresh UART2 DMA submission with dreq=14 within 8 seconds!")
        sys.exit(1)

    # Terminate Dom0 dmesg stream
    dom0_dmesg_proc.terminate()

    # Compute Dom0 single-clock sendall_uptime and gap_ms
    sendall_uptime = arm_dom0_uptime + (sendall_time - (sample_t0 + sample_t1) / 2.0)
    gap_ms = (sendall_uptime - printk_uptime) * 1000.0

    print(f"  - Printk uptime: {printk_uptime:.6f} s, Sendall uptime: {sendall_uptime:.6f} s")
    print(f"  - Signed timing gap (sendall - printk): {gap_ms:.2f} ms")

    with open(f"{ARTIFACT_DIR}/rx_timing_gap.txt", "w") as f:
        f.write(f"matched_line: {matched_line}\n")
        f.write(f"sample_t0: {sample_t0:.6f}\n")
        f.write(f"sample_t1: {sample_t1:.6f}\n")
        f.write(f"sample_rtt_ms: {sample_rtt_ms:.2f}\n")
        f.write(f"arm_dom0_uptime: {arm_dom0_uptime:.6f}\n")
        f.write(f"printk_uptime: {printk_uptime:.6f}\n")
        f.write(f"sendall_time: {sendall_time:.6f}\n")
        f.write(f"sendall_uptime: {sendall_uptime:.6f}\n")
        f.write(f"gap_ms: {gap_ms:.2f}\n")
        f.write(f"min_allowed_gap_ms: {min_allowed_gap_ms:.2f}\n")

    if gap_ms < min_allowed_gap_ms:
        print(f"FAIL: Premature send detected! gap_ms ({gap_ms:.2f} ms) is earlier than RTT margin ({min_allowed_gap_ms:.2f} ms)!")
        sys.exit(1)

    if gap_ms > 5000.0:
        print(f"FAIL: Gap between printk and sendall exceeds 5000 ms ({gap_ms:.2f} ms)!")
        sys.exit(1)

    # Wait for guest receiver to complete
    print("Waiting for guest receiver to complete...")
    guest_rx_proc.wait(timeout=10)
    if guest_rx_proc.returncode != 0:
        stderr = guest_rx_proc.stderr.read().decode()
        print(f"FAIL: Guest receiver failed ({guest_rx_proc.returncode}): {stderr}")
        sys.exit(1)

    subprocess.run(["scp", f"{GUEST_HOST}:/tmp/rx_received.bin", f"{ARTIFACT_DIR}/rx_received_guest.bin"], check=True)

    with open(f"{ARTIFACT_DIR}/rx_received_guest.bin", "rb") as f:
        received_rx = f.read()

    print(f"Guest received {len(received_rx)} / {PAYLOAD_SIZE} bytes.")
    if len(received_rx) != PAYLOAD_SIZE:
        print(f"FAIL: Expected {PAYLOAD_SIZE} bytes on guest, received {len(received_rx)} bytes!")
        sys.exit(1)

    received_rx_sha = hashlib.sha256(received_rx).hexdigest()
    if received_rx_sha != rx_sha:
        print(f"FAIL: RX Payload corrupted! Expected SHA {rx_sha}, received {received_rx_sha}")
        sys.exit(1)
    print(f"PASS: 4 KiB RX payload received bit-exact on guest! SHA: {received_rx_sha}")

finally:
    cleanup()

sock.close()
print("\nBidirectional wire transfers verified successfully!")
PYEOF

# Step 4: Verify Descriptor logs on Dom0
echo "[4/6] Verifying DMA descriptor logs in Dom0 dmesg..."
DOM0_DMESG=$(ssh "${DOM0_HOST}" "dmesg | grep -E 'amba_virt_dma: lease [0-9]+ UART2 DMA submitted' | tail -n 10")
echo "${DOM0_DMESG}" | tee "${ARTIFACT_DIR}/dom0_dma_descriptors.log"

if ! echo "${DOM0_DMESG}" | grep -q "dreq=13"; then
    echo "FAIL: Dom0 dmesg does not show DREQ 13 (TX) descriptor submission!"
    exit 1
fi
echo "  - Verified TX descriptor with DREQ 13"

if ! echo "${DOM0_DMESG}" | grep -q "dreq=14"; then
    echo "FAIL: Dom0 dmesg does not show DREQ 14 (RX) descriptor submission!"
    exit 1
fi
echo "  - Verified RX descriptor with DREQ 14"

# Step 5: Post-test telemetry and cleanup
echo "[5/6] Verifying post-test driver telemetry..."
GUEST_STATUS_POST=$(ssh "${GUEST_HOST}" "sudo cat /proc/tty/driver/amba_uart")
echo "  - /proc/tty/driver/amba_uart: ${GUEST_STATUS_POST}"
echo "${GUEST_STATUS_POST}" > "${ARTIFACT_DIR}/proc_tty_driver_amba_uart_post.txt"

# Step 6: Restore serial-getty service
echo "[6/6] Restoring guest serial-getty service..."
ssh "${GUEST_HOST}" "sudo systemctl start serial-getty@ttyAMBA0 2>/dev/null || true"

echo "============================================================"
echo " Envelope 2 Qualification PASSED: Ubuntu UART2 Generic DMA"
echo " All evidence preserved in ${ARTIFACT_DIR}"
echo "============================================================"
exit 0
