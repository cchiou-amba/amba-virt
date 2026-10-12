#!/usr/bin/env bash
#
# guest-os/deploy_guest.sh
# Automated, reproducible deployment script for Ambarella HVM guest VMs:
#   - Ubuntu 24.04 LTS HVM (n1-655-devkit-ubuntu, n1-655-pro-ubuntu)
#   - Alpine Linux 3.20 HVM (n1-655-devkit-alpine, n1-655-pro-alpine)
#   - BlackBerry QNX Neutrino 8.0 HVM (n1-655-devkit-qnx, n1-655-pro-qnx)
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
PROVISION=0
DRY_RUN=0

BUILD_GUEST_DIR="${ROOT_DIR}/build/guest"

usage() {
    cat <<EOF
Usage: $0 [options] <target-node>

Arguments:
  <target-node>         SSH host alias (e.g. n1-655-devkit-ubuntu, n1-655-pro-qnx,
                        n1-655-devkit-alpine)

Options:
  --distro=DISTRO       Override auto-detected distro (ubuntu, alpine, qnx)
  --reload              Unload existing kernel modules and reinsert fresh ones
  --enable-serial       Enable and start serial-getty / QNX console on target
  --provision           Install the demo service (binary, model, unit, environment) and enable it
                        so the guest starts the demo on every boot, with no manual step
  --test                Run automated post-deployment smoke test
  --dry-run             Show actions without executing remote commands
  -h, --help            Show this help message

Supported Targets & Defaults:
  n1-655-devkit-ubuntu   -> Ubuntu 24.04 HVM (port 2222, amba_virt.ko + amba_uart.ko)
  n1-655-pro-ubuntu      -> Ubuntu 24.04 HVM (port 2222, amba_virt.ko + amba_uart.ko)
  n1-655-devkit-qnx      -> QNX 8.0 HVM     (port 2322, amba-virt-resmgr + devc-seramb)
  n1-655-pro-qnx         -> QNX 8.0 HVM     (port 2322, amba-virt-resmgr + devc-seramb)
  n1-655-devkit-alpine   -> Alpine 3.20 HVM (port 2422, musl static binaries)
  n1-655-devkit-<name>   -> any guest-os/<name>/ that has a deploy-guest.conf (glibc, systemd)
  n1-655-pro-alpine      -> Alpine 3.20 HVM (port 2422, musl static binaries)

Environment:
  SSH_OPTS              Extra ssh/scp options, e.g. a jump host through Dom0 when a guest's port
                        forward is not available: SSH_OPTS="-J n1-655-devkit" $0 root@<guest-ip> --distro=alpine

Examples:
  $0 n1-655-devkit-ubuntu --reload --enable-serial --test
  $0 n1-655-pro-qnx --reload --test
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
        --provision)
            PROVISION=1
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
        *)
            # AMBA-GUEST-EXTENSIONS: a guest that ships guest-os/<name>/deploy-guest.conf is matched by <name> in the target
            for ext in "${SCRIPT_DIR}"/*/deploy-guest.conf; do
                [ -f "${ext}" ] || continue
                ext_name="$(basename "$(dirname "${ext}")")"
                case "${TARGET_NODE}" in *"${ext_name}"*) DISTRO="${ext_name}" ;; esac
            done
            if [ -z "${DISTRO}" ]; then
                echo "Warning: Could not infer distro from target name '${TARGET_NODE}'. Defaulting to ubuntu."
                DISTRO="ubuntu"
            fi
            ;;
    esac
fi

# A guest with its own recipe sets EXT_LINUX_DISTRO (the Linux/systemd deploy path) and EXT_DEMO_PORT
EXT_LINUX_DISTRO=""
EXT_DEMO_PORT=""
if [[ "${DISTRO}" =~ ^[a-z0-9_]+$ ]] && [ -f "${SCRIPT_DIR}/${DISTRO}/deploy-guest.conf" ]; then
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/${DISTRO}/deploy-guest.conf"
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
        ssh ${SSH_OPTS:-} "${TARGET_NODE}" "${cmd}"
    fi
}

copy_to_target() {
    local src="$1"
    local dst="$2"
    if [ "${DRY_RUN}" -eq 1 ]; then
        echo "[DRY-RUN] scp ${src} ${TARGET_NODE}:${dst}"
    else
        scp ${SSH_OPTS:-} "${src}" "${TARGET_NODE}:${dst}"
    fi
}

log_step "Verifying connectivity to target node: ${TARGET_NODE} (Flavor: ${DISTRO})"
if [ "${DRY_RUN}" -eq 0 ]; then
    if ! ssh ${SSH_OPTS:-} -o ConnectTimeout=5 "${TARGET_NODE}" "echo 'Target connected: \$(uname -a)'"; then
        echo "Error: Unable to connect to ${TARGET_NODE} via SSH." >&2
        echo "Please ensure the guest VM is running and host alias is configured in ~/.ssh/config." >&2
        exit 1
    fi
fi

STAGING_DIR="${BUILD_GUEST_DIR}/${DISTRO}"

# Deployment by target OS flavor
case "${DISTRO}" in
    ubuntu|alpine|"${EXT_LINUX_DISTRO:-ubuntu}")
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

        # 3b. Provision the demo service, so a cold boot of the guest starts it by itself
        if [ "${PROVISION}" -eq 1 ]; then
            log_step "Provisioning the demo service on ${TARGET_NODE}..."
            DEMO_DIR="${ROOT_DIR}/guest-os/apps/amba-virt-demo"
            MODEL="${ROOT_DIR}/guest-os/apps/cavalry-yolo/n1-655_yolox_s_amba_optimized.bin"
            if [ -n "${EXT_DEMO_PORT}" ]; then
                DEMO_BIN="${DEMO_DIR}/amba_virt_demo"
                DEMO_PORT="${EXT_DEMO_PORT}"
            elif [ "${DISTRO}" = "ubuntu" ]; then
                DEMO_BIN="${DEMO_DIR}/amba_virt_demo"
                DEMO_PORT=8080
            else
                DEMO_BIN="${DEMO_DIR}/amba_virt_demo-musl"
                DEMO_PORT=8081
            fi
            for f in "${DEMO_BIN}" "${MODEL}" "${DEMO_DIR}/service/amba-virt-demo.env"; do
                if [ "${DRY_RUN}" -eq 0 ] && [ ! -e "${f}" ]; then
                    echo "Error: ${f} is missing; build it first (make guest-${DISTRO})." >&2
                    exit 1
                fi
            done
            copy_to_target "${DEMO_BIN}" "/tmp/amba_staging/amba_virt_demo"
            copy_to_target "${MODEL}" "/tmp/amba_staging/n1-655_yolox_s_amba_optimized.bin"
            copy_to_target "${DEMO_DIR}/service/amba-virt-demo.env" "/tmp/amba_staging/amba-virt-demo.env"
            run_remote "sudo cp /tmp/amba_staging/amba_virt_demo /usr/local/bin/amba_virt_demo && sudo chmod 0755 /usr/local/bin/amba_virt_demo"
            run_remote "sudo mkdir -p /opt/cavalry && sudo cp /tmp/amba_staging/n1-655_yolox_s_amba_optimized.bin /opt/cavalry/n1-655_yolox_s_amba_optimized.bin && sudo chmod 0644 /opt/cavalry/n1-655_yolox_s_amba_optimized.bin"
            run_remote "sed 's/^AMBA_VIRT_DEMO_BIND=.*/AMBA_VIRT_DEMO_BIND=0.0.0.0:${DEMO_PORT}/' /tmp/amba_staging/amba-virt-demo.env > /tmp/amba_staging/demo.env.new && sudo cp /tmp/amba_staging/demo.env.new /etc/amba-virt-demo.env && sudo chmod 0644 /etc/amba-virt-demo.env"
            if [ "${DISTRO}" = "ubuntu" ] || [ -n "${EXT_LINUX_DISTRO}" ]; then
                copy_to_target "${DEMO_DIR}/service/amba-virt-demo.service" "/tmp/amba_staging/amba-virt-demo.service"
                run_remote "sudo cp /tmp/amba_staging/amba-virt-demo.service /etc/systemd/system/amba-virt-demo.service && sudo systemctl daemon-reload && sudo systemctl enable amba-virt-demo.service"
            else
                copy_to_target "${DEMO_DIR}/service/amba-virt-demo.openrc" "/tmp/amba_staging/amba-virt-demo"
                run_remote "sudo cp /tmp/amba_staging/amba-virt-demo /etc/init.d/amba-virt-demo && sudo chmod 0755 /etc/init.d/amba-virt-demo && sudo rc-update add amba-virt-demo default"
            fi
        fi

        # 3b2. Hosts with SELinux: give what was copied its default label (restorecon exists only where SELinux does)
        if [ "${PROVISION}" -eq 1 ]; then
            run_remote "if command -v restorecon >/dev/null 2>&1; then sudo restorecon -RF /usr/local/bin /etc/systemd/system /etc/amba-virt-demo.env /opt/cavalry /lib/modules/\$(uname -r)/extra 2>/dev/null || true; fi"
        fi

        # 3c. The installed files must not be empty (a failed copy must stop the deployment)
        if [ "${PROVISION}" -eq 1 ] && [ "${DRY_RUN}" -eq 0 ]; then
            if ! run_remote "for f in /usr/local/bin/amba_virt_demo /opt/cavalry/n1-655_yolox_s_amba_optimized.bin /etc/amba-virt-demo.env /lib/modules/\$(uname -r)/extra/iav_virt.ko; do [ -s \$f ] || { echo \"empty or missing: \$f\" >&2; exit 1; }; done"; then
                echo "Error: provisioning left an empty or missing file on ${TARGET_NODE}." >&2
                exit 1
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
            run_remote "chmod +x /system/bin/amba* /system/bin/cavalry* /system/bin/devc-ser* /system/bin/qnx* 2>/dev/null || true"
        fi

        # 3. Reload QNX resource managers if requested
        if [ "${RELOAD}" -eq 1 ]; then
            log_step "Restarting QNX resource managers on ${TARGET_NODE}..."
            run_remote "slay -f qnx-getty qnx-serial-console.sh amba-virt-resmgr amba-cavalry-resmgr devc-ser8250 devc-seramb 2>/dev/null || true"
            run_remote "if [ -x /system/bin/amba-virt-resmgr ]; then /system/bin/amba-virt-resmgr >/dev/null 2>&1 & fi"
            run_remote "if [ -x /system/bin/devc-seramb ]; then /system/bin/devc-seramb -p /dev/ser3 -a 0x0c000000 -i 144 >/dev/null 2>&1 & elif [ -x /system/bin/devc-ser8250 ]; then /system/bin/devc-ser8250 -e -b115200 -c24000000/16 -u3 0x0c000000^2,144 >/dev/null 2>&1 & fi"
        fi

        # 4. Enable QNX interactive login console if requested
        if [ "${ENABLE_SERIAL}" -eq 1 ]; then
            log_step "Configuring QNX interactive console on /dev/ser3 (with auto-respawn)..."
            run_remote "slay -f qnx-getty qnx-serial-console.sh 2>/dev/null || true"
            run_remote "if [ -x /system/bin/qnx-serial-console.sh ]; then /system/bin/qnx-serial-console.sh /dev/ser3 >/dev/null 2>&1 & echo 'Interactive auto-respawn console supervisor active on /dev/ser3'; elif [ -x /system/bin/qnx-getty ]; then /system/bin/qnx-getty /dev/ser3 >/dev/null 2>&1 & echo 'Interactive auto-respawn getty daemon active on /dev/ser3'; fi"
        fi


        # 5. Post-deployment smoke test
        if [ "${RUN_SMOKE_TEST}" -eq 1 ]; then
            log_step "Running QNX sanity verification..."
            run_remote "pidin && ls -l /dev/amba* /dev/ser* /dev/cavalry* 2>/dev/null || true"
        fi
        ;;
esac

log_step "Deployment to ${TARGET_NODE} completed successfully!"
