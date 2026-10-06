#!/usr/bin/env python3
"""QEMU AArch64 TCG Runner for Lean Ubuntu 24.04 HVM Bake & Verification.

Contract:
- Full system emulation via qemu-system-aarch64 (-machine virt,accel=tcg -cpu max)
- 2 GiB temporary build RAM
- Private copy of UEFI vars and read-only UEFI code
- VirtIO block and network devices
- Serial logging and execution timeout enforcement
- Headless verification of serial console and SSH login
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

try:
    import pexpect
except ImportError:
    pexpect = None


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def resolve_uefi_files(uefi_code_hint: str = "", uefi_vars_hint: str = ""):
    code_path = uefi_code_hint
    vars_path = uefi_vars_hint

    if not code_path or not os.path.exists(code_path):
        candidates = [
            "/usr/share/AAVMF/AAVMF_CODE.fd",
            "/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd",
            "/usr/share/qemu-efi-aarch64/QEMU_EFI.fd",
        ]
        for c in candidates:
            if os.path.exists(c):
                code_path = c
                break

    if not vars_path or not os.path.exists(vars_path):
        candidates = [
            "/usr/share/AAVMF/AAVMF_VARS.fd",
            "/usr/share/AAVMF/AAVMF_VARS.ms.fd",
            "/usr/share/AAVMF/AAVMF_VARS.snakeoil.fd",
        ]
        for c in candidates:
            if os.path.exists(c):
                vars_path = c
                break

    if not code_path or not os.path.exists(code_path):
        raise RuntimeError("Unable to locate AArch64 UEFI CODE firmware (qemu-efi-aarch64)")
    if not vars_path or not os.path.exists(vars_path):
        raise RuntimeError("Unable to locate AArch64 UEFI VARS template (qemu-efi-aarch64)")

    return os.path.abspath(code_path), os.path.abspath(vars_path)


def run_bake(disk: str, cidata: str, uefi_code: str, uefi_vars: str, serial_log: str, timeout: int, work_dir: str):
    print("=== [QEMU-RUNNER] Starting NoCloud Bake Phase ===")
    os.makedirs(work_dir, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(serial_log)), exist_ok=True)

    # Create private writable UEFI VARS copy
    private_vars = os.path.join(work_dir, "bake_vars.fd")
    shutil.copyfile(uefi_vars, private_vars)
    os.chmod(private_vars, 0o600)

    ssh_port = find_free_port()
    print(f"Allocated temporary SSH host port: 127.0.0.1:{ssh_port}")

    qemu_cmd = [
        "qemu-system-aarch64",
        "-machine", "virt",
        "-accel", "tcg,thread=multi",
        "-cpu", "max",
        "-smp", "4",
        "-m", "2048",
        "-nographic",
        "-drive", f"if=pflash,format=raw,readonly=on,file={uefi_code}",
        "-drive", f"if=pflash,format=raw,file={private_vars}",
        "-drive", f"if=none,id=hd0,format=qcow2,file={disk}",
        "-device", "virtio-blk-pci,drive=hd0",
        "-drive", f"if=none,id=cd0,format=raw,readonly=on,file={cidata}",
        "-device", "virtio-blk-pci,drive=cd0",
        "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{ssh_port}-:22",
        "-device", "virtio-net-pci,netdev=net0",
    ]

    print(f"QEMU Bake Command: {' '.join(qemu_cmd)}")
    log_file = open(serial_log, "w", buffering=1)

    start_time = time.time()
    marker_found = False

    proc = subprocess.Popen(
        qemu_cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    try:
        while True:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.1)
                continue

            log_file.write(line)
            # Print selective progress indicators and failure notices to stdout
            if (
                "[AMBA-PROVISION]" in line
                or "AMBA_IMAGE_PROVISION_COMPLETE" in line
                or "cloud-init" in line
                or "[FAILED]" in line
                or "Failed to start" in line
                or "AMBA_IMAGE_PROVISION_FAILED" in line
            ):
                sys.stdout.write(f"  [QEMU] {line}")
                sys.stdout.flush()

            if "AMBA_IMAGE_PROVISION_FAILED" in line or "[FAILED] Failed to start amba-image-provision" in line:
                print(f"=== [QEMU-RUNNER] FATAL: In-VM provisioning failure detected: {line.strip()} ===", file=sys.stderr)
                proc.kill()
                proc.wait()
                raise RuntimeError(f"In-VM provisioning failed immediately: {line.strip()}")

            if "AMBA_IMAGE_PROVISION_COMPLETE" in line:
                print("=== [QEMU-RUNNER] Detected AMBA_IMAGE_PROVISION_COMPLETE marker! ===")
                marker_found = True

            elapsed = time.time() - start_time
            if elapsed > timeout:
                raise TimeoutError(f"QEMU bake timed out after {timeout} seconds")

        proc.wait(timeout=120)
    except Exception as e:
        print(f"Error during QEMU bake: {e}", file=sys.stderr)
        proc.kill()
        proc.wait()
        raise
    finally:
        log_file.close()

    if not marker_found:
        raise RuntimeError("QEMU bake completed without emitting AMBA_IMAGE_PROVISION_COMPLETE")

    if proc.returncode != 0:
        raise RuntimeError(f"QEMU bake process exited with non-zero code {proc.returncode}")

    print("=== [QEMU-RUNNER] NoCloud Bake Phase Completed Successfully ===")
    return 0


def run_verify(disk: str, uefi_code: str, uefi_vars: str, serial_log: str, timeout: int, work_dir: str):
    print("=== [QEMU-RUNNER] Starting Seedless Verification Phase ===")
    os.makedirs(work_dir, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(serial_log)), exist_ok=True)

    private_vars = os.path.join(work_dir, "verify_vars.fd")
    shutil.copyfile(uefi_vars, private_vars)
    os.chmod(private_vars, 0o600)

    ssh_port = find_free_port()
    print(f"Allocated verification SSH host port: 127.0.0.1:{ssh_port}")

    qemu_cmd = [
        "qemu-system-aarch64",
        "-machine", "virt",
        "-accel", "tcg,thread=multi",
        "-cpu", "max",
        "-smp", "4",
        "-m", "2048",
        "-nographic",
        "-drive", f"if=pflash,format=raw,readonly=on,file={uefi_code}",
        "-drive", f"if=pflash,format=raw,file={private_vars}",
        "-drive", f"if=none,id=hd0,format=qcow2,file={disk}",
        "-device", "virtio-blk-pci,drive=hd0",
        "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{ssh_port}-:22",
        "-device", "virtio-net-pci,netdev=net0",
    ]

    print(f"QEMU Verify Command: {' '.join(qemu_cmd)}")
    log_file = open(serial_log, "w", buffering=1)

    # Use pexpect to manage serial console interaction
    child = pexpect.spawn(
        qemu_cmd[0],
        qemu_cmd[1:],
        encoding="utf-8",
        codec_errors="replace",
        timeout=timeout,
        logfile=log_file,
    )

    start_time = time.time()
    console_verified = False

    try:
        # 1. Verify serial console login
        print("Waiting for serial login prompt...")
        index = child.expect(["login:", pexpect.TIMEOUT, pexpect.EOF], timeout=timeout)
        if index != 0:
            raise RuntimeError(f"Serial login prompt not reached within timeout ({timeout}s)")

        print("Serial login prompt reached. Submitting username 'ubuntu'...")
        child.sendline("ubuntu")
        child.expect("Password:")
        print("Password prompt reached. Submitting password 'ubuntu'...")
        child.sendline("ubuntu")
        child.expect(r"\$ ")
        print("Serial console shell prompt reached! Executing 'id'...")
        child.sendline("id")
        child.expect(r"uid=\d+\(ubuntu\)")
        print("Serial console login verified successfully!")
        console_verified = True

        # 2. Verify password SSH login and assertions
        print(f"Verifying SSH login on 127.0.0.1:{ssh_port}...")
        ssh_cmd_base = [
            "sshpass", "-p", "ubuntu",
            "ssh",
            "-o", "ConnectTimeout=5",
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-o", "LogLevel=ERROR",
            "-p", str(ssh_port),
            "ubuntu@127.0.0.1",
        ]

        # Wait for SSH service to accept connection
        ssh_ready = False
        ssh_deadline = start_time + timeout
        while time.time() < ssh_deadline:
            res = subprocess.run(ssh_cmd_base + ["echo 'SSH_ALIVE'"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if res.returncode == 0 and "SSH_ALIVE" in res.stdout:
                ssh_ready = True
                print("SSH login verified successfully!")
                break
            time.sleep(5)

        if not ssh_ready:
            raise RuntimeError("SSH failed to accept login within timeout")

        # Run verification assertions inside guest
        print("=== Running package and filesystem assertions inside guest ===")
        assertion_script = """
set -euo pipefail

echo "Checking apt..."
sudo apt-get check

echo "Checking required retained packages..."
for pkg in apt base-files bash ca-certificates cloud-guest-utils curl dbus e2fsprogs grub-efi-arm64 iproute2 kmod netplan.io openssh-server pciutils python3 sudo systemd util-linux; do
    if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "ok installed"; then
        echo "FAIL: Required package $pkg is missing!" >&2
        exit 1
    fi
done

echo "Checking required removed packages are absent..."
for pkg in cloud-init rsyslog snapd unattended-upgrades fwupd; do
    if dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "ok installed"; then
        echo "FAIL: Package $pkg is still installed!" >&2
        exit 1
    fi
done

echo "Checking EFI fallback bootloader..."
if ! sudo find /boot/efi -iname "bootaa64.efi" 2>/dev/null | grep -qi "bootaa64.efi"; then
    echo "FAIL: BOOTAA64.EFI fallback bootloader is missing!" >&2
    exit 1
fi

echo "Cleaning host keys and machine identity for production delivery..."
sudo rm -f /etc/ssh/ssh_host_*_key*
sudo truncate -s 0 /etc/machine-id
sudo rm -f /var/lib/dbus/machine-id
sync

echo "Initiating clean guest poweroff..."
sudo systemctl poweroff || sudo poweroff -f
"""
        res = subprocess.run(ssh_cmd_base + [assertion_script], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        print(res.stdout)
        if res.returncode != 0:
            print(res.stderr, file=sys.stderr)
            raise RuntimeError(f"Guest assertions failed with exit code {res.returncode}")

        # Wait for QEMU child to finish poweroff
        child.expect(pexpect.EOF, timeout=120)
        child.close()
        print("=== [QEMU-RUNNER] Verification Phase Completed Successfully ===")

    except Exception as e:
        print(f"Error during verification: {e}", file=sys.stderr)
        if child.isalive():
            child.close(force=True)
        raise
    finally:
        log_file.close()

    return 0


def main():
    parser = argparse.ArgumentParser(description="QEMU AArch64 TCG Runner for Ubuntu HVM")
    parser.add_argument("--mode", choices=["bake", "verify"], required=True, help="Execution mode")
    parser.add_argument("--disk", required=True, help="Path to working QCOW2 image")
    parser.add_argument("--cidata", default="", help="Path to CIDATA ISO (required for bake)")
    parser.add_argument("--uefi-code", default="", help="Path to UEFI code image")
    parser.add_argument("--uefi-vars", default="", help="Path to UEFI vars image")
    parser.add_argument("--serial-log", required=True, help="Path to serial log output")
    parser.add_argument("--timeout", type=int, default=2700, help="Timeout in seconds")
    parser.add_argument("--work-dir", default="/tmp/qemu_runner", help="Scratch work directory")

    args = parser.parse_args()

    uefi_code, uefi_vars = resolve_uefi_files(args.uefi_code, args.uefi_vars)
    print(f"Resolved UEFI Code: {uefi_code}")
    print(f"Resolved UEFI Vars: {uefi_vars}")

    if args.mode == "bake":
        if not args.cidata or not os.path.exists(args.cidata):
            print("Error: --cidata is required and must exist in bake mode", file=sys.stderr)
            sys.exit(1)
        sys.exit(run_bake(args.disk, args.cidata, uefi_code, uefi_vars, args.serial_log, args.timeout, args.work_dir))
    elif args.mode == "verify":
        sys.exit(run_verify(args.disk, uefi_code, uefi_vars, args.serial_log, args.timeout, args.work_dir))


if __name__ == "__main__":
    main()
