#!/usr/bin/env python3
"""
patch_bcd.py - Dynamically patch Windows BCD GPT Disk and Partition GUIDs
"""
import sys, uuid, struct
import hivex

def make_device_element(disk_guid_str, part_guid_str, start_offset=0):
    disk_u = uuid.UUID(disk_guid_str)
    part_u = uuid.UUID(part_guid_str)
    # 88-byte BCD device element for GPT partition (type 6)
    # Offset 16: Type = 6 (GPT partition)
    # Offset 24: Data size = 72 bytes
    # Offset 32: Partition GUID (PARTUUID)
    # Offset 48: Starting offset (uint64, 0)
    # Offset 56: Disk GUID (PTUUID)
    data = bytearray(88)
    struct.pack_into("<I", data, 16, 6)   # Type 6 = GPT partition
    struct.pack_into("<I", data, 24, 72)  # Data size = 72 bytes
    data[32:48] = part_u.bytes_le
    struct.pack_into("<Q", data, 48, start_offset)
    data[56:72] = disk_u.bytes_le
    return bytes(data)

def patch_bcd(bcd_path, disk_guid_str, esp_part_guid_str, os_part_guid_str):
    print(f"[*] Patching BCD at {bcd_path}...")
    print(f"    Disk GUID:     {disk_guid_str}")
    print(f"    ESP Part GUID: {esp_part_guid_str}")
    print(f"    OS Part GUID:  {os_part_guid_str}")

    esp_elem = make_device_element(disk_guid_str, esp_part_guid_str, start_offset=0)
    os_elem = make_device_element(disk_guid_str, os_part_guid_str, start_offset=0)

    h = hivex.Hivex(bcd_path, write=True)
    objs = h.node_get_child(h.root(), "Objects")
    if not objs:
        raise RuntimeError("Objects not found in BCD")

    # 1. Update {bootmgr}
    bm = h.node_get_child(objs, "{9dea862c-5cdd-4e70-acc1-f32b344d4795}")
    if bm:
        els = h.node_get_child(bm, "Elements")
        if els:
            c = h.node_get_child(els, "11000001")
            if c:
                h.node_set_values(c, [{"key": "Element", "t": 3, "value": esp_elem}])
                print("  -> Updated {bootmgr} 11000001 (device) to ESP partition")
            # Set bootmgr timeout to 0 for instant boot
            c_to = h.node_get_child(els, "25000004")
            if c_to:
                h.node_set_values(c_to, [{"key": "Element", "t": 3, "value": b"\x00\x00\x00\x00"}])
                print("  -> Set {bootmgr} timeout to 0")

    # 2. Update all OS loader and recovery objects
    for obj in h.node_children(objs):
        obj_name = h.node_name(obj)
        if obj_name == "{9dea862c-5cdd-4e70-acc1-f32b344d4795}":
            continue
        els = h.node_get_child(obj, "Elements")
        if not els:
            continue

        c11 = h.node_get_child(els, "11000001")
        if c11:
            vals = h.node_values(c11)
            if vals and len(h.value_value(vals[0])[1]) == 88:
                h.node_set_values(c11, [{"key": "Element", "t": 3, "value": os_elem}])
                print(f"  -> Updated {obj_name} 11000001 (device) to OS partition")

        c21 = h.node_get_child(els, "21000001")
        if c21:
            vals = h.node_values(c21)
            if vals and len(h.value_value(vals[0])[1]) == 88:
                h.node_set_values(c21, [{"key": "Element", "t": 3, "value": os_elem}])
                print(f"  -> Updated {obj_name} 21000001 (osdevice) to OS partition")

        # Disable BootDebug (260000a0) so bootloader does not halt waiting for serial debugger
        c_dbg = h.node_get_child(els, "260000a0")
        if c_dbg:
            h.node_set_values(c_dbg, [{"key": "Element", "t": 3, "value": b"\x00"}])
            print(f"  -> Disabled BootDebug on {obj_name}")


        c_nic = h.node_get_child(els, "26000022")
        if not c_nic:
            c_nic = h.node_add_child(els, "26000022")
        h.node_set_values(c_nic, [{"key": "Element", "t": 3, "value": b"\x01"}])
        print(f"  -> Enabled NoIntegrityChecks on {obj_name}")

        # Enable bootlog and sos for diagnostic output
        c_blog = h.node_get_child(els, "26000070")
        if not c_blog:
            c_blog = h.node_add_child(els, "26000070")
        h.node_set_values(c_blog, [{"key": "Element", "t": 3, "value": b"\x01"}])

        c_sos = h.node_get_child(els, "26000071")
        if not c_sos:
            c_sos = h.node_add_child(els, "26000071")
        h.node_set_values(c_sos, [{"key": "Element", "t": 3, "value": b"\x01"}])

        # IgnoreAllFailures: BootStatusPolicy = 1
        c_bsp = h.node_get_child(els, "25000020")
        if c_bsp:
            h.node_set_values(c_bsp, [{"key": "Element", "t": 3, "value": b"\x01\x00\x00\x00\x00\x00\x00\x00"}])

    new_path = f"{bcd_path}.patched"
    h.commit(new_path)
    import os
    os.replace(new_path, bcd_path)
    print("[*] BCD committed successfully!")

if __name__ == "__main__":
    if len(sys.argv) < 5:
        print(f"Usage: {sys.argv[0]} <bcd_path> <disk_guid> <esp_part_guid> <os_part_guid>")
        sys.exit(1)
    patch_bcd(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
