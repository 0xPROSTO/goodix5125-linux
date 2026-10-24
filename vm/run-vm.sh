#!/usr/bin/env bash
# Boots the throwaway Windows VM used to provision/extract the Goodix PSK.
# All state lives in ../.vm and is deleted by destroy-vm.sh.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
D=$HERE/../.vm
OVMF=/usr/share/edk2/x64
cd "$D"

[ -f disk.qcow2 ] || qemu-img create -f qcow2 disk.qcow2 64G
[ -f OVMF_VARS.fd ] || cp "$OVMF/OVMF_VARS.4m.fd" OVMF_VARS.fd

if [ ! -f setup.img ]; then
    truncate -s 64M setup.img
    mformat -i setup.img -F -v GXSETUP ::
    mcopy -i setup.img "$HERE/autounattend.xml" "$HERE/gx-run.ps1" \
        "$HERE/../tools/extract-psk.ps1" ::
fi

mkdir -p tpm
if ! [ -S tpm/sock ] || ! pgrep -f "swtpm socket --tpmstate dir=$D/tpm" >/dev/null; then
    rm -f tpm/sock
    swtpm socket --tpmstate dir="$D/tpm" --ctrl type=unixio,path="$D/tpm/sock" \
        --tpm2 --daemon --pid file="$D/tpm/pid"
    sleep 1
fi

exec qemu-system-x86_64 -name gxvm \
    -machine q35,accel=kvm -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time \
    -smp 4 -m "${VM_MEM:-3584}" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF/OVMF_CODE.4m.fd" \
    -drive if=pflash,format=raw,file="$D/OVMF_VARS.fd" \
    -chardev socket,id=chrtpm,path="$D/tpm/sock" \
    -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-tis,tpmdev=tpm0 \
    -drive file="$D/disk.qcow2",if=none,id=hd0,format=qcow2,cache=unsafe \
    -device ide-hd,drive=hd0,bus=ide.0,bootindex=1 \
    -drive file="$D/win11.iso",media=cdrom,if=none,id=cd0,readonly=on \
    -device ide-cd,drive=cd0,bus=ide.1,bootindex=2 \
    -device qemu-xhci,id=xhci \
    -drive file="$D/setup.img",format=raw,if=none,id=stick \
    -device usb-storage,bus=xhci.0,drive=stick,removable=on \
    -device usb-host,id=gxfp,bus=xhci.0,vendorid=0x27c6,productid=0x5125 \
    -device usb-tablet,bus=xhci.0 \
    -netdev user,id=n0 -device e1000e,netdev=n0 \
    -serial file:"$D/serial.log" \
    -display none -vnc unix:"$D/vnc.sock" \
    -qmp unix:"$D/qmp.sock",server=on,wait=off \
    -pidfile "$D/qemu.pid"
