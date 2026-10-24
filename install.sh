#!/usr/bin/env bash
# Installs the Goodix 27c6:5125 fingerprint driver on CachyOS / Arch.
#
#   ./install.sh          build + install libfprint-goodix5125, fprintd, the key
#   ./install.sh --sudo   also allow the fingerprint for sudo
#
# Run as your normal user; it calls sudo where needed.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
WITH_SUDO=0
[ "${1:-}" = "--sudo" ] && WITH_SUDO=1

die() { echo "error: $*" >&2; exit 1; }

[ "$EUID" -ne 0 ] || die "run as your user, not root (makepkg refuses root)"
[ -s "$HERE/psk.bin" ] || [ -s /etc/goodix5125/psk.bin ] || die "psk.bin (sensor key) not found"

echo "==> building libfprint with the goodix5125 driver"
cd "$HERE/packaging"
rm -f libfprint-goodix5125-*.pkg.tar.zst
makepkg -sf --noconfirm

echo "==> installing"
sudo pacman -U --noconfirm "$HERE"/packaging/libfprint-goodix5125-*.pkg.tar.zst
sudo pacman -S --needed --noconfirm fprintd

if [ -s "$HERE/psk.bin" ]; then
    sudo install -D -m 600 -o root -g root "$HERE/psk.bin" /etc/goodix5125/psk.bin
fi

# The development rule gave every local user raw access to the sensor; only
# fprintd (root) needs it now.
if [ -e /etc/udev/rules.d/70-goodix-5125.rules ]; then
    sudo rm -f /etc/udev/rules.d/70-goodix-5125.rules
    sudo udevadm control --reload
    sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=27c6
fi

if [ "$WITH_SUDO" = 1 ] && ! grep -q pam_fprintd /etc/pam.d/sudo; then
    sudo cp /etc/pam.d/sudo /etc/pam.d/sudo.before-fprintd
    sudo sed -i '0,/^auth/s//auth\t\tsufficient\tpam_fprintd.so\nauth/' /etc/pam.d/sudo
    echo "==> sudo now accepts the fingerprint (backup: /etc/pam.d/sudo.before-fprintd)"
fi

sudo systemctl try-restart fprintd.service || true
echo
echo "Done. Enroll a finger: GNOME Settings -> System -> Users -> Fingerprint Login,"
echo "or in a terminal: fprintd-enroll -f right-index-finger"
