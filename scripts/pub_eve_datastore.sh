#!/bin/sh
# Publish a newly built EVE BaseOS rootfs image to a local datastore directory
# and register / uplink it in ZedControl via scripts/zcli.
#
#   ./scripts/pub_eve_datastore.sh ~/public_html/eve-images/
#   ./scripts/pub_eve_datastore.sh ~/public_html/eve-images/ --dry-run
#   ./scripts/pub_eve_datastore.sh ~/public_html/eve-images/ --datastore=LocalHTTP
#
# Requires:
#   - ZCLI_TOKEN in the environment
#   - EVE build outputs in eve/eve/dist/arm64/current/installer/

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ZCLI="$ROOT/scripts/zcli"

DEST_DIR=
DATASTORE=
IMAGE_NAME=
EVE_DIST=
DRY_RUN=0

usage() {
	cat << 'EOF' >&2
usage: scripts/pub_eve_datastore.sh DEST_DIR [OPTIONS]

Stage an EVE rootfs.img into a versioned subdirectory under DEST_DIR and
register/uplink the image record in ZedControl.

Arguments:
  DEST_DIR               Target web/staging directory (e.g. ~/public_html/eve-images/)

Options:
  --datastore=NAME       ZedControl datastore name (default: auto-detected or $ZCLI_DATASTORE)
  --image-name=NAME      Image name in ZedControl (default: eve-<version>)
  --eve-dist=DIR         Custom path to EVE installer dist directory
  --dry-run              Display staging and zcli commands without executing
  -h, --help             Show this help message
EOF
}

if [ -z "${ZCLI_TOKEN:-}" ]; then
	echo "scripts/pub_eve_datastore.sh: set ZCLI_TOKEN (do not commit it)" >&2
	exit 1
fi

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
	--datastore=*)
		DATASTORE="${1#--datastore=}"
		shift
		;;
	--image-name=*)
		IMAGE_NAME="${1#--image-name=}"
		shift
		;;
	--eve-dist=*)
		EVE_DIST="${1#--eve-dist=}"
		shift
		;;
	-*)
		echo "scripts/pub_eve_datastore.sh: unknown option: $1" >&2
		usage
		exit 1
		;;
	*)
		if [ -z "$DEST_DIR" ]; then
			DEST_DIR="$1"
			shift
		else
			echo "scripts/pub_eve_datastore.sh: unexpected extra argument: $1" >&2
			usage
			exit 1
		fi
		;;
	esac
done

if [ -z "$DEST_DIR" ]; then
	echo "scripts/pub_eve_datastore.sh: missing DEST_DIR" >&2
	usage
	exit 1
fi

# Expand leading tilde if unexpanded by caller shell
case "$DEST_DIR" in
"~"*)
	DEST_DIR="$HOME${DEST_DIR#\~}"
	;;
esac
DEST_DIR="${DEST_DIR%/}"

# Locate EVE installer directory
INSTALLER_DIR=""
if [ -n "$EVE_DIST" ] && [ -d "$EVE_DIST" ]; then
	INSTALLER_DIR="$EVE_DIST"
elif [ -d "$ROOT/eve/eve/dist/arm64/current/installer" ]; then
	INSTALLER_DIR="$ROOT/eve/eve/dist/arm64/current/installer"
elif [ -d "$ROOT/../eve/eve/dist/arm64/current/installer" ]; then
	INSTALLER_DIR="$ROOT/../eve/eve/dist/arm64/current/installer"
fi

if [ -z "$INSTALLER_DIR" ] || [ ! -f "$INSTALLER_DIR/rootfs.img" ] || [ ! -f "$INSTALLER_DIR/eve_version" ]; then
	echo "scripts/pub_eve_datastore.sh: cannot locate EVE build artifacts (rootfs.img, eve_version)" >&2
	echo "Checked: $ROOT/eve/eve/dist/arm64/current/installer" >&2
	echo "         $ROOT/../eve/eve/dist/arm64/current/installer" >&2
	echo "Ensure you built EVE via 'make eve' in eve/build, or specify --eve-dist=PATH." >&2
	exit 1
fi

ROOTFS_SRC="$INSTALLER_DIR/rootfs.img"
EVE_VER=$(tr -d '\r\n' < "$INSTALLER_DIR/eve_version")
IMAGE_NAME="${IMAGE_NAME:-$EVE_VER}"

# Target versioned directory
TARGET_DIR="$DEST_DIR/$IMAGE_NAME"
TARGET_FILE="$TARGET_DIR/rootfs.img"

# Compute relative datastore URL path
if echo "$DEST_DIR" | grep -q "public_html/"; then
	REL_PREFIX="${DEST_DIR#*public_html/}"
else
	REL_PREFIX="$(basename "$DEST_DIR")"
fi
REL_PREFIX=$(echo "$REL_PREFIX" | sed -e 's|^/||' -e 's|/$||')
IMAGE_URL="${REL_PREFIX}/${IMAGE_NAME}/rootfs.img"

echo "=== Staging EVE BaseOS Image ==="
echo "Version     : $EVE_VER"
echo "Source      : $ROOTFS_SRC"
echo "Destination : $TARGET_FILE"
echo "Image Name  : $IMAGE_NAME"
echo "Image URL   : $IMAGE_URL"

# Compute checksum and byte size
IMAGE_SHA=$(sha256sum "$ROOTFS_SRC" | awk '{print $1}')
IMAGE_SIZE=$(stat -c %s "$ROOTFS_SRC")
echo "SHA-256     : $IMAGE_SHA"
echo "Size (bytes): $IMAGE_SIZE"

# Copy artifact to versioned destination
if [ "$DRY_RUN" -eq 1 ]; then
	echo "[DRY-RUN] mkdir -p \"$TARGET_DIR\""
	echo "[DRY-RUN] cp \"$ROOTFS_SRC\" \"$TARGET_FILE\""
else
	mkdir -p "$TARGET_DIR"
	if [ -f "$TARGET_FILE" ]; then
		DEST_SHA=$(sha256sum "$TARGET_FILE" | awk '{print $1}')
		if [ "$DEST_SHA" = "$IMAGE_SHA" ]; then
			echo "Target file already exists and checksum matches; skipping copy."
		else
			echo "Updating target file at $TARGET_FILE..."
			cp "$ROOTFS_SRC" "$TARGET_FILE"
		fi
	else
		echo "Copying $ROOTFS_SRC -> $TARGET_FILE..."
		cp "$ROOTFS_SRC" "$TARGET_FILE"
	fi
fi

# Determine datastore
if [ -z "$DATASTORE" ]; then
	DATASTORE="${ZCLI_DATASTORE:-}"
fi

if [ -z "$DATASTORE" ]; then
	echo "Auto-detecting local HTTP datastore from ZedControl..."
	DATASTORE=$("$ZCLI" -- --format=json datastore show 2>/dev/null | jq -r '
		.list[]
		| select(.originType == "ORIGIN_LOCAL" and .dsType == "DATASTORE_TYPE_HTTP" and (.dsPath | startswith("~")))
		| .name' | head -n 1)

	if [ -z "$DATASTORE" ]; then
		DATASTORE=$("$ZCLI" -- --format=json datastore show 2>/dev/null | jq -r '
			.list[]
			| select(.originType == "ORIGIN_LOCAL" and .dsType == "DATASTORE_TYPE_HTTP")
			| .name' | head -n 1)
	fi
fi

if [ -z "$DATASTORE" ]; then
	echo "scripts/pub_eve_datastore.sh: could not auto-detect local HTTP datastore." >&2
	echo "Specify --datastore=NAME or export ZCLI_DATASTORE." >&2
	exit 1
fi

echo "Datastore   : $DATASTORE"
echo ""

echo "=== Registering in ZedControl ==="
if [ "$DRY_RUN" -eq 1 ]; then
	cat << EOF
[DRY-RUN] $ZCLI -- image create "$IMAGE_NAME" \\
    --title="EVE $EVE_VER" \\
    --type=Eve \\
    --image-format=raw \\
    --arch=ARM64 \\
    --datastore-name="$DATASTORE" \\
    --image-url="$IMAGE_URL"

[DRY-RUN] $ZCLI -- image uplink "$IMAGE_NAME" \\
    --datastore-name="$DATASTORE" \\
    --image-url="$IMAGE_URL" \\
    --image-sha="$IMAGE_SHA" \\
    --image-size="$IMAGE_SIZE"

[DRY-RUN] $ZCLI -- image show "$IMAGE_NAME"
EOF
else
	# Create image record if it does not already exist
	echo "Creating image record '$IMAGE_NAME'..."
	if "$ZCLI" -- image show "$IMAGE_NAME" >/dev/null 2>&1; then
		echo "Image record '$IMAGE_NAME' already exists; updating uplink..."
	else
		"$ZCLI" -- image create "$IMAGE_NAME" \
			--title="EVE $EVE_VER" \
			--type=Eve \
			--image-format=raw \
			--arch=ARM64 \
			--datastore-name="$DATASTORE" \
			--image-url="$IMAGE_URL"
	fi

	echo "Uplinking image checksum and file size..."
	"$ZCLI" -- image uplink "$IMAGE_NAME" \
		--datastore-name="$DATASTORE" \
		--image-url="$IMAGE_URL" \
		--image-sha="$IMAGE_SHA" \
		--image-size="$IMAGE_SIZE"

	echo ""
	echo "Image verification:"
	"$ZCLI" -- image show "$IMAGE_NAME"
fi

echo ""
echo "=== Success ==="
echo "Image '$IMAGE_NAME' is registered and ready in ZedControl."
echo ""
echo "To update an edge node, run:"
echo "  ./scripts/zcli -- edge-node eveimage-update <NODE> --image=\"$IMAGE_NAME\" --activate"
echo ""
echo "Example for n1-655-devkit:"
echo "  ./scripts/zcli -- edge-node eveimage-update n1-655-devkit --image=\"$IMAGE_NAME\" --activate"
