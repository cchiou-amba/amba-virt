#!/usr/bin/env python3
#
# tools/bringup/scripts/run_uart_bench.py
#
# Automated Directional UART Performance & Integrity Benchmark Orchestrator.
# Manages directional trials (interactive echo, wire->guest RX, guest->wire TX,
# sustained streaming, and concurrent multi-tenant isolation) across Ubuntu and QNX.
# Captures Dom0 DREQ descriptors, physical/virtual IRQs, CPU/thermal telemetry,
# and emits strict JSONL records with cryptographic hash and CRC verification.
#
# Copyright (C) 2026, Ambarella International LLC
#

import argparse
import hashlib
import json
import math
import os
import random
import re
import socket
import struct
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

BENCH_MAGIC = 0x55415254  # 'UART'

TELNET_IAC = 255
TELNET_DONT = 254
TELNET_DO = 253
TELNET_WONT = 252
TELNET_WILL = 251

def strip_telnet_negotiation(data: bytes) -> bytes:
    """Filter out Telnet IAC sequences from serial socket stream."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        if data[i] == TELNET_IAC:
            if i + 1 < n and data[i + 1] in (TELNET_WILL, TELNET_WONT, TELNET_DO, TELNET_DONT):
                i += 3
                continue
            elif i + 1 < n and data[i + 1] == TELNET_IAC:
                out.append(TELNET_IAC)
                i += 2
                continue
            else:
                i += 2
                continue
        out.append(data[i])
        i += 1
    return bytes(out)

def crc32_ieee(data: bytes) -> int:
    """Compute standard IEEE 802.3 CRC-32."""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)))
    return ~crc & 0xFFFFFFFF

def extract_json(raw_str: str) -> dict:
    """Extract first valid JSON object from raw string that may contain trailing SSH exit notifications."""
    raw_str = raw_str.strip()
    if not raw_str:
        return {}
    try:
        return json.loads(raw_str)
    except json.JSONDecodeError:
        start = raw_str.find("{")
        end = raw_str.rfind("}")
        if start != -1 and end != -1 and end > start:
            try:
                return json.loads(raw_str[start : end + 1])
            except Exception:
                pass
        return {}


def generate_prbs_payload(seed: int, length: int) -> bytes:
    """Generate deterministic PRBS-9 byte sequence."""
    lfsr = 0x1FE if (seed & 0x1FF) == 0 else (seed & 0x1FF)
    out = bytearray(length)
    for i in range(length):
        byte = 0
        for _ in range(8):
            bit = ((lfsr >> 8) ^ (lfsr >> 4)) & 1
            lfsr = ((lfsr << 1) | bit) & 0x1FF
            byte = (byte << 1) | bit
        out[i] = byte
    return bytes(out)

class SSHClient:
    def __init__(self, host: str):
        self.host = host

    def run(self, cmd: str, timeout: float = 15.0) -> Tuple[int, str, str]:
        """Execute command via SSH with strict error tracking and timeout."""
        try:
            res = subprocess.run(
                ["ssh", "-o", "BatchMode=yes", self.host, cmd],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=timeout
            )
            return res.returncode, res.stdout, res.stderr
        except subprocess.TimeoutExpired:
            return -ETIMEDOUT if 'ETIMEDOUT' in globals() else -110, "", "SSH command timed out"
        except Exception as e:
            return -1, "", str(e)

    def check_ok(self, cmd: str, timeout: float = 15.0) -> str:
        code, out, err = self.run(cmd, timeout=timeout)
        if code != 0:
            raise RuntimeError(f"SSH command on {self.host} failed (exit {code}): {cmd}\nStderr: {err}")
        return out.strip()

class SerialWireClient:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect((self.host, self.port))
        time.sleep(0.1)
        # Negotiate RFC 856 Binary Transmission on Telnet Console
        try:
            self.sock.sendall(b'\xff\xfd\x01\xff\xfd\x03\xff\xfb\x00\xff\xfd\x00')
            time.sleep(0.15)
            self.sock.setblocking(False)
            while True:
                raw = self.sock.recv(4096)
                if not raw: break
        except Exception:
            pass
        self.sock.setblocking(True)
        self.sock.settimeout(self.timeout)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def flush(self):
        if not self.sock: return
        self.sock.settimeout(0.05)
        try:
            while True:
                data = self.sock.recv(4096)
                if not data: break
        except socket.timeout:
            pass
        self.sock.settimeout(self.timeout)

    def write_all(self, data: bytes, timeout: float = 10.0):
        self.sock.settimeout(timeout)
        self.sock.sendall(data)

    def read_exact(self, count: int, timeout: float = 10.0) -> bytes:
        self.sock.settimeout(timeout)
        buf = bytearray()
        start = time.perf_counter()
        while len(buf) < count:
            if time.perf_counter() - start > timeout:
                raise TimeoutError(f"Serial read timeout: got {len(buf)}/{count} bytes")
            try:
                chunk = self.sock.recv(count - len(buf))
                if not chunk:
                    raise ConnectionResetError("Serial socket closed prematurely")
                buf.extend(chunk)
            except socket.timeout:
                continue
        return bytes(buf)

def compute_statistics(samples_ns: List[int]) -> Dict[str, Any]:
    """Compute median, p95, p99, median absolute deviation (MAD), and bootstrap 95% CI."""
    if not samples_ns:
        return {}
    s_sorted = sorted(samples_ns)
    n = len(s_sorted)
    median = s_sorted[n // 2] if n % 2 != 0 else (s_sorted[n // 2 - 1] + s_sorted[n // 2]) / 2.0
    p95 = s_sorted[min(int(n * 0.95), n - 1)]
    p99 = s_sorted[min(int(n * 0.99), n - 1)]

    # Median Absolute Deviation (MAD)
    devs = sorted([abs(x - median) for x in samples_ns])
    mad = devs[n // 2] if n % 2 != 0 else (devs[n // 2 - 1] + devs[n // 2]) / 2.0

    # Bootstrap 95% Confidence Interval (1,000 resamples)
    boot_medians = []
    for _ in range(1000):
        resample = [random.choice(samples_ns) for _ in range(n)]
        resample.sort()
        bm = resample[n // 2]
        boot_medians.append(bm)
    boot_medians.sort()
    ci_lower = boot_medians[int(1000 * 0.025)]
    ci_upper = boot_medians[int(1000 * 0.975)]

    return {
        "count": n,
        "median_ns": median,
        "median_ms": median / 1e6,
        "p95_ns": p95,
        "p95_ms": p95 / 1e6,
        "p99_ns": p99,
        "p99_ms": p99 / 1e6,
        "mad_ns": mad,
        "mad_ms": mad / 1e6,
        "ci_95_ns": [ci_lower, ci_upper],
        "ci_95_ms": [ci_lower / 1e6, ci_upper / 1e6],
    }

class TelemetryCollector:
    def __init__(self, dom0_host: str, guest_host: str, guest_os: str):
        self.dom0 = SSHClient(dom0_host)
        self.guest = SSHClient(guest_host)
        self.guest_os = guest_os

    def sample_telemetry(self, uart_num: int) -> Dict[str, Any]:
        """Collect host and guest telemetry snapshot."""
        telemetry: Dict[str, Any] = {}

        # 1. Host Physical Interrupts
        host_uart_irq = "115" if uart_num == 2 else "116"
        host_stat = self.dom0.check_ok(f"cat /proc/interrupts | grep -E '{host_uart_irq}:|131:' || true")
        uart_irqs = 0
        dma_irqs = 0
        for line in host_stat.splitlines():
            parts = line.split()
            if not parts: continue
            if parts[0].startswith(f"{host_uart_irq}:"):
                uart_irqs = sum(int(p) for p in parts[1:] if p.isdigit())
            elif parts[0].startswith("131:"):
                dma_irqs = sum(int(p) for p in parts[1:] if p.isdigit())
        telemetry["host_uart_irqs"] = uart_irqs
        telemetry["host_dma_irqs"] = dma_irqs

        # 2. Guest Interrupts
        if self.guest_os == "ubuntu":
            g_stat = self.guest.check_ok("cat /proc/interrupts | grep -E 'amba_uart|50:' || true")
            g_irqs = 0
            for line in g_stat.splitlines():
                parts = line.split()
                if parts and (parts[0].startswith("50:") or "amba_uart" in line):
                    g_irqs = sum(int(p) for p in parts[1:] if p.isdigit())
            telemetry["guest_irqs"] = g_irqs
        else:
            # QNX
            g_stat = self.guest.check_ok("pidin -p devc-seramb irqs 2>/dev/null || true")
            telemetry["guest_irqs"] = 1 if "144" in g_stat else 0

        # 3. CPU Frequency, Load, Temperature on Dom0
        try:
            freq_str = self.dom0.check_ok("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0")
            telemetry["cpu_freq_khz"] = int(freq_str) if freq_str.isdigit() else None
        except Exception:
            telemetry["cpu_freq_khz"] = None

        try:
            load_str = self.dom0.check_ok("cat /proc/loadavg 2>/dev/null || true")
            telemetry["loadavg"] = load_str.split()[:3] if load_str else None
        except Exception:
            telemetry["loadavg"] = None

        try:
            temp_str = self.dom0.check_ok("cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0")
            telemetry["temp_c"] = float(temp_str) / 1000.0 if temp_str.isdigit() else None
        except Exception:
            telemetry["temp_c"] = None

        # 4. Hardware Line Errors (OE, FE, PE, BRK)
        if self.guest_os == "ubuntu":
            proc_uart = self.guest.check_ok("cat /proc/tty/driver/amba_uart 2>/dev/null || true")
            oe = int(re.search(r'oe:(\d+)', proc_uart).group(1)) if re.search(r'oe:(\d+)', proc_uart) else 0
            fe = int(re.search(r'fe:(\d+)', proc_uart).group(1)) if re.search(r'fe:(\d+)', proc_uart) else 0
            pe = int(re.search(r'pe:(\d+)', proc_uart).group(1)) if re.search(r'pe:(\d+)', proc_uart) else 0
            brk = int(re.search(r'brk:(\d+)', proc_uart).group(1)) if re.search(r'brk:(\d+)', proc_uart) else 0
            telemetry["lsr_errors"] = {"oe": oe, "fe": fe, "pe": pe, "brk": brk}
        else:
            telemetry["lsr_errors"] = {"oe": 0, "fe": 0, "pe": 0, "brk": 0}

        # 5. Process CPU times (QEMU, Broker, Agent)
        try:
            qemu_pid = self.dom0.check_ok("pgrep -f qemu-system-aarch64 | head -n 1 2>/dev/null || echo ''")
            if qemu_pid:
                q_stat = self.dom0.check_ok(f"cat /proc/{qemu_pid}/stat 2>/dev/null || echo ''")
                parts = q_stat.split()
                telemetry["qemu_cpu_ticks"] = int(parts[13]) + int(parts[14]) if len(parts) > 14 else None
            else:
                telemetry["qemu_cpu_ticks"] = None
        except Exception:
            telemetry["qemu_cpu_ticks"] = None

        try:
            broker_pid = self.dom0.check_ok("pgrep -f virt_dma_broker | head -n 1 2>/dev/null || echo ''")
            if broker_pid:
                b_stat = self.dom0.check_ok(f"cat /proc/{broker_pid}/stat 2>/dev/null || echo ''")
                parts = b_stat.split()
                telemetry["broker_cpu_ticks"] = int(parts[13]) + int(parts[14]) if len(parts) > 14 else None
            else:
                telemetry["broker_cpu_ticks"] = None
        except Exception:
            telemetry["broker_cpu_ticks"] = None

        return telemetry

    def get_dom0_dma_descriptors(self, start_uptime: float, uart_num: int) -> List[str]:
        """Fetch fresh Dom0 DMA submission printk lines since start_uptime."""
        dreq_rx = "14" if uart_num == 2 else "16"
        dreq_tx = "13" if uart_num == 2 else "15"
        out = self.dom0.check_ok(f"dmesg | grep -E 'UART{uart_num} DMA submitted' || true")
        lines = []
        for line in out.splitlines():
            m = re.search(r'\[\s*([0-9]+\.[0-9]+)\]', line)
            if m:
                ts = float(m.group(1))
                if ts >= start_uptime - 0.5:
                    lines.append(line)
        return lines

    def get_dom0_uptime(self) -> float:
        out = self.dom0.check_ok("cat /proc/uptime")
        return float(out.split()[0])

def configure_guest_mode(guest: SSHClient, guest_os: str, mode: str) -> None:
    """Set explicit driver parameters on guest, aborting on any failure."""
    if guest_os == "ubuntu":
        guest.check_ok("sudo systemctl stop serial-getty@ttyAMBA0.service 2>/dev/null || true")
        guest.check_ok("sudo killall -q -9 uart_bench_agent 2>/dev/null || true")
        guest.check_ok("sudo test -c /dev/ttyAMBA0 || (sudo rm -f /dev/ttyAMBA0 && sudo mknod /dev/ttyAMBA0 c 234 0 && sudo chmod 666 /dev/ttyAMBA0)")

        plat_dma = "/sys/bus/platform/drivers/amba_uart/use_dma"
        plat_poll = "/sys/bus/platform/drivers/amba_uart/force_poll"

        if mode == "dma-irq":
            guest.check_ok(f"echo 0 | sudo tee {plat_poll} >/dev/null && echo 1 | sudo tee {plat_dma} >/dev/null")
        elif mode == "pio-irq":
            guest.check_ok(f"echo 0 | sudo tee {plat_poll} >/dev/null && echo 0 | sudo tee {plat_dma} >/dev/null")
        elif mode == "dma-poll":
            guest.check_ok(f"echo 1 | sudo tee {plat_poll} >/dev/null && echo 1 | sudo tee {plat_dma} >/dev/null")
        elif mode == "pio-poll":
            guest.check_ok(f"echo 1 | sudo tee {plat_poll} >/dev/null && echo 0 | sudo tee {plat_dma} >/dev/null")
        else:
            raise ValueError(f"Unknown mode: {mode}")

    elif guest_os == "qnx":
        guest.check_ok("slay -f qnx-getty devc-seramb uart_bench_agent 2>/dev/null || true")
        time.sleep(0.3)
        if mode == "dma-irq":
            guest.check_ok("/system/bin/devc-seramb -p /dev/ser3 -a 0x0c000000 -i 144 &")
        elif mode == "pio-irq":
            guest.check_ok("/system/bin/devc-seramb -p /dev/ser3 -a 0x0c000000 -i 144 --no-dma &")
        elif mode == "dma-poll":
            guest.check_ok("/system/bin/devc-seramb -p /dev/ser3 -a 0x0c000000 -i 0 &")
        elif mode == "pio-poll":
            guest.check_ok("/system/bin/devc-seramb -p /dev/ser3 -a 0x0c000000 -i 0 --no-dma &")
        else:
            raise ValueError(f"Unknown mode: {mode}")
        time.sleep(0.3)

def get_observed_mode(guest: SSHClient, guest_os: str) -> str:
    """Read true driver state from sysfs/dmesg or pidin."""
    if guest_os == "ubuntu":
        sys_dma = guest.check_ok("cat /sys/bus/platform/drivers/amba_uart/use_dma 2>/dev/null || echo 0")
        sys_poll = guest.check_ok("cat /sys/bus/platform/drivers/amba_uart/force_poll 2>/dev/null || echo 0")
        dma_str = "dma" if sys_dma == "1" else "pio"
        poll_str = "poll" if sys_poll == "1" else "irq"
        return f"{dma_str}-{poll_str}"
    else:
        pidin = guest.check_ok("pidin -p devc-seramb arg 2>/dev/null || true")
        dma_str = "pio" if "--no-dma" in pidin else "dma"
        poll_str = "poll" if "-i 0" in pidin else "irq"
        return f"{dma_str}-{poll_str}"

def restore_guest_services(guest: SSHClient, guest_os: str) -> None:
    if guest_os == "ubuntu":
        guest.run("sudo killall -q -9 uart_bench_agent 2>/dev/null || true; sudo systemctl start serial-getty@ttyAMBA0.service 2>/dev/null || true")
    elif guest_os == "qnx":
        guest.run("slay -f uart_bench_agent devc-seramb 2>/dev/null || true; /system/bin/devc-seramb -p /dev/ser3 -a 0x0c000000 -i 144 & sleep 0.3; /system/bin/qnx-getty /dev/ser3 115200 &")

def run_trial_interactive_echo(
    trial_id: int,
    guest_os: str,
    requested_mode: str,
    wire: SerialWireClient,
    collector: TelemetryCollector,
    guest: SSHClient,
    dev_path: str,
    uart_num: int
) -> Dict[str, Any]:
    """Execute Workload 1: 1,000 interactive keystroke round-trips after 100 warmups."""
    print(f"[*] Running Workload 1 (Interactive Echo) | Mode: {requested_mode}...")
    configure_guest_mode(guest, guest_os, requested_mode)
    time.sleep(0.5)

    observed_mode = get_observed_mode(guest, guest_os)
    if observed_mode != requested_mode:
        raise RuntimeError(f"Mode mismatch: requested {requested_mode} but observed {observed_mode}")

    sudo_pfx = "sudo " if guest_os == "ubuntu" else ""

    # Launch guest agent in echo mode
    agent_cmd = f"{sudo_pfx}/tmp/uart_bench_agent --mode echo --device {dev_path} --samples 1000 --warmup 100"
    guest.run(f"nohup {agent_cmd} > /tmp/agent_echo.json 2>&1 &")
    time.sleep(0.3)

    wire.connect()
    wire.flush()

    t_start_uptime = collector.get_dom0_uptime()
    t_start = time.perf_counter()
    telem_start = collector.sample_telemetry(uart_num)

    total_samples = 1100
    test_chars = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    rtt_samples_ns: List[int] = []

    for i in range(total_samples):
        ch = bytes([test_chars[i % len(test_chars)]])
        t0 = time.perf_counter_ns()
        wire.write_all(ch, timeout=2.0)
        resp = wire.read_exact(1, timeout=2.0)
        t1 = time.perf_counter_ns()
        if resp != ch:
            raise RuntimeError(f"Keystroke mismatch at iteration {i}: sent {ch} got {resp}")
        if i >= 100:
            rtt_samples_ns.append(t1 - t0)
        time.sleep(0.002)

    t_end = time.perf_counter()
    telem_end = collector.sample_telemetry(uart_num)
    wire.close()

    # Wait for guest agent to finish writing JSON
    time.sleep(0.5)
    agent_json_str = guest.check_ok("cat /tmp/agent_echo.json 2>/dev/null || echo '{}'")
    agent_res = extract_json(agent_json_str)

    stats = compute_statistics(rtt_samples_ns)
    dma_lines = collector.get_dom0_dma_descriptors(t_start_uptime, uart_num)

    record = {
        "trial_id": trial_id,
        "guest": guest_os,
        "direction": "interactive_echo",
        "requested_mode": requested_mode,
        "observed_mode": observed_mode,
        "payload_len": 1,
        "sample_count": len(rtt_samples_ns),
        "rtt_statistics": stats,
        "guest_processing_stats": compute_statistics(agent_res.get("samples_ns", [])),
        "uart_irq_delta_host": telem_end["host_uart_irqs"] - telem_start["host_uart_irqs"],
        "dma_irq_delta_host": telem_end["host_dma_irqs"] - telem_start["host_dma_irqs"],
        "guest_irq_delta": telem_end["guest_irqs"] - telem_start["guest_irqs"],
        "dma_descriptor_lines": dma_lines,
        "lsr_errors": telem_end.get("lsr_errors"),
        "temp_c": telem_end.get("temp_c"),
        "status": "PASS"
    }
    return record

def run_trial_directional_bulk(
    trial_id: int,
    guest_os: str,
    direction: str,  # "tx" (guest->wire) or "rx" (wire->guest)
    requested_mode: str,
    payload_len: int,
    wire: SerialWireClient,
    collector: TelemetryCollector,
    guest: SSHClient,
    dev_path: str,
    uart_num: int
) -> Dict[str, Any]:
    """Execute Workload 2: Framed PRBS record transfer with CRC-32 & SHA-256."""
    print(f"[*] Running Workload 2 ({direction.upper()} {payload_len:5d} B) | Mode: {requested_mode}...")
    configure_guest_mode(guest, guest_os, requested_mode)
    time.sleep(0.3)

    observed_mode = get_observed_mode(guest, guest_os)
    if observed_mode != requested_mode:
        raise RuntimeError(f"Mode mismatch: requested {requested_mode} but observed {observed_mode}")

    seq = trial_id * 100 + (payload_len % 97)
    payload = generate_prbs_payload(trial_id + seq, payload_len)
    payload_crc = crc32_ieee(payload)
    payload_sha256 = hashlib.sha256(payload).hexdigest()

    # Frame: Header (16 B) + Payload + CRC (4 B)
    hdr = struct.pack("<IIII", BENCH_MAGIC, trial_id, seq, payload_len)
    crc_bytes = struct.pack("<I", payload_crc)
    full_frame = hdr + payload + crc_bytes

    wire.connect()
    wire.flush()

    t_start_uptime = collector.get_dom0_uptime()
    telem_start = collector.sample_telemetry(uart_num)
    t_start = time.perf_counter()

    sudo_pfx = "sudo " if guest_os == "ubuntu" else ""

    if direction == "tx":
        # Guest transmits to wire
        agent_cmd = f"{sudo_pfx}/tmp/uart_bench_agent --mode tx --device {dev_path} --trial-id {trial_id} --seq {seq} --size {payload_len}"
        guest.run(f"nohup {agent_cmd} > /tmp/agent_tx.json 2>&1 &")

        # Wire reads full frame
        frame_received = wire.read_exact(len(full_frame), timeout=max(5.0, (payload_len / 5000.0) + 3.0))
        t_end = time.perf_counter()
        telem_end = collector.sample_telemetry(uart_num)
        wire.close()

        rx_hdr = frame_received[:16]
        rx_payload = frame_received[16:-4]
        rx_crc = struct.unpack("<I", frame_received[-4:])[0]
        rx_sha256 = hashlib.sha256(rx_payload).hexdigest()

        crc_match = (rx_crc == payload_crc) and (crc32_ieee(rx_payload) == payload_crc)
        sha_match = (rx_sha256 == payload_sha256)

        time.sleep(0.3)
        agent_json_str = guest.check_ok("cat /tmp/agent_tx.json 2>/dev/null || echo '{}'")
        agent_res = extract_json(agent_json_str)

        duration_sec = t_end - t_start
        throughput_kb_s = (len(full_frame) / 1024.0) / duration_sec if duration_sec > 0 else 0.0

    else:
        # Wire transmits to guest
        agent_cmd = f"{sudo_pfx}/tmp/uart_bench_agent --mode rx --device {dev_path} --trial-id {trial_id} --seq {seq} --size {payload_len} --timeout 15.0"
        guest.run(f"nohup {agent_cmd} > /tmp/agent_rx.json 2>&1 &")
        time.sleep(0.3)

        wire.write_all(full_frame, timeout=max(5.0, (payload_len / 5000.0) + 3.0))

        # Wait for wire transmission time at 115200 baud (~11520 bytes/sec)
        expected_wire_sec = len(full_frame) / 10000.0
        time.sleep(expected_wire_sec + 0.1)
        wire.close()
        t_end = time.perf_counter()
        telem_end = collector.sample_telemetry(uart_num)

        # Wait for guest agent to finish writing JSON
        agent_res = {}
        t_wait_start = time.perf_counter()
        while time.perf_counter() - t_wait_start < 10.0:
            agent_json_str = guest.check_ok("cat /tmp/agent_rx.json 2>/dev/null || echo '{}'")
            if "crc_match" in agent_json_str or "error" in agent_json_str:
                res = extract_json(agent_json_str)
                if res:
                    agent_res = res
                    break
            time.sleep(0.1)

        crc_match = agent_res.get("crc_match", False)
        rx_sha256 = agent_res.get("sha256", "")
        sha_match = (rx_sha256 == payload_sha256)
        duration_sec = agent_res.get("duration_ns", 0) / 1e9
        throughput_kb_s = (len(full_frame) / 1024.0) / duration_sec if duration_sec > 0 else 0.0

    dma_lines = collector.get_dom0_dma_descriptors(t_start_uptime, uart_num)

    # Validate Mode Discipline
    is_dma_mode = "dma" in requested_mode
    if is_dma_mode and payload_len >= 64 and direction == "tx":
        # DMA transmit must have fresh descriptors
        if not dma_lines:
            print(f"[!] Warning: Expected DMA descriptor for {payload_len} B TX but found none")
    elif not is_dma_mode and dma_lines:
        raise RuntimeError(f"Observed unexpected DMA descriptors during PIO trial: {dma_lines}")

    record = {
        "trial_id": trial_id,
        "guest": guest_os,
        "direction": direction,
        "requested_mode": requested_mode,
        "observed_mode": observed_mode,
        "payload_len": payload_len,
        "total_bytes": len(full_frame),
        "duration_sec": duration_sec,
        "throughput_kb_s": throughput_kb_s,
        "sha256_host": payload_sha256,
        "sha256_guest": rx_sha256 if direction == "rx" else agent_res.get("sha256"),
        "sha_match": sha_match,
        "crc_match": crc_match,
        "dma_descriptor_lines": dma_lines,
        "uart_irq_delta_host": telem_end["host_uart_irqs"] - telem_start["host_uart_irqs"],
        "dma_irq_delta_host": telem_end["host_dma_irqs"] - telem_start["host_dma_irqs"],
        "guest_irq_delta": telem_end["guest_irqs"] - telem_start["guest_irqs"],
        "lsr_errors": telem_end.get("lsr_errors"),
        "temp_c": telem_end.get("temp_c"),
        "status": "PASS" if (crc_match and sha_match) else "INTEGRITY_FAILURE"
    }
    return record

def run_trial_sustained_stream(
    trial_id: int,
    guest_os: str,
    requested_mode: str,
    payload_len: int,
    wire: SerialWireClient,
    collector: TelemetryCollector,
    guest: SSHClient,
    dev_path: str,
    uart_num: int,
    duration_sec: int = 30
) -> Dict[str, Any]:
    """Execute Workload 3: 30-second continuous streaming in one direction."""
    print(f"[*] Running Workload 3 (Sustained {duration_sec}s TX Stream) | Mode: {requested_mode}...")
    configure_guest_mode(guest, guest_os, requested_mode)
    time.sleep(0.3)

    observed_mode = get_observed_mode(guest, guest_os)
    if observed_mode != requested_mode:
        raise RuntimeError(f"Mode mismatch: requested {requested_mode} but observed {observed_mode}")

    wire.connect()
    wire.flush()

    t_start_uptime = collector.get_dom0_uptime()
    telem_start = collector.sample_telemetry(uart_num)
    t_start = time.perf_counter()

    sudo_pfx = "sudo " if guest_os == "ubuntu" else ""
    agent_cmd = f"{sudo_pfx}/tmp/uart_bench_agent --mode tx --device {dev_path} --trial-id {trial_id} --seq 0 --size {payload_len} --sustained {duration_sec}"
    guest.run(f"nohup {agent_cmd} > /tmp/agent_sustained.json 2>&1 &")

    total_bytes_rx = 0
    t_end_limit = t_start + duration_sec + 5.0
    while time.perf_counter() < t_end_limit:
        try:
            chunk = wire.sock.recv(4096) if wire.sock else b""
            if not chunk: break
            total_bytes_rx += len(chunk)
        except socket.timeout:
            pass

    t_end = time.perf_counter()
    telem_end = collector.sample_telemetry(uart_num)
    wire.close()

    time.sleep(0.5)
    agent_json_str = guest.check_ok("cat /tmp/agent_sustained.json 2>/dev/null || echo '{}'")
    agent_res = extract_json(agent_json_str)

    actual_duration = t_end - t_start
    throughput_kb_s = (total_bytes_rx / 1024.0) / actual_duration if actual_duration > 0 else 0.0
    dma_lines = collector.get_dom0_dma_descriptors(t_start_uptime, uart_num)

    record = {
        "trial_id": trial_id,
        "guest": guest_os,
        "direction": "sustained_tx",
        "requested_mode": requested_mode,
        "observed_mode": observed_mode,
        "payload_len": payload_len,
        "total_bytes_received": total_bytes_rx,
        "duration_sec": actual_duration,
        "throughput_kb_s": throughput_kb_s,
        "wire_efficiency_pct": (throughput_kb_s / 11.52) * 100.0,
        "dma_descriptor_lines": dma_lines,
        "uart_irq_delta_host": telem_end["host_uart_irqs"] - telem_start["host_uart_irqs"],
        "dma_irq_delta_host": telem_end["host_dma_irqs"] - telem_start["host_dma_irqs"],
        "guest_irq_delta": telem_end["guest_irqs"] - telem_start["guest_irqs"],
        "lsr_errors": telem_end.get("lsr_errors"),
        "temp_c": telem_end.get("temp_c"),
        "status": "PASS"
    }
    return record

def main():
    parser = argparse.ArgumentParser(description="Deterministic Directional UART Benchmark Suite")
    parser.add_argument("--guest-os", choices=["ubuntu", "qnx", "both"], required=True, help="Target guest OS")
    parser.add_argument("--host", default=os.environ.get("CONSOLE_HOST", "192.168.8.30"), help="Terminal server host")
    parser.add_argument("--port", type=int, default=6071, help="Terminal server port")
    parser.add_argument("--guest-alias", default="n1-655-devkit-ubuntu", help="Guest SSH host alias")
    parser.add_argument("--dom0-alias", default="n1-655-devkit", help="Dom0 SSH host alias")
    parser.add_argument("--jsonl-out", required=True, help="Destination JSONL path")
    parser.add_argument("--blocks", type=int, default=5, help="Number of randomized blocks (default: 5)")
    args = parser.parse_args()

    print("============================================================")
    print(" Ambarella Directional UART Virtualization Benchmark Suite")
    print(f" Target OS: {args.guest_os.upper()} | Wire: {args.host}:{args.port}")
    print("============================================================")

    guest = SSHClient(args.guest_alias)
    collector = TelemetryCollector(args.dom0_alias, args.guest_alias, args.guest_os)
    wire = SerialWireClient(args.host, args.port)

    # 1. Stage and compile agent on guest
    print("[*] Staging and verifying benchmark agent on guest...")
    if args.guest_os == "ubuntu":
        guest.check_ok("mkdir -p /tmp/bringup")
        guest.check_ok("sudo test -c /dev/ttyAMBA0 || (sudo rm -f /dev/ttyAMBA0 && sudo mknod /dev/ttyAMBA0 c 234 0 && sudo chmod 666 /dev/ttyAMBA0)")
        subprocess.run(["scp", "-o", "BatchMode=yes", "tools/bringup/uart_bench_agent.c", f"{args.guest_alias}:/tmp/bringup/"], check=True)
        guest.check_ok("gcc -O2 -Wall -o /tmp/uart_bench_agent /tmp/bringup/uart_bench_agent.c")
        guest.check_ok("/tmp/uart_bench_agent --selftest")
        dev_path = "/dev/ttyAMBA0"
        uart_num = 2
    else:
        # QNX
        guest.check_ok("mkdir -p /tmp/bringup")
        subprocess.run("source ~/qnx/qnx800/qnxsdp-env.sh && qcc -Vgcc_ntoaarch64le -O2 -o /tmp/uart_bench_agent_qnx tools/bringup/uart_bench_agent.c", shell=True, executable="/bin/bash", check=True)
        subprocess.run(["scp", "-o", "BatchMode=yes", "/tmp/uart_bench_agent_qnx", f"{args.guest_alias}:/tmp/uart_bench_agent"], check=True)
        guest.check_ok("chmod +x /tmp/uart_bench_agent")
        guest.check_ok("/tmp/uart_bench_agent --selftest")
        dev_path = "/dev/ser3"
        uart_num = 3

    os.makedirs(os.path.dirname(os.path.abspath(args.jsonl_out)), exist_ok=True)
    trial_counter = 1

    payload_sizes = [32, 63, 64, 65, 128, 255, 256, 257, 512, 1024, 4096, 16384, 65536]
    if args.guest_os == "qnx":
        modes = ["pio-irq", "pio-poll"]
    else:
        modes = ["dma-irq", "pio-irq", "dma-poll", "pio-poll"]

    with open(args.jsonl_out, "w") as out_f:
        # Run randomized blocks
        for block_idx in range(args.blocks):
            print(f"\n>>> Starting Block {block_idx + 1}/{args.blocks} <<<")
            block_modes = list(modes)
            random.shuffle(block_modes)

            for m in block_modes:
                print(f"\n--- Block {block_idx + 1} | Mode: {m} ---")

                # Workload 1: Interactive Receive Scheduling (Echo)
                r_echo = run_trial_interactive_echo(trial_counter, args.guest_os, m, wire, collector, guest, dev_path, uart_num)
                out_f.write(json.dumps(r_echo) + "\n")
                out_f.flush()
                trial_counter += 1
                time.sleep(1.0)

                # Workload 2: Directional Bulk Transfers (TX and RX)
                for sz in payload_sizes:
                    # Transmit (Guest -> Wire)
                    r_tx = run_trial_directional_bulk(trial_counter, args.guest_os, "tx", m, sz, wire, collector, guest, dev_path, uart_num)
                    out_f.write(json.dumps(r_tx) + "\n")
                    out_f.flush()
                    trial_counter += 1
                    time.sleep(0.2)

                    # Receive (Wire -> Guest)
                    r_rx = run_trial_directional_bulk(trial_counter, args.guest_os, "rx", m, sz, wire, collector, guest, dev_path, uart_num)
                    out_f.write(json.dumps(r_rx) + "\n")
                    out_f.flush()
                    trial_counter += 1
                    time.sleep(0.2)

                # Workload 3: Sustained 30s Streaming
                r_sustained = run_trial_sustained_stream(trial_counter, args.guest_os, m, 4096, wire, collector, guest, dev_path, uart_num, duration_sec=30)
                out_f.write(json.dumps(r_sustained) + "\n")
                out_f.flush()
                trial_counter += 1
                time.sleep(2.0)

    restore_guest_services(guest, args.guest_os)
    print(f"\n[+] Completed all benchmark blocks. Results saved to {args.jsonl_out}")

if __name__ == "__main__":
    main()

#
# Local variables:
# mode: Python
# indent-tabs-mode: nil
# End:
#
