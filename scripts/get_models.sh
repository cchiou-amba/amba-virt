#!/bin/sh
# Pull Ambarella hardware-details JSON from ZEDEDA Cloud into models/.
# ZCLI_TOKEN must already be in the environment. This script does not set it.
#
#   ./scripts/get_models.sh
#   ./scripts/get_models.sh N1-655-Cooper-Pro
#   ./scripts/get_models.sh --dry-run
#   ./scripts/get_models.sh --brand=Ambarella

set -eu

# Line-buffer stdout/stderr so --dry-run JSON is not stuck behind the next
# model's progress line when this script is not a TTY.
if [ ! -t 1 ] && command -v stdbuf >/dev/null 2>&1; then
	case "${GET_MODELS_STDBUF:-}" in
	1) ;;
	*) GET_MODELS_STDBUF=1 exec stdbuf -oL -eL "$0" "$@" ;;
	esac
fi

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"
MODELS="$ROOT/models"
BRAND=Ambarella
DRY_RUN=0

usage() {
	echo "usage: scripts/get_models.sh [--dry-run] [--brand=NAME] [MODEL...]" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/get_models.sh: set ZCLI_TOKEN (do not commit it)" >&2
	exit 1
fi

NAMES=
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
	--brand=*)
		BRAND=${1#--brand=}
		shift
		;;
	--brand)
		if [ "$#" -lt 2 ]; then
			echo "scripts/get_models.sh: --brand needs a name" >&2
			exit 1
		fi
		BRAND=$2
		shift 2
		;;
	--)
		shift
		break
		;;
	-*)
		echo "scripts/get_models.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		NAMES="$NAMES $1"
		shift
		;;
	esac
done
while [ "$#" -gt 0 ]; do
	NAMES="$NAMES $1"
	shift
done

if [ -z "$BRAND" ]; then
	echo "scripts/get_models.sh: --brand must not be empty" >&2
	exit 1
fi

have_jq() { command -v jq >/dev/null 2>&1; }
have_py() { command -v python3 >/dev/null 2>&1; }

if ! have_jq && ! have_py; then
	echo "scripts/get_models.sh: need jq or python3" >&2
	exit 1
fi

zcli_json() {
	"$ZCLI" -- --format=json "$@"
}

# Names of models for $BRAND (one per line).
list_names() {
	raw=$(zcli_json model show --brand="$BRAND") || return 1
	if have_jq; then
		printf '%s\n' "$raw" | jq -r '
			if type == "array" then .[].name
			elif has("list") then .list[].name
			elif has("config") then .config.name
			else .name
			end
		'
	else
		printf '%s\n' "$raw" | python3 -c '
import json, sys
data = json.load(sys.stdin)
if isinstance(data, list):
    names = [x.get("name") for x in data]
elif isinstance(data, dict) and "list" in data:
    names = [x.get("name") for x in data.get("list") or []]
elif isinstance(data, dict) and "config" in data:
    names = [data["config"].get("name")]
else:
    names = [data.get("name")]
for n in names:
    if n:
        print(n)
'
	fi
}

# Hardware-details subset (arch/productURL/productStatus/attr/ioMemberList).
reshape() {
	if have_jq; then
		jq '
			(if has("config") then .config else . end) as $c
			| if ($c | has("ioMemberList") | not) then
				error("missing ioMemberList")
			else
				{
					arch: ($c.arch // $c.type),
					productURL: $c.productURL,
					productStatus: $c.productStatus,
					attr: $c.attr,
					ioMemberList: $c.ioMemberList
				}
			end
		'
	else
		python3 -c '
import json, sys
data = json.load(sys.stdin)
cfg = data["config"] if isinstance(data, dict) and "config" in data else data
if not isinstance(cfg, dict) or "ioMemberList" not in cfg:
    sys.stderr.write("scripts/get_models.sh: missing ioMemberList\n")
    sys.exit(1)
out = {
    "arch": cfg["arch"] if "arch" in cfg else cfg.get("type"),
    "productURL": cfg.get("productURL"),
    "productStatus": cfg.get("productStatus"),
    "attr": cfg.get("attr"),
    "ioMemberList": cfg["ioMemberList"],
}
json.dump(out, sys.stdout, indent=2)
sys.stdout.write("\n")
'
	fi
}

safe_name() {
	# Cloud model names are used as filenames under models/.
	case "$1" in
	""|*/*|*..*|*" "*|*"	"*)
		echo "scripts/get_models.sh: unsafe model name: $1" >&2
		return 1
		;;
	esac
	return 0
}

if [ -z "$NAMES" ]; then
	NAMES=$(list_names) || {
		echo "scripts/get_models.sh: failed to list models for brand $BRAND" >&2
		exit 1
	}
fi

# shellcheck disable=SC2086
set -- $NAMES
if [ "$#" -eq 0 ]; then
	echo "scripts/get_models.sh: no models found for brand $BRAND" >&2
	exit 1
fi

mkdir -p "$MODELS"

wrote=0
for name in "$@"; do
	safe_name "$name" || exit 1
	echo "scripts/get_models.sh: $name" >&2
	raw=$(zcli_json model show "$name" --detail) || {
		echo "scripts/get_models.sh: failed to show $name" >&2
		exit 1
	}
	hw=$(printf '%s\n' "$raw" | reshape) || {
		echo "scripts/get_models.sh: failed to reshape $name" >&2
		exit 1
	}
	dest="$MODELS/$name.json"
	if [ "$DRY_RUN" -eq 1 ]; then
		echo "scripts/get_models.sh: would write $dest" >&2
		printf '%s\n' "$hw"
	else
		tmp=$(mktemp "$dest.tmp.XXXXXX")
		printf '%s\n' "$hw" >"$tmp"
		mv "$tmp" "$dest"
		echo "scripts/get_models.sh: wrote $dest" >&2
	fi
	wrote=$((wrote + 1))
done

echo "scripts/get_models.sh: $wrote model(s)" >&2
