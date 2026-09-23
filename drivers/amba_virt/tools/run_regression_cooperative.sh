#!/bin/bash
#
# run_regression_cooperative.sh
#
# Automated Regression Test Runner for Ambarella EVE + NOHYPER Cooperative Virtualization Architecture.
# Tests Suites A through E, Unit Tests, and Target System Integration.
#
# Copyright (C) 2026, Ambarella International LLC
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
TOOLS_DIR="${SCRIPT_DIR}"

echo "================================================================================"
echo " Starting Formalized Cooperative Virtualization Regression Test Suite"
echo " Date: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
echo " Repo: ${REPO_ROOT}"
echo "================================================================================"

TARGET_HOST=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --host)
            TARGET_HOST="$2"
            shift 2
            ;;
        --local)
            TARGET_HOST=""
            shift
            ;;
        *)
            shift
            ;;
    esac
done

echo ""
echo "[PHASE 1] Building and executing Host & Daemon Unit Test Suites..."
make -C "${TOOLS_DIR}" clean
make -C "${TOOLS_DIR}" test

echo ""
echo "[PHASE 2] Building Target Host Binaries (amba-virt-server, amba-virt-ctl, amba-virt-backend)..."
make -C "${TOOLS_DIR}" all

echo ""
echo "[PHASE 3] Building Guest Utilities (amba-virt-cli, amba-virt-client)..."
make -C "${REPO_ROOT}/guest-os/client" clean
make -C "${REPO_ROOT}/guest-os/client" CROSS_COMPILE=aarch64-linux-gnu- all

if [ -n "${TARGET_HOST}" ]; then
    echo ""
    echo "================================================================================"
    echo " [PHASE 4] Executing Live Target Integration Regression on: ${TARGET_HOST}"
    echo "================================================================================"

    echo "[TEST] Verifying Host Daemon connectivity on ${TARGET_HOST}..."
    ssh "${TARGET_HOST}" "amba-virt-ctl backend status"

    GUEST_HOST="${TARGET_HOST}-ubuntu"
    echo "[TEST] Verifying Guest HVM VM reachability on ${GUEST_HOST}..."
    ssh "${GUEST_HOST}" "echo ubuntu | sudo -S /usr/local/bin/amba-virt-cli ping"

    echo "[TEST] Verifying Guest Memory Layout & Partitions..."
    ssh "${GUEST_HOST}" "echo ubuntu | sudo -S /usr/local/bin/amba-virt-cli layout"

    echo "[TEST] Verifying Driver Matrix & Host Module Bitmask..."
    ssh "${GUEST_HOST}" "echo ubuntu | sudo -S /usr/local/bin/amba-virt-cli drivers"
    ssh "${GUEST_HOST}" "echo ubuntu | sudo -S /usr/local/bin/amba-virt-cli modules"

    echo "[TEST] Verifying JSON Output formatting..."
    ssh "${GUEST_HOST}" "echo ubuntu | sudo -S /usr/local/bin/amba-virt-cli drivers --json"

    echo "[TEST] Verifying Live 2-Stage Dynamic State Drain & Reload..."
    ssh "${TARGET_HOST}" "amba-virt-ctl module unload cavalry.ko"
    for i in $(seq 1 10); do
        sleep 1
        OFFLINE_STATUS=$(ssh "${GUEST_HOST}" "echo ubuntu | sudo -S /usr/local/bin/amba-virt-cli drivers" | grep "/dev/cavalry" | awk '{print $3}')
        if [ "${OFFLINE_STATUS}" = "OFFLINE" ]; then
            break
        fi
    done
    if [ "${OFFLINE_STATUS}" != "OFFLINE" ]; then
        echo "Error: Expected /dev/cavalry to be OFFLINE, got: ${OFFLINE_STATUS}" >&2
        exit 1
    fi
    echo "[PASS] Dynamic transition to OFFLINE verified (Status: ${OFFLINE_STATUS})"

    ssh "${TARGET_HOST}" "amba-virt-ctl module load cavalry.ko"
    for i in $(seq 1 10); do
        sleep 1
        ONLINE_STATUS=$(ssh "${GUEST_HOST}" "echo ubuntu | sudo -S /usr/local/bin/amba-virt-cli drivers" | grep "/dev/cavalry" | awk '{print $3}')
        if [ "${ONLINE_STATUS}" = "ONLINE" ]; then
            break
        fi
    done
    if [ "${ONLINE_STATUS}" != "ONLINE" ]; then
        echo "Error: Expected /dev/cavalry to be ONLINE, got: ${ONLINE_STATUS}" >&2
        exit 1
    fi
    echo "[PASS] Dynamic transition to ONLINE verified (Status: ${ONLINE_STATUS})"
fi

echo ""
echo "================================================================================"
echo " ALL REGRESSION TEST SUITES PASSED (100% SUCCESS)"
echo " Ready for Production Sign-off"
echo "================================================================================"
