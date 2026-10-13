#!/usr/bin/env bash
# Sourced by guest-os/build_guest.sh for --distro=fedora. Defines build_distro().
# Uses the helpers of build_guest.sh (log_step, ensure_binfmt, ensure_image) and its ROOT_DIR and BUILD_DIR.

FEDORA_OUT="${BUILD_DIR}/fedora"
IMAGE_FEDORA="amba-guest-builder:fedora-44"
# The kernel of the Fedora 44 image (guest-os/fedora/fedora-build/base-image.lock KERNEL_VERSION)
FEDORA_KERNEL="6.19.10-300.fc44.aarch64"

build_distro() {
    echo "=========================================================="
    echo " Building Fedora 44 ARM64 Guest Artifacts"
    echo "=========================================================="
    mkdir -p "${FEDORA_OUT}"

    ensure_binfmt
    ensure_image "${IMAGE_FEDORA}" "Dockerfile.fedora"

    log_step "Compiling Fedora 44 kernel modules inside ${IMAGE_FEDORA} container..."
    docker run --rm --platform linux/arm64 \
        -v "${ROOT_DIR}":/workspace -w /workspace \
        -e GUEST_KERNEL="${FEDORA_KERNEL}" \
        "${IMAGE_FEDORA}" bash -c '
            set -euo pipefail
            KDIR="/usr/src/kernels/${GUEST_KERNEL}"
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
                cp -vf "/workspace/guest-os/linux/${dir}/${ko}.ko" /workspace/build/guest/fedora/
            done
        '

    log_step "Compiling amba-virt-client natively (aarch64-linux-gnu-g++ glibc)..."
    make -C "${ROOT_DIR}/guest-os/client" \
        clean all \
        CROSS_COMPILE=aarch64-linux-gnu-
    cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-client" "${FEDORA_OUT}/"
    if [ -f "${ROOT_DIR}/guest-os/client/amba-virt-cli" ]; then
        cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-cli" "${FEDORA_OUT}/"
    fi

    echo ""
    log_step "Fedora 44 artifacts staged in ${FEDORA_OUT}:"
    ls -lh "${FEDORA_OUT}"
}
