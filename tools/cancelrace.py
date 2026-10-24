#!/usr/bin/env python3
"""Reproduces what GNOME Settings does around its background identification:
VerifyStart, later VerifyStop and, without waiting for its reply, another
request on the same claim. Prints whether that request was refused with
"already in progress", which is what broke deleting and enrolling there.

  cancelrace.py [delay before stop, s]...   (default: 0.3 1 3)

Needs no finger and changes nothing: the follow-up request is a VerifyStart.
"""

import sys
import time

from gi.repository import Gio, GLib

BUS = Gio.bus_get_sync(Gio.BusType.SYSTEM)
DEV = "/net/reactivated/Fprint/Device/0"


def call(method, args=None):
    t0 = time.monotonic()
    try:
        BUS.call_sync("net.reactivated.Fprint", DEV, "net.reactivated.Fprint.Device", method,
                      args, None, Gio.DBusCallFlags.NONE, 30000, None)
        err = None
    except GLib.Error as e:
        err = e.message.split(":")[-1].strip()
    return time.monotonic() - t0, err


def main():
    delays = [float(a) for a in sys.argv[1:]] or [0.3, 1.0, 3.0]
    _, err = call("Claim", GLib.Variant("(s)", ("",)))
    if err:
        sys.exit(f"claim: {err}")
    try:
        for d in delays:
            call("VerifyStart", GLib.Variant("(s)", ("any",)))
            time.sleep(d)
            BUS.call("net.reactivated.Fprint", DEV, "net.reactivated.Fprint.Device",
                     "VerifyStop", None, None, Gio.DBusCallFlags.NONE, 30000, None, None)
            _, err2 = call("VerifyStart", GLib.Variant("(s)", ("any",)))
            print(f"stop after {d:.1f}s, next request at once: {err2 or 'ok'}", flush=True)
            if not err2:
                call("VerifyStop")
    finally:
        call("Release")


if __name__ == "__main__":
    main()
