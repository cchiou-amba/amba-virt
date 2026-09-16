#!/usr/bin/env python3
"""
test_concurrent_multi_hvm.py

Concurrent Multi-HVM Validation Test for Ambarella N1-655 amba-virt
Launches concurrent hardware-accelerated deep learning inference across:
  - Tenant 0: Ubuntu 24.04 HVM (n1-655-devkit-hvm, CID 7, /dev/amba_virt_shm0)
  - Tenant 1: Alpine Linux HVM (n1-655-devkit-alpine, CID 6, /dev/amba_virt_shm1)

Copyright (C) 2026, Ambarella International LLC
"""

import subprocess
import threading
import time
import sys

UBUNTU_CMD = "cd ~ && ./cavalry_multi_tenant_test --role 'Tenant-0-Ubuntu' --iters 50 vp_clk_cavalry.bin"
ALPINE_CMD = "cd ~ && ./cavalry_multi_tenant_test --role 'Tenant-1-Alpine' --iters 50 vp_clk_cavalry.bin"

SSH_BASE = [
    "ssh",
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "-o", "LogLevel=ERROR",
]

results = {}

def run_guest(guest_name, host_alias, cmd):
    print(f"[{guest_name}] Launching on {host_alias}: {cmd}")
    full_cmd = SSH_BASE + [host_alias, cmd]
    t0 = time.time()
    res = subprocess.run(full_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    t1 = time.time()
    results[guest_name] = {
        "returncode": res.returncode,
        "stdout": res.stdout,
        "stderr": res.stderr,
        "wall_time_s": t1 - t0,
    }
    print(f"[{guest_name}] Completed in {t1 - t0:.2f} seconds (exit={res.returncode})")

def main():
    print("=" * 70)
    print("  Ambarella N1-655 Concurrent Multi-HVM Silicon Validation")
    print("  Ubuntu 24.04 HVM (Tenant 0) || Alpine Linux HVM (Tenant 1)")
    print("=" * 70)

    # Check dmesg baseline on Dom0 before run
    dom0_before = subprocess.run(
        SSH_BASE + ["n1-655-devkit", "dmesg | grep -E 'ESR_EL2|Stage-2|Data Abort|fault' | tail -n 10"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    ).stdout.strip()

    t_launch = time.time()
    th_ubuntu = threading.Thread(target=run_guest, args=("Tenant-0-Ubuntu", "n1-655-devkit-hvm", UBUNTU_CMD))
    th_alpine = threading.Thread(target=run_guest, args=("Tenant-1-Alpine", "n1-655-devkit-alpine", ALPINE_CMD))

    th_ubuntu.start()
    th_alpine.start()

    th_ubuntu.join()
    th_alpine.join()
    t_end = time.time()

    total_wall = t_end - t_launch
    print("\n" + "=" * 70)
    print(f"  Concurrent Execution Finished in {total_wall:.2f} s")
    print("=" * 70 + "\n")

    all_passed = True
    for name, r in results.items():
        print(f"--- {name} Output ---")
        print(r["stdout"])
        if r["stderr"].strip():
            print(f"--- {name} Stderr ---\n{r['stderr']}")
        if r["returncode"] != 0 or "VERDICT: PASS" not in r["stdout"]:
            all_passed = False
            print(f">>> {name}: FAILED (code {r['returncode']}) <<<")
        else:
            print(f">>> {name}: PASSED (Golden MD5 Verified) <<<")
        print()

    # Check dmesg on Dom0 for any Stage-2 faults during concurrent execution
    dom0_after = subprocess.run(
        SSH_BASE + ["n1-655-devkit", "dmesg | grep -E 'ESR_EL2|Stage-2|Data Abort|fault' | tail -n 10"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    ).stdout.strip()

    print("=" * 70)
    print("  Silicon Integrity & Stage-2 Fault Audit")
    print("=" * 70)
    if dom0_after == dom0_before:
        print("[PASS] Gate 3: Zero Stage-2 Data Aborts or Translation Faults on Dom0!")
    else:
        print(f"[WARN] Dom0 reported messages:\n{dom0_after}")

    if all_passed and (dom0_after == dom0_before):
        print("\n>>> ALL MULTI-HVM CONCURRENCY GATES PASSED (100% SUCCESS) <<<")
        return 0
    else:
        print("\n>>> MULTI-HVM VALIDATION ENCOUNTERED FAILURES <<<")
        return 1

if __name__ == "__main__":
    sys.exit(main())
