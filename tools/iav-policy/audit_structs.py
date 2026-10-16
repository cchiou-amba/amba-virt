#!/usr/bin/env python3
#
# Copyright (C) 2026, Ambarella International LLC.
#
# Audit the argument structs of the IAV ioctls for fields that can carry an address, a pointer, a
# file descriptor or an offset, including nested structs. Input: the generated ioctl table and the
# preprocessed driver UAPI headers. Output: one block per ioctl with the risky fields found.
#
#   audit_structs.py <iav_ioctl_table.inc> <preprocessed-headers.i> [--ioctl NAME ...]
#
# A struct with no risky field has an empty list, which is what the allow-list review relies on.
# The check is by field name and type, so a field named innocently is not found: the review notes
# say so where it matters.

import re
import sys

RISKY_NAME = re.compile(r"(addr|phys|ptr|pointer|fd\b|_fd|offset|handle|dma_buf|buf_id)", re.I)


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def parse_structs(text):
    text = strip_comments(text)
    structs = {}
    i = 0
    pat = re.compile(r"\b(struct|union)\s+(\w+)\s*\{")
    while True:
        m = pat.search(text, i)
        if not m:
            break
        depth, j = 1, m.end()
        while depth and j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
            j += 1
        body = text[m.end():j - 1]
        structs.setdefault(m.group(2), body)
        i = m.end()
    return structs


def fields(body):
    out = []
    depth = 0
    cur = ""
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if ch == ";" and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    return [f for f in out if f]


def audit(name, structs, seen=None, path=""):
    seen = seen or set()
    if name in seen or name not in structs:
        return []
    seen = seen | {name}
    found = []
    for f in fields(structs[name]):
        f1 = re.sub(r"\s+", " ", f)
        m = re.search(r"(\w+)\s*(\[[^\]]*\])*\s*(:\s*\d+)?$", f1)
        fname = m.group(1) if m else f1
        risky = "*" in f1 or RISKY_NAME.search(fname) or re.search(r"\bunsigned long\b|\bsize_t\b|\buintptr_t\b|\bvoid\b", f1)
        if risky:
            found.append((path + fname, f1))
        for sm in re.finditer(r"\b(struct|union)\s+(\w+)\b", f1):
            found += audit(sm.group(2), structs, seen, path + fname + ".")
    return found


def main():
    table, pre = sys.argv[1], sys.argv[2]
    only = []
    if "--ioctl" in sys.argv:
        only = sys.argv[sys.argv.index("--ioctl") + 1:]
    structs = parse_structs(open(pre).read())
    for line in open(table):
        m = re.match(r"IAV_IOCTL\((\w+), (\w+), (\w+), (\w+), (.*?), ([\w|]+), ([\w|0]+)(?:, (.*))?\)\s*$", line)
        if not m:
            continue
        name, size = m.group(1), m.group(5)
        if m.group(8) and m.group(8) != "0":
            size = m.group(8)  # the handler copies the wire-size type, so that is the struct to audit
        if only and name not in only:
            continue
        sm = re.search(r"struct (\w+)", size)
        sname = sm.group(1) if sm else None
        if sname is None:
            print("%s struct=- (%s) risky=[]" % (name, size))
            continue
        if sname not in structs:
            print("%s struct=%s UNRESOLVED" % (name, sname))
            continue
        r = audit(sname, structs)
        print("%s struct=%s risky=%s" % (name, sname, ["%s: %s" % x for x in r]))


if __name__ == "__main__":
    main()
