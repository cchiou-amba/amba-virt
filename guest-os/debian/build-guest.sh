#!/usr/bin/env bash
# Sourced by guest-os/build_guest.sh for --distro=debian. Defines build_distro().
# Uses the helpers of build_guest.sh (log_step, ensure_binfmt, ensure_image) and its ROOT_DIR and BUILD_DIR.

DEBIAN_OUT="${BUILD_DIR}/debian"
IMAGE_DEBIAN="amba-guest-builder:debian-13"
# The kernel of the Debian 13 (trixie) image (guest-os/debian/debian-build/base-image.lock KERNEL_VERSION)
DEBIAN_KERNEL="6.12.111+deb13-arm64"

build_distro() {
    echo "=========================================================="
    echo " Building Debian 13 (trixie) ARM64 Guest Artifacts"
    echo "=========================================================="
    mkdir -p "${DEBIAN_OUT}"

    ensure_binfmt
    ensure_image "${IMAGE_DEBIAN}" "Dockerfile.debian"

    log_step "Compiling Debian 13 (trixie) kernel modules inside ${IMAGE_DEBIAN} container..."
    docker run --rm --platform linux/arm64 \
        -v "${ROOT_DIR}":/workspace -w /workspace \
        -e GUEST_KERNEL="${DEBIAN_KERNEL}" \
        "${IMAGE_DEBIAN}" bash -c '
            set -euo pipefail
            KDIR="/usr/src/linux-headers-${GUEST_KERNEL}"
            if [ ! -d "${KDIR}" ]; then
                echo "Error: ${KDIR} is not in the builder image" >&2
                exit 1
            fi
            for entry in "amba-virt:amba_virt" "amba-gdma:ambarella-gdma" "test-gdma:testGDMA" \
                         "amba-cavalry:amba_cavalry" "amba-iav:iav_virt" "amba-uart:amba_uart" "amba-dma:amba_dma"; do
                dir="${entry%%:*}"
                ko="${entry##*:}"
                echo "[*] Compiling ${ko}.ko against ${KDIR}..."
                make -C "/workspace/guest-os/linux/${dir}" KDIR_HVM="${KDIR}" clean modules
                cp -vf "/workspace/guest-os/linux/${dir}/${ko}.ko" /workspace/build/guest/debian/
            done
        '

    log_step "Compiling amba-virt-client natively (aarch64-linux-gnu-g++ glibc)..."
    make -C "${ROOT_DIR}/guest-os/client" \
        clean all \
        CROSS_COMPILE=aarch64-linux-gnu-
    cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-client" "${DEBIAN_OUT}/"
    if [ -f "${ROOT_DIR}/guest-os/client/amba-virt-cli" ]; then
        cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-cli" "${DEBIAN_OUT}/"
    fi

    echo ""
    log_step "Debian 13 (trixie) artifacts staged in ${DEBIAN_OUT}:"
    ls -lh "${DEBIAN_OUT}"
}
