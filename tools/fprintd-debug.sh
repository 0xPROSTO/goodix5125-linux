#!/usr/bin/env bash
# Debug switch for the installed driver under fprintd.
#
#   fprintd-debug.sh on       log match scores to the journal and dump captures
#   fprintd-debug.sh collect  copy the dumps to captures/fprintd (0700, yours)
#   fprintd-debug.sh off      back to normal, dumps wiped
#
# Dumps are fingerprint images: they stay on this machine, never share them.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
DROPIN=/etc/systemd/system/fprintd.service.d/goodix5125-debug.conf
DUMP=/var/lib/fprint/goodix5125-dump   # fprintd may only write under /var/lib/fprint

case "${1:-}" in
on)
    sudo install -d -m 700 "$DUMP"
    sudo install -d -m 755 "$(dirname "$DROPIN")"
    printf '[Service]\nEnvironment=G_MESSAGES_DEBUG=libfprint-goodix5125\nEnvironment=GOODIX5125_DUMP=%s\n' "$DUMP" |
        sudo tee "$DROPIN" >/dev/null
    sudo systemctl daemon-reload
    sudo systemctl restart fprintd.service
    echo "debug on: journalctl -u fprintd shows scores, captures go to $DUMP"
    ;;
collect)
    install -d -m 700 "$HERE/captures/fprintd"
    sudo sh -c "cp $DUMP/*.pgm '$HERE/captures/fprintd/' 2>/dev/null || true"
    sudo chown -R "$(id -u):$(id -g)" "$HERE/captures/fprintd"
    chmod 600 "$HERE"/captures/fprintd/*.pgm 2>/dev/null || true
    echo "copied $(ls "$HERE/captures/fprintd" | wc -l) files to captures/fprintd"
    ;;
off)
    sudo rm -f "$DROPIN"
    sudo rm -rf "$DUMP"
    sudo systemctl daemon-reload
    sudo systemctl restart fprintd.service
    echo "debug off"
    ;;
*)
    echo "usage: $0 on|collect|off" >&2
    exit 2
    ;;
esac
