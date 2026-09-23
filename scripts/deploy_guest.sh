#!/bin/sh
#
# scripts/deploy_guest.sh
# Top-level wrapper script to deploy artifacts to HVM guest VMs and NOHYPER containers.
#
# Copyright (C) 2026, Ambarella International LLC
#

set -eu
ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
exec "${ROOT_DIR}/guest-os/deploy_guest.sh" "$@"
