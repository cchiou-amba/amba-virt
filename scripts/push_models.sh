#!/bin/sh
# Push Ambarella hardware-details JSON from models/ back to ZEDEDA Cloud.
# Inverse of get_models.sh. ZCLI_TOKEN must already be in the environment.
#
#   ./scripts/push_models.sh
#   ./scripts/push_models.sh N1-655-Cooper-Pro
#   ./scripts/push_models.sh --dry-run
#   ./scripts/push_models.sh --brand=Ambarella
#
# models/ is bind-mounted read-only into the zcli container at
# /home/zcli/models, so --hardware-details refers to that path, not the host
# path. Note the controller does not preserve ioMemberList order: a re-pull
# after a push will usually reorder entries even though nothing changed.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"
MODELS="$ROOT/models"
CTR_MODELS=/home/zcli/models
BRAND=Ambarella
DRY_RUN=0

usage() {
	echo "usage: scripts/push_models.sh [--dry-run] [--brand=NAME] [MODEL...]" >&2
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/push_models.sh: set ZCLI_TOKEN (do not commit it)" >&2
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
			echo "scripts/push_models.sh: --brand needs a name" >&2
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
		echo "scripts/push_models.sh: unknown option: $1" >&2
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
	echo "scripts/push_models.sh: --brand must not be empty" >&2
	exit 1
fi

have_jq() { command -v jq >/dev/null 2>&1; }
have_py() { command -v python3 >/dev/null 2>&1; }

if ! have_jq && ! have_py; then
	echo "scripts/push_models.sh: need jq or python3" >&2
	exit 1
fi

safe_name() {
	# Cloud model names are used as filenames under models/.
	case "$1" in
	""|*/*|*..*|*" "*|*"	"*)
		echo "scripts/push_models.sh: unsafe model name: $1" >&2
		return 1
		;;
	esac
	return 0
}

# Reject anything the controller would refuse before spending a round trip on
# it. get_models.sh writes exactly these five keys.
validate() {
	if have_jq; then
		jq -e '
			if (has("ioMemberList") | not) then
				error("missing ioMemberList")
			elif (.ioMemberList | type) != "array" then
				error("ioMemberList is not an array")
			elif (.ioMemberList | length) == 0 then
				error("ioMemberList is empty")
			elif (has("arch") | not) then
				error("missing arch")
			else
				true
			end
		' >/dev/null
	else
		python3 -c '
import json, sys
data = json.load(sys.stdin)
if not isinstance(data, dict):
    sys.stderr.write("not a JSON object\n"); sys.exit(1)
iml = data.get("ioMemberList")
if not isinstance(iml, list) or not iml:
    sys.stderr.write("missing or empty ioMemberList\n"); sys.exit(1)
if "arch" not in data:
    sys.stderr.write("missing arch\n"); sys.exit(1)
'
	fi
}

# Names of models for $BRAND (one per line), used when no names are given.
list_names() {
	raw=$("$ZCLI" -- --format=json model show --brand="$BRAND") || return 1
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

if [ -z "$NAMES" ]; then
	# Only push models we actually have a local file for.
	remote=$(list_names) || {
		echo "scripts/push_models.sh: failed to list models for brand $BRAND" >&2
		exit 1
	}
	for name in $remote; do
		if [ -f "$MODELS/$name.json" ]; then
			NAMES="$NAMES $name"
		else
			echo "scripts/push_models.sh: skipping $name (no local models/$name.json)" >&2
		fi
	done
fi

# shellcheck disable=SC2086
set -- $NAMES
if [ "$#" -eq 0 ]; then
	echo "scripts/push_models.sh: nothing to push" >&2
	exit 1
fi

pushed=0
for name in "$@"; do
	safe_name "$name" || exit 1
	src="$MODELS/$name.json"
	if [ ! -f "$src" ]; then
		echo "scripts/push_models.sh: no such file: $src" >&2
		exit 1
	fi
	if ! validate <"$src"; then
		echo "scripts/push_models.sh: $src failed validation" >&2
		exit 1
	fi
	if [ "$DRY_RUN" -eq 1 ]; then
		echo "scripts/push_models.sh: would push $src" >&2
		echo "  scripts/zcli -- model update $name --hardware-details=$CTR_MODELS/$name.json" >&2
	else
		echo "scripts/push_models.sh: $name" >&2
		"$ZCLI" -- model update "$name" \
			--hardware-details="$CTR_MODELS/$name.json" || {
			echo "scripts/push_models.sh: failed to update $name" >&2
			exit 1
		}
		echo "scripts/push_models.sh: pushed $src" >&2
	fi
	pushed=$((pushed + 1))
done

echo "scripts/push_models.sh: $pushed model(s)" >&2
