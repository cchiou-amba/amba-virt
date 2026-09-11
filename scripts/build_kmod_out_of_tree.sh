#!/bin/sh
# Build amba_virt.ko out-of-tree against target kernel tree or headers.
#
#   ./scripts/build_kmod_out_of_tree.sh
#   ./scripts/build_kmod_out_of_tree.sh --kdir=/path/to/linux-headers
#   ./scripts/build_kmod_out_of_tree.sh --clean

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SRC="$ROOT/poc"
OUT_DIR="$ROOT/build/kmod"
EVE_DIR=$(CDPATH= cd -- "$ROOT/../eve" 2>/dev/null && pwd || true)
EVE_KERNEL_DIR=$(CDPATH= cd -- "$ROOT/../eve-kernel" 2>/dev/null && pwd || true)
[ -z "$EVE_KERNEL_DIR" ] && [ -d "$ROOT/eve-kernel" ] && EVE_KERNEL_DIR="$ROOT/eve-kernel"
DRIVERS_DIR=$(CDPATH= cd -- "$ROOT/../drivers" 2>/dev/null && pwd || true)
[ -z "$DRIVERS_DIR" ] && [ -d "$ROOT/drivers" ] && DRIVERS_DIR="$ROOT/drivers"

KDIR=""
CLEAN=0

ARCH="${ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"

usage() {
    cat <<EOF
Usage: scripts/build_kmod_out_of_tree.sh [options]

Options:
  --kdir=DIR          Path to kernel source tree or linux-headers
  --clean             Clean build artifacts
  -h, --help          Show this help
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    -h|--help)
        usage
        exit 0
        ;;
    --clean)
        CLEAN=1
        shift
        ;;
    --kdir=*)
        KDIR="${1#--kdir=}"
        shift
        ;;
    --kdir)
        if [ "$#" -lt 2 ]; then
            echo "build_kmod_out_of_tree.sh: --kdir requires an argument" >&2
            exit 1
        fi
        KDIR="$2"
        shift 2
        ;;
    *)
        echo "build_kmod_out_of_tree.sh: unknown option: $1" >&2
        usage
        exit 1
        ;;
    esac
done

# Resolve default KDIR if not specified
if [ -z "$KDIR" ]; then
    if [ -d "$ROOT/build/usr/src" ]; then
        HDR=$(ls -d "$ROOT/build/usr/src/linux-headers-"* 2>/dev/null | head -n 1 || true)
        if [ -n "$HDR" ] && [ -d "$HDR" ]; then
            KDIR="$HDR"
        fi
    fi
    if [ -z "$KDIR" ] && [ -n "$EVE_KERNEL_DIR" ] && [ -f "$EVE_KERNEL_DIR/Module.symvers" ]; then
        KDIR="$EVE_KERNEL_DIR"
    elif [ -z "$KDIR" ] && [ -n "$EVE_DIR" ] && [ -d "$EVE_DIR/eve-kernel" ]; then
        KDIR="$EVE_DIR/eve-kernel"
    fi
fi

if [ -z "$KDIR" ] || [ ! -f "$KDIR/Makefile" ]; then
    echo "build_kmod_out_of_tree.sh: kernel directory not found: ${KDIR:-<unset>}" >&2
    echo "Please specify --kdir=/path/to/kernel-or-headers" >&2
    exit 1
fi

if [ "$CLEAN" -eq 1 ]; then
    if [ -d "$OUT_DIR" ]; then
        make -C "$KDIR" M="$OUT_DIR" clean || true
        rm -rf "$OUT_DIR"
        echo "Cleaned $OUT_DIR"
    fi
    exit 0
fi

mkdir -p "$OUT_DIR/include"

# Stage sources into build directory
install -m 0644 "$SRC/kmod/nohyper/amba_virt_nohyper.c" "$OUT_DIR/amba_virt_nohyper.c"
install -m 0644 "$SRC/kmod/common/amba_virt_core.c"     "$OUT_DIR/amba_virt_core.c"
install -m 0644 "$SRC/kmod/common/amba_virt_core.h"     "$OUT_DIR/amba_virt_core.h"
install -m 0644 "$SRC/include/uapi/amba_virt.h"         "$OUT_DIR/include/amba_virt.h"

cat > "$OUT_DIR/Kbuild" <<'EOF'
# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)/include

obj-m := amba_virt.o
amba_virt-y := amba_virt_nohyper.o amba_virt_core.o
EOF

echo "Building amba_virt.ko against $KDIR..."
make -C "$KDIR" M="$OUT_DIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" modules

CAVALRY_DIR=""
if [ -n "$DRIVERS_DIR" ] && [ -d "$DRIVERS_DIR/cavalry/cavalry_v3" ]; then
    CAVALRY_DIR="$DRIVERS_DIR/cavalry"
elif [ -n "$EVE_DIR" ] && [ -d "$EVE_DIR/cavalry/cavalry_v3" ]; then
    CAVALRY_DIR="$EVE_DIR/cavalry"
fi

if [ -n "$CAVALRY_DIR" ]; then
    echo "Building cavalry.ko against $KDIR..."
    make -C "$KDIR" M="$CAVALRY_DIR/cavalry_v3" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
        AMBARELLA_DRV_CFLAGS="-I$CAVALRY_DIR/include/cavalry_v3 -DAMBA_AMYOC_BUILD -DAMBA_SOC_N1_655" modules
fi

# Detect kernel module signing key and sign modules
KEY=""
CERT=""
if [ -f "$KDIR/certs/signing_key.pem" ]; then
    KEY="$KDIR/certs/signing_key.pem"
    CERT="$KDIR/certs/signing_key.x509"
elif [ -f "$ROOT/build/certs/signing_key.pem" ]; then
    KEY="$ROOT/build/certs/signing_key.pem"
    CERT="$ROOT/build/certs/signing_key.x509"
elif [ -n "$EVE_KERNEL_DIR" ] && [ -f "$EVE_KERNEL_DIR/certs/signing_key.pem" ]; then
    KEY="$EVE_KERNEL_DIR/certs/signing_key.pem"
    CERT="$EVE_KERNEL_DIR/certs/signing_key.x509"
elif [ -n "$EVE_DIR" ] && [ -f "$EVE_DIR/eve-kernel/certs/signing_key.pem" ]; then
    KEY="$EVE_DIR/eve-kernel/certs/signing_key.pem"
    CERT="$EVE_DIR/eve-kernel/certs/signing_key.x509"
fi

if [ -n "$KEY" ] && [ -f "$KEY" ] && [ -f "$CERT" ]; then
    SIGN_FILE=""
    if [ -x "$KDIR/scripts/sign-file" ]; then
        SIGN_FILE="$KDIR/scripts/sign-file"
    elif [ -x "$ROOT/build/bin/sign-file" ]; then
        SIGN_FILE="$ROOT/build/bin/sign-file"
    else
        SIGN_SRC=""
        if [ -f "$KDIR/scripts/sign-file.c" ]; then
            SIGN_SRC="$KDIR/scripts/sign-file.c"
        elif [ -n "$EVE_KERNEL_DIR" ] && [ -f "$EVE_KERNEL_DIR/scripts/sign-file.c" ]; then
            SIGN_SRC="$EVE_KERNEL_DIR/scripts/sign-file.c"
        elif [ -n "$EVE_DIR" ] && [ -f "$EVE_DIR/eve-kernel/scripts/sign-file.c" ]; then
            SIGN_SRC="$EVE_DIR/eve-kernel/scripts/sign-file.c"
        fi
        if [ -n "$SIGN_SRC" ]; then
            mkdir -p "$ROOT/build/bin"
            gcc "$SIGN_SRC" -lcrypto -o "$ROOT/build/bin/sign-file"
            SIGN_FILE="$ROOT/build/bin/sign-file"
        fi
    fi

    if [ -n "$SIGN_FILE" ] && [ -x "$SIGN_FILE" ]; then
        echo "Signing amba_virt.ko with $(basename "$KEY")..."
        "$SIGN_FILE" sha256 "$KEY" "$CERT" "$OUT_DIR/amba_virt.ko"
        if [ -n "$CAVALRY_DIR" ] && [ -f "$CAVALRY_DIR/cavalry_v3/cavalry.ko" ]; then
            echo "Signing cavalry.ko with $(basename "$KEY")..."
            "$SIGN_FILE" sha256 "$KEY" "$CERT" "$CAVALRY_DIR/cavalry_v3/cavalry.ko"
        fi
    else
        echo "Warning: sign-file binary could not be built; modules left unsigned" >&2
    fi
else
    echo "Notice: No signing_key.pem found; modules left unsigned (Development Mode without key)"
fi

echo "Successfully built: $OUT_DIR/amba_virt.ko"
if [ -n "$CAVALRY_DIR" ] && [ -f "$CAVALRY_DIR/cavalry_v3/cavalry.ko" ]; then
    echo "Successfully built: $CAVALRY_DIR/cavalry_v3/cavalry.ko"
fi
