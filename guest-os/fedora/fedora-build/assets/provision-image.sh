#!/bin/bash
# In-VM Provisioning Script for Lean Fedora 44 ARM64 HVM Image
# Executed via amba-image-provision.service after cloud-final.service

set -euo pipefail
trap 'echo "AMBA_IMAGE_PROVISION_FAILED: line $LINENO" > /dev/console 2>/dev/null || true; echo "AMBA_IMAGE_PROVISION_FAILED: line $LINENO"' ERR

echo "=== [AMBA-PROVISION] Starting in-VM image provisioning ==="

# 1. User & Authentication
echo "=== [AMBA-PROVISION] Configuring user fedora and credentials ==="
id -u fedora >/dev/null 2>&1 || useradd -m -s /bin/bash -G wheel fedora
echo "fedora:fedora" | chpasswd
passwd -u fedora 2>/dev/null || true
chage -M -1 fedora 2>/dev/null || true

cat << 'EOF' > /etc/sudoers.d/90-fedora-user
fedora ALL=(ALL) NOPASSWD:ALL
EOF
chmod 0440 /etc/sudoers.d/90-fedora-user

# SSH server drop-in (sshd uses the first value it reads, so this file sorts first); do not accept the client's locale
mkdir -p /etc/ssh/sshd_config.d
cat << 'EOF' > /etc/ssh/sshd_config.d/00-ambarella.conf
PasswordAuthentication yes
KbdInteractiveAuthentication yes
PermitRootLogin no
EOF
sed -i 's/^AcceptEnv/#AcceptEnv/' /etc/ssh/sshd_config

systemctl daemon-reload
systemctl enable ssh-hostkeys.service || true
systemctl enable sshd.service || true
systemctl enable serial-getty@ttyAMA0.service || true

# 2. Network: systemd-networkd and systemd-resolved instead of NetworkManager
echo "=== [AMBA-PROVISION] Configuring systemd-networkd ==="
mkdir -p /etc/systemd/network
if [ -f /tmp/assets/10-ambarella.network ]; then
    cp -f /tmp/assets/10-ambarella.network /etc/systemd/network/10-ambarella.network
fi
systemctl enable systemd-networkd.service systemd-resolved.service
systemctl disable systemd-networkd-wait-online.service 2>/dev/null || true
systemctl disable NetworkManager.service NetworkManager-wait-online.service NetworkManager-dispatcher.service 2>/dev/null || true
ln -sf ../run/systemd/resolve/stub-resolv.conf /etc/resolv.conf

# 3. Console & boot loader (the base image already sets GRUB_TIMEOUT=0)
echo "=== [AMBA-PROVISION] Configuring console and kernel command line ==="
grep -q '\bquiet\b' /etc/default/grub || sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="quiet /' /etc/default/grub
grubby --update-kernel=ALL --args="quiet"

# 4. Storage & Root Growth (btrfs)
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
    EFI_SRC=$(find /boot/efi -iname "shimaa64.efi" 2>/dev/null | head -n 1)
    if [ -n "$EFI_SRC" ] && [ -f "$EFI_SRC" ]; then
        echo "Found EFI source: $EFI_SRC, copying to /boot/efi/EFI/BOOT/BOOTAA64.EFI"
        cp -f "$EFI_SRC" /boot/efi/EFI/BOOT/BOOTAA64.EFI
    fi
fi

# 7. Package Policy: Retain-set & Purges
echo "=== [AMBA-PROVISION] Applying package policy ==="

RETAIN_PACKAGES=(
    bash ca-certificates cloud-utils-growpart coreutils crypto-policies curl dbus-broker
    dnf5 dracut e2fsprogs efibootmgr glibc grub2-common grub2-efi-aa64 grub2-tools
    grub2-tools-minimal grubby iproute iputils kernel-core kernel-modules-core kmod
    openssh-clients openssh-server passwd policycoreutils procps-ng rpm selinux-policy-targeted
    shadow-utils shim-aa64 sudo systemd systemd-networkd systemd-pam systemd-resolved
    systemd-udev tar util-linux vim-minimal btrfs-progs chrony libselinux-utils
)

REMOVE_PACKAGES=(
    7zip aardvark-dns catatonit cloud-init conmon console-login-helper-messages
    container-selinux containers-common containers-common-extra criu criu-libs crun
    device-mapper-event device-mapper-persistent-data fuse-overlayfs groff-base lvm2
    man-db NetworkManager nano nano-default-editor netavark passt passt-selinux
    podman podman-sequoia python3-dnf-plugin-tracer python3-tracer qemu-guest-agent
    qemu-user-static qemu-user-static-aarch64 qemu-user-static-alpha qemu-user-static-arm
    qemu-user-static-hexagon qemu-user-static-hppa qemu-user-static-loongarch64
    qemu-user-static-m68k qemu-user-static-microblaze qemu-user-static-mips qemu-user-static-or1k
    qemu-user-static-ppc qemu-user-static-riscv qemu-user-static-s390x qemu-user-static-sh4
    qemu-user-static-sparc qemu-user-static-x86 qemu-user-static-xtensa rsync sssd-client
    sssd-common sssd-kcm sssd-krb5-common tracer-common unzip whois-nls xfsprogs
    zram-generator zram-generator-defaults
)

# Never install documentation or weak dependencies again
cat << 'EOF' >> /etc/dnf/dnf.conf
install_weak_deps=False
tsflags=nodocs
EOF

# Mark the retain-set as user-installed so that removing the rest cannot take it along as an unused dependency
mapfile -t INSTALLED_RETAIN < <(rpm -q --qf '%{NAME}\n' "${RETAIN_PACKAGES[@]}" 2>/dev/null | grep -v "is not installed" | sort -u)
if [ "${#INSTALLED_RETAIN[@]}" -gt 0 ]; then
    dnf5 mark user -y "${INSTALLED_RETAIN[@]}"
fi

mapfile -t TO_REMOVE < <(rpm -q --qf '%{NAME}\n' "${REMOVE_PACKAGES[@]}" 2>/dev/null | grep -v "is not installed" | sort -u)

# Mask the periodic jobs (no package manager traffic on a running guest)
systemctl mask dnf-makecache.timer dnf5-makecache.timer fstrim.timer 2>/dev/null || true

if [ "${#TO_REMOVE[@]}" -gt 0 ]; then
    echo "Simulating package removal: ${TO_REMOVE[*]}"
    dnf5 remove --assumeno "${TO_REMOVE[@]}" > /tmp/purge-simulation.log 2>&1 || true
    cat /tmp/purge-simulation.log

    # Names of everything the transaction would remove: the lines that carry an architecture column
    awk 'NF >= 5 && $2 ~ /^(aarch64|noarch|x86_64|i686)$/ {print $1}' /tmp/purge-simulation.log | sort -u > /tmp/purge-names.txt
    if [ ! -s /tmp/purge-names.txt ]; then
        echo "[AMBA-PROVISION] ERROR: the removal simulation produced no package list" >&2
        exit 1
    fi
    for pkg in "${INSTALLED_RETAIN[@]}"; do
        if grep -qx "${pkg}" /tmp/purge-names.txt; then
            echo "[AMBA-PROVISION] ERROR: removal simulation attempts to remove retained package ${pkg}!" >&2
            exit 1
        fi
    done

    echo "Executing package removal..."
    dnf5 remove -y "${TO_REMOVE[@]}"
fi

# Remove documentation and manual pages that were installed before the rule existed
rm -rf /usr/share/doc/* /usr/share/man/* /usr/share/info/* /etc/cloud /var/lib/cloud

# Regenerate the ramdisks of the installed kernel once, after the removals
dracut --force --regenerate-all

# SELinux labels of what this script created
restorecon -RF /etc /usr/local /root /home /var/lib 2>/dev/null || true

# 8. Clean machine identity and SSH keys
echo "=== [AMBA-PROVISION] Cleaning temporary identity and SSH keys ==="
rm -f /etc/ssh/ssh_host_*_key*
truncate -s 0 /etc/machine-id

# 9. Clean caches and temporary logs
dnf5 clean all || true
rm -rf /var/cache/dnf /var/cache/libdnf5 /var/lib/dnf/repos
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
