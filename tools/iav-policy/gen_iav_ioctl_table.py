#!/usr/bin/env python3
"""Generate the IAV ioctl table for the broker and the guest frontend.

Usage: gen_iav_ioctl_table.py <dsp_v6 driver root> <out.inc> [<out.md>]

Reads the dispatch switches of the five ioctl magics the IAV driver serves on /dev/iav, locates
each handler, scans it and its callees (depth <= 3) for user-memory access, state checks and
blocking calls, and walks the argument struct for pointer, physical-address, fd and offset
fields. Writes a C include fragment of IAV_IOCTL(...) rows, one per ioctl, and optionally a
review table in markdown. The rules are mechanical: a row the script cannot resolve is a
build failure, never a guess. Run it again after every merge from the IAV driver upstream.
"""
import os
import re
import sys

ROOT = sys.argv[1]            # the dsp_v6 driver tree
OUT_INC = sys.argv[2]
OUT = sys.argv[3] if len(sys.argv) > 3 else None
DEFS = {"AMBA_SOC_N1_655", "AMBA_AMYOC_BUILD", "PROJECT_CV6S"}

SRC_DIRS = ["iav", "iav/v6p0", "imgproc", "imgproc/v6p0", "dsp", "msg", "ambcma", "ambnl"]
HDR_DIRS = ["include", "include/uapi", "include/uapi/specific", "include/driver",
            "include/driver/specific", "iav", "imgproc", "imgproc/v6p0", "dsp", "msg", "ambcma"]


def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), t, flags=re.S)
    t = re.sub(r"//[^\n]*", "", t)
    return t


def eval_cond(expr):
    """Evaluate #if expression over SoC macros only; None means unknown (keep)."""
    e = expr.strip()
    toks = re.findall(r"[A-Za-z_]\w*", e)
    ids = [t for t in toks if t != "defined"]
    if not ids:
        try:
            return bool(eval(e.replace("&&", " and ").replace("||", " or ")))
        except Exception:
            return None
    if not all(i.startswith("AMBA_SOC") or i in DEFS or i.startswith("PROJECT_") for i in ids):
        return None
    e = re.sub(r"defined\s*\(\s*(\w+)\s*\)", lambda m: "1" if m.group(1) in DEFS else "0", e)
    e = re.sub(r"defined\s+(\w+)", lambda m: "1" if m.group(1) in DEFS else "0", e)
    e = e.replace("&&", " and ").replace("||", " or ")
    e = re.sub(r"!(?!=)", " not ", e)
    e = re.sub(r"[A-Za-z_]\w*", "0", e)
    try:
        return bool(eval(e))
    except Exception:
        return None


def preprocess(t):
    """Drop code in #if blocks that are false for N1-655; keep unknown ones."""
    out, stack = [], []   # stack entries: [active, taken, parent_active, known]
    for line in t.split("\n"):
        s = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
        if m:
            d, rest = m.group(1), m.group(2)
            parent = all(x[0] for x in stack)
            if d in ("if", "ifdef", "ifndef"):
                if d == "if":
                    c = eval_cond(rest)
                elif d == "ifdef":
                    c = eval_cond("defined(%s)" % rest.strip().split()[0])
                else:
                    c = eval_cond("!defined(%s)" % rest.strip().split()[0])
                known = c is not None
                act = True if c is None else c
                stack.append([act, act and known, known])
            elif d == "elif":
                top = stack[-1]
                c = eval_cond(rest)
                if top[2] is False:
                    pass
                if top[1]:
                    top[0] = False
                else:
                    top[0] = True if c is None else c
                    top[1] = top[0] and c is not None
            elif d == "else":
                top = stack[-1]
                if top[2]:
                    top[0] = not top[1]
                    top[1] = True
                else:
                    top[0] = True
            elif d == "endif":
                stack.pop()
            out.append("")
            continue
        out.append(line if all(x[0] for x in stack) else "")
    return "\n".join(out)


def read(path):
    with open(path, errors="replace") as f:
        return f.read()


# ---- load sources and build a function index ------------------------------
sources = {}
for d in SRC_DIRS:
    p = os.path.join(ROOT, d)
    if not os.path.isdir(p):
        continue
    for fn in sorted(os.listdir(p)):
        if fn.endswith(".c"):
            path = os.path.join(p, fn)
            sources[path] = preprocess(strip_comments(read(path)))

func_re = re.compile(r"(?m)^(?!\s*(?:if|for|while|switch|return|else|do)\b)[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{}]*?)\)\s*\{", re.S)


def find_body(text, start):
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    return text[start:]


FUNCS = {}   # name -> (path, line, body)
for path, text in sources.items():
    for m in func_re.finditer(text):
        name = m.group(1)
        if name in FUNCS:
            continue
        brace = m.end() - 1
        line = text.count("\n", 0, m.start()) + 1
        FUNCS[name] = (path, line, find_body(text, brace))

# ---- headers: macros and structs ------------------------------------------
headers = {}
for d in HDR_DIRS:
    p = os.path.join(ROOT, d)
    if not os.path.isdir(p):
        continue
    for fn in sorted(os.listdir(p)):
        if fn.endswith(".h"):
            headers[os.path.join(p, fn)] = strip_comments(read(os.path.join(p, fn)))
all_hdr = "\n".join(headers.values())

STRUCTS = {}
TAGS = set()
for m in re.finditer(r"(?:typedef\s+)?struct\s+(\w+)\s*\{", all_hdr):
    name = m.group(1)
    if name in STRUCTS:
        continue
    STRUCTS[name] = find_body(all_hdr, m.end() - 1)
    TAGS.add(name)
# typedef struct [tag] { ... } alias;
for m in re.finditer(r"typedef\s+struct\s*(?:\w+)?\s*\{", all_hdr):
    body = find_body(all_hdr, m.end() - 1)
    tail = all_hdr[m.end() - 1 + len(body):m.end() - 1 + len(body) + 80]
    am = re.match(r"\s*(\w+)\s*(?:,\s*\*?\s*\w+)?\s*;", tail)
    if am and am.group(1) not in STRUCTS:
        STRUCTS[am.group(1)] = body

IOC = {}
for m in re.finditer(r"#define\s+(IAV_IOC_\w+)\s+(\w+)\s*\(([^\n]*)\)", all_hdr):
    IOC.setdefault(m.group(1), (m.group(2), m.group(3).strip()))

MAGIC_OF = {"IAVENC": "V", "IAVDEC": "D", "VIN": "VIN", "VOUT": "VOUT", "IAVIMG": "IMG"}


def ioc_info(name):
    if name not in IOC:
        return None
    macro, args = IOC[name]
    parts = [a.strip() for a in args.split(",")]
    direction = {"IOR": "R", "IOW": "W", "IOWR": "RW", "IO": "-"}.get(macro.split("_")[-1], macro)
    typ = parts[-1] if len(parts) > 1 else ""
    return macro, direction, typ


# ---- struct walking --------------------------------------------------------
PTR_FIELD = re.compile(r"\b(?:struct\s+\w+|\w+)\s*\*\s*(\w+)\s*[;\[]")
PHYS_NAME = re.compile(r"(phys|paddr|use_phys|_pa$|(?<!offset)_addr$|^addr$)", re.I)
OFFSET_EXCL = re.compile(r"(offset|_idx$|idx_)", re.I)
FD_NAME = re.compile(r"(dma_buf_fd|_fd\b|\bfd\b)", re.I)
OFF_NAME = re.compile(r"(offset|_pid\b|\bpid\b|part_id|mmap_type|_addr_offset)", re.I)


def struct_name_from(typ):
    m = re.search(r"struct\s+(\w+)", typ)
    if m:
        return m.group(1)
    t = typ.replace("*", "").strip()
    return t if t in STRUCTS else None


def walk_struct(name, depth=0, seen=None):
    seen = seen or set()
    res = {"ptr": [], "phys": [], "fd": [], "off": []}
    if name not in STRUCTS or name in seen or depth > 3:
        return res
    seen.add(name)
    body = STRUCTS[name]
    for raw in body.split(";"):
        line = " ".join(raw.split())
        if not line:
            continue
        mm = re.search(r"struct\s+(\w+)\s+\w+", line)
        if mm and "*" not in line:
            sub = walk_struct(mm.group(1), depth + 1, seen)
            for k in res:
                res[k] += ["%s." % mm.group(1) + x for x in sub[k]]
        fm = re.search(r"(\w+)\s*(?:\[[^\]]*\])?\s*(?::\s*\d+)?$", line)
        fname = fm.group(1) if fm else line
        if "*" in line and PTR_FIELD.search(line + ";"):
            res["ptr"].append(fname)
        if PHYS_NAME.search(fname) and not OFFSET_EXCL.search(fname):
            res["phys"].append(fname)
        if FD_NAME.search(fname):
            res["fd"].append(fname)
        if OFF_NAME.search(fname):
            res["off"].append(fname)
    return res


# ---- handler scanning ------------------------------------------------------
BLOCK = re.compile(r"\b(wait_event\w*|wait_for_completion\w*|schedule_timeout\w*|msleep\w*|usleep_range|down_interruptible|poll_wait|iav_wait\w*|wait_\w*_timeout)\b")
STATE = re.compile(r"\b(IAV_STATE_\w+|check_iav_state|iav_state_check\w*)\b")
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
KW = {"if", "for", "while", "switch", "return", "sizeof", "defined"}


def scan(name, depth=0, visited=None):
    visited = visited if visited is not None else set()
    info = {"cfu": [], "ctu": [], "gu": 0, "pu": 0, "state": set(), "block": set(), "depth_ptr": 0,
            "files": set(), "callees": set()}
    if name in visited or name not in FUNCS or depth > 3:
        return info
    visited.add(name)
    path, line, body = FUNCS[name]
    info["files"].add("%s:%d" % (os.path.relpath(path, ROOT), line))
    for m in re.finditer(r"copy_from_user\s*\(([^;]*?)\)\s*[;)]", body, re.S):
        info["cfu"].append(" ".join(m.group(1).split())[:90])
    for m in re.finditer(r"copy_to_user\s*\(([^;]*?)\)\s*[;)]", body, re.S):
        info["ctu"].append(" ".join(m.group(1).split())[:90])
    info["gu"] += len(re.findall(r"\bget_user\s*\(", body))
    info["pu"] += len(re.findall(r"\bput_user\s*\(", body))
    info["state"] |= set(STATE.findall(body))
    info["block"] |= set(BLOCK.findall(body))
    for c in set(CALL.findall(body)):
        if c in KW or c == name or c not in FUNCS:
            continue
        sub = scan(c, depth + 1, visited)
        for k in ("cfu", "ctu"):
            info[k] += ["%s> %s" % (c, x) for x in sub[k]]
        info["gu"] += sub["gu"]
        info["pu"] += sub["pu"]
        info["state"] |= sub["state"]
        info["block"] |= sub["block"]
        info["callees"].add(c)
    return info


if "plat_cmd_get_statistics" in FUNCS:
    p0, l0, _ = FUNCS["imgproc_get_statistics_entry"]
    FUNCS["imgproc_get_statistics_entry"] = (p0, l0, "{ plat_cmd_get_statistics(); }")

# ---- dispatch extraction ---------------------------------------------------
DISPATCH = [
    ("V enc", "iav_encode_ioctl"),
    ("D dec", "iav_decode_ioctl"),
    ("VIN global", "vin_global_ioctl"),
    ("VIN dev", "vin_device_ioctl"),
    ("VOUT", "iav_vout_ioctl"),
    ("IMAGE", "amba_imgproc_cmd"),
]

rows = []
for magic, fname in DISPATCH:
    if fname not in FUNCS:
        rows.append(dict(magic=magic, name="(dispatcher %s)" % fname, untraced="dispatcher not found"))
        continue
    path, line, body = FUNCS[fname]
    # split switch into case groups
    seq = re.split(r"(?m)^\s*(?=case\s+\w+\s*:|default\s*:)", body)
    pending = []
    for chunk in seq:
        cm = re.match(r"\s*case\s+(\w+)\s*:\s*(.*)", chunk, re.S)
        if not cm:
            continue
        pending.append(cm.group(1))
        rest = cm.group(2)
        if not re.search(r"\w", re.sub(r"^\s*", "", rest)) or re.match(r"\s*$", rest):
            continue
        # a handler body follows
        hm = re.search(r"(?:rval|ret)\s*=\s*(\w+)\s*\(([^;]*)\)\s*;", rest, re.S)
        hname = hm.group(1) if hm else None
        hargs = " ".join(hm.group(2).split()) if hm else ""
        guard = " ".join(re.sub(r"\s+", " ", rest.split("break;")[0]).split())[:0]
        for case in pending:
            rows.append(dict(magic=magic, name=case, handler=hname, hargs=hargs, rest=rest, dispatcher=fname,
                             dline="%s:%d" % (os.path.relpath(path, ROOT), line)))
        pending = []

# imgproc statistics group dispatches through imgproc_get_statistics_entry
# ---- compose rows ------------------------------------------------------------
GLOBAL_STATE_CHANGING = re.compile(r"(ENTER_IDLE|ENABLE_PREVIEW|START_ENCODE|STOP_ENCODE|ABORT_ENCODE|"
                                   r"SET_SYSTEM_RESOURCE|SET_.*RESOURCE|SET_CHAN_CONFIG|SET_CANVAS_CONFIG|"
                                   r"SET_PYRAMID|ENTER_DECODE|LEAVE_DECODE|SET_ISP_NN|SET_AISP|SET_VCAP|"
                                   r"VIN_SET_GLOBAL_MODE|VOUT_SET_MODE|SET_VOUT_MAX|SET_VIN_IDSP|"
                                   r"VIDEO_PROC)")
DEBUG_RISK = re.compile(r"(DSP_LOG|DEBUG|DRV_DSP_INFO|SETUP_IDSP_DEBUG|SET_REG|VIN_SET_DEVINFO|GDMA|COPY)")

entries = []
out = []
counts = {"PLAIN": 0, "OFFSET/FD": 0, "PHYS": 0, "POINTER": 0, "UNTRACED": 0}
policy_counts = {}
untraced = []
for r in rows:
    name = r["name"]
    info_ioc = ioc_info(name)
    macro, direction, typ = info_ioc if info_ioc else ("?", "?", "?")
    arg_is_value = bool(re.match(r"\(\s*u\d+\s*\)\s*arg", r.get("hargs", "")) or r.get("hargs", "").endswith(", arg") and "__user" not in r.get("hargs", "") and direction == "W" and "*" not in typ and typ not in ("", "?"))
    if r.get("untraced"):
        out.append((r["magic"], name, "?", "?", "?", "UNTRACED: " + r["untraced"], "", "", "", "DENY", "DENY"))
        untraced.append(name)
        continue
    h = r["handler"]
    sinfo = scan(h) if h else None
    if h is None or h not in FUNCS:
        reason = "handler %s not found in built sources" % (h or "(none parsed)")
        untraced.append("%s (%s)" % (name, reason))
        counts["UNTRACED"] += 1
        out.append((r["magic"], name, direction, typ, "?", "UNTRACED: " + reason, "", "", "", "DENY", "DENY"))
        continue
    scalar_t = bool(re.fullmatch(r"\s*(u8|u16|u32|u64|int|unsigned int|unsigned long|long)\s*", typ or ""))
    no_arg = bool(r.get("hargs") is not None and "arg" not in r.get("hargs", "") and r["magic"] in ("V enc", "D dec"))
    arg_is_value = (scalar_t and direction in ("W", "-") and not (sinfo["cfu"] or sinfo["gu"])
                    and r["magic"] in ("V enc", "D dec") and not no_arg)
    sname = struct_name_from(typ)
    inferred = None
    MANUAL = {"IAV_IOC_IMG_GET_STATISTICS": "img_statistics",
              "IAV_IOC_IMG_GET_STATISTICS_MAP": "img_statistics_map",
              "IAV_IOC_IMG_GET_LATEST_STATISTICS": "img_statistics",
              "IAV_IOC_IMG_GET_LATEST_STATISTICS_MAP": "img_statistics_map"}
    if name in MANUAL:
        sname = MANUAL[name]
        inferred = sname
    if not sname:
        cm = re.search(r"\(\s*(?:struct\s+)?(\w+)\s*(?:__user)?\s*\*\s*\)\s*arg", r.get("hargs", "")) or \
             re.search(r"\(\s*(?:__user\s+)?(?:struct\s+)?(\w+)\s*\*\s*\)\s*arg", r.get("hargs", ""))
        if cm and cm.group(1) in STRUCTS:
            sname = cm.group(1)
            inferred = sname
    if not sname:
        cands = []
        for c in sinfo["cfu"] + sinfo["ctu"]:
            for mm in re.finditer(r"sizeof\s*\(\s*(?:struct\s+)?(\w+)\s*\)", c):
                cands.append(mm.group(1))
        for c in cands:
            if c in STRUCTS:
                inferred = c
                break
        sname = inferred
    w = walk_struct(sname) if sname else {"ptr": [], "phys": [], "fd": [], "off": []}
    uses_user = bool(sinfo["cfu"] or sinfo["ctu"] or sinfo["gu"] or sinfo["pu"])
    cls = []
    if w["ptr"] or any("copy_from_user" in c and "ptr" in c.lower() for c in sinfo["cfu"]) :
        cls.append("POINTER")
    if w["phys"]:
        cls.append("PHYS")
    if w["fd"] or w["off"]:
        cls.append("OFFSET/FD")
    if not cls:
        cls.append("PLAIN")
    nested = len(set(c.split(">")[0] for c in sinfo["cfu"] + sinfo["ctu"] if ">" in c))
    scope = "global"
    if r["magic"] == "VIN dev":
        scope = "per-vsrc (first u32 = vsrc_id)"
    elif r["magic"] == "VOUT":
        scope = "per-vout (first u32 = vout id)"
    elif re.search(r"(chan|canvas|stream|vsrc|vinc|vin_id)", " ".join(w["ptr"] + w["phys"] + w["off"]) + " " + (STRUCTS.get(sname or "", "")[:2000]), re.I):
        scope = "per-id fields (chan/canvas/stream/vsrc) - review"
    state = ",".join(sorted(sinfo["state"])) or "-"
    block = ",".join(sorted(sinfo["block"])) or "-"
    gcls = "/".join(cls)
    is_state = bool(GLOBAL_STATE_CHANGING.search(name))
    is_dbg = bool(DEBUG_RISK.search(name))
    is_get = bool(re.search(r"_(GET|QUERY|WAIT)_|_GET$|_QUERY$|IAV_IOC_(GET|QUERY|WAIT)", name)) and not is_state
    if "PHYS" in cls or "POINTER" in cls:
        qual = "TRANSLATE"
        restricted = "TRANSLATE" if is_get else "DENY"
    elif is_dbg:
        qual, restricted = "ALLOW", "DENY"
    elif is_state:
        qual, restricted = "ALLOW (primary only)", "DENY"
    elif is_get:
        qual, restricted = "ALLOW", "ALLOW"
    else:
        qual, restricted = "ALLOW", "DENY"
    for c in cls:
        counts[c] = counts.get(c, 0) + 1
    policy_counts[(qual, restricted)] = policy_counts.get((qual, restricted), 0) + 1
    argkind = "no-arg" if no_arg else "value-in-arg" if (arg_is_value and not inferred) else ("ptr->struct %s%s" % (sname, " (inferred; macro says %s)" % typ if inferred else "") if sname else typ)
    cu = "from:%d to:%d get:%d put:%d" % (len(sinfo["cfu"]), len(sinfo["ctu"]), sinfo["gu"], sinfo["pu"])
    if nested:
        cu += " (%d via callees)" % nested
    detail = []
    for k, lab in (("ptr", "ptr"), ("phys", "phys"), ("fd", "fd"), ("off", "off")):
        if w[k]:
            detail.append("%s: %s" % (lab, ",".join(sorted(set(w[k]))[:6])))
    direct_block = bool(BLOCK.search(FUNCS[h][2])) if h in FUNCS else False
    if no_arg:
        kind, size = "IAV_ARG_NONE", "0"
    elif arg_is_value and not inferred:
        kind, size = "IAV_ARG_VALUE", "0"
    else:
        kind = "IAV_ARG_PTR"
        if sname:
            size = "sizeof(struct %s)" % sname if sname in TAGS else "sizeof(%s)" % sname
        elif scalar_t:
            size = "sizeof(%s)" % typ.strip()
        else:
            raise SystemExit("cannot size the argument of %s (type %r)" % (name, typ))
    cflags = []
    if "POINTER" in cls:
        cflags.append("IAV_CLS_POINTER")
    if "PHYS" in cls:
        cflags.append("IAV_CLS_PHYS")
    if "OFFSET/FD" in cls:
        cflags.append("IAV_CLS_OFFSET_FD")
    if not cflags:
        cflags.append("IAV_CLS_PLAIN")
    fl = []
    if is_state:
        fl.append("IAV_F_STATE")
    if is_dbg:
        fl.append("IAV_F_DEBUG")
    if direct_block:
        fl.append("IAV_F_BLOCKING")
    if r["magic"] in ("VIN dev", "VOUT"):
        fl.append("IAV_F_LEADING_ID")
    magic_enum = {"V enc": "IAV_MAGIC_ENC", "D dec": "IAV_MAGIC_DEC", "VIN global": "IAV_MAGIC_VIN",
                  "VIN dev": "IAV_MAGIC_VIN", "VOUT": "IAV_MAGIC_VOUT", "IMAGE": "IAV_MAGIC_IMAGE"}[r["magic"]]
    dirs = {"R": "IAV_DIR_R", "W": "IAV_DIR_W", "RW": "IAV_DIR_RW", "-": "IAV_DIR_NONE"}.get(direction, "IAV_DIR_NONE")
    entries.append((name, magic_enum, kind, dirs, size, "|".join(cflags), "|".join(fl) if fl else "0"))
    out.append((r["magic"], name, direction, argkind, sorted(sinfo["files"])[0] if sinfo["files"] else "?",
                gcls + (" | " + "; ".join(detail) if detail else ""), cu, state, block + " | " + scope,
                qual, restricted))

if untraced:
    raise SystemExit("unresolved ioctls: %s" % ", ".join(untraced))
entries.sort()
with open(OUT_INC, "w") as f:
    f.write("/*\n * Generated by tools/iav-policy/gen_iav_ioctl_table.py from the IAV driver sources. Do not edit.\n")
    f.write(" * Built for N1-655 (AMBA_SOC_N1_655). %d ioctls. Every row is\n" % len(entries))
    f.write(" * IAV_IOCTL(name, magic, arg kind, direction, argument size, class, flags).\n *\n")
    f.write(" * Copyright (C) 2026, Ambarella International LLC.\n */\n\n")
    for e in entries:
        f.write("IAV_IOCTL(%s, %s, %s, %s, %s, %s, %s)\n" % e)
print("rows", len(entries), counts)
if OUT:
    with open(OUT, "w") as f:
        f.write("# IAV ioctl table (generated review table)\n\n")
        f.write("Summary: " + ", ".join("%s=%d" % (k, v) for k, v in counts.items()) + "; total rows=%d\n\n" % len(out))
        f.write("| Magic | Ioctl | Dir | Argument | Handler | Class and fields | User-memory access | State | Blocking / scope | Draft qual | Draft restricted |\n")
        f.write("|---|---|---|---|---|---|---|---|---|---|---|\n")
        for o in out:
            f.write("| " + " | ".join(str(x).replace("|", "/") for x in o) + " |\n")
