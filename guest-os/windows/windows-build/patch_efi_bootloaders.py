#!/usr/bin/env python3
"""
patch_efi_bootloaders.py - Patch Windows 11 ARM64 bootloaders for EVE-OS KVM/OVMF

On ARM64 UEFI OVMF, the firmware volume / NOR flash is registered as Handle 0,
causing winload.efi to construct ArcBootDeviceName as rdisk(1). However, in NT
kernel mode, only the single physical VirtIO block device exists (rdisk(0)).
This mismatch triggers BugCheck 0x7B (INACCESSIBLE_BOOT_DEVICE).

This script patches:
1. bootmgfw.efi: ImgpValidateImageHash -> returns STATUS_SUCCESS (0)
2. winload.efi:
   - ImgpValidateImageHash -> returns STATUS_SUCCESS (0)
   - rdisk(%d) argument at 0x18602c -> mov w3, #0 (forces rdisk(0))
3. Recalculates official PE checksums.
"""

import sys, os, struct

def patch_file(path, patches):
    if not path or path == "/dev/null" or not os.path.exists(path):
        return
    with open(path, 'rb') as f:
        data = bytearray(f.read())
    if len(data) < 512:
        return
    for off, b in patches:
        if off + len(b) <= len(data):
            data[off:off+len(b)] = b
    # Recalculate PE checksum
    pe_off = struct.unpack_from('<I', data, 0x3c)[0]
    chk_off = pe_off + 0x58
    struct.pack_into('<I', data, chk_off, 0)
    tot = 0
    for i in range(0, len(data), 2):
        tot += struct.unpack_from('<H', data, i)[0]
        tot = (tot & 0xffff) + (tot >> 16)
    tot = (tot & 0xffff) + (tot >> 16) + len(data)
    struct.pack_into('<I', data, chk_off, tot)
    with open(path, 'wb') as f:
        f.write(data)
    print(f"[*] Successfully patched {path} (PE Checksum: {hex(tot)})")

def main():
    # bootmgfw.efi & bootaa64.efi:
    # 1. 0x4b070 (1004bc70): Catalog & revocation verification -> returns STATUS_SUCCESS (0)
    # 2. 0x4d438 (1004e038): Authenticode verification in ImgpLoadPEImage -> returns STATUS_SUCCESS (0)
    # 3. 0x4d548 (1004e148): Inner catalog digest validation -> returns STATUS_SUCCESS (0)
    # 4. 0x4df28 (1004eb28): Inner integrity verification -> returns STATUS_SUCCESS (0)
    bootmgfw_patches = [
        (0x4b070, struct.pack('<II', 0x52800000, 0xd65f03c0)),
        (0x4d438, struct.pack('<II', 0x52800000, 0xd65f03c0)),
        (0x4d548, struct.pack('<II', 0x52800000, 0xd65f03c0)),
        (0x4df28, struct.pack('<II', 0x52800000, 0xd65f03c0)),
    ]
    # winload.efi: rdisk(0) alignment patch at 0x18602c
    winload_patches = [
        (0x18602c, struct.pack('<I', 0x52800003))
    ]

    for arg in sys.argv[1:]:
        if not arg or arg == "/dev/null" or not os.path.exists(arg):
            continue
        base = os.path.basename(arg).lower()
        if "winload" in base:
            patch_file(arg, winload_patches)
        elif "bootmgfw" in base or "bootaa64" in base:
            patch_file(arg, bootmgfw_patches)
        else:
            print(f"[?] Unknown EFI loader: {arg}")



if __name__ == "__main__":
    main()
