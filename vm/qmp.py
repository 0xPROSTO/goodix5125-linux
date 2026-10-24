#!/usr/bin/env python3
"""Tiny QMP client for the throwaway Windows VM.

  qmp.py SOCK shot FILE.png       screenshot
  qmp.py SOCK keys KEY [N] [GAP]  press KEY N times (qcode names, e.g. ret, spc)
  qmp.py SOCK type TEXT           type ASCII text
  qmp.py SOCK quit                hard-stop the VM
"""

import json
import socket
import sys
import time


class QMP:

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile("rw")
        self.f.readline()  # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            msg = json.loads(self.f.readline())
            if "return" in msg or "error" in msg:
                return msg

    def key(self, *qcodes):
        keys = [{"type": "qcode", "data": k} for k in qcodes]
        return self.cmd("send-key", keys=keys)


SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
           "&": "7", "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
           ":": "semicolon", '"': "apostrophe", "<": "comma", ">": "dot",
           "?": "slash", "|": "backslash", "{": "bracket_left",
           "}": "bracket_right", "~": "grave_accent"}
PLAIN = {" ": "spc", "-": "minus", "=": "equal", ";": "semicolon",
         "'": "apostrophe", ",": "comma", ".": "dot", "/": "slash",
         "\\": "backslash", "[": "bracket_left", "]": "bracket_right",
         "`": "grave_accent", "\n": "ret"}


def type_text(q, text):
    for ch in text:
        if ch.isalpha():
            q.key("shift", ch.lower()) if ch.isupper() else q.key(ch)
        elif ch.isdigit():
            q.key(ch)
        elif ch in SHIFTED:
            q.key("shift", SHIFTED[ch])
        else:
            q.key(PLAIN[ch])
        time.sleep(0.03)


def main():
    sock, op, *rest = sys.argv[1:]
    q = QMP(sock)
    if op == "shot":
        print(q.cmd("screendump", filename=rest[0], format="png"))
    elif op == "keys":
        n = int(rest[1]) if len(rest) > 1 else 1
        gap = float(rest[2]) if len(rest) > 2 else 0.5
        for _ in range(n):
            q.key(*rest[0].split("+"))
            time.sleep(gap)
    elif op == "type":
        type_text(q, rest[0])
    elif op == "quit":
        print(q.cmd("quit"))


if __name__ == "__main__":
    main()
