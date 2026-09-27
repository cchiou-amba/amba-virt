#!/usr/bin/env python3
#
# tools/bringup/scripts/test_uart_agent_local.py
#
# Local loopback verification for uart_bench_agent across all payload sizes.
#
# Copyright (C) 2026, Ambarella International LLC
#

import hashlib
import json
import os
import pty
import struct
import subprocess
import sys
import time

BENCH_MAGIC = 0x55415254

def crc32_ieee(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)))
    return ~crc & 0xFFFFFFFF

def generate_prbs_payload(seed: int, length: int) -> bytes:
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

def test_payload(size: int, agent_bin: str) -> bool:
    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)

    trial_id = 42
    seq = 7
    payload = generate_prbs_payload(trial_id + seq, size)
    payload_crc = crc32_ieee(payload)
    payload_sha256 = hashlib.sha256(payload).hexdigest()

    hdr = struct.pack("<IIII", BENCH_MAGIC, trial_id, seq, size)
    crc_bytes = struct.pack("<I", payload_crc)
    full_frame = hdr + payload + crc_bytes

    # 1. Test RX on agent (master writes to slave, agent reads from slave)
    rx_proc = subprocess.Popen(
        [agent_bin, "--mode", "rx", "--device", slave_name, "--trial-id", str(trial_id), "--seq", str(seq), "--size", str(size), "--timeout", "5.0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    time.sleep(0.1)

    # Write full_frame to master_fd in small chunks to simulate partial wire arrival
    chunk_sz = 64
    for offset in range(0, len(full_frame), chunk_sz):
        os.write(master_fd, full_frame[offset:offset + chunk_sz])
        time.sleep(0.001)

    stdout, stderr = rx_proc.communicate(timeout=5.0)
    if rx_proc.returncode != 0:
        print(f"[-] RX failed for size {size}: returncode={rx_proc.returncode}\nStderr: {stderr}")
        os.close(master_fd)
        os.close(slave_fd)
        return False

    res = json.loads(stdout)
    if not res.get("crc_match") or res.get("sha256") != payload_sha256:
        print(f"[-] RX integrity mismatch for size {size}: {res}")
        os.close(master_fd)
        os.close(slave_fd)
        return False

    # 2. Test TX on agent (agent writes to slave, master reads from master)
    tx_proc = subprocess.Popen(
        [agent_bin, "--mode", "tx", "--device", slave_name, "--trial-id", str(trial_id), "--seq", str(seq), "--size", str(size)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    received_bytes = bytearray()
    start_t = time.perf_counter()
    while len(received_bytes) < len(full_frame) and time.perf_counter() - start_t < 5.0:
        try:
            chunk = os.read(master_fd, 4096)
            if chunk:
                received_bytes.extend(chunk)
        except Exception:
            pass

    tx_stdout, tx_stderr = tx_proc.communicate(timeout=5.0)
    os.close(master_fd)
    os.close(slave_fd)

    if tx_proc.returncode != 0:
        print(f"[-] TX failed for size {size}: returncode={tx_proc.returncode}\nStderr: {tx_stderr}")
        return False

    tx_res = json.loads(tx_stdout)
    rx_payload = bytes(received_bytes[16:-4])
    if hashlib.sha256(rx_payload).hexdigest() != payload_sha256:
        print(f"[-] TX hash mismatch for size {size}")
        return False

    return True

def main():
    agent_bin = "/tmp/uart_bench_agent"
    subprocess.run(["gcc", "-O2", "-Wall", "-Werror", "-o", agent_bin, "tools/bringup/uart_bench_agent.c"], check=True)
    sizes = [32, 63, 64, 65, 128, 255, 256, 257, 512, 1024, 4096, 16384, 65536]

    print(f"[*] Testing local loopback across {len(sizes)} payload sizes...")
    for sz in sizes:
        ok = test_payload(sz, agent_bin)
        if not ok:
            print(f"[!] FAILED on payload size {sz}")
            sys.exit(1)
        print(f"  [+] Size {sz:5d} B: PASS (CRC32 and SHA-256 match)")

    print("[+] All loopback self-tests PASSED successfully!")

if __name__ == "__main__":
    main()
