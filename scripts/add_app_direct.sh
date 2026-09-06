#!/bin/sh
# Add Other/direct-attach interfaces to a local apps/<Name>.json.
# Does not talk to the controller. ZCLI_TOKEN is not required.
#
#   ./scripts/add_app_direct.sh ubuntu_24_04-container \
#     --if=cavalry --if=gpio0 --if=iav

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
APPS="$ROOT/apps"
DRY_RUN=0
FORCE=0
NAME=
IFS_ADD=

usage() {
	echo "usage: scripts/add_app_direct.sh EDGE-APP --if=NAME... [--dry-run] [--force]" >&2
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	-h|--help)
		usage
		exit 0
		;;
	--dry-run)
		DRY_RUN=1
		shift
		;;
	--force)
		FORCE=1
		shift
		;;
	--if=*)
		IFS_ADD="$IFS_ADD ${1#--if=}"
		shift
		;;
	--if)
		if [ "$#" -lt 2 ]; then
			echo "scripts/add_app_direct.sh: --if needs a name" >&2
			exit 1
		fi
		IFS_ADD="$IFS_ADD $2"
		shift 2
		;;
	-*)
		echo "scripts/add_app_direct.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -n "$NAME" ]; then
			echo "scripts/add_app_direct.sh: only one edge-app name" >&2
			exit 1
		fi
		NAME=$1
		shift
		;;
	esac
done

if [ -z "$NAME" ] || [ -z "$IFS_ADD" ]; then
	usage
	exit 1
fi

src="$APPS/$NAME.json"
if [ ! -f "$src" ]; then
	echo "scripts/add_app_direct.sh: missing $src (run pull_app.sh first)" >&2
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "scripts/add_app_direct.sh: need python3" >&2
	exit 1
fi

# shellcheck disable=SC2086
export ADD_APP_DIRECT_FILE="$src"
export ADD_APP_DIRECT_DRY="$DRY_RUN"
export ADD_APP_DIRECT_FORCE="$FORCE"
export ADD_APP_DIRECT_IFS="$IFS_ADD"
export ADD_APP_DIRECT_LIB="$ROOT/scripts/app_manifest.py"
python3 << 'PY'
import json, os, sys, importlib.util

spec = importlib.util.spec_from_file_location(
    "app_manifest", os.environ["ADD_APP_DIRECT_LIB"])
am = importlib.util.module_from_spec(spec)
spec.loader.exec_module(am)

path = os.environ["ADD_APP_DIRECT_FILE"]
dry = os.environ.get("ADD_APP_DIRECT_DRY") == "1"
force = os.environ.get("ADD_APP_DIRECT_FORCE") == "1"
names = [n for n in os.environ.get("ADD_APP_DIRECT_IFS", "").split() if n]

man = am.sanitize(am.load(path))

vmmode = str(man.get("vmmode") or "")
if "HVM" in vmmode and "NOHYPER" not in vmmode and not force:
    sys.stderr.write(
        "scripts/add_app_direct.sh: vmmode %s looks like HVM; "
        "do not add VisORC interfaces (pass --force to override)\n" % vmmode
    )
    sys.exit(1)

ifs = man.get("interfaces")
if not isinstance(ifs, list):
    ifs = []
    man["interfaces"] = ifs

template = None
for i in ifs:
    if isinstance(i, dict) and i.get("name") == "eth0":
        template = i
        break
if template is None:
    for i in ifs:
        if isinstance(i, dict):
            template = i
            break

have = {i.get("name") for i in ifs if isinstance(i, dict)}
added = []
for name in names:
    if name in have:
        sys.stderr.write("scripts/add_app_direct.sh: already has %s\n" % name)
        continue
    obj = am.interface_from_template(template, name)
    ifs.append(obj)
    have.add(name)
    added.append(name)

if dry:
    print(json.dumps({"would_add": added, "interfaces": ifs}, indent=2))
    sys.exit(0)

am.dump(path, man)
sys.stderr.write("scripts/add_app_direct.sh: added %s to %s\n" %
                 (", ".join(added) if added else "(none)", path))
PY
