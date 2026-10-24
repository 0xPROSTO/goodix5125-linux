#!/usr/bin/env bash
# Stops the throwaway VM and deletes every trace of it (disk, ISO, TPM state,
# UEFI vars, logs). Saves psk.bin first if it was extracted.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
D=$HERE/../.vm

[ -f "$D/qemu.pid" ] && kill "$(cat "$D/qemu.pid")" 2>/dev/null
[ -f "$D/tpm/pid" ] && kill "$(cat "$D/tpm/pid")" 2>/dev/null
sleep 1

if [ -f "$D/setup.img" ] && mdir -i "$D/setup.img" ::psk.bin >/dev/null 2>&1; then
    mcopy -n -i "$D/setup.img" ::psk.bin "$HERE/../psk.bin" 2>/dev/null
    chmod 600 "$HERE/../psk.bin"
fi

rm -rf "$D"
echo "VM destroyed"
