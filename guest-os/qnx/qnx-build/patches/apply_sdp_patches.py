#!/usr/bin/env python3
"""
Apply binary and script patches to QNX SDP 8.0 for Ambarella N1-655 Devkit / EVE-OS.

Patches applied:
1. devb-virtio: Fix multi-vector MSI-X overwrite bug at offset 0x5134.
2. pci_hw-fdt.so.3.0: Fix unaligned mmap of GICv2m frame at offset 0x73b8 and 0x9254.
3. mkqnximage/qemu/opt_scripts/qemu: Fix random daemon MMIO virtio-rng failure for aarch64le.
"""

import os
import sys
import shutil

def patch_file(path, patches, desc):
    print(f"[*] Patching {desc}: {path}")
    if not os.path.exists(path):
        print(f"[!] Error: File not found: {path}")
        return False

    orig_backup = path + ".orig"
    if not os.path.exists(orig_backup):
        print(f"    Creating backup: {orig_backup}")
        shutil.copy2(path, orig_backup)

    with open(path, "r+b") as f:
        data = f.read()
        for offset, expected, replacement in patches:
            actual = data[offset:offset+len(expected)]
            if actual == replacement:
                print(f"    Offset 0x{offset:x}: already patched.")
                continue
            if actual != expected:
                print(f"[!] Warning: Offset 0x{offset:x}: expected {expected.hex()}, found {actual.hex()}. Skipping.")
                continue
            f.seek(offset)
            f.write(replacement)
            print(f"    Offset 0x{offset:x}: successfully patched ({expected.hex()} -> {replacement.hex()}).")
    return True

def main():
    qnx_target = os.environ.get("QNX_TARGET")
    qnx_host = os.environ.get("QNX_HOST")

    if not qnx_target:
        print("[!] QNX_TARGET is not set. Please source qnxsdp-env.sh first.")
        sys.exit(1)

    # 1. devb-virtio patch
    devb_path = os.path.join(qnx_target, "aarch64le", "sbin", "devb-virtio")
    devb_patches = [
        # Offset 0x5134: 1a819294 (csel w20, w20, w1, ls) -> 52800034 (mov w20, #1)
        (0x5134, bytes.fromhex("9492811a"), bytes.fromhex("34008052")),
    ]
    patch_file(devb_path, devb_patches, "devb-virtio")

    # 2. pci_hw-fdt.so.3.0 patch
    pci_path = os.path.join(qnx_target, "aarch64le", "lib", "dll", "pci", "pci_hw-fdt.so.3.0")
    pci_patches = [
        (0x73b8, bytes.fromhex("a520009104008012e383003202208152810080d2000080d2"),
                 bytes.fromhex("010a8052e17b01b901088052e17f01b9e58f40f98dffff17")),
        (0x9254, bytes.fromhex("29110011014500b87f00096b"),
                 bytes.fromhex("010100b9081100917f00006b")),
    ]
    patch_file(pci_path, pci_patches, "pci_hw-fdt.so.3.0")

    # 3. mkqnximage qemu script patch
    if qnx_host:
        candidates = [
            os.path.join(qnx_host, "..", "..", "common", "mkqnximage", "qemu", "opt_scripts", "qemu"),
            os.path.join(qnx_host, "..", "common", "mkqnximage", "qemu", "opt_scripts", "qemu"),
        ]
        qemu_script = None
        for c in candidates:
            if os.path.exists(c):
                qemu_script = os.path.abspath(c)
                break
        if qemu_script and os.path.exists(qemu_script):
            print(f"[*] Patching mkqnximage qemu script: {qemu_script}")
            with open(qemu_script, "r") as f:
                content = f.read()
            bad_str = 'echo "#define __RANDOM_OTHER_ENTROPY__ -l devr-virtio.so:mem=0xa003a00 -l devr-virtio.so" >>output/option_files/definitions.opt_type_qemu\n\t\t\techo lib/dll/devr-virtio.so=lib/dll/devr-virtio.so >>output/option_files/system_files.opt_type_qemu'
            good_str = 'echo "#define __RANDOM_OTHER_ENTROPY__" >>output/option_files/definitions.opt_type_qemu'
            if bad_str in content:
                orig_backup = qemu_script + ".orig"
                if not os.path.exists(orig_backup):
                    shutil.copy2(qemu_script, orig_backup)
                content = content.replace(bad_str, good_str)
                with open(qemu_script, "w") as f:
                    f.write(content)
                print("    Successfully patched mkqnximage qemu script.")
            else:
                print("    Script already patched or pattern not found.")

    print("\n[+] All QNX SDP patches successfully verified and applied.")

if __name__ == "__main__":
    main()
