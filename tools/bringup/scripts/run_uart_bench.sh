#!/usr/bin/env bash
#
# tools/bringup/scripts/run_uart_bench.sh
#
# Automated Directional UART Benchmark Test Runner.
# Runs directional trials using native compiled uart_bench_agent.
#
# Copyright (C) 2026, Ambarella International LLC
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRINGUP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
AGENT_BIN="${BRINGUP_DIR}/build/uart_bench_agent"

if [ ! -x "${AGENT_BIN}" ]; then
    echo "[*] Building bringup binaries..."
    make -C "${BRINGUP_DIR}"
fi

echo "============================================================"
echo " AMBARELLA HARDWARE UART BENCHMARK HARNESS"
echo "============================================================"

# Self-test the native agent binary
echo "[*] Running uart_bench_agent self-test..."
"${AGENT_BIN}" --mode selftest

# Run CppUTest suite for unit verification
echo "[*] Running CppUTest verification suite..."
make -C "${BRINGUP_DIR}" test

echo "============================================================"
echo " All benchmark agent and protocol tests completed cleanly."
echo "============================================================"

#
# Local variables:
# mode: Shell-script
# sh-basic-offset: 4
# tab-width: 4
# indent-tabs-mode: nil
# End:
#
