#!/usr/bin/env python3
# Sanitize ACE manifest JSON from `zcli --format=json edge-app show --detail`.
# That dump includes UI keys (__*) and JSON nulls. Putting them back in
# `edge-app update --manifest` yields BadReqBody / request body parsing failed.

import json
import os
import sys

DROP_KEYS = frozenset({"imagestatus"})
IF_KEYS = ("name", "type", "optional", "directattach", "privateip", "acls")


def sanitize(obj):
    if isinstance(obj, dict):
        out = {}
        for key, val in obj.items():
            if key.startswith("__") or key in DROP_KEYS or val is None:
                continue
            out[key] = sanitize(val)
        return out
    if isinstance(obj, list):
        return [sanitize(item) for item in obj]
    return obj


def interface_from_template(template, name):
    obj = {}
    if isinstance(template, dict):
        for key in IF_KEYS:
            if key in template:
                obj[key] = template[key]
    obj["name"] = name
    obj["directattach"] = True
    obj["acls"] = []
    obj.setdefault("type", "")
    obj.setdefault("optional", False)
    obj.setdefault("privateip", False)
    return obj


def load(path):
    with open(path) as f:
        return json.load(f)


def dump(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def show_config(data):
    if isinstance(data, dict) and "config" in data:
        return data["config"]
    return data


def live_interfaces(cfg):
    man = cfg.get("manifestJSON")
    if man is None:
        man = cfg.get("manifest")
    if isinstance(man, str):
        man = json.loads(man)
    if not isinstance(man, dict):
        man = {}
    return [
        i.get("name")
        for i in man.get("interfaces") or []
        if isinstance(i, dict) and i.get("name")
    ]


def local_interfaces(man):
    return [
        i.get("name")
        for i in man.get("interfaces") or []
        if isinstance(i, dict) and i.get("name")
    ]


def main(argv):
    if len(argv) == 2 and argv[1] == "sanitize":
        json.dump(sanitize(json.load(sys.stdin)), sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0
    if len(argv) == 3 and argv[1] == "sanitize-file":
        path = argv[2]
        dump(path, sanitize(load(path)))
        return 0
    if len(argv) == 4 and argv[1] == "clone":
        src, dst_name = argv[2], argv[3]
        if "/" in dst_name or dst_name in (".", "..") or " " in dst_name:
            sys.stderr.write("scripts/app_manifest.py: unsafe name %s\n" %
                             dst_name)
            return 1
        man = sanitize(load(src))
        man["name"] = dst_name
        dump(os.path.join(os.path.dirname(src), dst_name + ".json"), man)
        sys.stderr.write("scripts/app_manifest.py: cloned %s -> %s.json\n" %
                         (src, dst_name))
        return 0
    if len(argv) == 3 and argv[1] == "check-new-ifs":
        local = load(argv[2])
        cfg = show_config(json.load(sys.stdin))
        count = cfg.get("appInstCount") or 0
        live = live_interfaces(cfg)
        added = [n for n in local_interfaces(local) if n not in set(live)]
        sys.stderr.write(
            "scripts/app_manifest.py: appInstCount=%s live=%s added=%s\n" %
            (count, ",".join(live) or "(none)", ",".join(added) or "(none)")
        )
        if added and count:
            sys.stderr.write(
                "scripts/push_app.sh: gmwtus will not add %s while "
                "this edge-app has %s instance record(s), including Halted. "
                "Stop/deactivate is not enough. Do not delete those "
                "instances unless you accept losing their implicit volumes.\n" %
                (", ".join(added), count)
            )
            return 2
        return 0
    sys.stderr.write(
        "usage: scripts/app_manifest.py sanitize < in.json\n"
        "       scripts/app_manifest.py sanitize-file apps/NAME.json\n"
        "       scripts/app_manifest.py clone apps/SRC.json NEW-NAME\n"
        "       scripts/app_manifest.py check-new-ifs apps/NAME.json < show.json\n"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
