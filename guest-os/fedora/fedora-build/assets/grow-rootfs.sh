#!/bin/bash
set -euo pipefail

ROOT_DEV=$(findmnt -n -o SOURCE / 2>/dev/null || true)
# btrfs reports the device with the subvolume in brackets: /dev/vda2[/root]
ROOT_DEV=${ROOT_DEV%%[*}
if [ -z "$ROOT_DEV" ]; then
    echo "grow-rootfs: unable to determine root mount device" >&2
    exit 1
fi

REAL_DEV=$(readlink -f "$ROOT_DEV")
if [ ! -b "$REAL_DEV" ]; then
    echo "grow-rootfs: root device $REAL_DEV is not a block device" >&2
    exit 1
fi

FSTYPE=$(findmnt -n -o FSTYPE / 2>/dev/null || true)
if [ "$FSTYPE" != "btrfs" ]; then
    echo "grow-rootfs: root filesystem is not btrfs ($FSTYPE); skipping resize" >&2
    exit 0
fi

PARENT_DISK=$(lsblk -no PKNAME "$REAL_DEV" 2>/dev/null | tr -d ' ' || true)
PART_NUM=$(lsblk -no PARTN "$REAL_DEV" 2>/dev/null | tr -d ' ' || true)
if [ -z "$PARENT_DISK" ] || [ -z "$PART_NUM" ]; then
    echo "grow-rootfs: unrecognized partition layout for $REAL_DEV (PARENT=$PARENT_DISK, PARTN=$PART_NUM)" >&2
    exit 1
fi

DISK_DEV="/dev/$PARENT_DISK"
echo "grow-rootfs: extending partition $PART_NUM on $DISK_DEV..."
set +e
GROW_OUT=$(growpart "$DISK_DEV" "$PART_NUM" 2>&1)
GROW_RC=$?
set -e

if [ "$GROW_RC" -eq 0 ]; then
    echo "grow-rootfs: growpart succeeded: $GROW_OUT"
elif [ "$GROW_RC" -eq 1 ]; then
    echo "grow-rootfs: growpart reported no change: $GROW_OUT"
else
    echo "grow-rootfs: growpart failed (rc=$GROW_RC): $GROW_OUT" >&2
    exit "$GROW_RC"
fi

echo "grow-rootfs: resizing the btrfs filesystem on /..."
btrfs filesystem resize max /
echo "grow-rootfs: complete"
