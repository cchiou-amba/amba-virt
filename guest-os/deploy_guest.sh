#!/usr/bin/env bash
#
# guest-os/deploy_guest.sh
# Automated, reproducible deployment script for Ambarella HVM guest VMs and NOHYPER containers:
#   - Ubuntu 24.04 LTS HVM (n1-655-devkit-ubuntu, n1-655-pro-ubuntu)
#   - Alpine Linux 3.20 HVM (n1-655-devkit-alpine, n1-655-pro-alpine)
#   - BlackBerry QNX Neutrino 8.0 HVM (n1-655-devkit-qnx, n1-655-pro-qnx)
#   - Bare-Metal NOHYPER Containers (n1-655-devkit-nohyper, n1-655-pro-nohyper)
#
# Copyright (C) 2026, Ambarella International LLC
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET_NODE=""
DISTRO=""
RELOAD=0
ENABLE_SERIAL=0
RUN_SMOKE_TEST=0
DRY_RUN=0

BUILD_GUEST_DIR="${ROOT_DIR}/build/guest"

usage() {
    cat <<EOF
Usage: $0 [options] <target-node>

Arguments:
  <target-node>         SSH host alias (e.g. n1-655-devkit-ubuntu, n1-655-pro-qnx,
                        n1-655-pro-nohyper, n1-655-devkit-alpine)

Options:
  --distro=DISTRO       Override auto-detected distro (ubuntu, alpine, qnx, nohyper)
  --reload              Unload existing kernel modules and reinsert fresh ones
  --enable-serial       Enable and start serial-getty / QNX console on target
  --test                Run automated post-deployment smoke test
  --dry-run             Show actions without executing remote commands
  -h, --help            Show this help message

Supported Targets & Defaults:
  n1-655-devkit-ubuntu   -> Ubuntu 24.04 HVM (port 2222, amba_virt.ko + amba_uart.ko)
  n1-655-pro-ubuntu      -> Ubuntu 24.04 HVM (port 2222, amba_virt.ko + amba_uart.ko)
  n1-655-devkit-qnx      -> QNX 8.0 HVM     (port 2322, amba-virt-resmgr + devc-seramb)
  n1-655-pro-qnx         -> QNX 8.0 HVM     (port 2322, amba-virt-resmgr + devc-seramb)
  n1-655-devkit-alpine   -> Alpine 3.20 HVM (port 2422, musl static binaries)
  n1-655-pro-alpine      -> Alpine 3.20 HVM (port 2422, musl static binaries)
  n1-655-devkit-nohyper  -> NOHYPER Bare-Metal Container (port 4222)
  n1-655-pro-nohyper     -> NOHYPER Bare-Metal Container (port 4222)

Examples:
  $0 n1-655-devkit-ubuntu --reload --enable-serial --test
  $0 n1-655-pro-qnx --reload --test
  $0 n1-655-devkit-nohyper --reload --test
EOF
}

# Parse options
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
        --reload)
            RELOAD=1
            shift
            ;;
        --enable-serial)
            ENABLE_SERIAL=1
            shift
            ;;
        --test)
            RUN_SMOKE_TEST=1
            shift
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
            echo "Error: Unknown option $1" >&2
            usage
            exit 1
            ;;
        *)
            if [ -z "${TARGET_NODE}" ]; then
                TARGET_NODE="$1"
                shift
            else
                echo "Error: Unexpected positional argument: $1" >&2
                usage
                exit 1
            fi
            ;;
    esac
done

if [ -z "${TARGET_NODE}" ]; then
    echo "Error: Target node is required." >&2
    usage
    exit 1
fi

# Auto-detect distro flavor from target node name if not specified
if [ -z "${DISTRO}" ]; then
    case "${TARGET_NODE}" in
        *ubuntu*)  DISTRO="ubuntu" ;;
        *alpine*)  DISTRO="alpine" ;;
        *qnx*)     DISTRO="qnx" ;;
        *nohyper*) DISTRO="nohyper" ;;
        *)
            echo "Warning: Could not infer distro from target name '${TARGET_NODE}'. Defaulting to ubuntu."
            DISTRO="ubuntu"
            ;;
    esac
fi

log_step() {
    echo ""
    echo "[$(date '+%H:%M:%S')] >>> $1"
}

run_remote() {
    local cmd="$1"
    if [ "${DRY_RUN}" -eq 1 ]; then
        echo "[DRY-RUN] ssh ${TARGET_NODE} \"${cmd}\""
    else
        ssh "${TARGET_NODE}" "${cmd}"
    fi
}

copy_to_target() {
    local src="$1"
    local dst="$2"
    if [ "${DRY_RUN}" -eq 1 ]; then
        echo "[DRY-RUN] scp ${src} ${TARGET_NODE}:${dst}"
    else
        scp "${src}" "${TARGET_NODE}:${dst}"
    fi
}

log_step "Verifying connectivity to target node: ${TARGET_NODE} (Flavor: ${DISTRO})"
if [ "${DRY_RUN}" -eq 0 ]; then
    if ! ssh -o ConnectTimeout=5 "${TARGET_NODE}" "echo 'Target connected: \$(uname -a)'"; then
        echo "Error: Unable to connect to ${TARGET_NODE} via SSH." >&2
        echo "Please ensure the guest VM / container is running and host alias is configured in ~/.ssh/config." >&2
        exit 1
    fi
fi

STAGING_DIR="${BUILD_GUEST_DIR}/${DISTRO}"

# Deployment by target OS flavor
case "${DISTRO}" in
    ubuntu|alpine|nohyper)
        log_step "Deploying Linux Guest Artifacts (${DISTRO}) to ${TARGET_NODE}..."

        # 1. Ensure remote directories exist and are writable for staging
        run_remote "sudo mkdir -p /lib/modules/\$(uname -r)/extra /usr/local/bin /tmp/amba_staging && sudo chmod 777 /tmp/amba_staging"

        # 2. Stage kernel modules
        if [ -d "${STAGING_DIR}" ]; then
            KMODS=$(find "${STAGING_DIR}" -name "*.ko" 2>/dev/null || true)
            if [ -n "${KMODS}" ]; then
                log_step "Copying kernel modules..."
                for ko in ${KMODS}; do
                    echo "  - Staging $(basename "${ko}")"
                    copy_to_target "${ko}" "/tmp/amba_staging/"
                done
                run_remote "sudo cp /tmp/amba_staging/*.ko /lib/modules/\$(uname -r)/extra/ && sudo depmod -a"
            fi

            # 3. Stage userspace binaries
            BINS=$(find "${STAGING_DIR}" -maxdepth 1 -type f -executable ! -name "*.ko" 2>/dev/null || true)
            if [ -n "${BINS}" ]; then
                log_step "Copying userspace binaries..."
                for bin in ${BINS}; do
                    echo "  - Staging $(basename "${bin}")"
                    copy_to_target "${bin}" "/tmp/amba_staging/"
                done
                run_remote "sudo cp /tmp/amba_staging/* /usr/local/bin/ 2>/dev/null || true"
                run_remote "sudo chmod +x /usr/local/bin/amba-virt* /usr/local/bin/cavalry* 2>/dev/null || true"
            fi
        fi

        # 4. Handle kernel module reload & insertion
        if [ "${RELOAD}" -eq 1 ]; then
            log_step "Reloading kernel modules on ${TARGET_NODE}..."
            run_remote "sudo rmmod amba_uart 2>/dev/null || true"
            run_remote "sudo modprobe amba_virt 2>/dev/null || sudo insmod /lib/modules/\$(uname -r)/extra/amba_virt.ko 2>/dev/null || true"
            run_remote "if [ -f /lib/modules/\$(uname -r)/extra/amba_uart.ko ]; then sudo modprobe amba_uart 2>/dev/null || sudo insmod /lib/modules/\$(uname -r)/extra/amba_uart.ko; fi"
        fi


        # 5. Enable serial console getty service if requested
        if [ "${ENABLE_SERIAL}" -eq 1 ]; then
            log_step "Configuring interactive serial-getty service on ttyAMBA0..."
            run_remote "if [ -e /dev/ttyAMBA0 ]; then if command -v systemctl >/dev/null 2>&1; then sudo systemctl enable --now serial-getty@ttyAMBA0.service; elif [ -f /etc/inittab ]; then grep -q ttyAMBA0 /etc/inittab || echo 'ttyAMBA0::respawn:/sbin/getty 115200 ttyAMBA0' | sudo tee -a /etc/inittab; fi; echo 'serial-getty on ttyAMBA0 configured successfully'; else echo 'Notice: /dev/ttyAMBA0 not present yet; skipping service enable'; fi"
        fi

        # 6. Post-deployment smoke test
        if [ "${RUN_SMOKE_TEST}" -eq 1 ]; then
            log_step "Running smoke test on ${TARGET_NODE}..."
            run_remote "if command -v amba-virt-client >/dev/null 2>&1; then amba-virt-client --smoke || true; else echo 'amba-virt-client not found, checking dmesg...'; dmesg | tail -n 20; fi"
        fi
        ;;

    qnx)
        log_step "Deploying QNX 8.0 Guest Artifacts to ${TARGET_NODE}..."

        # 1. Create staging directory on QNX
        run_remote "mkdir -p /system/bin /system/lib /data/tmp/amba_staging"

        # 2. Stage QNX binaries and resource managers
        if [ -d "${STAGING_DIR}" ]; then
            log_step "Copying QNX resource managers and libraries..."
            for item in "${STAGING_DIR}"/*; do
                if [ -f "${item}" ]; then
                    echo "  - Staging $(basename "${item}")"
                    copy_to_target "${item}" "/data/tmp/amba_staging/"
                fi
            done
            run_remote "cp /data/tmp/amba_staging/*.so* /system/lib/ 2>/dev/null || true"
            run_remote "cp /data/tmp/amba_staging/* /system/bin/ 2>/dev/null || true"
            run_remote "chmod +x /system/bin/amba* /system/bin/cavalry* /system/bin/devc-ser* 2>/dev/null || true"
        fi


        # 3. Reload QNX resource managers if requested
        if [ "${RELOAD}" -eq 1 ]; then
            log_step "Restarting QNX resource managers on ${TARGET_NODE}..."
            run_remote "slay -f qnx-getty qnx-serial-console.sh amba-virt-resmgr amba-cavalry-resmgr devc-ser8250 devc-seramb 2>/dev/null || true"
            run_remote "if [ -x /system/bin/amba-virt-resmgr ]; then /system/bin/amba-virt-resmgr >/dev/null 2>&1 & fi"
            run_remote "if [ -x /system/bin/devc-seramb ]; then /system/bin/devc-seramb -p /dev/ser3 >/dev/null 2>&1 & elif [ -x /system/bin/devc-ser8250 ]; then /system/bin/devc-ser8250 -e -F -b115200 0xffe0019000 >/dev/null 2>&1 & fi"
        fi

        # 4. Enable QNX interactive login console if requested
        if [ "${ENABLE_SERIAL}" -eq 1 ]; then
            log_step "Configuring QNX interactive console on /dev/ser3 (with auto-respawn)..."
            run_remote "slay -f qnx-getty qnx-serial-console.sh 2>/dev/null || true"
            run_remote "if [ -x /system/bin/qnx-getty ]; then /system/bin/qnx-getty /dev/ser3 >/dev/null 2>&1 & echo 'Interactive auto-respawn getty daemon active on /dev/ser3'; elif [ -x /system/bin/qnx-serial-console.sh ]; then /system/bin/qnx-serial-console.sh /dev/ser3 >/dev/null 2>&1 & echo 'Interactive auto-respawn console supervisor active on /dev/ser3'; fi"
        fi


        # 5. Post-deployment smoke test
        if [ "${RUN_SMOKE_TEST}" -eq 1 ]; then
            log_step "Running QNX sanity verification..."
            run_remote "pidin && ls -l /dev/amba* /dev/ser* /dev/cavalry* 2>/dev/null || true"
        fi
        ;;
esac

log_step "Deployment to ${TARGET_NODE} completed successfully!"
