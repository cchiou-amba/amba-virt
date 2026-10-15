#!/bin/bash
# Lean Debian 13 AArch64 HVM Deployment Pipeline
# Ambarella EVE Platform
#
# Stages the QCOW2 into the HTTP datastore, registers the image and the edge-app in ZedControl, creates the instance
# on n1-655-pro, waits for SSH and prints the memory the idle guest uses. Guest drivers and the demo are deployed
# afterwards with guest-os/deploy_guest.sh (see README.md).

set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
DIST_DIR="$SCRIPT_DIR/output/dist"
QCOW2_IMAGE="$DIST_DIR/debian-13-arm64-cloudimg.qcow2"
SHA_FILE="$DIST_DIR/debian-13-arm64-cloudimg.qcow2.sha256"
ZCLI="$ROOT_DIR/scripts/zcli"
CLONE_APP="$ROOT_DIR/scripts/clone_app.sh"
CREATE_APP="$ROOT_DIR/scripts/create_app.sh"
PUSH_APP="$ROOT_DIR/scripts/push_app.sh"
CREATE_INST="$ROOT_DIR/scripts/create_instance.sh"

APP_NAME="debian_13"
APP_JSON="$ROOT_DIR/apps/$APP_NAME.json"
IMAGE_NAME="debian-13-trixie-arm64-hvm"
NODE="${AMBA_NODE:-n1-655-pro}"   # AMBA_NODE, AMBA_SSH_ALIAS, AMBA_MODEL_NAME deploy the same image to the other board
INSTANCE="$APP_NAME.$NODE"
SSH_ALIAS="${AMBA_SSH_ALIAS:-n1-655-pro-debian}"
# Accelerator memory: one admission ticket. amba-virt-server grants the Cavalry slice and the private IAV slice, and
# attaches the shared camera windows when the ticket's camera policy says so (doc/EVE-Ambarella-Models.md). The digit of
# the ticket is a label for the controller's one-instance-per-group rule, not a slice index.
ADAPTERS=(amba_shm:amba_shm2)
SSH_HOST_PORT=2522
DEMO_PORT=8082
VOLUME_MAXSIZE_KB=4194304   # 4 GiB, as the Alpine guest; the Ubuntu template asks for 10 GiB, more than the pro board had free
GUEST_USER="debian"
GUEST_PASSWORD="debian"
MODEL_NAME="${AMBA_MODEL_NAME:-N1-655-Cooper-Pro}"
MEASURE_DIR="$SCRIPT_DIR/output/measurements"

DRY_RUN=0
DATASTORE_NAME=""
SKIP_INSTANCE=0

usage() {
    cat << 'EOF'
usage: deploy.sh [OPTIONS]

Stage, register and deploy the lean Debian 13 HVM image on n1-655-pro.

Options:
  --datastore=NAME  Target HTTP datastore in ZedControl (auto-detected if omitted)
  --skip-instance   Stop after the edge-app is created or updated (no instance)
  --dry-run         Display commands without mutating cloud or node
  -h, --help        Show this help message
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --datastore=*) DATASTORE_NAME="${1#--datastore=}"; shift ;;
        --skip-instance) SKIP_INSTANCE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "deploy.sh: unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

echo "=== Validating deployment prerequisites ==="
if [ -z "${ZCLI_TOKEN:-}" ]; then
    echo "deploy.sh: ZCLI_TOKEN is unset in environment. Export ZCLI_TOKEN before running." >&2
    exit 1
fi
for tool in jq python3 sha256sum sshpass; do
    command -v "$tool" >/dev/null 2>&1 || { echo "deploy.sh: required tool '$tool' not found" >&2; exit 1; }
done
if [ ! -f "$QCOW2_IMAGE" ] || [ ! -f "$SHA_FILE" ]; then
    echo "deploy.sh: build artifact missing: $QCOW2_IMAGE (run 'make guest-debian-image' first)" >&2
    exit 1
fi
IMG_SHA=$(awk '{print $1}' "$SHA_FILE")
IMG_SIZE=$(stat -c %s "$QCOW2_IMAGE")
[ "$(sha256sum "$QCOW2_IMAGE" | awk '{print $1}')" = "$IMG_SHA" ] || { echo "deploy.sh: checksum file does not match the image" >&2; exit 1; }
echo "Image Artifact: $QCOW2_IMAGE"
echo "Image Size    : $IMG_SIZE bytes"
echo "Image SHA-256 : $IMG_SHA"

if [ "$SKIP_INSTANCE" -eq 0 ]; then
    echo "=== Checking that the controller's model has the adapters of this guest and that no running guest holds them ==="
    MODEL_JSON=$("$ZCLI" -- --format=json model show "$MODEL_NAME" --detail 2>/dev/null)
    for spec in "${ADAPTERS[@]}"; do
        label="${spec#*:}"
        if ! echo "$MODEL_JSON" | jq -e --arg a "$label" '[.. | objects | select(.logicallabel? == $a)] | length > 0' >/dev/null; then
            echo "deploy.sh: STOP: model $MODEL_NAME has no adapter '$label'." >&2
            exit 1
        fi
    done
    # One instance holds an assigngrp at a time: another online instance with one of our adapters must be stopped first
    # (./scripts/restart_instance.sh NAME --stop).
    while IFS=$'\t' read -r other _ _ state; do
        [ -n "$other" ] && [ "$other" != "$INSTANCE" ] && [ "$state" = "Online" ] || continue
        for spec in "${ADAPTERS[@]}"; do
            label="${spec#*:}"
            if "$ROOT_DIR/scripts/show_instances.sh" "$other" 2>/dev/null | awk -v l="$label" '$2 == "adapter" && $3 == l {f = 1} END {exit !f}'; then
                if [ "$DRY_RUN" -eq 1 ]; then
                    echo "[DRY-RUN] $other is online and holds adapter '$label'; a real run stops here until it is stopped (./scripts/restart_instance.sh $other --stop)"
                    break
                fi
                echo "deploy.sh: STOP: $other is online and holds adapter '$label'. Stop it first: ./scripts/restart_instance.sh $other --stop" >&2
                exit 1
            fi
        done
    done < <("$ROOT_DIR/scripts/show_instances.sh" --edge-node="$NODE" 2>/dev/null)
fi

echo "=== Resolving ZedControl HTTP Datastore ==="
if [ -z "$DATASTORE_NAME" ]; then
    DATASTORE_NAME="${ZCLI_DATASTORE:-}"
fi
if [ -z "$DATASTORE_NAME" ]; then
    DATASTORE_NAME=$("$ZCLI" -- --format=json datastore show 2>/dev/null | jq -r '
        .list[] | select(.originType == "ORIGIN_LOCAL" and .dsType == "DATASTORE_TYPE_HTTP") | .name' | head -n 1 || true)
fi
if [ -z "$DATASTORE_NAME" ]; then
    echo "deploy.sh: could not auto-detect the local HTTP datastore. Specify with --datastore=NAME." >&2
    exit 1
fi
DS_DETAIL=$("$ZCLI" -- --format=json datastore show "$DATASTORE_NAME" --detail 2>/dev/null || true)
[ -n "$DS_DETAIL" ] || { echo "deploy.sh: failed to fetch details for datastore '$DATASTORE_NAME'" >&2; exit 1; }
DATASTORE_ID=$(echo "$DS_DETAIL" | jq -r '.config.id // .id')
DS_PATH=$(echo "$DS_DETAIL" | jq -r '.config.dsPath // .dsPath // ""')
echo "Resolved Datastore: $DATASTORE_NAME (ID: $DATASTORE_ID)"

STAGING_DIR="${LOCAL_DATASTORE_DIR:-${HTTP_DATASTORE_DIR:-}}"
if [ -z "$STAGING_DIR" ]; then
    if [ -n "$DS_PATH" ] && [[ "$DS_PATH" == "~"* ]]; then
        subpath="${DS_PATH#~*/}"
        if [ "$subpath" = "$DS_PATH" ]; then STAGING_DIR="${HOME}/public_html"; else STAGING_DIR="${HOME}/public_html/$subpath"; fi
    elif [ -d "$HOME/public_html" ]; then
        STAGING_DIR="$HOME/public_html"
    elif [ -d "/var/www/html" ]; then
        STAGING_DIR="/var/www/html"
    else
        STAGING_DIR="$HOME/public_html"
    fi
fi
TARGET_STAGED_FILE="$STAGING_DIR/$(basename "$QCOW2_IMAGE")"
RELATIVE_IMG_URL="$(basename "$QCOW2_IMAGE")"
echo "Staging Destination: $TARGET_STAGED_FILE"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[DRY-RUN] Staging: cp $QCOW2_IMAGE $TARGET_STAGED_FILE"
    echo "[DRY-RUN] Register image $IMAGE_NAME on datastore $DATASTORE_NAME"
    echo "[DRY-RUN] Create or update apps/$APP_NAME.json and the edge-app $APP_NAME"
    echo "[DRY-RUN] Create instance $INSTANCE (adapters ${ADAPTERS[*]}, ssh $SSH_HOST_PORT, demo $DEMO_PORT)"
    echo "[DRY-RUN] Dry run completed."
    exit 0
fi

echo "=== Staging QCOW2 into HTTP Datastore ==="
mkdir -p "$STAGING_DIR"
if [ -f "$TARGET_STAGED_FILE" ] && [ "$(sha256sum "$TARGET_STAGED_FILE" | awk '{print $1}')" = "$IMG_SHA" ]; then
    echo "Staged image already exists and matches checksum. Skipping copy."
else
    cp -f "$QCOW2_IMAGE" "$TARGET_STAGED_FILE"
fi

echo "=== Registering / Uplinking Image in ZedControl ==="
# zcli prints a 404 record (and exits 0) for an unknown image, so test for an id, not for output
EXISTING_IMG=$("$ZCLI" -- --format=json image show "$IMAGE_NAME" --detail 2>/dev/null | jq -r '.config.id // .id // empty' || true)
if [ -z "$EXISTING_IMG" ]; then
    "$ZCLI" -- image create "$IMAGE_NAME" \
        --title="Lean Debian 13 ARM64 HVM" \
        --type=Application \
        --image-format=qcow2 \
        --arch=ARM64 \
        --datastore-name="$DATASTORE_NAME" \
        --image-url="$RELATIVE_IMG_URL"
fi
"$ZCLI" -- image uplink "$IMAGE_NAME" \
    --datastore-name="$DATASTORE_NAME" \
    --image-url="$RELATIVE_IMG_URL" \
    --image-sha="$IMG_SHA" \
    --image-size="$IMG_SIZE"
IMAGE_ID=$("$ZCLI" -- --format=json image show "$IMAGE_NAME" --detail | jq -r '.config.id // .id')
echo "Resolved Image ID: $IMAGE_ID"

echo "=== Writing edge-app manifest $APP_JSON ==="
if [ ! -f "$APP_JSON" ]; then
    "$CLONE_APP" ubuntu_24_04 "$APP_NAME"
fi
python3 - << EOF
import json

app_file = "$APP_JSON"
with open(app_file, "r") as f:
    data = json.load(f)

data["name"] = "$APP_NAME"
for img in data.get("images", []):
    img["imagename"] = "$IMAGE_NAME"
    img["imageid"] = "$IMAGE_ID"
    img["datastore"] = [{"id": "$DATASTORE_ID", "name": "$DATASTORE_NAME"}]
    img["maxsize"] = "$VOLUME_MAXSIZE_KB"   # volume the node must admit; the root grows to it at first boot

# Port forwards: ssh and the demo
for iface in data.get("interfaces", []):
    if iface.get("name") != "eth0":
        continue
    for acl in iface.get("acls", []):
        lport = None
        for m in acl.get("matches", []):
            if m.get("type") == "lport":
                lport = m["value"]
        if lport == "2222":
            for m in acl["matches"]:
                if m.get("type") == "lport":
                    m["value"] = "$SSH_HOST_PORT"
        elif lport == "8080":
            for m in acl["matches"]:
                if m.get("type") == "lport":
                    m["value"] = "$DEMO_PORT"
            for act in acl["actions"]:
                act["portmapto"] = {"appPort": $DEMO_PORT}

# Resources: 2 CPUs and 512 MiB until the measured value is applied
for res in data.get("resources", []):
    if res.get("name") == "cpus":
        res["value"] = "2"
    if res.get("name") == "memory":
        res["value"] = "524288.00"

# No cloud-init: the image has none
if "configuration" in data and "customConfig" in data["configuration"]:
    data["configuration"]["customConfig"]["add"] = False
    data["configuration"]["customConfig"]["template"] = ""

with open(app_file, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
print("Manifest written.")
EOF

echo "=== Creating or updating the edge-app $APP_NAME ==="
if "$ZCLI" -- edge-app show "$APP_NAME" >/dev/null 2>&1; then
    "$PUSH_APP" "$APP_NAME"
else
    "$CREATE_APP" "$APP_NAME" --version=1.0
fi

if [ "$SKIP_INSTANCE" -eq 1 ]; then
    echo "=== Stopping after the edge-app (--skip-instance) ==="
    exit 0
fi

delete_instance_wait() {
    local inst="$1"
    if "$ZCLI" -- --format=json edge-app-instance show "$inst" >/dev/null 2>&1; then
        echo "Deleting existing instance '$inst'..."
        "$ZCLI" -- edge-app-instance delete "$inst" -f || true
        local tries=0
        while [ "$tries" -lt 60 ]; do
            if ! "$ZCLI" -- edge-app-instance show "$inst" >/dev/null 2>&1; then
                echo "Instance '$inst' deleted."
                break
            fi
            sleep 3
            tries=$((tries + 1))
        done
    fi
}

wait_ssh() {
    local host_alias="$1" timeout_secs="$2" elapsed=0
    echo "Waiting for SSH on $host_alias (timeout: ${timeout_secs}s)..."
    while [ "$elapsed" -lt "$timeout_secs" ]; do
        if ssh -o ConnectTimeout=5 -o BatchMode=yes "$host_alias" "echo connected" >/dev/null 2>&1; then
            echo "SSH on $host_alias is available."
            return 0
        elif sshpass -p "$GUEST_PASSWORD" ssh -o ConnectTimeout=5 "$host_alias" "mkdir -p ~/.ssh && chmod 700 ~/.ssh" >/dev/null 2>&1; then
            echo "Password login succeeded on $host_alias. Authorizing the SSH key..."
            sshpass -p "$GUEST_PASSWORD" ssh-copy-id -i ~/.ssh/id_rsa.ambarella.pub "$host_alias" >/dev/null 2>&1 || true
        fi
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo "Timeout waiting for SSH on $host_alias" >&2
    return 1
}

create_instance() {
    local args=()
    for spec in "${ADAPTERS[@]}"; do args+=(--adapter="$spec"); done
    "$CREATE_INST" "$INSTANCE" \
        --edge-app="$APP_NAME" \
        --edge-node="$NODE" \
        --network-instance=eth0:defaultLocal-$NODE \
        "${args[@]}" \
        --no-custom-configuration
}

echo "=== Creating the 512 MiB baseline instance $INSTANCE ==="
delete_instance_wait "$INSTANCE"
create_instance
wait_ssh "$SSH_ALIAS" 300

echo "=== Measuring the idle guest (waiting 300 s for the system to settle) ==="
sleep 300
mkdir -p "$MEASURE_DIR"
MEMINFO_LOG="$MEASURE_DIR/meminfo_$(date +'%Y%m%d_%H%M%S').txt"
ssh "$SSH_ALIAS" "cat /proc/meminfo; free -k; systemctl --failed" > "$MEMINFO_LOG"
MEM_TOTAL=$(grep "MemTotal:" "$MEMINFO_LOG" | awk '{print $2}')
MEM_AVAIL=$(grep "MemAvailable:" "$MEMINFO_LOG" | awk '{print $2}')
IDLE_USED=$((MEM_TOTAL - MEM_AVAIL))
TARGET_MEM=$((IDLE_USED + 262144))
REMAINDER=$((TARGET_MEM % 65536))
if [ "$REMAINDER" -ne 0 ]; then
    TARGET_MEM=$((TARGET_MEM + 65536 - REMAINDER))
fi
if [ "$TARGET_MEM" -lt 524288 ]; then
    TARGET_MEM=524288
fi
echo "MemTotal     : $MEM_TOTAL KiB"
echo "MemAvailable : $MEM_AVAIL KiB"
echo "Idle Used    : $IDLE_USED KiB"
echo "Computed Mem : $TARGET_MEM KiB (idle used + 256 MiB headroom, rounded up to 64 MiB, at least 512 MiB)"

if [ "$TARGET_MEM" -ne 524288 ]; then
    echo "=== Applying resources.memory = $TARGET_MEM KiB and recreating the instance ==="
    python3 - << EOF
import json
with open("$APP_JSON") as f:
    data = json.load(f)
for res in data.get("resources", []):
    if res.get("name") == "memory":
        res["value"] = "%.2f" % $TARGET_MEM
with open("$APP_JSON", "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
EOF
    delete_instance_wait "$INSTANCE"
    "$PUSH_APP" "$APP_NAME"
    create_instance
    wait_ssh "$SSH_ALIAS" 300
fi

echo "=== Deployment Complete: $INSTANCE ==="
