#!/bin/bash
# In-VM Provisioning Script for Lean Debian 13 (trixie) ARM64 HVM Image
# Executed via amba-image-provision.service after cloud-final.service

set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
trap 'echo "AMBA_IMAGE_PROVISION_FAILED: line $LINENO" > /dev/console 2>/dev/null || true; echo "AMBA_IMAGE_PROVISION_FAILED: line $LINENO"' ERR

echo "=== [AMBA-PROVISION] Starting in-VM image provisioning ==="

# 1. User & Authentication
echo "=== [AMBA-PROVISION] Configuring user debian and credentials ==="
id -u debian >/dev/null 2>&1 || useradd -m -s /bin/bash -U debian
echo "debian:debian" | chpasswd
passwd -u debian 2>/dev/null || true
chage -M -1 debian 2>/dev/null || true

cat << 'EOF' > /etc/sudoers.d/90-debian-user
debian ALL=(ALL) NOPASSWD:ALL
EOF
chmod 0440 /etc/sudoers.d/90-debian-user

# SSH server drop-in; do not accept the client's locale (the image ships no locale data beyond C.UTF-8)
mkdir -p /etc/ssh/sshd_config.d
cat << 'EOF' > /etc/ssh/sshd_config.d/50-ambarella.conf
PasswordAuthentication yes
KbdInteractiveAuthentication yes
PermitRootLogin no
EOF
sed -i 's/^AcceptEnv/#AcceptEnv/' /etc/ssh/sshd_config

# SSH hostkeys service
systemctl daemon-reload
systemctl enable ssh-hostkeys.service || true
systemctl enable getty@tty0.service || true
systemctl enable serial-getty@ttyAMA0.service || true

# 2. Netplan & Network
echo "=== [AMBA-PROVISION] Configuring netplan ==="
mkdir -p /etc/netplan
rm -f /etc/netplan/50-cloud-init.yaml /etc/netplan/*cloud*
if [ -f /tmp/assets/01-ambarella.yaml ]; then
    cp -f /tmp/assets/01-ambarella.yaml /etc/netplan/01-ambarella.yaml
fi
chmod 0600 /etc/netplan/01-ambarella.yaml

# 3. Console & GRUB (the base image already sets GRUB_TIMEOUT=0)
echo "=== [AMBA-PROVISION] Configuring console and GRUB ==="
mkdir -p /etc/default/grub.d
cat << 'EOF' > /etc/default/grub.d/50-console.cfg
GRUB_CMDLINE_LINUX_DEFAULT="quiet console=tty0 console=ttyAMA0,115200"
EOF
if command -v update-grub >/dev/null 2>&1; then
    update-grub || true
fi

# 4. Storage & Root Growth
echo "=== [AMBA-PROVISION] Configuring root growth service ==="
chmod 0755 /usr/local/sbin/grow-rootfs.sh
systemctl enable grow-rootfs.service || true

# 5. Logging limits
echo "=== [AMBA-PROVISION] Configuring journald limits ==="
mkdir -p /etc/systemd/journald.conf.d
cat << 'EOF' > /etc/systemd/journald.conf.d/10-limits.conf
[Journal]
Storage=persistent
SystemMaxUse=16M
RuntimeMaxUse=16M
EOF

# 6. EFI Fallback Verification
echo "=== [AMBA-PROVISION] Verifying EFI fallback bootloader ==="
mkdir -p /boot/efi/EFI/BOOT
if [ ! -f /boot/efi/EFI/BOOT/BOOTAA64.EFI ] && [ ! -f /boot/efi/EFI/BOOT/bootaa64.efi ]; then
    EFI_SRC=$(find /boot/efi -iname "shimaa64.efi" -o -iname "grubaa64.efi" 2>/dev/null | head -n 1)
    if [ -n "$EFI_SRC" ] && [ -f "$EFI_SRC" ]; then
        echo "Found EFI source: $EFI_SRC, copying to /boot/efi/EFI/BOOT/BOOTAA64.EFI"
        cp -f "$EFI_SRC" /boot/efi/EFI/BOOT/BOOTAA64.EFI
    fi
fi

# 6b. Kernel: the generic Debian kernel replaces the cloud kernel (the cloud kernel is built without the dmaengine core,
# CONFIG_DMADEVICES is not set, so amba_dma.ko cannot link against it). Keep this version equal to KERNEL_VERSION in base-image.lock.
GENERIC_KERNEL="6.12.111+deb13-arm64"
echo "=== [AMBA-PROVISION] Installing the generic kernel ${GENERIC_KERNEL} ==="
apt-get update -o Dpkg::Use-Pty=0
apt-get install -y --no-install-recommends -o Dpkg::Use-Pty=0 "linux-image-${GENERIC_KERNEL}"

# 7. Package Policy: Retain-set & Purges
echo "=== [AMBA-PROVISION] Applying package policy ==="

RETAIN_PACKAGES=(
    apt base-files bash ca-certificates cloud-guest-utils coreutils curl dbus
    e2fsprogs efibootmgr grub-cloud-arm64 grub-efi-arm64-bin grub-efi-arm64-signed
    grub-efi-arm64-unsigned grub2-common initramfs-tools iproute2 iputils-ping kmod
    libpam-systemd linux-image-6.12.111+deb13-arm64 netplan.io openssh-client openssh-server
    pciutils python3 shim-signed sudo systemd systemd-resolved systemd-sysv
    systemd-timesyncd udev util-linux vim-tiny
)

REMOVE_PACKAGES=(
    apt-listchanges bash-completion bind9-host cloud-image-utils cloud-init
    linux-image-6.12.111+deb13-cloud-arm64 linux-image-cloud-arm64
    cloud-initramfs-growroot cloud-utils eject genisoimage groff-base libc-l10n
    locales man-db manpages nano netcat-openbsd polkitd python3-debianbts
    python3-reportbug qemu-utils reportbug screen socat ssh-import-id tcpdump
    traceroute unattended-upgrades vim vim-runtime wget whiptail
)

# Mark retain-set manual (batched dpkg-query & apt-mark)
mapfile -t INSTALLED_RETAIN < <(dpkg-query -W -f='${Package} ${Status}\n' "${RETAIN_PACKAGES[@]}" 2>/dev/null | grep "ok installed" | awk '{print $1}')
if [ "${#INSTALLED_RETAIN[@]}" -gt 0 ]; then
    apt-mark manual "${INSTALLED_RETAIN[@]}" >/dev/null 2>&1 || true
fi

# Removal candidates that are installed
mapfile -t TO_REMOVE < <(dpkg-query -W -f='${Package} ${Status}\n' "${REMOVE_PACKAGES[@]}" 2>/dev/null | grep "ok installed" | awk '{print $1}')

# Pre-stop services targeted for removal and mask the periodic timers (no package manager traffic on a running guest)
systemctl stop unattended-upgrades.service apt-daily.timer apt-daily-upgrade.timer apt-listchanges.timer man-db.timer 2>/dev/null || true
systemctl mask apt-daily.timer apt-daily-upgrade.timer apt-listchanges.timer dpkg-db-backup.timer e2scrub_all.timer fstrim.timer man-db.timer 2>/dev/null || true

# Never install documentation, manual pages or extra locales again
mkdir -p /etc/dpkg/dpkg.cfg.d
cat << 'EOF' > /etc/dpkg/dpkg.cfg.d/01-ambarella-nodoc
path-exclude=/usr/share/doc/*
path-include=/usr/share/doc/*/copyright
path-exclude=/usr/share/man/*
path-exclude=/usr/share/info/*
EOF

# Temporarily divert update-initramfs so it does not run once per purged package under emulation
if [ -f /usr/sbin/update-initramfs ]; then
    dpkg-divert --local --rename --add /usr/sbin/update-initramfs
    ln -sf /bin/true /usr/sbin/update-initramfs
fi

if [ "${#TO_REMOVE[@]}" -gt 0 ]; then
    echo "Simulating package purge: ${TO_REMOVE[*]}"
    apt-get purge --auto-remove -s -y -o Dpkg::Use-Pty=0 "${TO_REMOVE[@]}" > /tmp/purge-simulation.log

    # The simulation must not remove an essential package nor anything in the retain-set
    if grep -E "^Remv (base-files|bash|systemd|util-linux|coreutils|apt|dpkg|libc6) " /tmp/purge-simulation.log; then
        echo "[AMBA-PROVISION] ERROR: Purge simulation attempts to remove essential packages!" >&2
        cat /tmp/purge-simulation.log >&2
        exit 1
    fi
    for pkg in "${INSTALLED_RETAIN[@]}"; do
        if grep -E "^Remv ${pkg} " /tmp/purge-simulation.log; then
            echo "[AMBA-PROVISION] ERROR: Purge simulation attempts to remove retained package ${pkg}!" >&2
            cat /tmp/purge-simulation.log >&2
            exit 1
        fi
    done

    echo "Executing package purge with auto-remove..."
    apt-get purge --auto-remove -y -o Dpkg::Use-Pty=0 "${TO_REMOVE[@]}"
fi

# The image has no locale data beyond the built-in C.UTF-8
cat << 'EOF' > /etc/default/locale
LANG=C.UTF-8
EOF

# Remove documentation and manual pages that were installed before the exclusion rule existed
find /usr/share/doc -mindepth 2 ! -name copyright -type f -delete 2>/dev/null || true
find /usr/share/doc -mindepth 1 -type d -empty -delete 2>/dev/null || true
rm -rf /usr/share/man/* /usr/share/info/* /usr/share/locale/* /etc/cloud /var/lib/cloud

# Restore update-initramfs and generate a single ramdisk
if [ -f /usr/sbin/update-initramfs.distrib ]; then
    rm -f /usr/sbin/update-initramfs
    dpkg-divert --local --rename --remove /usr/sbin/update-initramfs
    echo "=== [AMBA-PROVISION] Generating final initramfs ==="
    update-initramfs -u
fi
if command -v update-grub >/dev/null 2>&1; then
    update-grub || true
fi

# 8. Clean machine identity and SSH keys
echo "=== [AMBA-PROVISION] Cleaning temporary identity and SSH keys ==="
rm -f /etc/ssh/ssh_host_*_key*
truncate -s 0 /etc/machine-id
rm -f /var/lib/dbus/machine-id

# 9. Clean caches and temporary logs
rm -rf /var/lib/apt/lists/*
rm -rf /var/cache/apt/archives/*
rm -rf /tmp/* /var/tmp/*
find /var/log -type f -exec truncate -s 0 {} +

# Return the free blocks to the image (the bake VM maps discard), so the compressed image does not carry them
sync
fstrim -av || true

# 10. Disable and remove amba-image-provision service
systemctl disable amba-image-provision.service 2>/dev/null || true
rm -f /etc/systemd/system/amba-image-provision.service

sync

echo "=== [AMBA-PROVISION] Provisioning complete ==="
echo "AMBA_IMAGE_PROVISION_COMPLETE" > /dev/console 2>/dev/null || true
echo "AMBA_IMAGE_PROVISION_COMPLETE"

# Shutdown guest
systemctl poweroff || poweroff -f
