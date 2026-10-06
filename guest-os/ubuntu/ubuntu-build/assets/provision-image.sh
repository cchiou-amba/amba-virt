#!/bin/bash
# In-VM Provisioning Script for Lean Ubuntu 24.04 ARM64 HVM Image
# Executed via amba-image-provision.service after cloud-final.service

set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
trap 'echo "AMBA_IMAGE_PROVISION_FAILED: line $LINENO" > /dev/console 2>/dev/null || true; echo "AMBA_IMAGE_PROVISION_FAILED: line $LINENO"' ERR

echo "=== [AMBA-PROVISION] Starting in-VM image provisioning ==="

# 1. User & Authentication
echo "=== [AMBA-PROVISION] Configuring user ubuntu and credentials ==="
id -u ubuntu >/dev/null 2>&1 || useradd -m -s /bin/bash -U ubuntu
echo "ubuntu:ubuntu" | chpasswd
passwd -u ubuntu 2>/dev/null || true
chage -M -1 ubuntu 2>/dev/null || true

cat << 'EOF' > /etc/sudoers.d/90-ubuntu-user
ubuntu ALL=(ALL) NOPASSWD:ALL
EOF
chmod 0440 /etc/sudoers.d/90-ubuntu-user

# SSH server drop-in
mkdir -p /etc/ssh/sshd_config.d
cat << 'EOF' > /etc/ssh/sshd_config.d/50-ambarella.conf
PasswordAuthentication yes
KbdInteractiveAuthentication yes
PermitRootLogin no
EOF

# SSH hostkeys service
systemctl daemon-reload
systemctl enable ssh-hostkeys.service || true
systemctl enable getty@tty0.service || true
systemctl enable serial-getty@ttyAMA0.service || true

# 2. Netplan & Network
echo "=== [AMBA-PROVISION] Configuring netplan ==="
mkdir -p /etc/netplan
rm -f /etc/netplan/50-cloud-init.yaml /etc/netplan/*cloud*

# Ensure 01-ambarella.yaml is in place
if [ -f /tmp/assets/01-ambarella.yaml ]; then
    cp -f /tmp/assets/01-ambarella.yaml /etc/netplan/01-ambarella.yaml
fi

# 3. Console & GRUB
echo "=== [AMBA-PROVISION] Configuring console and GRUB ==="
mkdir -p /etc/default/grub.d
cat << 'EOF' > /etc/default/grub.d/50-console.cfg
GRUB_CMDLINE_LINUX_DEFAULT="console=tty0 console=ttyAMA0,115200"
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
    EFI_SRC=$(find /boot/efi -iname "grubaa64.efi" -o -iname "shimaa64.efi" -o -iname "shim*.efi" 2>/dev/null | head -n 1)
    if [ -n "$EFI_SRC" ] && [ -f "$EFI_SRC" ]; then
        echo "Found EFI source: $EFI_SRC, copying to /boot/efi/EFI/BOOT/BOOTAA64.EFI"
        cp -f "$EFI_SRC" /boot/efi/EFI/BOOT/BOOTAA64.EFI
    fi
fi

# 7. Package Policy: Retain-set & Purges
echo "=== [AMBA-PROVISION] Applying package policy ==="

RETAIN_PACKAGES=(
    apt base-files bash ca-certificates cloud-guest-utils curl dbus
    e2fsprogs efibootmgr gnupg grub-efi-arm64 grub-efi-arm64-bin
    grub-efi-arm64-signed initramfs-tools iproute2 iputils-ping kmod
    locales netplan.io openssh-client openssh-server pciutils python3
    sudo systemd systemd-resolved systemd-sysv systemd-timesyncd udev
    ubuntu-keyring ubuntu-minimal util-linux vim-tiny
)

REMOVE_PACKAGES=(
    apport apport-core-dump-handler apport-symptoms bpfcc-tools bpftrace
    cloud-init cloud-initramfs-copymods cloud-initramfs-dyn-netconf fwupd
    fwupd-signed landscape-common linux-headers-generic linux-headers-virtual
    linux-tools-common lxd-agent-loader lxd-installer man-db manpages
    manpages-dev modemmanager motd-news-config multipath-tools open-iscsi
    open-vm-tools packagekit packagekit-tools pollinate rsyslog snapd
    sosreport ssh-import-id
    unattended-upgrades udisks2 usb-modeswitch usb-modeswitch-data git git-man
)

# Mark retain-set manual (batched dpkg-query & apt-mark)
mapfile -t INSTALLED_RETAIN < <(dpkg-query -W -f='${Package} ${Status}\n' "${RETAIN_PACKAGES[@]}" 2>/dev/null | grep "ok installed" | awk '{print $1}')
if [ "${#INSTALLED_RETAIN[@]}" -gt 0 ]; then
    apt-mark manual "${INSTALLED_RETAIN[@]}" >/dev/null 2>&1 || true
fi

# Mark installed kernel images and modules manual
mapfile -t INSTALLED_KERNELS < <(dpkg-query -W -f='${Package} ${Status}\n' 'linux-image-*-generic' 'linux-modules-*-generic' 'linux-image-virtual' 'linux-virtual' 2>/dev/null | grep "ok installed" | awk '{print $1}')
if [ "${#INSTALLED_KERNELS[@]}" -gt 0 ]; then
    apt-mark manual "${INSTALLED_KERNELS[@]}" >/dev/null 2>&1 || true
fi

# Expand removal candidates (batched dpkg-query)
mapfile -t TO_REMOVE < <(dpkg-query -W -f='${Package} ${Status}\n' "${REMOVE_PACKAGES[@]}" 'linux-headers-*-generic' 'linux-tools-*' 'ubuntu-server' 2>/dev/null | grep "ok installed" | awk '{print $1}')

# Pre-stop services targeted for removal to eliminate systemd IPC timeout delays under TCG
systemctl stop snapd.service snapd.socket modemmanager.service rsyslog.service syslog.socket multipathd.service multipathd.socket open-iscsi.service iscsid.service iscsid.socket sysstat-collect.timer sysstat-summary.timer sysstat.service 2>/dev/null || true
systemctl mask sysstat-collect.timer sysstat-summary.timer sysstat.service 2>/dev/null || true

# Configure fast gzip compression for initramfs if initramfs-tools is present
if [ -f /etc/initramfs-tools/initramfs.conf ]; then
    sed -i 's/^COMPRESS=.*/COMPRESS=gzip/' /etc/initramfs-tools/initramfs.conf 2>/dev/null || true
    grep -q "COMPRESSLEVEL=1" /etc/initramfs-tools/initramfs.conf 2>/dev/null || echo "COMPRESSLEVEL=1" >> /etc/initramfs-tools/initramfs.conf
fi

# Temporarily divert update-initramfs if present so it does not trigger repetitively
if [ -f /usr/sbin/update-initramfs ]; then
    dpkg-divert --local --rename --add /usr/sbin/update-initramfs
    ln -sf /bin/true /usr/sbin/update-initramfs
fi

if [ "${#TO_REMOVE[@]}" -gt 0 ]; then
    echo "Simulating package purge: ${TO_REMOVE[*]}"
    # Dry run purge with auto-remove without --allow-remove-essential
    apt-get purge --auto-remove -s -y -o Dpkg::Use-Pty=0 "${TO_REMOVE[@]}" > /tmp/purge-simulation.log

    # Verify simulation does not remove essential packages
    if grep -E "^Remv (base-files|bash|systemd|util-linux|coreutils|apt) " /tmp/purge-simulation.log; then
        echo "[AMBA-PROVISION] ERROR: Purge simulation attempts to remove essential packages!" >&2
        cat /tmp/purge-simulation.log >&2
        exit 1
    fi

    echo "Executing package purge with auto-remove..."
    apt-get purge --auto-remove -y -o Dpkg::Use-Pty=0 "${TO_REMOVE[@]}"
fi

# Restore update-initramfs and generate single optimized ramdisk
if [ -f /usr/sbin/update-initramfs.distrib ]; then
    rm -f /usr/sbin/update-initramfs
    dpkg-divert --local --rename --remove /usr/sbin/update-initramfs
    echo "=== [AMBA-PROVISION] Generating final initramfs (gzip) ==="
    update-initramfs -u || true
fi
if command -v update-grub >/dev/null 2>&1; then
    update-grub || true
fi

# Verify locale
locale-gen en_US.UTF-8 || true
update-locale LANG=en_US.UTF-8 || true

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

# 10. Disable and remove amba-image-provision service
systemctl disable amba-image-provision.service 2>/dev/null || true
rm -f /etc/systemd/system/amba-image-provision.service

sync

echo "=== [AMBA-PROVISION] Provisioning complete ==="
echo "AMBA_IMAGE_PROVISION_COMPLETE" > /dev/console 2>/dev/null || true
echo "AMBA_IMAGE_PROVISION_COMPLETE"

# Shutdown guest
systemctl poweroff || poweroff -f
