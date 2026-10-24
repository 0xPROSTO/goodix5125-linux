#!/usr/bin/env python3
"""Measures fprintd verification timing on the real sensor: how long until
the sensor is ready, and how long from the touch to the result and to the
end of the action. Prompts with desktop notifications.

  latency.py [rounds]
"""

import subprocess
import sys
import time

from gi.repository import Gio, GLib

BUS = Gio.bus_get_sync(Gio.BusType.SYSTEM)
NAME = "net.reactivated.Fprint"
DEV = "/net/reactivated/Fprint/Device/0"
IFACE = "net.reactivated.Fprint.Device"


def call(method, args=None):
    return BUS.call_sync(NAME, DEV, IFACE, method, args, None, Gio.DBusCallFlags.NONE,
                         60000, None)


def notify(title, body=""):
    subprocess.run(["notify-send", "-a", "Goodix 5125", "-u", "critical", "-t", "4000",
                    title, body], check=False)


def one_round(loop_ctx):
    marks = {}
    result = {}

    def on_props(conn, sender, path, iface, signal, params):
        changed = params.unpack()[1]
        now = time.monotonic()
        if changed.get("finger-needed") and "ready" not in marks:
            marks["ready"] = now
        if changed.get("finger-present") and "touch" not in marks:
            marks["touch"] = now

    def on_status(conn, sender, path, iface, signal, params):
        res, done = params.unpack()
        if res in ("verify-match", "verify-no-match") and "result" not in marks:
            marks["result"] = time.monotonic()
            result["r"] = res

    subs = [BUS.signal_subscribe(NAME, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                 DEV, None, Gio.DBusSignalFlags.NONE, on_props),
            BUS.signal_subscribe(NAME, IFACE, "VerifyStatus", DEV, None,
                                 Gio.DBusSignalFlags.NONE, on_status)]
    t0 = time.monotonic()
    call("VerifyStart", GLib.Variant("(s)", ("any",)))
    while "result" not in marks and time.monotonic() - t0 < 60:
        loop_ctx.iteration(False) or time.sleep(0.002)
    call("VerifyStop")
    stopped = time.monotonic()
    for s in subs:
        BUS.signal_unsubscribe(s)
    return t0, marks, stopped, result.get("r", "timeout")


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    ctx = GLib.MainContext.default()
    call("Claim", GLib.Variant("(s)", ("",)))
    try:
        for i in range(1, rounds + 1):
            notify(f"Замер {i}/{rounds}", "Приложи правый указательный, подержи секунду и убери")
            t0, m, stopped, res = one_round(ctx)
            def ms(a, b):
                return f"{(b - a) * 1000:5.0f}" if a is not None and b is not None else "    ?"
            print(f"{i}: {res:16s} ready {ms(t0, m.get('ready'))} ms"
                  f" | touch -> result {ms(m.get('touch'), m.get('result'))} ms"
                  f" | result -> stopped {ms(m.get('result'), stopped)} ms", flush=True)
            time.sleep(2)
    finally:
        call("Release")
    notify("Замер окончен")


if __name__ == "__main__":
    main()
