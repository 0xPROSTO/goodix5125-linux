#!/usr/bin/env python3
"""Read-only probe for the Goodix 27c6:5125 fingerprint MCU.

Sends only query commands (firmware/IAP version, PSK hash, OTP, chip ID,
MCU state). Nothing here erases, flashes or writes the PSK, so the Windows
pairing stays intact.

Run:  sudo .venv/bin/python tools/probe.py
"""

import json
import struct
import sys
import time

import usb.core
import usb.util

VID, PID = 0x27C6, 0x5125

FLAGS_MP = 0xA0
FLAGS_TLS = 0xB0

CMD_NOP = 0x00
CMD_READ_SENSOR_REGISTER = 0x82
CMD_ENABLE_CHIP = 0x96
CMD_READ_OTP = 0xA6
CMD_FIRMWARE_VERSION = 0xA8
CMD_QUERY_MCU_STATE = 0xAE
CMD_ACK = 0xB0
CMD_PRESET_PSK_READ = 0xE4
CMD_GET_IAP_VERSION = 0xF6

# Hard whitelist: the probe refuses to send anything else.
READ_ONLY = {
    CMD_NOP, CMD_READ_SENSOR_REGISTER, CMD_ENABLE_CHIP, CMD_READ_OTP,
    CMD_FIRMWARE_VERSION, CMD_QUERY_MCU_STATE, CMD_PRESET_PSK_READ,
    CMD_GET_IAP_VERSION
}


def pack(payload, flags=FLAGS_MP):
    head = struct.pack("<BH", flags, len(payload))
    return head + bytes([sum(head) & 0xFF]) + payload


def unpack(data):
    flags, length = struct.unpack("<BH", data[:3])
    if sum(data[:3]) & 0xFF != data[3]:
        raise ValueError(f"bad pack checksum: {data[:8].hex()}")
    return flags, data[4:4 + length]


def proto(cmd, payload, checksum=True):
    body = struct.pack("<BH", cmd, len(payload) + 1) + payload
    return body + bytes([(0xAA - sum(body)) & 0xFF if checksum else 0x88])


def unproto(data):
    cmd, length = struct.unpack("<BH", data[:3])
    return cmd, data[3:2 + length]


class Dev:

    def __init__(self):
        self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            sys.exit("Goodix 27c6:5125 not found")
        cfg = self.dev.get_active_configuration()
        intf = usb.util.find_descriptor(cfg, bInterfaceClass=0x0A)
        self.intf = intf.bInterfaceNumber
        if self.dev.is_kernel_driver_active(self.intf):
            self.dev.detach_kernel_driver(self.intf)
        usb.util.claim_interface(self.dev, self.intf)
        ep = lambda d: usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(
                e.bEndpointAddress) == d)
        self.ep_out = ep(usb.util.ENDPOINT_OUT).bEndpointAddress
        self.ep_in = ep(usb.util.ENDPOINT_IN).bEndpointAddress
        self.log = []

    def close(self):
        usb.util.release_interface(self.dev, self.intf)
        usb.util.dispose_resources(self.dev)

    def write(self, data):
        if len(data) % 64:
            data += b"\x00" * (64 - len(data) % 64)
        for i in range(0, len(data), 64):
            self.dev.write(self.ep_out, data[i:i + 64], 2000)

    def read(self, timeout=2000):
        data = bytes(self.dev.read(self.ep_in, 0x10000, timeout))
        self.log.append(("in", data.hex()))
        return data

    def drain(self):
        try:
            while True:
                self.read(100)
        except usb.core.USBTimeoutError:
            pass

    def cmd(self, cmd, payload, reply=True, checksum=True):
        assert cmd in READ_ONLY, f"command {cmd:#x} is not read-only"
        msg = pack(proto(cmd, payload, checksum))
        self.log.append(("out", msg.hex()))
        self.write(msg)
        flags, body = unpack(self.read())
        rcmd, rbody = unproto(body)
        if rcmd != CMD_ACK or rbody[0] != cmd:
            raise ValueError(f"expected ACK for {cmd:#x}, got {body.hex()}")
        if not reply:
            return None
        flags, body = unpack(self.read())
        rcmd, rbody = unproto(body)
        if rcmd != cmd:
            raise ValueError(f"expected reply {cmd:#x}, got {body.hex()}")
        return rbody


def step(results, name, fn):
    try:
        value = fn()
        results[name] = value
        print(f"[ok]   {name}: {value}")
    except Exception as e:  # keep going, every answer is useful
        results[name] = f"ERROR: {e!r}"
        print(f"[fail] {name}: {e!r}")


def main():
    d = Dev()
    r = {}
    d.drain()

    def nop():
        msg = pack(proto(CMD_NOP, b"\x00\x00\x00\x00", checksum=False))
        d.log.append(("out", msg.hex()))
        d.write(msg)
        try:
            return d.read(200).hex()
        except usb.core.USBTimeoutError:
            return "no reply (normal)"

    step(r, "nop", nop)
    step(r, "enable_chip", lambda: d.cmd(CMD_ENABLE_CHIP, b"\x01\x00", reply=False) or "acked")
    step(r, "nop2", nop)
    step(r, "firmware_version", lambda: d.cmd(CMD_FIRMWARE_VERSION, b"\x00\x00").split(b"\x00")[0].decode(errors="replace"))
    step(r, "iap_version", lambda: d.cmd(CMD_GET_IAP_VERSION, b"\x14\x00").split(b"\x00")[0].decode(errors="replace"))

    def psk(flags):
        m = d.cmd(CMD_PRESET_PSK_READ, struct.pack("<II", flags, 0))
        if m[0] != 0:
            return f"status={m[0]:#x} raw={m.hex()}"
        f, n = struct.unpack("<II", m[1:9])
        return {"flags": hex(f), "len": n, "data": m[9:9 + n].hex()}

    step(r, "psk_hash_bb020003", lambda: psk(0xBB020003))
    step(r, "otp", lambda: d.cmd(CMD_READ_OTP, b"\x00\x00").hex())
    step(r, "chip_id_reg0", lambda: d.cmd(CMD_READ_SENSOR_REGISTER, b"\x00\x00\x00\x04").hex())
    step(r, "mcu_state", lambda: d.cmd(CMD_QUERY_MCU_STATE, b"\x55").hex())

    d.close()

    r["_log"] = d.log
    out = "probe-result.json"
    with open(out, "w") as f:
        json.dump(r, f, indent=2)
    print(f"\nSaved full transcript to {out}")


if __name__ == "__main__":
    main()
