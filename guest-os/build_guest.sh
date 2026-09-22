#!/usr/bin/env bash
#
# guest-os/build_guest.sh
# Automated fast cross-compilation driver for Ambarella HVM guest targets:
#   - Ubuntu 24.04 LTS (AArch64 HVM: amba_virt.ko + amba-virt-client)
#   - Alpine Linux 3.20 (AArch64 HVM: amba_virt.ko + amba-virt-client)
#   - BlackBerry QNX Neutrino 8.0 (AArch64 HVM: amba-virt-resmgr + amba-virt-client)
#
# Copyright (C) 2026, Ambarella International LLC
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build/guest"
UBUNTU_OUT="${BUILD_DIR}/ubuntu"
ALPINE_OUT="${BUILD_DIR}/alpine"
QNX_OUT="${BUILD_DIR}/qnx"

HEADERS_DIR="${BUILD_DIR}/headers"
UBUNTU_HEADERS="${HEADERS_DIR}/ubuntu"
ALPINE_HEADERS="${HEADERS_DIR}/alpine"

IMAGE_UBUNTU="amba-guest-builder:ubuntu-24.04"
IMAGE_ALPINE="amba-guest-builder:alpine-3.20"

DISTRO="all"
DO_CLEAN=0
DO_DISTCLEAN=0
BUILD_QNX_IMAGE=0

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --distro=NAME       Target guest OS: ubuntu, alpine, qnx, or all (default: all)
  --qnx-image         Also build full QNX 8.0 HVM disk image (qnx-8.0-arm64-cloudimg.qcow2)
  --clean             Clean guest output staging directories (preserves cached headers)
  --distclean         Full clean including cached guest kernel headers
  -h, --help          Show this help message

Examples:
  $0 --distro=ubuntu
  $0 --distro=alpine
  $0 --distro=qnx
  $0 --distro=all
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --distro=*)
            DISTRO="${1#--distro=}"
            shift
            ;;
        --distro)
            DISTRO="$2"
            shift 2
            ;;
        --qnx-image)
            BUILD_QNX_IMAGE=1
            shift
            ;;
        --clean)
            DO_CLEAN=1
            shift
            ;;
        --distclean)
            DO_DISTCLEAN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

log_step() {
    echo ""
    echo "[$(date '+%H:%M:%S')] >>> $1"
}

do_clean() {
    echo "=== Cleaning guest build artifacts ==="
    rm -rf "${UBUNTU_OUT}" "${ALPINE_OUT}" "${QNX_OUT}"
    make -C "${ROOT_DIR}/guest-os/linux/amba-virt" clean 2>/dev/null || true
    make -C "${ROOT_DIR}/guest-os/linux/amba-gdma" clean 2>/dev/null || true
    make -C "${ROOT_DIR}/guest-os/linux/amba-cavalry" clean 2>/dev/null || true
    make -C "${ROOT_DIR}/guest-os/linux/test-gdma" clean 2>/dev/null || true
    make -C "${ROOT_DIR}/guest-os/client" clean 2>/dev/null || true
    make -C "${ROOT_DIR}/guest-os/qnx/amba-virt" clean 2>/dev/null || true
    echo "Guest build artifacts cleaned."
}

do_distclean() {
    do_clean
    echo "=== Removing cached guest kernel headers ==="
    rm -rf "${BUILD_DIR}"
    echo "Guest build cache fully cleaned."
}

if [ "${DO_DISTCLEAN}" -eq 1 ]; then
    do_distclean
    exit 0
fi

if [ "${DO_CLEAN}" -eq 1 ]; then
    do_clean
    exit 0
fi

ensure_binfmt() {
    if ! docker run --rm --platform linux/arm64 alpine:3.20 uname -m >/dev/null 2>&1; then
        log_step "Registering QEMU ARM64 binfmt handler..."
        docker run --privileged --rm tonistiigi/binfmt --install arm64
    fi
}

ensure_image() {
    local img_tag="$1"
    local dockerfile="$2"
    if ! docker image inspect "${img_tag}" >/dev/null 2>&1; then
        log_step "Building cached builder image: ${img_tag}..."
        docker build --platform linux/arm64 \
            -t "${img_tag}" \
            -f "${ROOT_DIR}/guest-os/docker/${dockerfile}" \
            "${ROOT_DIR}/guest-os/docker"
        log_step "Cached builder image ${img_tag} created successfully."
    else
        echo "[*] Using cached builder image: ${img_tag}"
    fi
}

ensure_ubuntu_headers() {
    local kdir
    kdir="$(ls -d "${UBUNTU_HEADERS}"/linux-headers-*-generic 2>/dev/null | tail -n 1 || true)"
    if [ -n "${kdir}" ] && [ -d "${kdir}" ] && [ -x "${kdir}/scripts/basic/fixdep" ]; then
        echo "${kdir}"
        return 0
    fi

    log_step "Extracting Ubuntu 24.04 ARM64 kernel headers to build/guest/headers/ubuntu..."
    mkdir -p "${UBUNTU_HEADERS}"
    ensure_image "${IMAGE_UBUNTU}" "Dockerfile.ubuntu"

    local cid
    cid="$(docker create "${IMAGE_UBUNTU}")"
    docker cp "${cid}:/usr/src/." "${UBUNTU_HEADERS}/"
    docker rm "${cid}" >/dev/null

    kdir="$(ls -d "${UBUNTU_HEADERS}"/linux-headers-*-generic 2>/dev/null | tail -n 1)"
    local kcommon
    kcommon="$(ls -d "${UBUNTU_HEADERS}"/linux-headers-*[!generic] 2>/dev/null | tail -n 1 || true)"

    log_step "Preparing host kbuild helper tools for Ubuntu headers..."
    gcc -O2 -o "${kdir}/scripts/basic/fixdep" "${kcommon}/scripts/basic/fixdep.c"
    if [ -f "${kcommon}/scripts/mod/modpost.c" ]; then
        gcc -O2 -I"${kdir}/scripts/mod" -I"${kcommon}/scripts/mod" \
            -o "${kdir}/scripts/mod/modpost" \
            "${kcommon}/scripts/mod/modpost.c" \
            "${kcommon}/scripts/mod/file2alias.c" \
            "${kcommon}/scripts/mod/sumversion.c" \
            "${kcommon}/scripts/mod/symsearch.c"
    fi
    if [ -f "${kcommon}/scripts/genksyms/genksyms.c" ]; then
        gcc -O2 -I"${kdir}/scripts/genksyms" -I"${kcommon}/scripts/genksyms" \
            -o "${kdir}/scripts/genksyms/genksyms" \
            "${kcommon}/scripts/genksyms/genksyms.c" \
            "${kdir}/scripts/genksyms/lex.lex.c" \
            "${kdir}/scripts/genksyms/parse.tab.c"
    fi

    echo "${kdir}"
}

ensure_alpine_headers() {
    local kdir
    kdir="$(ls -d "${ALPINE_HEADERS}"/linux-headers-*-virt 2>/dev/null | tail -n 1 || true)"
    if [ -n "${kdir}" ] && [ -d "${kdir}" ] && [ -x "${kdir}/scripts/basic/fixdep" ]; then
        echo "${kdir}"
        return 0
    fi

    log_step "Extracting Alpine Linux 3.20 ARM64 kernel headers to build/guest/headers/alpine..."
    mkdir -p "${ALPINE_HEADERS}"
    ensure_image "${IMAGE_ALPINE}" "Dockerfile.alpine"

    local cid
    cid="$(docker create "${IMAGE_ALPINE}")"
    docker cp "${cid}:/usr/src/." "${ALPINE_HEADERS}/"
    docker rm "${cid}" >/dev/null

    kdir="$(ls -d "${ALPINE_HEADERS}"/linux-headers-*-virt 2>/dev/null | tail -n 1)"

    log_step "Preparing host kbuild helper tools for Alpine headers..."
    gcc -O2 -o "${kdir}/scripts/basic/fixdep" "${kdir}/scripts/basic/fixdep.c"
    if [ -f "${kdir}/scripts/mod/modpost.c" ]; then
        gcc -O2 -I"${kdir}/scripts/mod" \
            -o "${kdir}/scripts/mod/modpost" \
            "${kdir}/scripts/mod/modpost.c" \
            "${kdir}/scripts/mod/file2alias.c" \
            "${kdir}/scripts/mod/sumversion.c" \
            "${kdir}/scripts/mod/symsearch.c"
    fi
    if [ -f "${kdir}/scripts/genksyms/genksyms.c" ]; then
        gcc -O2 -I"${kdir}/scripts/genksyms" \
            -o "${kdir}/scripts/genksyms/genksyms" \
            "${kdir}/scripts/genksyms/genksyms.c" \
            "${kdir}/scripts/genksyms/lex.lex.c" \
            "${kdir}/scripts/genksyms/parse.tab.c"
    fi

    echo "${kdir}"
}

build_ubuntu() {
    echo "=========================================================="
    echo " Building Ubuntu 24.04 LTS ARM64 Guest Artifacts"
    echo "=========================================================="
    mkdir -p "${UBUNTU_OUT}"

    if which aarch64-linux-gnu-gcc >/dev/null 2>&1 && which aarch64-linux-gnu-g++ >/dev/null 2>&1; then
        log_step "Using native host cross-compiler: $(which aarch64-linux-gnu-gcc) (Fast Path)"
        local kdir
        kdir="$(ensure_ubuntu_headers)"

        log_step "Compiling amba_virt.ko natively (ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-)..."
        make -C "${ROOT_DIR}/guest-os/linux/amba-virt" \
            KDIR_HVM="${kdir}" \
            ARCH=arm64 \
            CROSS_COMPILE=aarch64-linux-gnu- \
            clean modules
        cp -vf "${ROOT_DIR}/guest-os/linux/amba-virt/amba_virt.ko" "${UBUNTU_OUT}/"

        log_step "Compiling ambarella-gdma.ko natively..."
        make -C "${ROOT_DIR}/guest-os/linux/amba-gdma" \
            KDIR_HVM="${kdir}" \
            ARCH=arm64 \
            CROSS_COMPILE=aarch64-linux-gnu- \
            clean modules
        cp -vf "${ROOT_DIR}/guest-os/linux/amba-gdma/ambarella-gdma.ko" "${UBUNTU_OUT}/"

        log_step "Compiling testGDMA.ko natively..."
        make -C "${ROOT_DIR}/guest-os/linux/test-gdma" \
            KDIR_HVM="${kdir}" \
            ARCH=arm64 \
            CROSS_COMPILE=aarch64-linux-gnu- \
            clean modules
        cp -vf "${ROOT_DIR}/guest-os/linux/test-gdma/testGDMA.ko" "${UBUNTU_OUT}/"

        log_step "Compiling amba_cavalry.ko natively..."
        make -C "${ROOT_DIR}/guest-os/linux/amba-cavalry" \
            KDIR_HVM="${kdir}" \
            ARCH=arm64 \
            CROSS_COMPILE=aarch64-linux-gnu- \
            clean modules
        cp -vf "${ROOT_DIR}/guest-os/linux/amba-cavalry/amba_cavalry.ko" "${UBUNTU_OUT}/"

        log_step "Compiling amba-virt-client natively (aarch64-linux-gnu-g++ glibc)..."
        make -C "${ROOT_DIR}/guest-os/client" \
            clean all \
            CROSS_COMPILE=aarch64-linux-gnu-
        cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-client" "${UBUNTU_OUT}/"
        if [ -f "${ROOT_DIR}/guest-os/client/amba-virt-cli" ]; then
            cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-cli" "${UBUNTU_OUT}/"
        fi
    else
        log_step "[NOTE] Host aarch64 cross-compiler not found. Falling back to container build..."
        ensure_binfmt
        ensure_image "${IMAGE_UBUNTU}" "Dockerfile.ubuntu"

        log_step "Running compilation inside ${IMAGE_UBUNTU} container..."
        docker run --rm --platform linux/arm64 \
            -v "${ROOT_DIR}":/workspace -w /workspace \
            "${IMAGE_UBUNTU}" bash -c '
                set -euo pipefail
                KDIR=$(ls -d /lib/modules/*/build 2>/dev/null | tail -n 1)
                echo "[*] Compiling amba_virt.ko against ${KDIR}..."
                make -C /workspace/guest-os/linux/amba-virt KDIR_HVM="${KDIR}" clean modules
                cp -vf /workspace/guest-os/linux/amba-virt/amba_virt.ko /workspace/build/guest/ubuntu/

                echo "[*] Compiling ambarella-gdma.ko..."
                make -C /workspace/guest-os/linux/amba-gdma KDIR_HVM="${KDIR}" clean modules
                cp -vf /workspace/guest-os/linux/amba-gdma/ambarella-gdma.ko /workspace/build/guest/ubuntu/

                echo "[*] Compiling testGDMA.ko..."
                make -C /workspace/guest-os/linux/test-gdma KDIR_HVM="${KDIR}" clean modules
                cp -vf /workspace/guest-os/linux/test-gdma/testGDMA.ko /workspace/build/guest/ubuntu/

                echo "[*] Compiling amba_cavalry.ko..."
                make -C /workspace/guest-os/linux/amba-cavalry KDIR_HVM="${KDIR}" clean modules
                cp -vf /workspace/guest-os/linux/amba-cavalry/amba_cavalry.ko /workspace/build/guest/ubuntu/

                echo "[*] Compiling amba-virt-client..."
                make -C /workspace/guest-os/client clean all
                cp -vf /workspace/guest-os/client/amba-virt-client /workspace/build/guest/ubuntu/
                if [ -f /workspace/guest-os/client/amba-virt-cli ]; then
                    cp -vf /workspace/guest-os/client/amba-virt-cli /workspace/build/guest/ubuntu/
                fi
            '
    fi

    echo ""
    log_step "Ubuntu 24.04 artifacts staged in ${UBUNTU_OUT}:"
    ls -lh "${UBUNTU_OUT}"
}

build_alpine() {
    echo "=========================================================="
    echo " Building Alpine Linux 3.20 ARM64 Guest Artifacts"
    echo "=========================================================="
    mkdir -p "${ALPINE_OUT}"

    if which aarch64-linux-gnu-gcc >/dev/null 2>&1 && which aarch64-linux-gnu-g++ >/dev/null 2>&1; then
        log_step "Using native host cross-compiler: $(which aarch64-linux-gnu-gcc) (Fast Path)"
        local kdir
        kdir="$(ensure_alpine_headers)"

        log_step "Compiling amba_virt.ko natively (ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-)..."
        make -C "${ROOT_DIR}/guest-os/linux/amba-virt" \
            KDIR_HVM="${kdir}" \
            ARCH=arm64 \
            CROSS_COMPILE=aarch64-linux-gnu- \
            CONFIG_GCC_PLUGINS=n \
            clean modules
        cp -vf "${ROOT_DIR}/guest-os/linux/amba-virt/amba_virt.ko" "${ALPINE_OUT}/"

        log_step "Compiling ambarella-gdma.ko natively..."
        make -C "${ROOT_DIR}/guest-os/linux/amba-gdma" \
            KDIR_HVM="${kdir}" \
            ARCH=arm64 \
            CROSS_COMPILE=aarch64-linux-gnu- \
            CONFIG_GCC_PLUGINS=n \
            clean modules
        cp -vf "${ROOT_DIR}/guest-os/linux/amba-gdma/ambarella-gdma.ko" "${ALPINE_OUT}/"

        log_step "Compiling testGDMA.ko natively..."
        make -C "${ROOT_DIR}/guest-os/linux/test-gdma" \
            KDIR_HVM="${kdir}" \
            ARCH=arm64 \
            CROSS_COMPILE=aarch64-linux-gnu- \
            CONFIG_GCC_PLUGINS=n \
            clean modules
        cp -vf "${ROOT_DIR}/guest-os/linux/test-gdma/testGDMA.ko" "${ALPINE_OUT}/"

        log_step "Compiling amba-virt-client natively (aarch64-linux-gnu-g++ -static musl/standalone)..."
        make -C "${ROOT_DIR}/guest-os/client" \
            clean all \
            CROSS_COMPILE=aarch64-linux-gnu- \
            STATIC=1
        cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-client" "${ALPINE_OUT}/"
        if [ -f "${ROOT_DIR}/guest-os/client/amba-virt-cli" ]; then
            cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-cli" "${ALPINE_OUT}/"
        fi
    else
        log_step "[NOTE] Host aarch64 cross-compiler not found. Falling back to container build..."
        ensure_binfmt
        ensure_image "${IMAGE_ALPINE}" "Dockerfile.alpine"

        log_step "Running compilation inside ${IMAGE_ALPINE} container..."
        docker run --rm --platform linux/arm64 \
            -v "${ROOT_DIR}":/workspace -w /workspace \
            "${IMAGE_ALPINE}" sh -c '
                set -eu
                KDIR=$(ls -d /usr/src/linux-headers-*-virt 2>/dev/null | tail -n 1)
                echo "[*] Compiling amba_virt.ko against ${KDIR}..."
                make -C /workspace/guest-os/linux/amba-virt KDIR_HVM="${KDIR}" clean modules
                cp -vf /workspace/guest-os/linux/amba-virt/amba_virt.ko /workspace/build/guest/alpine/

                echo "[*] Compiling ambarella-gdma.ko..."
                make -C /workspace/guest-os/linux/amba-gdma KDIR_HVM="${KDIR}" clean modules
                cp -vf /workspace/guest-os/linux/amba-gdma/ambarella-gdma.ko /workspace/build/guest/alpine/

                echo "[*] Compiling testGDMA.ko..."
                make -C /workspace/guest-os/linux/test-gdma KDIR_HVM="${KDIR}" clean modules
                cp -vf /workspace/guest-os/linux/test-gdma/testGDMA.ko /workspace/build/guest/alpine/

                echo "[*] Compiling amba-virt-client (static musl)..."
                make -C /workspace/guest-os/client clean all STATIC=1
                cp -vf /workspace/guest-os/client/amba-virt-client /workspace/build/guest/alpine/
            '
    fi

    echo ""
    log_step "Alpine Linux artifacts staged in ${ALPINE_OUT}:"
    ls -lh "${ALPINE_OUT}"
}

locate_qnx_env() {
    if which qcc >/dev/null 2>&1; then
        return 0
    fi
    for cand in "${HOME}/qnx/qnx800/qnxsdp-env.sh" "${HOME}/qnx800/qnxsdp-env.sh"; do
        if [ -f "${cand}" ]; then
            log_step "Sourcing QNX SDP environment: ${cand}"
            # shellcheck disable=SC1090
            source "${cand}"
            return 0
        fi
    done
    return 1
}

build_qnx() {
    echo "=========================================================="
    echo " Building BlackBerry QNX Neutrino 8.0 Guest Artifacts"
    echo "=========================================================="
    mkdir -p "${QNX_OUT}"

    if ! locate_qnx_env; then
        if [ "${DISTRO}" = "qnx" ]; then
            echo "Error: QNX SDP 8.0 environment (qcc) not found in PATH or standard paths." >&2
            echo "Please ensure QNX SDP 8.0 is installed and source qnxsdp-env.sh." >&2
            exit 1
        else
            echo "[WARNING] QNX SDP 8.0 environment not found. Skipping QNX build."
            return 0
        fi
    fi

    log_step "Compiling amba-virt-resmgr and libamba_virt (resource manager daemon & library)..."
    make -C "${ROOT_DIR}/guest-os/qnx/amba-virt" clean all
    cp -vf "${ROOT_DIR}/guest-os/qnx/amba-virt/amba-virt-resmgr" "${QNX_OUT}/"
    cp -vf "${ROOT_DIR}/guest-os/qnx/amba-virt/libamba_virt.so" "${QNX_OUT}/"
    cp -vf "${ROOT_DIR}/guest-os/qnx/amba-virt/libamba_virt.a" "${QNX_OUT}/"

    log_step "Compiling amba-cavalry-resmgr (/dev/cavalry resource manager)..."
    make -C "${ROOT_DIR}/guest-os/qnx/amba-cavalry" clean all
    cp -vf "${ROOT_DIR}/guest-os/qnx/amba-cavalry/amba-cavalry-resmgr" "${QNX_OUT}/"

    log_step "Compiling amba-virt-client and amba-virt-cli (QNX client)..."
    make -C "${ROOT_DIR}/guest-os/client" OS=qnx clean all
    cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-client" "${QNX_OUT}/"
    if [ -f "${ROOT_DIR}/guest-os/client/amba-virt-cli" ]; then
        cp -vf "${ROOT_DIR}/guest-os/client/amba-virt-cli" "${QNX_OUT}/"
    fi

    log_step "Compiling cavalry_hvm_demo (QNX AI benchmark demo)..."
    make -C "${ROOT_DIR}/guest-os/apps/cavalry-demo" OS=qnx clean all
    cp -vf "${ROOT_DIR}/guest-os/apps/cavalry-demo/cavalry_hvm_demo" "${QNX_OUT}/"

    log_step "Compiling cavalry_hvm_yolo (QNX YOLO inference engine)..."
    make -C "${ROOT_DIR}/guest-os/apps/cavalry-yolo" OS=qnx clean all
    cp -vf "${ROOT_DIR}/guest-os/apps/cavalry-yolo/cavalry_hvm_yolo" "${QNX_OUT}/"

    if [ "${BUILD_QNX_IMAGE}" -eq 1 ]; then
        log_step "Building QNX 8.0 HVM disk image..."
        "${ROOT_DIR}/guest-os/qnx/qnx-build/build.sh"
        if [ -f "${ROOT_DIR}/guest-os/qnx/qnx-build/output/dist/qnx-8.0-arm64-cloudimg.qcow2" ]; then
            cp -vf "${ROOT_DIR}/guest-os/qnx/qnx-build/output/dist/qnx-8.0-arm64-cloudimg.qcow2" "${QNX_OUT}/"
        fi
    fi

    echo ""
    log_step "QNX 8.0 artifacts staged in ${QNX_OUT}:"
    ls -lh "${QNX_OUT}"
}

case "${DISTRO}" in
    ubuntu)
        build_ubuntu
        ;;
    alpine)
        build_alpine
        ;;
    qnx)
        build_qnx
        ;;
    all)
        build_ubuntu
        build_alpine
        build_qnx
        ;;
    *)
        echo "Error: Unsupported distro '${DISTRO}'. Supported: ubuntu, alpine, qnx, all" >&2
        exit 1
        ;;
esac

echo ""
echo "=========================================================="
echo " Guest OS Cross-Compilation Complete!"
echo " Staged artifacts directory: ${BUILD_DIR}"
echo "=========================================================="
