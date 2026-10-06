#!/bin/bash
# Lean Ubuntu 24.04 AArch64 HVM Deployment & Qualification Pipeline
# Ambarella EVE Platform

set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
DIST_DIR="$SCRIPT_DIR/output/dist"
QCOW2_IMAGE="$DIST_DIR/ubuntu-24.04-arm64-cloudimg.qcow2"
SHA_FILE="$DIST_DIR/ubuntu-24.04-arm64-cloudimg.qcow2.sha256"
MANIFEST_FILE="$DIST_DIR/build-manifest.json"
APP_JSON="$ROOT_DIR/apps/ubuntu_24_04.json"
ZCLI="$ROOT_DIR/scripts/zcli"
PUSH_APP="$ROOT_DIR/scripts/push_app.sh"
CREATE_INST="$ROOT_DIR/scripts/create_instance.sh"
ARTIFACTS_DIR="$ROOT_DIR/plan/plan_custom_ubuntu_hvm_image.artifacts"

DRY_RUN=0
DATASTORE_NAME=""
IMAGE_NAME="ubuntu-24.04-server-cloudimg-arm64"

usage() {
    cat << 'EOF'
usage: deploy.sh [OPTIONS]

Stage, register, deploy, and qualify the lean Ubuntu 24.04 HVM image on EVE nodes.

Options:
  --datastore=NAME  Target HTTP datastore in ZedControl (auto-detected if omitted)
  --dry-run         Display commands and verification plan without mutating cloud or nodes
  -h, --help        Show this help message
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --datastore=*)
            DATASTORE_NAME="${1#--datastore=}"
            shift
            ;;
        --datastore)
            if [ "$#" -lt 2 ]; then
                echo "deploy.sh: --datastore requires an argument" >&2
                exit 1
            fi
            DATASTORE_NAME="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "deploy.sh: unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            echo "deploy.sh: unexpected positional argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

echo "=== Validating deployment prerequisites ==="
if [ -z "${ZCLI_TOKEN:-}" ]; then
    echo "deploy.sh: ZCLI_TOKEN is unset in environment. Export ZCLI_TOKEN before running." >&2
    exit 1
fi

if [ ! -f "$QCOW2_IMAGE" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "[DRY-RUN] Image artifact '$QCOW2_IMAGE' not built yet. Using placeholder metadata for dry run."
        IMG_SHA="0000000000000000000000000000000000000000000000000000000000000000"
        IMG_SIZE=620224512
    else
        echo "deploy.sh: build artifact missing: $QCOW2_IMAGE" >&2
        echo "Run 'make guest-ubuntu-image' first." >&2
        exit 1
    fi
else
    if [ ! -f "$SHA_FILE" ]; then
        echo "deploy.sh: checksum file missing: $SHA_FILE" >&2
        exit 1
    fi
    IMG_SHA=$(awk '{print $1}' "$SHA_FILE")
    IMG_SIZE=$(stat -c %s "$QCOW2_IMAGE")
fi

echo "Image Artifact: $QCOW2_IMAGE"
echo "Image Size    : $IMG_SIZE bytes"
echo "Image SHA-256 : $IMG_SHA"

# 1. Resolve HTTP Datastore via zcli
echo "=== Resolving ZedControl HTTP Datastore ==="
if [ -z "$DATASTORE_NAME" ]; then
    DATASTORE_NAME="${ZCLI_DATASTORE:-}"
fi

if [ -z "$DATASTORE_NAME" ]; then
    echo "Querying ZedControl for local HTTP datastore..."
    DATASTORE_NAME=$("$ZCLI" -- --format=json datastore show 2>/dev/null | jq -r '
        .list[]
        | select(.originType == "ORIGIN_LOCAL" and .dsType == "DATASTORE_TYPE_HTTP")
        | .name' | head -n 1 || true)
fi

if [ -z "$DATASTORE_NAME" ]; then
    echo "deploy.sh: could not auto-detect local HTTP datastore. Specify with --datastore=NAME." >&2
    exit 1
fi

DS_DETAIL=$("$ZCLI" -- --format=json datastore show "$DATASTORE_NAME" --detail 2>/dev/null || true)
if [ -z "$DS_DETAIL" ]; then
    echo "deploy.sh: failed to fetch details for datastore '$DATASTORE_NAME'" >&2
    exit 1
fi

DATASTORE_ID=$(echo "$DS_DETAIL" | jq -r '.config.id // .id')
DS_PATH=$(echo "$DS_DETAIL" | jq -r '.config.dsPath // .dsPath // ""')
DS_FQDN=$(echo "$DS_DETAIL" | jq -r '.config.dsFQDN // .config.fqdn // .dsFQDN // .fqdn // ""')

echo "Resolved Datastore: $DATASTORE_NAME (ID: $DATASTORE_ID)"
echo "Datastore FQDN    : $DS_FQDN"
echo "Datastore Path    : $DS_PATH"

# Resolve local staging directory
STAGING_DIR="${LOCAL_DATASTORE_DIR:-${HTTP_DATASTORE_DIR:-}}"
if [ -z "$STAGING_DIR" ]; then
    if [ -n "$DS_PATH" ] && [[ "$DS_PATH" == "~"* ]]; then
        subpath="${DS_PATH#~*/}"
        if [ "$subpath" = "$DS_PATH" ]; then
            STAGING_DIR="${HOME}/public_html"
        else
            STAGING_DIR="${HOME}/public_html/$subpath"
        fi
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
echo "Image Relative URL : $RELATIVE_IMG_URL"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[DRY-RUN] Staging: cp $QCOW2_IMAGE $TARGET_STAGED_FILE"
    echo "[DRY-RUN] Register image in ZedControl on datastore $DATASTORE_NAME"
    echo "[DRY-RUN] Update $APP_JSON with image/datastore IDs and remove cloud-init"
    echo "[DRY-RUN] Dry run completed."
    exit 0
fi

# 2. Stage QCOW2 into local HTTP datastore
echo "=== Staging QCOW2 into HTTP Datastore ==="
mkdir -p "$STAGING_DIR"
if [ -f "$TARGET_STAGED_FILE" ]; then
    EXISTING_SHA=$(sha256sum "$TARGET_STAGED_FILE" | awk '{print $1}')
    if [ "$EXISTING_SHA" = "$IMG_SHA" ]; then
        echo "Staged image already exists and matches checksum. Skipping copy."
    else
        echo "Updating staged image: $QCOW2_IMAGE -> $TARGET_STAGED_FILE"
        cp -f "$QCOW2_IMAGE" "$TARGET_STAGED_FILE"
    fi
else
    echo "Copying $QCOW2_IMAGE -> $TARGET_STAGED_FILE"
    cp -f "$QCOW2_IMAGE" "$TARGET_STAGED_FILE"
fi

# 3. Create or update image record in ZedControl
echo "=== Registering / Uplinking Image in ZedControl ==="
EXISTING_IMG=$("$ZCLI" -- --format=json image show "$IMAGE_NAME" 2>/dev/null || true)
if [ -z "$EXISTING_IMG" ]; then
    echo "Creating image record '$IMAGE_NAME'..."
    "$ZCLI" -- image create "$IMAGE_NAME" \
        --title="Lean Ubuntu 24.04 ARM64 HVM" \
        --type=VM \
        --image-format=qcow2 \
        --arch=ARM64 \
        --datastore-name="$DATASTORE_NAME" \
        --image-url="$RELATIVE_IMG_URL"
fi

echo "Uplinking image record '$IMAGE_NAME' with SHA-256..."
"$ZCLI" -- image uplink "$IMAGE_NAME" \
    --datastore-name="$DATASTORE_NAME" \
    --image-url="$RELATIVE_IMG_URL" \
    --image-sha="$IMG_SHA" \
    --image-size="$IMG_SIZE"

IMAGE_DETAIL=$("$ZCLI" -- --format=json image show "$IMAGE_NAME" --detail)
IMAGE_ID=$(echo "$IMAGE_DETAIL" | jq -r '.config.id // .id')
echo "Resolved Image ID: $IMAGE_ID"

# 4. Update apps/ubuntu_24_04.json
echo "=== Updating edge-app manifest $APP_JSON ==="
python3 - << EOF
import json

app_file = "$APP_JSON"
with open(app_file, "r") as f:
    data = json.load(f)

# Update image and datastore references
for img in data.get("images", []):
    img["imagename"] = "$IMAGE_NAME"
    img["imageid"] = "$IMAGE_ID"
    img["datastore"] = [{"id": "$DATASTORE_ID", "name": "$DATASTORE_NAME"}]

# Remove cloud-init template and disable customConfig
if "configuration" in data and "customConfig" in data["configuration"]:
    data["configuration"]["customConfig"]["add"] = False
    data["configuration"]["customConfig"]["template"] = ""

with open(app_file, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
print("Manifest updated successfully.")
EOF

# 5. Push edge app
echo "=== Pushing edge-app manifest via scripts/push_app.sh ==="
"$PUSH_APP" ubuntu_24_04 --dry-run
"$PUSH_APP" ubuntu_24_04

# Helper function to delete instance if present and wait for teardown
delete_instance_wait() {
    local inst="$1"
    echo "Checking if instance '$inst' exists..."
    local check
    check=$("$ZCLI" -- --format=json edge-app-instance show "$inst" 2>/dev/null || true)
    if [ -n "$check" ]; then
        echo "Deleting existing instance '$inst'..."
        "$ZCLI" -- edge-app-instance delete "$inst" -f || true
        echo "Waiting for '$inst' deletion to complete..."
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

    # Purge stale DHCP lease on target node so newly created instance receives static 10.1.0.131
    local node=""
    if [[ "$inst" == *"n1-655-pro"* ]]; then
        node="n1-655-pro"
    elif [[ "$inst" == *"n1-655-devkit"* ]]; then
        node="n1-655-devkit"
    fi
    if [ -n "$node" ]; then
        echo "Purging stale DHCP lease on $node..."
        ssh -o BatchMode=yes -o ConnectTimeout=5 "$node" "
            sed -i '/10\.1\.0\.131/d' /run/zedrouter/dnsmasq.leases/* 2>/dev/null || true
            eve exec pillar killall dnsmasq 2>/dev/null || true
            eve exec pillar /usr/sbin/dnsmasq -u nobody -g nobody -C /run/zedrouter/dnsmasq.bn1.conf 2>/dev/null || true
        " >/dev/null 2>&1 || true
    fi
}

# Helper function to wait for SSH availability
wait_ssh() {
    local host_alias="$1"
    local timeout_secs="$2"
    echo "Waiting for SSH on $host_alias (timeout: ${timeout_secs}s)..."
    local elapsed=0
    while [ "$elapsed" -lt "$timeout_secs" ]; do
        if ssh -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=no "$host_alias" "echo connected" >/dev/null 2>&1; then
            echo "SSH on $host_alias is available!"
            return 0
        elif sshpass -p ubuntu ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no "$host_alias" "mkdir -p ~/.ssh && chmod 700 ~/.ssh" >/dev/null 2>&1; then
            echo "SSH password login succeeded on $host_alias. Authorizing SSH key..."
            sshpass -p ubuntu ssh-copy-id -o StrictHostKeyChecking=no -i ~/.ssh/id_rsa.ambarella.pub "$host_alias" >/dev/null 2>&1 || true
            if ssh -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=no "$host_alias" "echo connected" >/dev/null 2>&1; then
                echo "SSH on $host_alias is available with pubkey!"
                return 0
            fi
        fi
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo "Timeout waiting for SSH on $host_alias" >&2
    return 1
}

# 6. Delete old instances before recreating
delete_instance_wait "ubuntu_24_04.n1-655-pro"
delete_instance_wait "ubuntu_24_04.n1-655-devkit"

# 7. Create 1 GiB baseline instances
echo "=== Creating baseline 1 GiB instances ==="
"$CREATE_INST" ubuntu_24_04.n1-655-pro \
    --edge-app=ubuntu_24_04 \
    --edge-node=n1-655-pro \
    --network-instance=eth0:defaultLocal-n1-655-pro \
    --adapter=amba_shm:amba_shm \
    --no-custom-configuration

"$CREATE_INST" ubuntu_24_04.n1-655-devkit \
    --edge-app=ubuntu_24_04 \
    --edge-node=n1-655-devkit \
    --network-instance=eth0:defaultLocal-n1-655-devkit \
    --adapter=amba_shm:amba_shm \
    --no-custom-configuration

echo "=== Waiting for baseline instances to boot and reach SSH ==="
wait_ssh "n1-655-pro-ubuntu" 300
wait_ssh "n1-655-devkit-ubuntu" 300

# 8. Memory measurement
echo "Waiting 5 minutes (300s) for system stabilization before measurement..."
sleep 300

TIMESTAMP=$(date +'%Y%m%d_%H%M%S')
MEMINFO_LOG="$ARTIFACTS_DIR/meminfo_${TIMESTAMP}.txt"
ssh n1-655-pro-ubuntu "cat /proc/meminfo; free -k; systemctl --failed" > "$MEMINFO_LOG"

MEM_TOTAL=$(grep "MemTotal:" "$MEMINFO_LOG" | awk '{print $2}')
MEM_AVAIL=$(grep "MemAvailable:" "$MEMINFO_LOG" | awk '{print $2}')
IDLE_USED=$((MEM_TOTAL - MEM_AVAIL))
HEADROOM=262144 # 256 MiB
TARGET_MEM=$((IDLE_USED + HEADROOM))

# Round up to next 65536 KiB (64 MiB) boundary
REMAINDER=$((TARGET_MEM % 65536))
if [ "$REMAINDER" -ne 0 ]; then
    TARGET_MEM=$((TARGET_MEM + 65536 - REMAINDER))
fi

# Ensure sensible minimum floor of 512 MiB
if [ "$TARGET_MEM" -lt 524288 ]; then
    TARGET_MEM=524288
fi

echo "Measurement Summary:"
echo "MemTotal     : $MEM_TOTAL KiB"
echo "MemAvailable : $MEM_AVAIL KiB"
echo "Idle Used    : $IDLE_USED KiB"
echo "Headroom     : $HEADROOM KiB"
echo "Computed Mem : $TARGET_MEM KiB"

# 9. Iterative qualification loop with computed memory
ITERATION=0
MAX_ITERATIONS=4
SUCCESS=0

while [ "$ITERATION" -le "$MAX_ITERATIONS" ]; do
    CURRENT_MEM_STR=$(printf "%.2f" "$TARGET_MEM")
    echo "=== Applying resources.memory = $CURRENT_MEM_STR KiB (Attempt $ITERATION / $MAX_ITERATIONS) ==="

    python3 - << EOF
import json

app_file = "$APP_JSON"
with open(app_file, "r") as f:
    data = json.load(f)

for res in data.get("resources", []):
    if res.get("name") == "memory":
        res["value"] = "$CURRENT_MEM_STR"

with open(app_file, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
EOF

    # Teardown existing instances before updating edge-app
    delete_instance_wait "ubuntu_24_04.n1-655-pro"
    delete_instance_wait "ubuntu_24_04.n1-655-devkit"

    "$PUSH_APP" ubuntu_24_04

    # Recreate instances with new memory reservation
    "$CREATE_INST" ubuntu_24_04.n1-655-pro \
        --edge-app=ubuntu_24_04 \
        --edge-node=n1-655-pro \
        --network-instance=eth0:defaultLocal-n1-655-pro \
        --adapter=amba_shm:amba_shm \
        --no-custom-configuration

    "$CREATE_INST" ubuntu_24_04.n1-655-devkit \
        --edge-app=ubuntu_24_04 \
        --edge-node=n1-655-devkit \
        --network-instance=eth0:defaultLocal-n1-655-devkit \
        --adapter=amba_shm:amba_shm \
        --no-custom-configuration

    # Verify boot and smoke checks
    set +e
    BOOT_OK=1
    wait_ssh "n1-655-pro-ubuntu" 240 || BOOT_OK=0
    wait_ssh "n1-655-devkit-ubuntu" 240 || BOOT_OK=0

    if [ "$BOOT_OK" -eq 1 ]; then
        echo "Boot and SSH succeeded on both nodes."
        # Deploy and verify Ambarella guest modules
        make -C "$ROOT_DIR" guest-ubuntu
        make -C "$ROOT_DIR" deploy-hvm-ubuntu TARGET=n1-655-pro-ubuntu || BOOT_OK=0
        make -C "$ROOT_DIR" deploy-hvm-ubuntu TARGET=n1-655-devkit-ubuntu || BOOT_OK=0
    fi
    set -e

    if [ "$BOOT_OK" -eq 1 ]; then
        echo "Qualification passed successfully at $CURRENT_MEM_STR KiB!"
        SUCCESS=1
        break
    else
        echo "Attempt $ITERATION failed at $CURRENT_MEM_STR KiB."
        ITERATION=$((ITERATION + 1))
        TARGET_MEM=$((TARGET_MEM + 65536))
    fi
done

if [ "$SUCCESS" -ne 1 ]; then
    echo "deploy.sh: Qualification failed after $MAX_ITERATIONS memory adjustments." >&2
    exit 1
fi

echo "=== Deployment and Qualification Complete ==="
