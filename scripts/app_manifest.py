#!/usr/bin/env python3
# Sanitize ACE manifest JSON from `zcli --format=json edge-app show --detail`.
# That dump includes UI keys (__*) and JSON nulls. Putting them back in
# `edge-app update --manifest` yields BadReqBody / request body parsing failed.

import base64
import glob
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


def custom_config_from_manifest(man):
    cfg = man.get("configuration")
    if not isinstance(cfg, dict):
        return None
    cc = cfg.get("customConfig")
    if not isinstance(cc, dict):
        return None
    template = cc.get("template") or ""
    if not template:
        return None
    # zcli instance create needs the full script here, not the marketplace
    # variable-group form. override/add must be true or EVE writes empty
    # CIDATA user-data.
    return {
        "name": cc.get("name") or "cloud-init",
        "add": True,
        "override": True,
        "allowStorageResize": bool(cc.get("allowStorageResize")),
        "fieldDelimiter": "",
        "template": template,
        "variableGroups": [],
    }


def harvest_ssh_keys():
    home = os.environ.get("HOME")
    if not home:
        sys.stderr.write("scripts/app_manifest.py: HOME environment variable not set\n")
        return []
    ssh_dir = os.path.join(home, ".ssh")
    keys = []

    # 1. Try authorized_keys first
    auth_keys_path = os.path.join(ssh_dir, "authorized_keys")
    if os.path.isfile(auth_keys_path):
        with open(auth_keys_path, "r") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    keys.append(line)

    # 2. If no keys in authorized_keys, look for public key files (*.pub)
    if not keys:
        pub_files = sorted(glob.glob(os.path.join(ssh_dir, "*.pub")))
        for pub_path in pub_files:
            with open(pub_path, "r") as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        keys.append(line)

    return keys


def generate_nohyper_custom_config(keys, out_path=None):
    if not keys:
        sys.stderr.write("scripts/app_manifest.py: no SSH public keys found in $HOME/.ssh/\n")
        return 1

    keys_block = "\n".join(keys)

    init_script = f"""#!/bin/sh
echo "`date` - /etc/init.sh starts" >> /var/log/init.sh.log
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends openssh-server sudo passwd

# User ubuntu setup
id ubuntu >/dev/null 2>&1 || useradd -m -s /bin/bash -G sudo ubuntu
echo "ubuntu:ubuntu" | chpasswd
passwd -u ubuntu || true
echo "ubuntu ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-ubuntu
chmod 0440 /etc/sudoers.d/90-ubuntu

# SSH directories
mkdir -p /run/sshd /etc/ssh/sshd_config.d /home/ubuntu/.ssh /root/.ssh
chmod 700 /home/ubuntu/.ssh /root/.ssh

# Dynamic SSH Key Injection
cat << 'KEY_EOF' > /home/ubuntu/.ssh/authorized_keys
{keys_block}
KEY_EOF
cp /home/ubuntu/.ssh/authorized_keys /root/.ssh/authorized_keys
chown -R ubuntu:ubuntu /home/ubuntu/.ssh
chmod 600 /home/ubuntu/.ssh/authorized_keys /root/.ssh/authorized_keys

# Root & Password authentication config
printf "PasswordAuthentication yes\\nKbdInteractiveAuthentication yes\\n" > /etc/ssh/sshd_config.d/00-passwordauth.conf
echo "PermitRootLogin yes" > /etc/ssh/sshd_config.d/root_login.conf

# Device node hygiene
[ -e /dev/ucode ] || mknod -m 666 /dev/ucode c 508 0 2>/dev/null || true

ssh-keygen -A
/usr/sbin/sshd
echo "`date` - /etc/init.sh ends" >> /var/log/init.sh.log
exec sleep infinity
"""

    cloud_config = """#cloud-config
runcmd:
  - EVE_ECO_CMD=/etc/init.sh
write_files:
  - path: /etc/init.sh
    permissions: "0755"
    owner: root:root
    content: |
"""
    for line in init_script.splitlines():
        if line:
            cloud_config += "      " + line + "\n"
        else:
            cloud_config += "\n"

    b64_template = base64.b64encode(cloud_config.encode("utf-8")).decode("ascii")

    cc = {
        "name": "cloud-init",
        "add": True,
        "override": True,
        "allowStorageResize": False,
        "fieldDelimiter": "###",
        "template": b64_template,
        "variableGroups": []
    }

    if out_path:
        dump(out_path, cc)
        sys.stderr.write(f"scripts/app_manifest.py: wrote dynamic custom config -> {out_path}\n")
    else:
        json.dump(cc, sys.stdout, indent=2)
        sys.stdout.write("\n")
    return 0


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
    if len(argv) in (3, 4) and argv[1] == "extract-custom-config":
        man = sanitize(load(argv[2]))
        cc = custom_config_from_manifest(man)
        if cc is None:
            sys.stderr.write(
                "scripts/app_manifest.py: no customConfig.template in %s\n" %
                argv[2]
            )
            return 2
        if len(argv) == 4:
            dump(argv[3], cc)
        else:
            json.dump(cc, sys.stdout, indent=2)
            sys.stdout.write("\n")
        return 0
    if len(argv) in (2, 3) and argv[1] == "generate-nohyper-custom-config":
        keys = harvest_ssh_keys()
        out_path = argv[2] if len(argv) == 3 else None
        return generate_nohyper_custom_config(keys, out_path)
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
        "       scripts/app_manifest.py extract-custom-config apps/NAME.json [out.json]\n"
        "       scripts/app_manifest.py generate-nohyper-custom-config [out.json]\n"
        "       scripts/app_manifest.py check-new-ifs apps/NAME.json < show.json\n"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
