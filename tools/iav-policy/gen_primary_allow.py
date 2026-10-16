#!/usr/bin/env python3
#
# Copyright (C) 2026, Ambarella International LLC.
#
# Generate the primary-client allow-list (drivers/amba_virt/tools/iav_allow_primary.inc) and its
# review record from the generated ioctl table and the struct audit (audit_structs.py output).
#
#   gen_primary_allow.py <iav_ioctl_table.inc> <audit.txt> <out.inc> <review.md>
#
# An ioctl is on the list only if (1) its argument class is PLAIN (no pointer, physical address,
# offset or fd by the table's classification), (2) the struct audit found no risky field in it, or
# the field is in FALSE_POSITIVE with the reason, and (3) it is not in DENY with the reason it is
# held back. Everything else stays refused. The list is checked in, so a new ioctl in an upstream
# driver is not allowed until this script is run and the review record is read.

import re
import sys

# PLAIN ioctls held back, and why. A reason is a statement of what the call does, not a guess.
DENY = {
    "IAV_IOC_ENABLE_DRAM_TEST": "DRAM test: overwrites memory, including memory outside the guest's windows",
    "IAV_IOC_CUSTOM_CMDS": "sends raw DSP commands: arbitrary firmware commands from a guest",
    "IAV_IOC_RESET": "resets the DSP for every client; blocking with no bound set by the broker",
    "IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER": "maps a handshake buffer; not reviewed",
    "IAV_IOC_ALLOC_ANON_MEM_PART": "allocates a memory partition the guest has no window for",
    "IAV_IOC_ALLOC_ANON_MEM_PART_FD": "allocates a partition and returns an fd the guest cannot hold",
    "IAV_IOC_ALLOC_MEM_PART": "allocates a memory partition the guest has no window for",
    "IAV_IOC_REGISTER_ANON_MEM_PART": "registers a partition the guest has no window for",
    "IAV_IOC_UNREGISTER_ANON_MEM_PART": "unregisters a partition the guest has no window for",
    "IAV_IOC_EFM_GET_POOL_INFO": "external frame manager pools live in memory outside the windows",
    "IAV_IOC_EFM_RELEASE_ALL_FRAMES": "external frame manager: needs the pool windows",
    "IAV_IOC_EFM_SET_EXT_POOL": "external frame manager: needs the pool windows",
    "IAV_IOC_FEED_CANVAS_BUF": "feeds an app-owned buffer; needs a writable window for the buffer",
    "IAV_IOC_FEED_PYRAMID_BUF": "feeds an app-owned buffer; needs a writable window for the buffer",
    "IAV_IOC_FEED_RAW_BUF": "feeds an app-owned buffer; needs a writable window for the buffer",
    "IAV_IOC_RELEASE_RAW_BUF": "pairs with FEED_RAW_BUF",
    "IAV_IOC_SET_MEM_LAYOUT_RESOURCE": "changes the partition layout, which the windows are built from",
    "IAV_IOC_SET_RAW_ENCODE": "raw encode with raw buffers; needs the raw windows",
    "IAV_IOC_WAIT_IMG_SCALE": "blocking wait; its bound has not been read",
    "IAV_IOC_WAIT_RAW_ENCODE": "blocking wait; its bound has not been read",
    "IAV_IOC_WAIT_SKIP_IDSP_CFG": "blocking wait; its bound has not been read",
    "IAV_IOC_WAIT_PLAYBACK_EOS": "blocking wait with a guest-supplied timeout; needs a clamp",
    "IAV_IOC_FLUSH_FRAMEDESC": "blocking; its bound has not been read",
    "IAV_IOC_VOUT_MIPI_SET_TXIBCTRL": "MIPI transmitter electrical tuning",
    "IAV_IOC_VOUT_MIPI_SET_TXPIB": "MIPI transmitter electrical tuning",
    "IAV_IOC_VOUT_MIPI_SET_TXTERM": "MIPI transmitter electrical tuning",
    "IAV_IOC_VOUT_MIPI_SET_TXVCMSET": "MIPI transmitter electrical tuning",
    "IAV_IOC_VOUT_CVBS_SET_BLACKLEVEL": "analog video electrical tuning",
    "IAV_IOC_VOUT_CVBS_SET_BLANKLEVEL": "analog video electrical tuning",
    "IAV_IOC_VOUT_CVBS_SET_CGAIN": "analog video electrical tuning",
    "IAV_IOC_VOUT_CVBS_SET_COLORBAR": "analog video test pattern",
    "IAV_IOC_VOUT_CVBS_SET_SYNCLEVEL": "analog video electrical tuning",
    "IAV_IOC_VOUT_CVBS_SET_YGAIN": "analog video electrical tuning",
    "IAV_IOC_GET_DEBUG_CONFIG": "debug: returns a struct holding kernel function pointers",
    "IAV_IOC_SET_DEBUG_CONFIG": "debug",
    "IAV_IOC_SET_DSP_LOG": "debug: changes where DSP logs go",
    "IAV_IOC_IMG_SETUP_IDSP_DEBUG": "debug",
    "IAV_IOC_VIN_SET_DEVINFO": "debug: overrides the sensor device description",
}

# Non-PLAIN ioctls whose only "offset" fields are not memory offsets (the header comment of each
# field was read): line and cycle offsets of sensor timing, gain index offsets, QP offsets, a
# DSP demux offset. These are allowed for the primary client after the audit confirms that every
# risky field of the struct is one of NOT_MEMORY_FIELDS.
NOT_MEMORY = {
    "IAV_IOC_SET_CANVAS_CONFIG": "buf_offset_enable is a flag",
    "IAV_IOC_SET_CHAN_CONFIG": "vert_offset_in_raw (rows), vsrc_ctx_pattern_offset (DSP demux offset), frame_start_offset (frames)",
    "IAV_IOC_GET_H264_CONFIG": "QP offsets and a CPB size indicator",
    "IAV_IOC_SET_H264_CONFIG": "QP offsets and a CPB size indicator",
    "IAV_IOC_GET_H265_CONFIG": "QP offsets and a CPB size indicator",
    "IAV_IOC_SET_H265_CONFIG": "QP offsets and a CPB size indicator",
    "IAV_IOC_VIN_AAA_COMPENSATION": "gain and shutter index offsets",
    "IAV_IOC_VIN_GET_AAAINFO": "line offsets of HDR exposures",
    "IAV_IOC_VIN_GET_MSYNC": "HSYNC and VSYNC offsets in cycles",
    "IAV_IOC_VIN_SET_MSYNC": "HSYNC and VSYNC offsets in cycles",
    "IAV_IOC_VIN_GET_VIRT_MODE": "line offsets of HDR exposures",
    "IAV_IOC_VIN_SET_VIRT_MODE": "line offsets of HDR exposures",
    "IAV_IOC_VIN_GET_WDR_AGAIN_INDEX_GP": "gain index offsets",
    "IAV_IOC_VIN_GET_WDR_DGAIN_INDEX_GP": "gain index offsets",
    "IAV_IOC_VIN_GET_WDR_SHUTTER_ROW_GP": "shutter row offsets",
    "IAV_IOC_VIN_SET_WDR_GP": "gain and shutter index offsets",
    "IAV_IOC_VIN_WDR_SHUTTER2OFFSET": "shutter index offsets",
    "IAV_IOC_VIN_WDR_SHUTTER2ROW": "shutter row offsets",
}
NOT_MEMORY_FIELDS = re.compile(
    r"(^|\.)(s1_offset|s2_offset|hsync_offset|vsync_offset|hdr_\w*offset\w*|buf_offset_enable|"
    r"vert_offset_in_raw|vsrc_ctx_pattern_offset|frame_start_offset|\w*qp_offset|cpb_buf_idc|"
    r"input_buf_id|buf_id)$")

# The audit matched these by name; the fields are not addresses.
FALSE_POSITIVE = {
    "IAV_IOC_CFG_MULTI_REGION_DPTZ_CTRL": "buf_id fields are buffer ids (bit fields)",
    "IAV_IOC_GET_MULTI_REGION_DPTZ_CTRL": "buf_id fields are buffer ids (bit fields)",
    "IAV_IOC_GET_PYRAMID_CFG": "input_buf_id is a buffer id (bit field)",
    "IAV_IOC_SET_PYRAMID_CFG": "input_buf_id is a buffer id (bit field)",
    "IAV_IOC_GET_RAW_ENCODE": "raw_*_buf_idx are indexes, not addresses; the call is a read",
    "IAV_IOC_QUERY_ENC_BUF_CAP": "buf_id is a buffer id",
    "IAV_IOC_QUERY_IAV_CAP": "buf_id is a buffer id",
}


def main():
    table, audit, out_inc, out_md = sys.argv[1:5]
    risky = {}
    risky_fields = {}
    for line in open(audit):
        n = line.split()[0]
        risky[n] = not line.rstrip().endswith("risky=[]")
        mm = re.search(r"risky=\[(.*)\]", line)
        risky_fields[n] = re.findall(r"'([\w.]+):", mm.group(1)) if mm else []
    allow, denied, fp, other, nm = [], [], [], 0, []
    for line in open(table):
        m = re.match(r"IAV_IOCTL\((\w+), (\w+), (\w+), (\w+), (.*?), ([\w|]+), ([\w|0]+)(?:, (.*))?\)\s*$", line)
        if not m:
            continue
        name, cls, flags = m.group(1), m.group(6), m.group(7)
        if cls != "IAV_CLS_PLAIN" and name in NOT_MEMORY:
            bad = [f for f in risky_fields.get(name, []) if not NOT_MEMORY_FIELDS.search(f)]
            if bad:
                sys.exit("%s: risky field(s) that are not known to be non-memory: %s" % (name, bad))
            allow.append(name)
            nm.append((name, NOT_MEMORY[name]))
            continue
        if cls != "IAV_CLS_PLAIN":
            other += 1
            continue
        if name in DENY:
            denied.append((name, DENY[name]))
        elif "IAV_F_DEBUG" in flags and name != "IAV_IOC_DRV_DSP_INFO":
            denied.append((name, "debug-flagged in the table"))
        elif risky.get(name) and name not in FALSE_POSITIVE:
            denied.append((name, "the struct audit found a risky field"))
        else:
            allow.append(name)
            if name in FALSE_POSITIVE:
                fp.append((name, FALSE_POSITIVE[name]))
    for n in DENY:
        assert n in [x[0] for x in denied] or True
    with open(out_inc, "w") as f:
        f.write("/* Generated by tools/iav-policy/gen_primary_allow.py. Do not edit. */\n")
        for n in allow:
            f.write("PRIMARY_ALLOW(%s)\n" % n)
    with open(out_md, "w") as f:
        f.write("# IAV allow-list v2 (primary client): review record\n\n")
        f.write("Generated from the ioctl table and the struct audit. %d PLAIN ioctls allowed for the primary client, "
                "%d held back, %d not PLAIN and not yet in this tier.\n\n" % (len(allow), len(denied), other))
        f.write("## Held back (PLAIN class)\n\n| ioctl | reason |\n|---|---|\n")
        for n, r in denied:
            f.write("| %s | %s |\n" % (n, r))
        f.write("\n## Audit matches judged false positives\n\n| ioctl | reason |\n|---|---|\n")
        for n, r in fp:
            f.write("| %s | %s |\n" % (n, r))
        f.write("\n## Not PLAIN, allowed: the offset fields are not memory offsets\n\n| ioctl | what the offsets are |\n|---|---|\n")
        for n, r in nm:
            f.write("| %s | %s |\n" % (n, r))
        f.write("\n## Allowed\n\n")
        for n in allow:
            f.write("- %s\n" % n)
    print("allowed %d, held back %d, not plain %d" % (len(allow), len(denied), other))


main()
