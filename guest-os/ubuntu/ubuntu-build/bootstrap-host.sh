#!/bin/bash
# Bootstrap declared host prerequisites for Ubuntu HVM image build

set -euo pipefail

DRY_RUN=0

usage() {
    cat << 'EOF'
usage: bootstrap-host.sh [OPTIONS]

Verify and install declared build prerequisites on Ubuntu/Debian host.

Options:
  --dry-run   Check declared prerequisites without installing packages
  -h, --help  Show this help message
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "bootstrap-host.sh: unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            echo "bootstrap-host.sh: unexpected positional argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

echo "=== Checking host OS and package manager ==="
if [ ! -f /etc/debian_version ] && ! command -v apt-get >/dev/null 2>&1; then
    echo "bootstrap-host.sh: error: host must be Debian/Ubuntu with apt-get" >&2
    exit 1
fi

if ! command -v sudo >/dev/null 2>&1; then
    echo "bootstrap-host.sh: error: sudo command is required on host" >&2
    exit 1
fi

DECLARED_PACKAGES=(
    qemu-system-arm
    qemu-efi-aarch64
    qemu-utils
    cloud-image-utils
    genisoimage
    python3
    python3-pexpect
    openssh-client
    sshpass
    curl
    gnupg
    ca-certificates
)

MISSING_PACKAGES=()
for pkg in "${DECLARED_PACKAGES[@]}"; do
    if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "ok installed"; then
        MISSING_PACKAGES+=("$pkg")
    fi
done

if [ "${#MISSING_PACKAGES[@]}" -eq 0 ]; then
    echo "All declared host prerequisites are installed."
    exit 0
fi

echo "Missing declared host packages: ${MISSING_PACKAGES[*]}"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[DRY-RUN] Would install missing packages via: sudo apt-get install -y ${MISSING_PACKAGES[*]}"
    exit 0
fi

echo "=== Verifying sudo privileges ==="
sudo -v

echo "=== Installing missing prerequisites noninteractively ==="
sudo DEBIAN_FRONTEND=noninteractive apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${MISSING_PACKAGES[@]}"

echo "Declared host prerequisites installed successfully."
