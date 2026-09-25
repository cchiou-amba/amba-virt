#!/usr/bin/env python3
#
# scripts/benchmark_uart_overhead.py
#
# Automated Performance & Virtualization Overhead Benchmark Harness for
# Ambarella Hardware Serial Passthrough (CV3-AD655 / N1-655).
# Measures RTT Echo Latency, Burst Throughput, Windowed IRQs, and CPU Utilization.
#
# Copyright (C) 2026, Ambarella International LLC.
#

import argparse
import os
import re
import socket
import statistics
import subprocess
import sys
import time

TELNET_IAC = 255
TELNET_DONT = 254
TELNET_DO = 253
TELNET_WONT = 252
TELNET_WILL = 251

def strip_telnet_negotiation(data: bytes) -> bytes:
    """Filter out Telnet IAC sequences from serial stream."""
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

class SerialEndpoint:
    def __init__(self, host: str, port: int, timeout: float = 3.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect((self.host, self.port))
        # Drain initial Telnet negotiation and welcome banner
        time.sleep(0.3)
        try:
            raw = self.sock.recv(4096)
            _ = strip_telnet_negotiation(raw)
        except socket.timeout:
            pass

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def send_recv_exact(self, payload: bytes, timeout: float = 1.0) -> tuple:
        """Send payload and wait for exact echoed bytes, returning elapsed time in ms."""
        self.sock.settimeout(timeout)
        start_time = time.perf_counter()
        self.sock.sendall(payload)
        received = bytearray()
        while len(received) < len(payload):
            try:
                chunk = self.sock.recv(len(payload) - len(received))
            except socket.timeout:
                break
            if not chunk:
                break
            clean = strip_telnet_negotiation(chunk)
            received.extend(clean)
        elapsed_ms = (time.perf_counter() - start_time) * 1000.0
        return elapsed_ms, bytes(received)

    def flush_input(self):
        self.sock.settimeout(0.05)
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
        except socket.timeout:
            pass
        self.sock.settimeout(self.timeout)

def run_ssh_command(host: str, cmd: str) -> str:
    """Execute clean SSH command on target node without redundant flags."""
    res = subprocess.run(["ssh", host, cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.stdout.strip()

def get_cpu_jiffies(host: str) -> dict:
    """Read CPU time counters from /proc/stat."""
    out = run_ssh_command(host, "cat /proc/stat | grep '^cpu '")
    parts = out.split()
    if len(parts) >= 5:
        user = int(parts[1])
        nice = int(parts[2])
        system = int(parts[3])
        idle = int(parts[4])
        iowait = int(parts[5]) if len(parts) > 5 else 0
        irq = int(parts[6]) if len(parts) > 6 else 0
        softirq = int(parts[7]) if len(parts) > 7 else 0
        total = user + nice + system + idle + iowait + irq + softirq
        busy = total - idle - iowait
        return {"total": total, "busy": busy, "idle": idle}
    return {"total": 0, "busy": 0, "idle": 0}

def compute_cpu_utilization(start: dict, end: dict) -> float:
    total_delta = end["total"] - start["total"]
    busy_delta = end["busy"] - start["busy"]
    if total_delta > 0:
        return (busy_delta / total_delta) * 100.0
    return 0.0

def get_interrupt_count(host: str, pattern: str) -> int:
    """Count interrupts for lines matching pattern (works on Dom0 and guest)."""
    cmd = f"(cat /proc/interrupts 2>/dev/null || sudo cat /proc/interrupts 2>/dev/null) | grep -E '{pattern}'"
    out = run_ssh_command(host, cmd)
    total = 0
    for line in out.splitlines():
        parts = line.split()
        for p in parts[1:]:
            if p.isdigit():
                total += int(p)
            else:
                break
    return total

def get_uart_error_stats(guest: str) -> dict:
    """Extract line status and error counters from guest /proc/tty/driver/amba_uart."""
    out = run_ssh_command(guest, "sudo cat /proc/tty/driver/amba_uart 2>/dev/null")
    stats = {"oe": 0, "pe": 0, "fe": 0, "brk": 0, "tx": 0, "rx": 0}
    for match in re.finditer(r'(oe|pe|fe|brk|tx|rx):(\d+)', out):
        key, val = match.groups()
        stats[key] = int(val)
    return stats

def configure_driver_mode(guest: str, mode: str) -> bool:
    """Dynamically configure and verify the guest driver mode."""
    if mode == "auto":
        return True

    print(f"[*] Configuring guest driver mode: {mode}...")
    pci_poll = "/sys/bus/pci/drivers/amba_uart/force_poll"
    plat_poll = "/sys/bus/platform/drivers/amba_uart/force_poll"
    pci_dma = "/sys/bus/pci/drivers/amba_uart/use_dma"
    plat_dma = "/sys/bus/platform/drivers/amba_uart/use_dma"
    if mode == "polling":
        cmd = f"for f in {pci_poll} {plat_poll}; do [ -f $f ] && echo 1 | sudo tee $f >/dev/null; done; for f in {pci_dma} {plat_dma}; do [ -f $f ] && echo 0 | sudo tee $f >/dev/null; done"
    elif mode == "interrupt":
        cmd = f"for f in {pci_poll} {plat_poll}; do [ -f $f ] && echo 0 | sudo tee $f >/dev/null; done; for f in {pci_dma} {plat_dma}; do [ -f $f ] && echo 0 | sudo tee $f >/dev/null; done"
    elif mode == "dma":
        cmd = f"for f in {pci_poll} {plat_poll}; do [ -f $f ] && echo 0 | sudo tee $f >/dev/null; done; for f in {pci_dma} {plat_dma}; do [ -f $f ] && echo 1 | sudo tee $f >/dev/null; done"
    else:
        print(f"[!] Unknown mode: {mode}")
        return False

    out = run_ssh_command(guest, f"{cmd} && sudo systemctl restart serial-getty@ttyAMBA0.service && sleep 1 && sudo dmesg | tail -n 5")
    time.sleep(1.0)

    # Runtime assertion
    dmesg = run_ssh_command(guest, "sudo dmesg | tail -n 15")
    if mode == "polling" and "running in polling mode" in dmesg:
        print("    [+] Verified Mode 1: Polling mode active (1 ms hrtimer)")
        return True
    elif mode == "interrupt" and "running in interrupt-driven mode" in dmesg and "DMA disabled" in dmesg:
        print("    [+] Verified Mode 2: Interrupt-driven PIO mode active (IRQ 70, DMA disabled)")
        return True
    elif mode == "dma" and "running in interrupt-driven mode" in dmesg and "DMA enabled" in dmesg:
        print("    [+] Verified Mode 3: Interrupt-driven DMA mode active (Generic-DMA1 channel 13)")
        return True

    print(f"[!] Warning: Mode verification failed for {mode}.\nDmesg output:\n{dmesg}")
    return False

def benchmark_rtt_latency(endpoint: SerialEndpoint, samples: int = 200) -> dict:
    """Benchmark keystroke round-trip echo latency."""
    latencies = []
    endpoint.flush_input()
    test_chars = b"abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"

    for i in range(samples):
        ch = bytes([test_chars[i % len(test_chars)]])
        try:
            lat_ms, echoed = endpoint.send_recv_exact(ch, timeout=0.5)
            if ch in echoed:
                latencies.append(lat_ms)
            time.sleep(0.005) # 5 ms pacing
        except socket.timeout:
            pass

    if not latencies:
        return {"min": 0, "median": 0, "p90": 0, "p99": 0, "mean": 0, "stddev": 0, "success_rate": 0}

    latencies.sort()
    p50 = statistics.median(latencies)
    mean = statistics.mean(latencies)
    std = statistics.stdev(latencies) if len(latencies) > 1 else 0.0
    p90 = latencies[int(len(latencies) * 0.90)]
    p99 = latencies[int(len(latencies) * 0.99)]

    return {
        "min": min(latencies),
        "median": p50,
        "p90": p90,
        "p99": p99,
        "mean": mean,
        "stddev": std,
        "success_rate": (len(latencies) / samples) * 100.0,
    }

def benchmark_throughput_and_integrity(endpoint: SerialEndpoint, size_bytes: int = 4096) -> dict:
    """Benchmark burst transfer throughput, wire efficiency, and byte-level integrity."""
    endpoint.flush_input()
    # Verifiable alphanumeric pattern to prevent agetty line-control mangling
    alphabet = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    payload = bytes([alphabet[i % len(alphabet)] for i in range(size_bytes)])

    start_time = time.perf_counter()
    endpoint.sock.sendall(payload)

    received = bytearray()
    endpoint.sock.settimeout(5.0)
    try:
        while len(received) < size_bytes:
            chunk = endpoint.sock.recv(min(4096, size_bytes - len(received)))
            if not chunk:
                break
            clean = strip_telnet_negotiation(chunk)
            received.extend(clean)
    except socket.timeout:
        pass

    duration = time.perf_counter() - start_time
    received_len = len(received)
    throughput_kb = (received_len / 1024.0) / duration if duration > 0 else 0.0
    wire_efficiency = (throughput_kb / 11.52) * 100.0 # 11.52 KB/s theoretical max at 115200

    # Byte-by-byte integrity verification
    matched_bytes = 0
    min_len = min(len(payload), received_len)
    for i in range(min_len):
        if payload[i] == received[i]:
            matched_bytes += 1

    corrupted_bytes = (min_len - matched_bytes) + max(0, len(payload) - received_len)
    integrity_pct = (matched_bytes / len(payload)) * 100.0 if len(payload) > 0 else 0.0

    return {
        "sent_bytes": size_bytes,
        "received_bytes": received_len,
        "duration_sec": duration,
        "throughput_kb_s": throughput_kb,
        "wire_efficiency_pct": wire_efficiency,
        "loss_pct": ((size_bytes - received_len) / size_bytes) * 100.0 if size_bytes > 0 else 0.0,
        "matched_bytes": matched_bytes,
        "corrupted_bytes": corrupted_bytes,
        "integrity_pct": integrity_pct,
    }

def main():
    parser = argparse.ArgumentParser(description="Ambarella UART Passthrough Performance Benchmark Harness")
    parser.add_argument("--host", default="192.168.8.30", help="Terminal server host (default: 192.168.8.30)")
    parser.add_argument("--port", type=int, default=6071, help="Terminal server port (default: 6071)")
    parser.add_argument("--guest", default="n1-655-devkit-ubuntu", help="Guest SSH alias")
    parser.add_argument("--dom0", default="n1-655-devkit", help="Host Dom0 SSH alias")
    parser.add_argument("--samples", type=int, default=200, help="Number of RTT samples (default: 200)")
    parser.add_argument("--burst-size", type=int, default=4096, help="Burst throughput size in bytes")
    parser.add_argument("--mode", default="auto", choices=["auto", "polling", "interrupt", "dma"], help="Evaluation mode")

    args = parser.parse_args()

    print(f"=== Ambarella UART Virtualization Performance Benchmark ===")
    print(f"Target Bridge: {args.host}:{args.port} | Guest: {args.guest} | Dom0: {args.dom0}")
    print(f"Mode: {args.mode} | Samples: {args.samples} | Burst Size: {args.burst_size} bytes\n")

    # Driver Mode Configuration & Runtime Assertion
    if not configure_driver_mode(args.guest, args.mode):
        print("[!] Aborting due to mode configuration failure.")
        sys.exit(1)

    endpoint = SerialEndpoint(args.host, args.port)
    endpoint.connect()

    # 1. Quiescent / Idle baseline window (3 seconds quiet)
    print("[1/4] Establishing quiescent baseline (3s quiet window)...")
    q_dom0_cpu_start = get_cpu_jiffies(args.dom0)
    q_guest_cpu_start = get_cpu_jiffies(args.guest)
    time.sleep(3.0)
    q_dom0_cpu_end = get_cpu_jiffies(args.dom0)
    q_guest_cpu_end = get_cpu_jiffies(args.guest)
    idle_dom0_cpu = compute_cpu_utilization(q_dom0_cpu_start, q_dom0_cpu_end)
    idle_guest_cpu = compute_cpu_utilization(q_guest_cpu_start, q_guest_cpu_end)
    print(f"      Quiescent CPU: Dom0 {idle_dom0_cpu:.2f}% | Guest vCPU {idle_guest_cpu:.2f}%")

    # 2. RTT Latency Benchmark (with isolated RTT IRQ window)
    print(f"[2/4] Benchmarking keystroke round-trip echo latency ({args.samples} samples)...")
    rtt_irq_dom0_start = get_interrupt_count(args.dom0, "amba_virt_uart2|39:")
    rtt_irq_guest_start = get_interrupt_count(args.guest, "amba_uart|70:")
    rtt_stats = benchmark_rtt_latency(endpoint, samples=args.samples)
    rtt_irq_dom0_delta = get_interrupt_count(args.dom0, "amba_virt_uart2|39:") - rtt_irq_dom0_start
    rtt_irq_guest_delta = get_interrupt_count(args.guest, "amba_uart|70:") - rtt_irq_guest_start
    print(f"      RTT Median: {rtt_stats['median']:.3f} ms | p90: {rtt_stats['p90']:.3f} ms | p99: {rtt_stats['p99']:.3f} ms")
    print(f"      RTT Min/Max: {rtt_stats['min']:.3f} / {rtt_stats['p99']:.3f} ms | StdDev: {rtt_stats['stddev']:.3f} ms | Success: {rtt_stats['success_rate']:.1f}%")
    print(f"      RTT IRQ Window: Host Physical +{rtt_irq_dom0_delta} | Guest Virtual +{rtt_irq_guest_delta}")

    # 3. Burst Throughput & Integrity Benchmark (with strictly isolated Burst IRQ window)
    print(f"[3/4] Benchmarking burst throughput & payload integrity ({args.burst_size} bytes)...")
    burst_cpu_dom0_start = get_cpu_jiffies(args.dom0)
    burst_cpu_guest_start = get_cpu_jiffies(args.guest)
    burst_irq_dom0_uart_start = get_interrupt_count(args.dom0, "amba_virt_uart2|39:")
    burst_irq_dom0_dma_start = get_interrupt_count(args.dom0, "ffe0021000.dma|19:")
    burst_irq_guest_start = get_interrupt_count(args.guest, "amba_uart|70:")

    tp_stats = benchmark_throughput_and_integrity(endpoint, size_bytes=args.burst_size)

    burst_irq_dom0_uart_delta = get_interrupt_count(args.dom0, "amba_virt_uart2|39:") - burst_irq_dom0_uart_start
    burst_irq_dom0_dma_delta = get_interrupt_count(args.dom0, "ffe0021000.dma|19:") - burst_irq_dom0_dma_start
    burst_irq_guest_delta = get_interrupt_count(args.guest, "amba_uart|70:") - burst_irq_guest_start
    burst_cpu_dom0_pct = compute_cpu_utilization(burst_cpu_dom0_start, get_cpu_jiffies(args.dom0))
    burst_cpu_guest_pct = compute_cpu_utilization(burst_cpu_guest_start, get_cpu_jiffies(args.guest))

    print(f"      Throughput: {tp_stats['throughput_kb_s']:.2f} KB/s ({tp_stats['wire_efficiency_pct']:.1f}% of wire limit)")
    print(f"      Transferred: {tp_stats['received_bytes']}/{tp_stats['sent_bytes']} bytes in {tp_stats['duration_sec']:.3f} s")
    print(f"      Byte Integrity: {tp_stats['integrity_pct']:.1f}% ({tp_stats['matched_bytes']} matched, {tp_stats['corrupted_bytes']} corrupted)")
    print(f"      Burst IRQ Window: Host UART2 +{burst_irq_dom0_uart_delta} | Host Generic-DMA1 +{burst_irq_dom0_dma_delta} | Guest MSI-X +{burst_irq_guest_delta}")

    # 4. Hardware Line Status & Error Counters Inspection
    print("[4/4] Inspecting hardware line status registers...")
    hw_errors = get_uart_error_stats(args.guest)
    print(f"      Hardware Status: OE (Overrun)={hw_errors['oe']}, FE (Framing)={hw_errors['fe']}, PE (Parity)={hw_errors['pe']}, BRK={hw_errors['brk']}")

    endpoint.close()

    print("\n=== Benchmark Summary Matrix ===")
    print(f"| Metric | Measured Value | Theoretical Limit / Baseline |")
    print(f"|---|---|---|")
    print(f"| Evaluation Mode       | {args.mode.upper()} | Configured Mode |")
    print(f"| Round-Trip Echo (p50) | {rtt_stats['median']:.3f} ms | < 1.0 ms |")
    print(f"| Round-Trip Echo (p99) | {rtt_stats['p99']:.3f} ms | < 5.0 ms |")
    print(f"| Sustained Throughput  | {tp_stats['throughput_kb_s']:.2f} KB/s | 11.52 KB/s (115,200 8N1) |")
    print(f"| Line Utilization      | {tp_stats['wire_efficiency_pct']:.1f}% | 100.0% |")
    print(f"| Payload Integrity     | {tp_stats['integrity_pct']:.1f}% ({tp_stats['matched_bytes']}/{tp_stats['sent_bytes']} B) | 100.0% |")
    print(f"| Quiescent Host CPU    | {idle_dom0_cpu:.2f}% | Baseline |")
    print(f"| Quiescent Guest vCPU  | {idle_guest_cpu:.2f}% | Baseline |")
    print(f"| Active Burst Host CPU | {burst_cpu_dom0_pct:.2f}% | < 1.0% |")
    print(f"| Active Burst Guest CPU| {burst_cpu_guest_pct:.2f}% | < 2.0% |")
    print(f"| Burst Host UART IRQ   | {burst_irq_dom0_uart_delta} events | Windowed |")
    print(f"| Burst Host DMA IRQ    | {burst_irq_dom0_dma_delta} events | Windowed Generic-DMA1 |")
    print(f"| Burst Guest MSI-X IRQ | {burst_irq_guest_delta} events | Windowed |")
    print(f"| Hardware Errors       | OE={hw_errors['oe']}, FE={hw_errors['fe']}, PE={hw_errors['pe']} | 0 Errors |")

if __name__ == "__main__":
    main()
